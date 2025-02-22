/*
 * Copyright 2019-2022 u-blox
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Only #includes of u_* and the C standard library are allowed here,
 * no platform stuff and no OS stuff.  Anything required from
 * the platform/OS must be brought in through u_port* to maintain
 * portability.
 */

/** @file
 * @brief Implementation of functions that are private to GNSS.
 * IMPORTANT: this code is changing a lot at the moment as we move
 * towards a more generic, streamed, approach - beware!
 */

#ifdef U_CFG_OVERRIDE
# include "u_cfg_override.h" // For a customer's configuration override
#endif

#include "stddef.h"    // NULL, size_t etc.
#include "stdint.h"    // int32_t etc.
#include "stdbool.h"
#include "string.h"    // memmove(), strstr()
#include "ctype.h"

#include "u_cfg_os_platform_specific.h"
#include "u_cfg_sw.h"

#include "u_error_common.h"

#include "u_assert.h"

#include "u_port.h"
#include "u_port_heap.h"
#include "u_port_os.h"
#include "u_port_uart.h"
#include "u_port_i2c.h"
#include "u_port_spi.h"
#include "u_port_debug.h"

#include "u_hex_bin_convert.h"

#include "u_at_client.h"

#include "u_ubx_protocol.h"

#include "u_device_shared.h"

#include "u_network_shared.h"

#include "u_gnss_module_type.h"
#include "u_gnss_type.h"
#include "u_gnss.h"
#include "u_gnss_msg.h"
#include "u_gnss_cfg.h"
#include "u_gnss_cfg_val_key.h"
#include "u_gnss_private.h"

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS
 * -------------------------------------------------------------- */

#ifndef U_GNSS_AT_BUFFER_LENGTH_BYTES
/** The length of a temporary buffer store a hex-encoded UBX-format
 * message when receiving responses over an AT interface.
 */
# define U_GNSS_AT_BUFFER_LENGTH_BYTES ((U_GNSS_MAX_UBX_PROTOCOL_MESSAGE_BODY_LENGTH_BYTES + \
                                         U_UBX_PROTOCOL_OVERHEAD_LENGTH_BYTES) * 2)
#endif

#ifndef U_GNSS_PRIVATE_SPI_READ_LENGTH_MIN_BYTES
/** The minimum number of bytes to read on an SPI transport when
 * trying to determine if there's anything valid to read.
 */
# define U_GNSS_PRIVATE_SPI_READ_LENGTH_MIN_BYTES 1
#endif

// Do some cross-checking
#if U_GNSS_PRIVATE_SPI_READ_LENGTH_MIN_BYTES > U_GNSS_DEFAULT_SPI_FILL_THRESHOLD
# error U_GNSS_PRIVATE_SPI_READ_LENGTH_MIN_BYTES must be less than or equal to U_GNSS_DEFAULT_SPI_FILL_THRESHOLD
#endif

#if U_GNSS_PRIVATE_SPI_READ_LENGTH_MIN_BYTES > U_GNSS_SPI_FILL_THRESHOLD_MAX
# error U_GNSS_PRIVATE_SPI_READ_LENGTH_MIN_BYTES must be less than or equal to U_GNSS_SPI_FILL_THRESHOLD_MAX
#endif

#if U_GNSS_DEFAULT_SPI_FILL_THRESHOLD > U_GNSS_SPI_BUFFER_LENGTH_BYTES
# error U_GNSS_DEFAULT_SPI_FILL_THRESHOLD must be less than or equal to U_GNSS_SPI_BUFFER_LENGTH_BYTES
#endif

#if U_GNSS_DEFAULT_SPI_FILL_THRESHOLD > U_GNSS_SPI_FILL_THRESHOLD_MAX
# error U_GNSS_DEFAULT_SPI_FILL_THRESHOLD must be less than or equal to U_GNSS_SPI_FILL_THRESHOLD_MAX
#endif

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/** Structure to hold a received UBX-format message.
 */
typedef struct {
    int32_t cls;
    int32_t id;
    char **ppBody;   /**< a pointer to a pointer that the received
                          message body will be written to. If *ppBody is NULL
                          memory will be allocated.  If ppBody is NULL
                          then the response is not captured (but this
                          structure may still be used for cls/id matching). */
    size_t bodySize; /**< the number of bytes of storage at *ppBody;
                          must be zero if ppBody is NULL or
                          *ppBody is NULL.  If non-zero it MUST be
                          large enough to fit the body in or the
                          CRC calculation will fail. */
} uGnssPrivateUbxReceiveMessage_t;

/* ----------------------------------------------------------------
 * VARIABLES THAT ARE SHARED THROUGHOUT THE GNSS IMPLEMENTATION
 * -------------------------------------------------------------- */

/** Root for the linked list of instances.
 */
uGnssPrivateInstance_t *gpUGnssPrivateInstanceList = NULL;

/** Mutex to protect the linked list.
 */
uPortMutexHandle_t gUGnssPrivateMutex = NULL;

/** The characteristics of the modules supported by this driver,
 * compiled into the driver.  Order is important: uGnssModuleType_t
 * is used to index into this array.
 */
const uGnssPrivateModule_t gUGnssPrivateModuleList[] = {
    {
        U_GNSS_MODULE_TYPE_M8, 0 /* features */
    },
    {
        U_GNSS_MODULE_TYPE_M9,
        ((1UL << (int32_t) U_GNSS_PRIVATE_FEATURE_CFGVALXXX) |
         (1UL << (int32_t) U_GNSS_PRIVATE_FEATURE_GEOFENCE)  /* features */
        )
    },
    {
        U_GNSS_MODULE_TYPE_M10,
        ((1UL << (int32_t) U_GNSS_PRIVATE_FEATURE_CFGVALXXX) /* features */
        )
    }
};

/** Number of items in the gUGnssPrivateModuleList array, has to be
 * done in this file and externed or GCC complains about asking
 * for the size of a partially defined type.
 */
const size_t gUGnssPrivateModuleListSize = sizeof(gUGnssPrivateModuleList) /
                                           sizeof(gUGnssPrivateModuleList[0]);

/* ----------------------------------------------------------------
 * VARIABLES
 * -------------------------------------------------------------- */

/** Table to convert a GNSS transport type into a streaming transport type.
 */
static const int32_t gGnssPrivateTransportTypeToStream[] = {
    U_GNSS_PRIVATE_STREAM_TYPE_NONE,  // U_GNSS_TRANSPORT_NONE
    U_GNSS_PRIVATE_STREAM_TYPE_UART,  // U_GNSS_TRANSPORT_UART
    U_ERROR_COMMON_INVALID_PARAMETER, // U_GNSS_TRANSPORT_AT
    U_GNSS_PRIVATE_STREAM_TYPE_I2C,   // U_GNSS_TRANSPORT_I2C
    U_GNSS_PRIVATE_STREAM_TYPE_SPI,   // U_GNSS_TRANSPORT_SPI
    U_GNSS_PRIVATE_STREAM_TYPE_UART,  // U_GNSS_TRANSPORT_UBX_UART
    U_GNSS_PRIVATE_STREAM_TYPE_I2C    // U_GNSS_TRANSPORT_UBX_I2C
};

/** Table to convert a port number to the UBX-CFG-VAL group ID that
 * configures that port number for output protocol. */
static const uGnssCfgValKeyGroupId_t gPortToCfgValGroupIdOutProt[] = {
    U_GNSS_CFG_VAL_KEY_GROUP_ID_I2COUTPROT,   // 0: I2C
    U_GNSS_CFG_VAL_KEY_GROUP_ID_UART1OUTPROT, // 1: UART/UART1
    U_GNSS_CFG_VAL_KEY_GROUP_ID_UART2OUTPROT, // 2: UART/UART2
    U_GNSS_CFG_VAL_KEY_GROUP_ID_USBOUTPROT,   // 3: USB
    U_GNSS_CFG_VAL_KEY_GROUP_ID_SPIOUTPROT    // 4: SPI
};

/** Table to convert an output protocol type to the UBX-CFG-VAL item ID
 * for that output protocol type. */
static const uint8_t gProtocolTypeToCfgValItemIdOutProt[] = {
    1, // 0: U_GNSS_PROTOCOL_UBX
    2, // 1: U_GNSS_PROTOCOL_NMEA
    4  // 2: U_GNSS_PROTOCOL_RTCM
};

/** Table to convert a UBX-CFG-VAL item ID for a protocol type into
 * one of our protocol types. */
static const int8_t gCfgValItemIdOutProtToProtocolType[] = {
    -1,
        U_GNSS_PROTOCOL_UBX,    // 1: UBX
        U_GNSS_PROTOCOL_NMEA,   // 2: NMEA
        -1,
        U_GNSS_PROTOCOL_RTCM    // 4: RTCM
    };

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: MESSAGE RELATED
 * -------------------------------------------------------------- */

// Match an NMEA ID with the wanted NMEA ID.  Both pointers must be
// to null-terminated strings.
static bool nmeaIdMatch(const char *pNmeaIdActual, const char *pNmeaIdWanted)
{
    bool match = true;

    if (pNmeaIdWanted != NULL) {  // A wanted string of NULL matches anything
        match = false;            // An actual string of NULL matches nothing (except NULL)
        if (pNmeaIdActual != NULL) {
            match = true;
            while ((*pNmeaIdWanted != 0) && match) {
                if ((*pNmeaIdActual == 0) ||
                    ((*pNmeaIdWanted != '?') && (*pNmeaIdWanted != *pNmeaIdActual))) {
                    match = false;
                }
                pNmeaIdActual++;
                pNmeaIdWanted++;
            }
        }
    }

    return match;
}

// Match an UBX ID with the wanted UXB ID, allowing ALL wildcards 0xFF.
static bool ubxIdMatch(uint16_t ubxIdActual, uint16_t ubxIdWanted)
{
    if ((ubxIdWanted &  U_GNSS_UBX_MESSAGE_ID_ALL) == U_GNSS_UBX_MESSAGE_ID_ALL) {
        ubxIdActual |= U_GNSS_UBX_MESSAGE_ID_ALL;
    }
    if ((ubxIdWanted & (U_GNSS_UBX_MESSAGE_CLASS_ALL << 8)) ==
        (U_GNSS_UBX_MESSAGE_CLASS_ALL << 8)) {
        ubxIdActual |= (U_GNSS_UBX_MESSAGE_CLASS_ALL << 8);
    }
    return ubxIdActual == ubxIdWanted;
}

// Match an RTCM ID with the wanted RTCM ID, allowing ALL wildcard 0xFFFF.
static bool rtcmIdMatch(uint16_t rtcmIdActual, uint16_t rtcmIdWanted)
{
    return (rtcmIdActual == rtcmIdWanted) || (rtcmIdWanted == U_GNSS_RTCM_MESSAGE_ID_ALL);
}

#ifdef U_GNSS_PRIVATE_DEBUG_PARSING
// Print out a message ID, only used when debugging message parsing.
static void printId(uGnssPrivateMessageId_t *pId)
{
    if (pId->type == U_GNSS_PROTOCOL_UBX) {
        uPortLog("UBX %04x", pId->id.ubx);
    } else if (pId->type == U_GNSS_PROTOCOL_NMEA) {
        uPortLog("NMEA %s", pId->id.nmea);
    } else if (pId->type == U_GNSS_PROTOCOL_RTCM) {
        uPortLog("RTCM %d", pId->id.rtcm);
    } else if (pId->type == U_GNSS_PROTOCOL_UNKNOWN) {
        uPortLog("UNKNOWN");
    } else {
        uPortLog("ERROR");
    }
}
#endif

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: STREAMING TRANSPORT ONLY
 * -------------------------------------------------------------- */

// Read or peek-at the data in the internal ring buffer.
static int32_t streamGetFromRingBuffer(uGnssPrivateInstance_t *pInstance,
                                       int32_t readHandle,
                                       char *pBuffer, size_t size,
                                       size_t offset,
                                       int32_t maxTimeMs,
                                       bool andRemove)
{
    int32_t errorCodeOrLength = (int32_t) U_ERROR_COMMON_INVALID_PARAMETER;
    size_t receiveSize;
    size_t totalSize = 0;
    int32_t x;
    size_t leftToRead = size;
    int32_t startTimeMs;

    if (pInstance != NULL) {
        startTimeMs = uPortGetTickTimeMs();
        errorCodeOrLength = (int32_t) U_ERROR_COMMON_TIMEOUT;
        while ((leftToRead > 0) &&
               (uPortGetTickTimeMs() - startTimeMs < maxTimeMs)) {
            if (andRemove) {
                receiveSize = (int32_t) uRingBufferReadHandle(&(pInstance->ringBuffer),
                                                              readHandle,
                                                              pBuffer, leftToRead);
            } else {
                receiveSize = (int32_t) uRingBufferPeekHandle(&(pInstance->ringBuffer),
                                                              readHandle,
                                                              pBuffer, leftToRead,
                                                              offset);
                offset += receiveSize;
            }
            leftToRead -= receiveSize;
            totalSize += receiveSize;
            if (pBuffer != NULL) {
                pBuffer += receiveSize;
            }
            if (receiveSize == 0) {
                x = uGnssPrivateStreamFillRingBuffer(pInstance,
                                                     U_GNSS_RING_BUFFER_MIN_FILL_TIME_MS,
                                                     maxTimeMs / 10);
                if (x < 0) {
                    errorCodeOrLength = x;
                }
            }
        }
        if (totalSize > 0) {
            errorCodeOrLength = (int32_t) totalSize;
        }
    }

    return errorCodeOrLength;
}

// Send a message over UART or I2C or SPI.
static int32_t sendMessageStream(uGnssPrivateInstance_t *pInstance,
                                 const char *pMessage,
                                 size_t messageLengthBytes, bool printIt)
{
    int32_t errorCodeOrSentLength = (int32_t) U_ERROR_COMMON_INVALID_PARAMETER;
    int32_t privateStreamTypeOrError;
    int32_t streamHandle;

    privateStreamTypeOrError = uGnssPrivateGetStreamType(pInstance->transportType);
    streamHandle = uGnssPrivateGetStreamHandle((uGnssPrivateStreamType_t) privateStreamTypeOrError,
                                               pInstance->transportHandle);
    switch (privateStreamTypeOrError) {
        case U_GNSS_PRIVATE_STREAM_TYPE_UART: {
            errorCodeOrSentLength = uPortUartWrite(streamHandle, pMessage, messageLengthBytes);
        }
        break;
        case U_GNSS_PRIVATE_STREAM_TYPE_I2C: {
            errorCodeOrSentLength = uPortI2cControllerSend(streamHandle, pInstance->i2cAddress,
                                                           pMessage, messageLengthBytes, false);
            if (errorCodeOrSentLength == 0) {
                errorCodeOrSentLength = messageLengthBytes;
            }
        }
        break;
        case U_GNSS_PRIVATE_STREAM_TYPE_SPI: {
            char spiBuffer[U_GNSS_SPI_FILL_THRESHOLD_MAX];
            size_t offset = 0;
            size_t thisLength;
            // In the SPI case we are always necessarily receiving while
            // we send, so we have to capture that data and store it in
            // our internal SPI buffer so as not to lose anything; we
            // don't want to allocate another receive buffer here though,
            // so we send in chunks of length up to our SPI fill-checking
            // buffer ('cos it's a convenient length).
            errorCodeOrSentLength = 0;
            for (size_t x = 0; (offset < messageLengthBytes) && (errorCodeOrSentLength >= 0); x++) {
                thisLength = messageLengthBytes - offset;
                if (thisLength > U_GNSS_SPI_FILL_THRESHOLD_MAX) {
                    thisLength = U_GNSS_SPI_FILL_THRESHOLD_MAX;
                }
                errorCodeOrSentLength = uPortSpiControllerSendReceiveBlock(streamHandle,
                                                                           pMessage + offset,
                                                                           thisLength,
                                                                           spiBuffer,
                                                                           thisLength);
                if (errorCodeOrSentLength > 0) {
                    offset += errorCodeOrSentLength;
                    // This will add any non-fill SPI received data to the
                    // internal SPI ring buffer
                    uGnssPrivateSpiAddReceivedData(pInstance, spiBuffer, errorCodeOrSentLength);
                }
            }
            if (errorCodeOrSentLength >= 0) {
                errorCodeOrSentLength = offset;
            }
        }
        break;
        default:
            break;
    }

    if (printIt && (errorCodeOrSentLength == messageLengthBytes)) {
        uPortLog("U_GNSS: sent command");
        uGnssPrivatePrintBuffer(pMessage, messageLengthBytes);
        uPortLog(".\n");
    }

    return errorCodeOrSentLength;
}

// Receive a UBX format message over UART or I2C or SPI.
// On entry pResponse should be set to the message class and ID of the
// expected response, wild cards permitted.  On success it will
// be set to the message ID received and the UBX message body length
// will be returned.
static int32_t receiveUbxMessageStream(uGnssPrivateInstance_t *pInstance,
                                       uGnssPrivateUbxReceiveMessage_t *pResponse,
                                       int32_t timeoutMs, bool printIt)
{
    int32_t errorCodeOrLength = 0;            // Deliberate choice to return 0 if pResponse
    uGnssPrivateMessageId_t privateMessageId; // indicates that no response is required
    char *pBuffer = NULL;

    if ((pInstance != NULL) && (pResponse != NULL) && (pResponse->ppBody != NULL)) {
        // Convert uGnssPrivateUbxReceiveMessage_t into uGnssPrivateMessageId_t
        privateMessageId.type = U_GNSS_PROTOCOL_UBX;
        privateMessageId.id.ubx = (U_GNSS_UBX_MESSAGE_CLASS_ALL << 8) | U_GNSS_UBX_MESSAGE_ID_ALL;
        if (pResponse->cls >= 0) {
            privateMessageId.id.ubx = (privateMessageId.id.ubx & 0x00ff) | (((uint16_t) pResponse->cls) << 8);
        }
        if (pResponse->id >= 0) {
            privateMessageId.id.ubx = (privateMessageId.id.ubx & 0xff00) | pResponse->id;
        }
        // Now wait for the message, allowing a buffer to be allocated by
        // the message receive function
        errorCodeOrLength = uGnssPrivateReceiveStreamMessage(pInstance,
                                                             &privateMessageId,
                                                             pInstance->ringBufferReadHandlePrivate,
                                                             &pBuffer, 0,
                                                             timeoutMs, NULL);
        if (errorCodeOrLength >= U_UBX_PROTOCOL_OVERHEAD_LENGTH_BYTES) {
            // Convert uGnssPrivateMessageId_t into uGnssPrivateUbxReceiveMessage_t
            pResponse->cls = privateMessageId.id.ubx >> 8;
            pResponse->id = privateMessageId.id.ubx & 0xFF;
            // Remove the protocol overhead from the length, we just want the body
            errorCodeOrLength -= U_UBX_PROTOCOL_OVERHEAD_LENGTH_BYTES;
            // Copy the body of the message into the response
            if (*(pResponse->ppBody) == NULL) {
                *(pResponse->ppBody) = (char *) pUPortMalloc(errorCodeOrLength);
            } else {
                if (errorCodeOrLength > (int32_t) pResponse->bodySize) {
                    errorCodeOrLength = (int32_t) pResponse->bodySize;
                }
            }
            if (*(pResponse->ppBody) != NULL) {
                memcpy(*(pResponse->ppBody), pBuffer + U_UBX_PROTOCOL_HEADER_LENGTH_BYTES, errorCodeOrLength);
                if (printIt) {
                    uPortLog("U_GNSS: decoded UBX response 0x%02x 0x%02x",
                             privateMessageId.id.ubx >> 8, privateMessageId.id.ubx & 0xff);
                    if (errorCodeOrLength > 0) {
                        uPortLog(":");
                        uGnssPrivatePrintBuffer(*(pResponse->ppBody), errorCodeOrLength);
                    }
                    uPortLog(" [body %d byte(s)].\n", errorCodeOrLength);
                }
            } else {
                errorCodeOrLength = (int32_t) U_ERROR_COMMON_NO_MEMORY;
            }
        } else if (printIt && (errorCodeOrLength == (int32_t) U_GNSS_ERROR_NACK)) {
            uPortLog("U_GNSS: got Nack for 0x%02x 0x%02x.\n",
                     pResponse->cls, pResponse->id);
        }

        // Free memory
        uPortFree(pBuffer);
    }

    return errorCodeOrLength;
}

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: AT TRANSPORT ONLY
 * -------------------------------------------------------------- */

// Send a UBX format message over an AT interface and receive
// the response.  No matching of message ID or class for
// the response is performed as it is not possible to get other
// responses when using an AT command.
static int32_t sendReceiveUbxMessageAt(const uAtClientHandle_t atHandle,
                                       const char *pSend,
                                       size_t sendLengthBytes,
                                       uGnssPrivateUbxReceiveMessage_t *pResponse,
                                       int32_t timeoutMs,
                                       bool printIt)
{
    int32_t errorCodeOrLength = (int32_t) U_ERROR_COMMON_NO_MEMORY;
    int32_t x;
    size_t bytesToSend;
    char *pBuffer;
    bool bufferReuse = false;
    int32_t bytesRead;
    size_t captureSize;
    int32_t clsNack = 0x05;
    int32_t idNack = 0x00;
    char ackBody[2] = {0};
    bool atPrintOn = uAtClientPrintAtGet(atHandle);
    bool atDebugPrintOn = uAtClientDebugGet(atHandle);

    U_ASSERT(pResponse != NULL);

    // Need a buffer to hex encode the message into
    // and receive the response into
    x = (int32_t) (sendLengthBytes * 2) + 1; // +1 for terminator
    if (x < U_GNSS_AT_BUFFER_LENGTH_BYTES + 1) {
        x = U_GNSS_AT_BUFFER_LENGTH_BYTES + 1;
    }
    pBuffer = (char *) pUPortMalloc(x);
    if (pBuffer != NULL) {
        errorCodeOrLength = (int32_t) U_GNSS_ERROR_TRANSPORT;
        bytesToSend = uBinToHex(pSend, sendLengthBytes, pBuffer);
        if (!printIt) {
            // Switch off the AT command printing if we've been
            // told not to print stuff; particularly important
            // on platforms where the C library leaks memory
            // when called from dynamically created tasks and this
            // is being called for the GNSS asynchronous API
            uAtClientPrintAtSet(atHandle, false);
            uAtClientDebugSet(atHandle, false);
        }
        // Add terminator
        *(pBuffer + bytesToSend) = 0;
        uAtClientLock(atHandle);
        uAtClientTimeoutSet(atHandle, timeoutMs);
        uAtClientCommandStart(atHandle, "AT+UGUBX=");
        uAtClientWriteString(atHandle, pBuffer, true);
        // Read the response
        uAtClientCommandStop(atHandle);
        if (printIt) {
            uPortLog("U_GNSS: sent UBX command");
            uGnssPrivatePrintBuffer(pSend, sendLengthBytes);
            uPortLog(".\n");
        }
        uAtClientResponseStart(atHandle, "+UGUBX:");
        // Read the hex-coded response back into pBuffer
        bytesRead = uAtClientReadString(atHandle, pBuffer, x, false);
        uAtClientResponseStop(atHandle);
        if ((uAtClientUnlock(atHandle) == 0) && (bytesRead >= 0) &&
            (pResponse->ppBody != NULL)) {
            // Decode the hex into the same buffer
            x = (int32_t) uHexToBin(pBuffer, bytesRead, pBuffer);
            if (x > 0) {
                // Deal with the output buffer
                captureSize = x;
                if (*(pResponse->ppBody) != NULL) {
                    if (captureSize > pResponse->bodySize) {
                        captureSize = pResponse->bodySize;
                    }
                } else {
                    // We can just re-use the buffer we already have
                    *(pResponse->ppBody) = pBuffer;
                    bufferReuse = true;
                }
                errorCodeOrLength = (int32_t) captureSize;
                if (captureSize > 0) {
                    // First check if we received a NACK
                    if ((uUbxProtocolDecode(pBuffer, x, &clsNack, &idNack,
                                            ackBody, sizeof(ackBody),
                                            NULL) == 2) &&
                        (ackBody[0] == pResponse->cls) &&
                        (ackBody[1] == pResponse->id)) {
                        // We got a NACK for the message class
                        // and ID we are monitoring
                        errorCodeOrLength = (int32_t) U_GNSS_ERROR_NACK;
                    } else {
                        // No NACK, we can decode the message body, noting
                        // that it is safe to decode back into the same buffer
                        errorCodeOrLength = uUbxProtocolDecode(pBuffer, x,
                                                               &(pResponse->cls),
                                                               &(pResponse->id),
                                                               *(pResponse->ppBody),
                                                               captureSize, NULL);
                        if (errorCodeOrLength > (int32_t) captureSize) {
                            errorCodeOrLength = (int32_t) captureSize;
                        }
                    }
                }
                if (printIt) {
                    if (errorCodeOrLength >= 0) {
                        uPortLog("U_GNSS: decoded UBX response 0x%02x 0x%02x",
                                 pResponse->cls, pResponse->id);
                        if (errorCodeOrLength > 0) {
                            uPortLog(":");
                            uGnssPrivatePrintBuffer(*(pResponse->ppBody), errorCodeOrLength);
                        }
                        uPortLog(" [body %d byte(s)].\n", errorCodeOrLength);
                    } else if (errorCodeOrLength == (int32_t) U_GNSS_ERROR_NACK) {
                        uPortLog("U_GNSS: got Nack for 0x%02x 0x%02x.\n",
                                 pResponse->cls, pResponse->id);
                    }
                }
            }
        }

        uAtClientPrintAtSet(atHandle, atPrintOn);
        uAtClientDebugSet(atHandle, atDebugPrintOn);

        if (!bufferReuse) {
            uPortFree(pBuffer);
        }
    }

    return errorCodeOrLength;
}

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: ANY TRANSPORT
 * -------------------------------------------------------------- */

// Send a UBX format message to the GNSS module and receive
// the response.
static int32_t sendReceiveUbxMessage(uGnssPrivateInstance_t *pInstance,
                                     int32_t messageClass,
                                     int32_t messageId,
                                     const char *pMessageBody,
                                     size_t messageBodyLengthBytes,
                                     uGnssPrivateUbxReceiveMessage_t *pResponse)
{
    int32_t errorCodeOrResponseLength = (int32_t) U_ERROR_COMMON_INVALID_PARAMETER;
    int32_t privateStreamTypeOrError;
    int32_t bytesToSend = 0;
    char *pBuffer;

    if ((pInstance != NULL) &&
        (((pMessageBody == NULL) && (messageBodyLengthBytes == 0)) ||
         (messageBodyLengthBytes > 0)) &&
        ((pResponse->bodySize == 0) || (pResponse->ppBody != NULL))) {
        errorCodeOrResponseLength = (int32_t) U_ERROR_COMMON_NO_MEMORY;
        privateStreamTypeOrError = uGnssPrivateGetStreamType(pInstance->transportType);
        // Allocate a buffer big enough to encode the outgoing message
        pBuffer = (char *) pUPortMalloc(messageBodyLengthBytes + U_UBX_PROTOCOL_OVERHEAD_LENGTH_BYTES);
        if (pBuffer != NULL) {
            errorCodeOrResponseLength = (int32_t) U_GNSS_ERROR_TRANSPORT;
            bytesToSend = uUbxProtocolEncode(messageClass, messageId,
                                             pMessageBody, messageBodyLengthBytes,
                                             pBuffer);
            if (bytesToSend > 0) {

                U_PORT_MUTEX_LOCK(pInstance->transportMutex);

                if ((pResponse != NULL) && (pResponse->ppBody != NULL) &&
                    (privateStreamTypeOrError >= 0)) {
                    // For a streaming transport, if we're going to wait for
                    // a response, make sure that any historical data is
                    // cleared from our handle in the ring buffer so that
                    // we don't pick it up instead and lock our read
                    // pointer before we do the send so that we are sure
                    // we won't lose the response
                    uGnssPrivateStreamFillRingBuffer(pInstance,
                                                     U_GNSS_RING_BUFFER_MIN_FILL_TIME_MS,
                                                     U_GNSS_RING_BUFFER_MAX_FILL_TIME_MS);
                    uRingBufferLockReadHandle(&(pInstance->ringBuffer),
                                              pInstance->ringBufferReadHandlePrivate);
                    uRingBufferFlushHandle(&(pInstance->ringBuffer),
                                           pInstance->ringBufferReadHandlePrivate);
                }
                if (privateStreamTypeOrError >= 0) {
                    errorCodeOrResponseLength = sendMessageStream(pInstance, pBuffer, bytesToSend,
                                                                  pInstance->printUbxMessages);
                    if (errorCodeOrResponseLength >= 0) {
                        errorCodeOrResponseLength = receiveUbxMessageStream(pInstance, pResponse,
                                                                            pInstance->timeoutMs,
                                                                            pInstance->printUbxMessages);
                    }
                } else {
                    // Not a stream, we're on AT
                    //lint -e{1773} Suppress attempt to cast away const: I'm not!
                    errorCodeOrResponseLength = sendReceiveUbxMessageAt((const uAtClientHandle_t)
                                                                        pInstance->transportHandle.pAt,
                                                                        pBuffer, bytesToSend,
                                                                        pResponse, pInstance->timeoutMs,
                                                                        pInstance->printUbxMessages);
                }

                // Make sure the read handle is always unlocked afterwards
                uRingBufferUnlockReadHandle(&(pInstance->ringBuffer), pInstance->ringBufferReadHandlePrivate);

                U_PORT_MUTEX_UNLOCK(pInstance->transportMutex);
            }

            // Free memory
            uPortFree(pBuffer);
        }
    }

    return errorCodeOrResponseLength;
}

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: MESSAGE PARSERS
 * -------------------------------------------------------------- */

/** UBX Parser function.
 *
 * @param parseHandle    the parse handle of the ring buffer to read from.
 * @param[in] pUserParam the user parameter passed to uRingBufferParseHandle().
 * @return               negative error or success code.
 */
static int32_t parseUbx(uParseHandle_t parseHandle, void *pUserParam)
{
    uGnssPrivateMessageId_t *pMsgId = (uGnssPrivateMessageId_t *) pUserParam;
    uint8_t by = 0;
    if (!uRingBufferGetByteUnprotected(parseHandle, &by)) {
        return U_ERROR_COMMON_TIMEOUT;
    }
    if (0xB5 != by) {
        return U_ERROR_COMMON_NOT_FOUND;    // = µ, 0xB5
    }
    if (!uRingBufferGetByteUnprotected(parseHandle, &by)) {
        return U_ERROR_COMMON_TIMEOUT;
    }
    if (0x62 != by) {
        return U_ERROR_COMMON_NOT_FOUND;    // = b
    }
    if (4 > uRingBufferBytesAvailableUnprotected(parseHandle)) {
        return U_ERROR_COMMON_TIMEOUT;
    }
    uint8_t ckb = 0;
    uint8_t cka = 0;
    uint8_t cls, id;
    uRingBufferGetByteUnprotected(parseHandle, &cls); // cls
    cka += cls;
    ckb += cka;
    uRingBufferGetByteUnprotected(parseHandle, &id); // id
    cka += id;
    ckb += cka;
    pMsgId->id.ubx = (cls << 8) + id;
    uRingBufferGetByteUnprotected(parseHandle, &by); // len low
    cka += by;
    ckb += cka;
    uint16_t l = by;
    uRingBufferGetByteUnprotected(parseHandle, &by); // len high
    cka += by;
    ckb += cka;
    l += (((uint16_t) by) << 8);
    if (l > uRingBufferBytesAvailableUnprotected(parseHandle)) {
        return U_ERROR_COMMON_TIMEOUT;
    }
    while (l --) {
        uRingBufferGetByteUnprotected(parseHandle, &by);
        cka += by;
        ckb += cka;
    }
    cka = cka & 0xFF;
    ckb = ckb & 0xFF;
    if (!uRingBufferGetByteUnprotected(parseHandle, &by)) {
        return U_ERROR_COMMON_TIMEOUT;
    }
    if (by != cka) {
        return U_ERROR_COMMON_NOT_FOUND;
    }
    if (!uRingBufferGetByteUnprotected(parseHandle, &by)) {
        return U_ERROR_COMMON_TIMEOUT;
    }
    if (by != ckb) {
        return U_ERROR_COMMON_NOT_FOUND;
    }
    // We can only claim this as a UBX-format message if
    // there was nothing that needed discarding first.
    if (uRingBufferBytesDiscardUnprotected(parseHandle) == 0) {
        pMsgId->type = U_GNSS_PROTOCOL_UBX;
    }
    return U_ERROR_COMMON_SUCCESS;
}

/** NMEA Parser function.
 *
 * @param parseHandle    the parse handle of the ring buffer to read from.
 * @param[in] pUserParam the user parameter passed to uRingBufferParseHandle().
 * @return               negative error or success code.
 */
static int32_t parseNmea(uParseHandle_t parseHandle, void *pUserParam)
{
    uGnssPrivateMessageId_t *pMsgId = (uGnssPrivateMessageId_t *) pUserParam;
    char ch = 0;
    const char *hex = "0123456789ABCDEF";
    if (!uRingBufferGetByteUnprotected(parseHandle, &ch)) {
        return U_ERROR_COMMON_TIMEOUT;
    }
    if ('$' != ch) {
        return U_ERROR_COMMON_NOT_FOUND;
    }
    char crc = 0;
    int i = 0;
    while (uRingBufferGetByteUnprotected(parseHandle, &ch)) {
        crc ^= ch;
        if (',' == ch) {
            break;
        }
        if (i >= U_GNSS_NMEA_MESSAGE_MATCH_LENGTH_CHARACTERS) {
            return U_ERROR_COMMON_NOT_FOUND;
        }
        if (('0' > ch) || ('Z' < ch) || (('9' < ch) && ('A' > ch))) {
            return U_ERROR_COMMON_NOT_FOUND;    // A-Z, 0-9
        }
        pMsgId->id.nmea[i++] = ch;
    }
    pMsgId->id.nmea[i] = '\0';
    while (uRingBufferGetByteUnprotected(parseHandle, &ch)) {
        if ((' ' > ch) || ('~' < ch)) {
            return U_ERROR_COMMON_NOT_FOUND;    // not in printable range 32 - 126
        }
        if ('*' == ch) {
            break;    // *
        }
        crc ^= ch;
    }
    if (!uRingBufferGetByteUnprotected(parseHandle, &ch)) {
        return U_ERROR_COMMON_TIMEOUT;
    }
    if (hex[(crc >> 4) & 0xF] != ch) {
        return U_ERROR_COMMON_NOT_FOUND;
    }
    if (!uRingBufferGetByteUnprotected(parseHandle, &ch)) {
        return U_ERROR_COMMON_TIMEOUT;
    }
    if (hex[crc & 0xF] != ch) {
        return U_ERROR_COMMON_NOT_FOUND;
    }
    if (!uRingBufferGetByteUnprotected(parseHandle, &ch)) {
        return U_ERROR_COMMON_TIMEOUT;
    }
    if ('\r' != ch) {
        return U_ERROR_COMMON_NOT_FOUND;
    }
    if (!uRingBufferGetByteUnprotected(parseHandle, &ch)) {
        return U_ERROR_COMMON_TIMEOUT;
    }
    if ('\n' != ch) {
        return U_ERROR_COMMON_NOT_FOUND;
    }
    // We can only claim this as an NMEA-format message if
    // there was nothing that needed discarding first.
    if (uRingBufferBytesDiscardUnprotected(parseHandle) == 0) {
        pMsgId->type = U_GNSS_PROTOCOL_NMEA;
    }
    return U_ERROR_COMMON_SUCCESS;
}

/** RTCM Parser function.
 *
 * @param parseHandle    the parse handle of the ring buffer to read from.
 * @param[in] pUserParam the user parameter passed to uRingBufferParseHandle().
 * @return               negative error or success code.
 */
static int32_t parseRtcm(uParseHandle_t parseHandle, void *pUserParam)
{
    uGnssPrivateMessageId_t *pMsgId = (uGnssPrivateMessageId_t *) pUserParam;
    uint8_t by;
    uint32_t crc = 0;
    if (!uRingBufferGetByteUnprotected(parseHandle, &by)) {
        return U_ERROR_COMMON_TIMEOUT;
    }
    if (0xD3 != by) {
        return U_ERROR_COMMON_NOT_FOUND;
    }
    // CRC24Q check
    const uint32_t _crc24qTable[] = {
        /* 00 */ 0x000000, 0x864cfb, 0x8ad50d, 0x0c99f6, 0x93e6e1, 0x15aa1a, 0x1933ec, 0x9f7f17,
        /* 08 */ 0xa18139, 0x27cdc2, 0x2b5434, 0xad18cf, 0x3267d8, 0xb42b23, 0xb8b2d5, 0x3efe2e,
        /* 10 */ 0xc54e89, 0x430272, 0x4f9b84, 0xc9d77f, 0x56a868, 0xd0e493, 0xdc7d65, 0x5a319e,
        /* 18 */ 0x64cfb0, 0xe2834b, 0xee1abd, 0x685646, 0xf72951, 0x7165aa, 0x7dfc5c, 0xfbb0a7,
        /* 20 */ 0x0cd1e9, 0x8a9d12, 0x8604e4, 0x00481f, 0x9f3708, 0x197bf3, 0x15e205, 0x93aefe,
        /* 28 */ 0xad50d0, 0x2b1c2b, 0x2785dd, 0xa1c926, 0x3eb631, 0xb8faca, 0xb4633c, 0x322fc7,
        /* 30 */ 0xc99f60, 0x4fd39b, 0x434a6d, 0xc50696, 0x5a7981, 0xdc357a, 0xd0ac8c, 0x56e077,
        /* 38 */ 0x681e59, 0xee52a2, 0xe2cb54, 0x6487af, 0xfbf8b8, 0x7db443, 0x712db5, 0xf7614e,
        /* 40 */ 0x19a3d2, 0x9fef29, 0x9376df, 0x153a24, 0x8a4533, 0x0c09c8, 0x00903e, 0x86dcc5,
        /* 48 */ 0xb822eb, 0x3e6e10, 0x32f7e6, 0xb4bb1d, 0x2bc40a, 0xad88f1, 0xa11107, 0x275dfc,
        /* 50 */ 0xdced5b, 0x5aa1a0, 0x563856, 0xd074ad, 0x4f0bba, 0xc94741, 0xc5deb7, 0x43924c,
        /* 58 */ 0x7d6c62, 0xfb2099, 0xf7b96f, 0x71f594, 0xee8a83, 0x68c678, 0x645f8e, 0xe21375,
        /* 60 */ 0x15723b, 0x933ec0, 0x9fa736, 0x19ebcd, 0x8694da, 0x00d821, 0x0c41d7, 0x8a0d2c,
        /* 68 */ 0xb4f302, 0x32bff9, 0x3e260f, 0xb86af4, 0x2715e3, 0xa15918, 0xadc0ee, 0x2b8c15,
        /* 70 */ 0xd03cb2, 0x567049, 0x5ae9bf, 0xdca544, 0x43da53, 0xc596a8, 0xc90f5e, 0x4f43a5,
        /* 78 */ 0x71bd8b, 0xf7f170, 0xfb6886, 0x7d247d, 0xe25b6a, 0x641791, 0x688e67, 0xeec29c,
        /* 80 */ 0x3347a4, 0xb50b5f, 0xb992a9, 0x3fde52, 0xa0a145, 0x26edbe, 0x2a7448, 0xac38b3,
        /* 88 */ 0x92c69d, 0x148a66, 0x181390, 0x9e5f6b, 0x01207c, 0x876c87, 0x8bf571, 0x0db98a,
        /* 90 */ 0xf6092d, 0x7045d6, 0x7cdc20, 0xfa90db, 0x65efcc, 0xe3a337, 0xef3ac1, 0x69763a,
        /* 98 */ 0x578814, 0xd1c4ef, 0xdd5d19, 0x5b11e2, 0xc46ef5, 0x42220e, 0x4ebbf8, 0xc8f703,
        /* a0 */ 0x3f964d, 0xb9dab6, 0xb54340, 0x330fbb, 0xac70ac, 0x2a3c57, 0x26a5a1, 0xa0e95a,
        /* a8 */ 0x9e1774, 0x185b8f, 0x14c279, 0x928e82, 0x0df195, 0x8bbd6e, 0x872498, 0x016863,
        /* b0 */ 0xfad8c4, 0x7c943f, 0x700dc9, 0xf64132, 0x693e25, 0xef72de, 0xe3eb28, 0x65a7d3,
        /* b8 */ 0x5b59fd, 0xdd1506, 0xd18cf0, 0x57c00b, 0xc8bf1c, 0x4ef3e7, 0x426a11, 0xc426ea,
        /* c0 */ 0x2ae476, 0xaca88d, 0xa0317b, 0x267d80, 0xb90297, 0x3f4e6c, 0x33d79a, 0xb59b61,
        /* c8 */ 0x8b654f, 0x0d29b4, 0x01b042, 0x87fcb9, 0x1883ae, 0x9ecf55, 0x9256a3, 0x141a58,
        /* d0 */ 0xefaaff, 0x69e604, 0x657ff2, 0xe33309, 0x7c4c1e, 0xfa00e5, 0xf69913, 0x70d5e8,
        /* d8 */ 0x4e2bc6, 0xc8673d, 0xc4fecb, 0x42b230, 0xddcd27, 0x5b81dc, 0x57182a, 0xd154d1,
        /* e0 */ 0x26359f, 0xa07964, 0xace092, 0x2aac69, 0xb5d37e, 0x339f85, 0x3f0673, 0xb94a88,
        /* e8 */ 0x87b4a6, 0x01f85d, 0x0d61ab, 0x8b2d50, 0x145247, 0x921ebc, 0x9e874a, 0x18cbb1,
        /* f0 */ 0xe37b16, 0x6537ed, 0x69ae1b, 0xefe2e0, 0x709df7, 0xf6d10c, 0xfa48fa, 0x7c0401,
        /* f8 */ 0x42fa2f, 0xc4b6d4, 0xc82f22, 0x4e63d9, 0xd11cce, 0x575035, 0x5bc9c3, 0xdd8538
    };
#define RTCM_CRC(crc, by) (crc << 8) ^ _crc24qTable[(by ^ (crc >> 16)) & 0xff]
    // CRC is over the entire message, 0xD3 included
    crc = RTCM_CRC(crc, by);
    if (!uRingBufferGetByteUnprotected(parseHandle, &by)) {
        return U_ERROR_COMMON_TIMEOUT;
    }
    if ((0xFC & by) != 0) {
        return U_ERROR_COMMON_NOT_FOUND;
    }
    uint16_t l = (by & 0x3) << 8;
    crc = RTCM_CRC(crc, by);
    if (!uRingBufferGetByteUnprotected(parseHandle, &by)) {
        return U_ERROR_COMMON_TIMEOUT;
    }
    l += by;
    // Length includes the two-byte message ID and the message
    // body, i.e. up to the start of the 3-byte CRC, i.e.
    // the total message length - 6.
    if (l > uRingBufferBytesAvailableUnprotected(parseHandle) + 3) {
        return U_ERROR_COMMON_TIMEOUT;
    }
    crc = RTCM_CRC(crc, by);
    uint8_t idLo, idHi;
    if (!uRingBufferGetByteUnprotected(parseHandle, &idLo)) {
        return U_ERROR_COMMON_TIMEOUT;
    }
    l--;
    crc = RTCM_CRC(crc, idLo);
    if (!uRingBufferGetByteUnprotected(parseHandle, &idHi)) {
        return U_ERROR_COMMON_TIMEOUT;
    }
    l--;
    crc = RTCM_CRC(crc, idHi);
    pMsgId->id.rtcm = (idHi >> 4) + (idLo << 4);
    while (l--) {
        uRingBufferGetByteUnprotected(parseHandle, &by);
        crc = RTCM_CRC(crc, by);
    }
    // Compare CRC
    for (int32_t x = 2; (x >= 0) && uRingBufferGetByteUnprotected(parseHandle, &by); x--) {
        if (by != (uint8_t) (crc >> (8 * x))) {
            return U_ERROR_COMMON_TIMEOUT;
        }
    }
    // We can only claim this as an RTCM-format message if
    // there was nothing that needed discarding first.
    if (uRingBufferBytesDiscardUnprotected(parseHandle) == 0) {
        pMsgId->type = U_GNSS_PROTOCOL_RTCM;
    }
    return U_ERROR_COMMON_SUCCESS;
}

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: PROTOCOL OUTPUT CONFIGURATION
 * -------------------------------------------------------------- */

// Set protocol out old-style, with UBX-CFG-PRT.
static int32_t setProtocolOutUbxCfgPrt(uGnssPrivateInstance_t *pInstance,
                                       uGnssProtocol_t protocol,
                                       bool onNotOff)
{
    int32_t errorCode = (int32_t) U_ERROR_COMMON_PLATFORM;
    // Message buffer for the 120-byte UBX-MON-MSGPP message
    char message[120] = {0};
    uint16_t mask = 0;
    uint64_t x;

    // Normally we would send the UBX-CFG-PRT message
    // by calling uGnssPrivateSendUbxMessage() which
    // would wait for an ack.  However, in this particular
    // case, the other parameters in the message are
    // serial port settings and, even though we are not
    // changing them, the returned UBX-ACK-ACK message
    // is often corrupted as a result.
    // The workaround is to avoid waiting for the ack by
    // using uGnssPrivateSendReceiveUbxMessage() with
    // an empty response buffer but, before we do that,
    // we send UBX-MON-MSGPP to determine the number of
    // messages received by the GNSS chip on the UART port
    // and then we check it again afterwards to be sure that
    // our UBX-CFG-PRT messages really were received.
    if (uGnssPrivateSendReceiveUbxMessage(pInstance,
                                          0x0a, 0x06,
                                          NULL, 0,
                                          message,
                                          sizeof(message)) == sizeof(message)) {
        // Get the number of messages received on the port
        x = uUbxProtocolUint64Decode(message + ((size_t) (unsigned) pInstance->portNumber * 16));
        // Now poll the GNSS chip for UBX-CFG-PRT to get the
        // existing configuration for the port we are connected on
        message[0] = (char) pInstance->portNumber;
        if (uGnssPrivateSendReceiveUbxMessage(pInstance,
                                              0x06, 0x00,
                                              message, 1,
                                              message, 20) == 20) {
            // Offsets 14 and 15 contain the output protocol bit-map
            mask = uUbxProtocolUint16Decode((const char *) &(message[14])); // *NOPAD*
            if (protocol == U_GNSS_PROTOCOL_ALL) {
                mask = 0xFFFF; // Everything out
            } else {
                if (protocol == U_GNSS_PROTOCOL_RTCM) {
                    // RTCM is the odd one out
                    protocol = 5;
                }
                if (onNotOff) {
                    mask |= 1 << protocol;
                } else {
                    mask &= ~(1 << protocol);
                }
            }
            *((uint16_t *) &(message[14])) = uUbxProtocolUint16Encode(mask); // *NOPAD*
            // Send the message and don't wait for response or ack
            errorCode = uGnssPrivateSendReceiveUbxMessage(pInstance,
                                                          0x06, 0x00,
                                                          message, 20,
                                                          NULL, 0);
            // Skip any serial port perturbance at the far end
            uPortTaskBlock(100);
            // Get the number of received messages again
            if (uGnssPrivateSendReceiveUbxMessage(pInstance,
                                                  0x0a, 0x06,
                                                  NULL, 0,
                                                  message,
                                                  sizeof(message)) == sizeof(message)) {
                x = uUbxProtocolUint64Decode(message + ((size_t) (unsigned) pInstance->portNumber * 16)) - x;
                // Should be three: UBX-MON-MSGPP, the poll for UBX-CFG-PRT
                // and then the UBX-CFG-PRT setting command itself.
                if (x == 3) {
                    errorCode = (int32_t) U_ERROR_COMMON_SUCCESS;
                }
            }
        }
    }

    return errorCode;
}

// Get protocol out old-style, with UBX-CFG-PRT.
static int32_t getProtocolOutUbxCfgPrt(uGnssPrivateInstance_t *pInstance)
{
    int32_t errorCodeOrBitMap = (int32_t) U_ERROR_COMMON_PLATFORM;
    // Message buffer for the 20-byte UBX-CFG-PRT message
    char message[20] = {0};

    // Poll the GNSS chip with UBX-CFG-PRT
    message[0] = (char) pInstance->portNumber;
    if (uGnssPrivateSendReceiveUbxMessage(pInstance,
                                          0x06, 0x00,
                                          message, 1,
                                          message,
                                          sizeof(message)) == sizeof(message)) {
        // Offsets 14 and 15 contain the output protocol bit-map
        errorCodeOrBitMap = (int32_t) uUbxProtocolUint16Decode((const char *) &(message[14])); // *NOPAD*
        if (errorCodeOrBitMap >= 0) {
            // Handle RTCM, the odd one out
            if (errorCodeOrBitMap & (1 << 5)) {
                errorCodeOrBitMap &= ~(1 << 5);
                errorCodeOrBitMap |= 1 << U_GNSS_PROTOCOL_RTCM;
            }
        } else {
            // Don't expect to have the top-bit set so flag an error
            errorCodeOrBitMap = U_ERROR_COMMON_PLATFORM;
        }
    }

    return errorCodeOrBitMap;
}

// Pack a logical on/off value into five bytes of UBX-CFG-VALSET entry,
// used by setProtocolOutUbxCfgVal(). pMessage must point to at least
// five bytes of buffer.
static size_t packUbxCfgValLogicalEntry(char *pMessage,
                                        uGnssCfgValKeyGroupId_t groupId,
                                        uint16_t itemId, bool onNotOff)
{
    char *pTmp = pMessage;
    uint32_t keyId;

    keyId = (0x10UL << 24) | ((groupId & 0xFF) << 16) | itemId;
    *((uint32_t *) pTmp) = uUbxProtocolUint32Encode(keyId);
    pTmp += 4;
    *pTmp++ = onNotOff;

    return pTmp - pMessage;
}

// Set protocol out with UBX-CFG-VALSET.
static int32_t setProtocolOutUbxCfgVal(uGnssPrivateInstance_t *pInstance,
                                       uGnssProtocol_t protocol,
                                       bool onNotOff)
{
    int32_t errorCode = (int32_t) U_ERROR_COMMON_PLATFORM;
    // Message buffer for the UBX-CFG-VALSET message body, four bytes
    // of header and then, for each protocol type, four bytes of key
    // ID and one byte of Boolean value
    char message[4 + ((4 + 1) * U_GNSS_PROTOCOL_UNKNOWN)];
    char *pMessage = message;;
    size_t messageSizeBytes;

    if (pInstance->portNumber < sizeof(gPortToCfgValGroupIdOutProt) /
        sizeof(gPortToCfgValGroupIdOutProt[0])) {
        // Assemble the 4-byte UBX-CFG-VALSET message header
        *pMessage++ = 0; // version
        *pMessage++ = U_GNSS_CFG_VAL_LAYER_RAM;
        *pMessage++ = 0; // reserved
        *pMessage++ = 0;
        // Add the key/value pairs
        if (protocol == U_GNSS_PROTOCOL_ALL) {
            for (size_t x = 0; x < sizeof(gProtocolTypeToCfgValItemIdOutProt) /
                 sizeof(gProtocolTypeToCfgValItemIdOutProt[0]); x++) {
                pMessage += packUbxCfgValLogicalEntry(pMessage,
                                                      gPortToCfgValGroupIdOutProt[pInstance->portNumber],
                                                      gProtocolTypeToCfgValItemIdOutProt[x],
                                                      onNotOff);
            }
        } else {
            errorCode = (int32_t) U_ERROR_COMMON_INVALID_PARAMETER;
            if (protocol < sizeof(gProtocolTypeToCfgValItemIdOutProt) /
                sizeof(gProtocolTypeToCfgValItemIdOutProt[0])) {
                pMessage += packUbxCfgValLogicalEntry(pMessage,
                                                      gPortToCfgValGroupIdOutProt[pInstance->portNumber],
                                                      gProtocolTypeToCfgValItemIdOutProt[protocol],
                                                      onNotOff);
            }
        }

        messageSizeBytes = pMessage - message;
        if (messageSizeBytes > 4) {
            // Have something worth sending, send UBX-CFG-VALSET
            errorCode = uGnssPrivateSendUbxMessage(pInstance,
                                                   0x06, 0x8a,
                                                   message, messageSizeBytes);
        }
    }

    return errorCode;
}

// Get protocol out with UBX-CFG-VALGET.
static int32_t getProtocolOutUbxCfgVal(uGnssPrivateInstance_t *pInstance)
{
    int32_t errorCodeOrBitMap = (int32_t) U_ERROR_COMMON_PLATFORM;
    // Message buffer for the UBX-CFG-VALGET message body:
    // four bytes of header and four bytes for the key ID of
    // our port number (with a wildcard item Id)
    char messageOut[4 + 4] = {0};
    char *pMessage = messageOut;
    size_t messageSizeBytes;
    char *pMessageIn = NULL;
    uint32_t keyId = 0;
    uint32_t y;

    // The 4-byte message header is all zeroes: version 0,
    // 0 for the RAM layer, position 0
    pMessage += 4;
    // Add the key for the current protocol type with a wild-card item ID
    if (pInstance->portNumber < sizeof(gPortToCfgValGroupIdOutProt) /
        sizeof(gPortToCfgValGroupIdOutProt[0])) {
        keyId = (0x10UL << 24) |
                ((gPortToCfgValGroupIdOutProt[pInstance->portNumber] & 0xFF) << 16) |
                U_GNSS_CFG_VAL_KEY_ITEM_ID_ALL;
        *((uint32_t *) pMessage) = uUbxProtocolUint32Encode(keyId);
        pMessage += sizeof(uint32_t);
    }

    messageSizeBytes = pMessage - messageOut;
    if (messageSizeBytes > 4) {
        // Send it off and wait for the response
        errorCodeOrBitMap = uGnssPrivateSendReceiveUbxMessageAlloc(pInstance,
                                                                   0x06, 0x8b,
                                                                   messageOut,
                                                                   messageSizeBytes,
                                                                   &pMessageIn);
        // 4 below since there must be at least four bytes of header
        if ((errorCodeOrBitMap > 4) && (pMessageIn != NULL)) {
            messageSizeBytes = (size_t) errorCodeOrBitMap;
            pMessage = pMessageIn + 4;
            errorCodeOrBitMap = 0;
            // After a four byte header, which we can ignore, the
            // received message should contain keys that begin
            // with the group part of our key ID, followed by the item ID
            // for each output protocol type, followed by a single
            // byte giving the logical value for that protocol type
            while (messageSizeBytes - (pMessage - pMessageIn) >= 4 + 1) {
                y = uUbxProtocolUint32Decode(pMessage);
                pMessage += 4;
                if (((y & 0xFFFF0000) == (keyId & 0xFFFF0000)) &&
                    ((y & 0xFF) < sizeof(gCfgValItemIdOutProtToProtocolType) /
                     sizeof(gCfgValItemIdOutProtToProtocolType[0])) &&
                    (gCfgValItemIdOutProtToProtocolType[y & 0xFF] >= 0)) {
                    if (*pMessage) {
                        errorCodeOrBitMap |= 1 << gCfgValItemIdOutProtToProtocolType[y & 0xFF];
                    }
                }
                // Do a proper increment of the pointer, based on the key ID
                // in the message, just in case it contains things we didn't
                // expect
                switch (y >> 28) {
                    case U_GNSS_CFG_VAL_KEY_SIZE_ONE_BIT:
                    //lint -fallthrough
                    case U_GNSS_CFG_VAL_KEY_SIZE_ONE_BYTE:
                        pMessage += 1;
                        break;
                    case U_GNSS_CFG_VAL_KEY_SIZE_TWO_BYTES:
                        pMessage += 2;
                        break;
                    case U_GNSS_CFG_VAL_KEY_SIZE_FOUR_BYTES:
                        pMessage += 4;
                        break;
                    case U_GNSS_CFG_VAL_KEY_SIZE_EIGHT_BYTES:
                        pMessage += 8;
                        break;
                    default:
                        break;
                }
            }
        } else {
            errorCodeOrBitMap = (int32_t) U_ERROR_COMMON_PLATFORM;
        }

        // Free memory from uGnssPrivateSendReceiveUbxMessageAlloc()
        uPortFree(pMessageIn);
    }

    return errorCodeOrBitMap;
}

/* ----------------------------------------------------------------
 * PUBLIC FUNCTIONS THAT ARE PRIVATE TO GNSS: MISC
 * -------------------------------------------------------------- */

// Find a GNSS instance in the list by instance handle.
uGnssPrivateInstance_t *pUGnssPrivateGetInstance(uDeviceHandle_t handle)
{
    uGnssPrivateInstance_t *pInstance = gpUGnssPrivateInstanceList;
    uDeviceHandle_t gnssHandle = uNetworkGetDeviceHandle(handle,
                                                         U_NETWORK_TYPE_GNSS);

    if (gnssHandle == NULL) {
        // If the network function returned nothing then the handle
        // we were given wasn't obtained through the network API,
        // just use what we were given
        gnssHandle = handle;
    }
    while ((pInstance != NULL) && (pInstance->gnssHandle != gnssHandle)) {
        pInstance = pInstance->pNext;
    }

    return pInstance;
}

// Get the module characteristics for a given instance.
const uGnssPrivateModule_t *pUGnssPrivateGetModule(uDeviceHandle_t gnssHandle)
{
    uGnssPrivateInstance_t *pInstance = gpUGnssPrivateInstanceList;
    const uGnssPrivateModule_t *pModule = NULL;

    while ((pInstance != NULL) && (pInstance->gnssHandle != gnssHandle)) {
        pInstance = pInstance->pNext;
    }

    if (pInstance != NULL) {
        pModule = pInstance->pModule;
    }

    return pModule;
}

// Print a UBX message in hex.
//lint -esym(522, uGnssPrivatePrintBuffer) Suppress "lacks side effects"
// when compiled out
void uGnssPrivatePrintBuffer(const char *pBuffer,
                             size_t bufferLengthBytes)
{
#if U_CFG_ENABLE_LOGGING
    for (size_t x = 0; x < bufferLengthBytes; x++) {
        uPortLog(" %02x", *pBuffer);
        pBuffer++;
    }
#else
    (void) pBuffer;
    (void) bufferLengthBytes;
#endif
}

// Set the protocol type output by the GNSS chip.
int32_t uGnssPrivateSetProtocolOut(uGnssPrivateInstance_t *pInstance,
                                   uGnssProtocol_t protocol,
                                   bool onNotOff)
{
    int32_t errorCode = (int32_t) U_ERROR_COMMON_INVALID_PARAMETER;

    if ((pInstance != NULL) &&
        (pInstance->transportType != U_GNSS_TRANSPORT_AT) &&
        (onNotOff || ((protocol != U_GNSS_PROTOCOL_ALL) &&
                      (protocol != U_GNSS_PROTOCOL_UBX)))) {
        if (U_GNSS_PRIVATE_HAS(pInstance->pModule, U_GNSS_PRIVATE_FEATURE_CFGVALXXX)) {
            errorCode = setProtocolOutUbxCfgVal(pInstance, protocol, onNotOff);
        } else {
            errorCode = setProtocolOutUbxCfgPrt(pInstance, protocol, onNotOff);
        }
    }

    return errorCode;
}

// Get the protocol types output by the GNSS chip.
int32_t uGnssPrivateGetProtocolOut(uGnssPrivateInstance_t *pInstance)
{
    int32_t errorCodeOrBitMap = (int32_t) U_ERROR_COMMON_NOT_INITIALISED;

    if (pInstance != NULL) {
        errorCodeOrBitMap = (int32_t) U_ERROR_COMMON_NOT_SUPPORTED;
        if (pInstance->transportType != U_GNSS_TRANSPORT_AT) {
            if (U_GNSS_PRIVATE_HAS(pInstance->pModule, U_GNSS_PRIVATE_FEATURE_CFGVALXXX)) {
                errorCodeOrBitMap = getProtocolOutUbxCfgVal(pInstance);
            } else {
                errorCodeOrBitMap = getProtocolOutUbxCfgPrt(pInstance);
            }
        }
    }

    return errorCodeOrBitMap;
}

// Shut down and free memory from a running pos task.
void uGnssPrivateCleanUpPosTask(uGnssPrivateInstance_t *pInstance)
{
    if (pInstance->posTaskFlags & U_GNSS_POS_TASK_FLAG_HAS_RUN) {
        // Make the pos task exit if it is running
        pInstance->posTaskFlags &= ~(U_GNSS_POS_TASK_FLAG_KEEP_GOING);
        // Wait for the task to exit
        U_PORT_MUTEX_LOCK(pInstance->posMutex);
        U_PORT_MUTEX_UNLOCK(pInstance->posMutex);
        // Free the mutex
        uPortMutexDelete(pInstance->posMutex);
        pInstance->posMutex = NULL;
        // Only now clear all of the flags so that it is safe
        // to start again
        pInstance->posTaskFlags = 0;
    }
}

// Check whether the GNSS chip is on-board the cellular module.
bool uGnssPrivateIsInsideCell(const uGnssPrivateInstance_t *pInstance)
{
    bool isInside = false;
    uAtClientHandle_t atHandle;
    int32_t bytesRead;
    char buffer[64]; // Enough for the ATI response

    if (pInstance != NULL) {
        atHandle = pInstance->transportHandle.pAt;
        if (pInstance->transportType == U_GNSS_TRANSPORT_AT) {
            // Simplest way to check is to send ATI and see if
            // it includes an "M8"
            uAtClientLock(atHandle);
            uAtClientCommandStart(atHandle, "ATI");
            uAtClientCommandStop(atHandle);
            uAtClientResponseStart(atHandle, NULL);
            bytesRead = uAtClientReadBytes(atHandle, buffer,
                                           sizeof(buffer) - 1, false);
            uAtClientResponseStop(atHandle);
            if ((uAtClientUnlock(atHandle) == 0) && (bytesRead > 0)) {
                // Add a terminator
                buffer[bytesRead] = 0;
                if (strstr("M8", buffer) != NULL) {
                    isInside = true;
                }
            }
        }
    }

    return isInside;
}

// Stop the asynchronous message receive task.
void uGnssPrivateStopMsgReceive(uGnssPrivateInstance_t *pInstance)
{
    char queueItem[U_GNSS_MSG_RECEIVE_TASK_QUEUE_ITEM_SIZE_BYTES];
    uGnssPrivateMsgReceive_t *pMsgReceive;
    uGnssPrivateMsgReader_t *pNext;

    if ((pInstance != NULL) && (pInstance->pMsgReceive != NULL)) {
        pMsgReceive = pInstance->pMsgReceive;

        // Sending the task anything will cause it to exit
        uPortQueueSend(pMsgReceive->taskExitQueueHandle, queueItem);
        U_PORT_MUTEX_LOCK(pMsgReceive->taskRunningMutexHandle);
        U_PORT_MUTEX_UNLOCK(pMsgReceive->taskRunningMutexHandle);
        // Wait for the task to actually exit: the STM32F4 platform
        // needs this additional delay for some reason or it stalls here
        uPortTaskBlock(U_CFG_OS_YIELD_MS);

        // Free all the readers; no need to lock the reader mutex since
        // we've shut the task down
        while (pMsgReceive->pReaderList != NULL) {
            pNext = pMsgReceive->pReaderList->pNext;
            uPortFree(pMsgReceive->pReaderList);
            pMsgReceive->pReaderList = pNext;
        }

        // Free all OS resources
        uPortTaskDelete(pMsgReceive->taskHandle);
        uPortMutexDelete(pMsgReceive->taskRunningMutexHandle);
        uPortQueueDelete(pMsgReceive->taskExitQueueHandle);
        uPortMutexDelete(pMsgReceive->readerMutexHandle);

        // Pause here to allow the deletions
        // to actually occur in the idle thread,
        // required by some RTOSs (e.g. FreeRTOS)
        uPortTaskBlock(U_CFG_OS_YIELD_MS);

        // Free the temporary buffer
        uPortFree(pMsgReceive->pTemporaryBuffer);

        // Give the ring buffer handle back
        uRingBufferGiveReadHandle(&(pInstance->ringBuffer),
                                  pMsgReceive->ringBufferReadHandle);

        // Add it's done
        uPortFree(pInstance->pMsgReceive);
        pInstance->pMsgReceive = NULL;
    }
}

/* ----------------------------------------------------------------
 * PUBLIC FUNCTIONS THAT ARE PRIVATE TO GNSS: MESSAGE RELATED
 * -------------------------------------------------------------- */

// Convert a public message ID to a private message ID.
int32_t uGnssPrivateMessageIdToPrivate(const uGnssMessageId_t *pMessageId,
                                       uGnssPrivateMessageId_t *pPrivateMessageId)
{
    int32_t errorCode = (int32_t) U_ERROR_COMMON_INVALID_PARAMETER;

    if ((pMessageId != NULL) && (pPrivateMessageId != NULL)) {
        pPrivateMessageId->type = pMessageId->type;
        switch (pMessageId->type) {
            case U_GNSS_PROTOCOL_UBX:
                pPrivateMessageId->id.ubx = pMessageId->id.ubx;
                errorCode = (int32_t) U_ERROR_COMMON_SUCCESS;
                break;
            case U_GNSS_PROTOCOL_NMEA:
                pPrivateMessageId->id.nmea[0] = 0;
                if (pMessageId->id.pNmea != NULL) {
                    strncpy(pPrivateMessageId->id.nmea, pMessageId->id.pNmea,
                            sizeof(pPrivateMessageId->id.nmea));
                }
                errorCode = (int32_t) U_ERROR_COMMON_SUCCESS;
                break;
            case U_GNSS_PROTOCOL_RTCM:
                pPrivateMessageId->id.rtcm = pMessageId->id.rtcm;
                errorCode = (int32_t) U_ERROR_COMMON_SUCCESS;
                break;
            case U_GNSS_PROTOCOL_UNKNOWN:
                errorCode = (int32_t) U_ERROR_COMMON_SUCCESS;
                break;
            default:
                break;
        }
    }

    return errorCode;
}

// Convert a private message ID to a public message ID.
int32_t uGnssPrivateMessageIdToPublic(const uGnssPrivateMessageId_t *pPrivateMessageId,
                                      uGnssMessageId_t *pMessageId, char *pNmea)
{
    int32_t errorCode = (int32_t) U_ERROR_COMMON_INVALID_PARAMETER;

    if ((pPrivateMessageId != NULL) && (pMessageId != NULL) &&
        ((pPrivateMessageId->type != U_GNSS_PROTOCOL_NMEA) || (pNmea != NULL))) {
        pMessageId->type = pPrivateMessageId->type;
        switch (pPrivateMessageId->type) {
            case U_GNSS_PROTOCOL_UBX:
                pMessageId->id.ubx = pPrivateMessageId->id.ubx;
                errorCode = (int32_t) U_ERROR_COMMON_SUCCESS;
                break;
            case U_GNSS_PROTOCOL_NMEA:
                strncpy(pNmea, pPrivateMessageId->id.nmea,
                        U_GNSS_NMEA_MESSAGE_MATCH_LENGTH_CHARACTERS + 1);
                // Ensure a terminator
                *(pNmea + U_GNSS_NMEA_MESSAGE_MATCH_LENGTH_CHARACTERS) = 0;
                pMessageId->id.pNmea = pNmea;
                errorCode = (int32_t) U_ERROR_COMMON_SUCCESS;
                break;
            case U_GNSS_PROTOCOL_RTCM:
                pMessageId->id.rtcm = pPrivateMessageId->id.rtcm;
                errorCode = (int32_t) U_ERROR_COMMON_SUCCESS;
                break;
            case U_GNSS_PROTOCOL_UNKNOWN:
                errorCode = (int32_t) U_ERROR_COMMON_SUCCESS;
                break;
            default:
                break;
        }
    }

    return errorCode;
}

// Return true if the given private message ID is wanted.
bool uGnssPrivateMessageIdIsWanted(uGnssPrivateMessageId_t *pMessageId,
                                   uGnssPrivateMessageId_t *pMessageIdWanted)
{
    bool isWanted = false;

    if (pMessageIdWanted->type == U_GNSS_PROTOCOL_ANY) {
        isWanted = true;
    } else if ((pMessageIdWanted->type == U_GNSS_PROTOCOL_ALL) &&
               (pMessageId->type != U_GNSS_PROTOCOL_UNKNOWN)) {
        isWanted = true;
    } else if ((pMessageIdWanted->type == U_GNSS_PROTOCOL_UNKNOWN) &&
               (pMessageId->type == U_GNSS_PROTOCOL_UNKNOWN)) {
        isWanted = true;
    } else if ((pMessageIdWanted->type == U_GNSS_PROTOCOL_RTCM) &&
               (pMessageId->type == U_GNSS_PROTOCOL_RTCM)) {
        isWanted = rtcmIdMatch(pMessageId->id.rtcm, pMessageIdWanted->id.rtcm);
    } else if ((pMessageIdWanted->type == U_GNSS_PROTOCOL_NMEA) &&
               (pMessageId->type == U_GNSS_PROTOCOL_NMEA)) {
        isWanted = nmeaIdMatch(pMessageId->id.nmea, pMessageIdWanted->id.nmea);
    } else if ((pMessageIdWanted->type == U_GNSS_PROTOCOL_UBX) &&
               (pMessageId->type == U_GNSS_PROTOCOL_UBX)) {
        isWanted = ubxIdMatch(pMessageId->id.ubx, pMessageIdWanted->id.ubx);
    }

    return isWanted;
}

/* ----------------------------------------------------------------
 * PUBLIC FUNCTIONS THAT ARE PRIVATE TO GNSS: STREAMING TRANSPORT ONLY
 * -------------------------------------------------------------- */

// Get the private stream type from a given GNSS transport type.
int32_t uGnssPrivateGetStreamType(uGnssTransportType_t transportType)
{
    int32_t errorCodeOrPrivateStreamType = (int32_t) U_ERROR_COMMON_INVALID_PARAMETER;

    if ((transportType >= 0) &&
        (transportType < sizeof(gGnssPrivateTransportTypeToStream) /
         sizeof(gGnssPrivateTransportTypeToStream[0]))) {
        errorCodeOrPrivateStreamType = gGnssPrivateTransportTypeToStream[transportType];
    }

    return errorCodeOrPrivateStreamType;
}

// Get the stream handle from the transport handle.
int32_t uGnssPrivateGetStreamHandle(uGnssPrivateStreamType_t privateStreamType,
                                    uGnssTransportHandle_t transportHandle)
{
    int32_t errorCodeOrStreamHandle = (int32_t) U_ERROR_COMMON_INVALID_PARAMETER;

    switch (privateStreamType) {
        case U_GNSS_PRIVATE_STREAM_TYPE_UART:
            errorCodeOrStreamHandle = transportHandle.uart;
            break;
        case U_GNSS_PRIVATE_STREAM_TYPE_I2C:
            errorCodeOrStreamHandle = transportHandle.i2c;
            break;
        case U_GNSS_PRIVATE_STREAM_TYPE_SPI:
            errorCodeOrStreamHandle = transportHandle.spi;
            break;
        default:
            break;
    }

    return errorCodeOrStreamHandle;
}

// Get the number of bytes waiting for us when using a streaming transport.
// IMPORTANT: this function should not do anything that has "global"
// effect on the instance data since it is called by
// uGnssPrivateStreamFillRingBuffer() which may be called at any time by
// the message receive task over in u_gnss_msg.c
int32_t uGnssPrivateStreamGetReceiveSize(uGnssPrivateInstance_t *pInstance)
{
    int32_t errorCodeOrReceiveSize = (int32_t) U_ERROR_COMMON_INVALID_PARAMETER;
    int32_t streamHandle;
    int32_t privateStreamTypeOrError;
    char buffer[2];

    if (pInstance != NULL) {
        privateStreamTypeOrError = uGnssPrivateGetStreamType(pInstance->transportType);
        streamHandle = uGnssPrivateGetStreamHandle((uGnssPrivateStreamType_t) privateStreamTypeOrError,
                                                   pInstance->transportHandle);
        switch (privateStreamTypeOrError) {
            case U_GNSS_PRIVATE_STREAM_TYPE_UART: {
                errorCodeOrReceiveSize = uPortUartGetReceiveSize(streamHandle);
            }
            break;
            case U_GNSS_PRIVATE_STREAM_TYPE_I2C: {
                int32_t i2cAddress = pInstance->i2cAddress;
                // The number of bytes waiting for us is available by a read of
                // I2C register addresses 0xFD and 0xFE in the GNSS chip.
                // The register address in the GNSS chip auto-increments, so sending
                // 0xFD, with no stop bit, and then a read request for two bytes
                // should get us the [big-endian] length
                buffer[0] = 0xFD;
                errorCodeOrReceiveSize = uPortI2cControllerSend(streamHandle, i2cAddress,
                                                                buffer, 1, true);
                if (errorCodeOrReceiveSize == 0) {
                    errorCodeOrReceiveSize = uPortI2cControllerSendReceive(streamHandle, i2cAddress,
                                                                           NULL, 0, buffer, sizeof(buffer));
                    if (errorCodeOrReceiveSize == sizeof(buffer)) {
                        errorCodeOrReceiveSize = (int32_t) ((((uint32_t) buffer[0]) << 8) + (uint32_t) buffer[1]);
                    }
                }
            }
            break;
            case U_GNSS_PRIVATE_STREAM_TYPE_SPI: {
                char spiBuffer[U_GNSS_SPI_FILL_THRESHOLD_MAX];
                size_t spiReadLength;
                // SPI handling is a little different: since there is no way
                // to tell if there is any valid data, one just has to read
                // it and see if it is not 0xFF fill, we actually do a read
                // of up to spiFillThreshold bytes here, then we can determine
                // whether there is any real stuff.  The data that is read is
                // stored in the internal SPI ring buffer and can be read out
                // by whoever called this function
                spiReadLength = pInstance->spiFillThreshold;
                if (spiReadLength < U_GNSS_PRIVATE_SPI_READ_LENGTH_MIN_BYTES) {
                    spiReadLength = U_GNSS_PRIVATE_SPI_READ_LENGTH_MIN_BYTES;
                }
                errorCodeOrReceiveSize = uPortSpiControllerSendReceiveBlock(streamHandle,
                                                                            NULL, 0,
                                                                            spiBuffer,
                                                                            spiReadLength);
                if (errorCodeOrReceiveSize > 0) {
                    // This will add any non-fill SPI received data to the
                    // internal SPI ring buffer
                    errorCodeOrReceiveSize = uGnssPrivateSpiAddReceivedData(pInstance,
                                                                            spiBuffer,
                                                                            errorCodeOrReceiveSize);
                }
            }
            break;
            default:
                break;
        }
    }

    return errorCodeOrReceiveSize;
}

// Find the given message ID in the ring buffer.
// IMPORTANT: this function should not do anything that has "global"
// effect on the instance data since it is called by
// uGnssPrivateStreamFillRingBuffer() which may be called at any time by
// the message receive task over in u_gnss_msg.c
int32_t uGnssPrivateStreamDecodeRingBuffer(uRingBuffer_t *pRingBuffer,
                                           int32_t readHandle,
                                           uGnssPrivateMessageId_t *pPrivateMessageId)
{
    int32_t errorCodeOrLength = (int32_t) U_ERROR_COMMON_INVALID_PARAMETER;
    char *pDiscard = NULL;

    if ((pRingBuffer != NULL) && (pPrivateMessageId != NULL)) {
        while (1) {
            U_RING_BUFFER_PARSER_f parserList[] = {
                parseUbx,
                parseNmea,
                parseRtcm,
                NULL
            };
            uGnssPrivateMessageId_t msg;
            memset(&msg, 0, sizeof(msg));
            msg.type = U_GNSS_PROTOCOL_UNKNOWN;
            errorCodeOrLength = uRingBufferParseHandle(pRingBuffer, readHandle, parserList, &msg);
            if (errorCodeOrLength <= 0) {
                break;
            } else if (uGnssPrivateMessageIdIsWanted(&msg, pPrivateMessageId)) {
                memcpy(pPrivateMessageId, &msg, sizeof(uGnssPrivateMessageId_t));
#ifdef U_GNSS_PRIVATE_DEBUG_PARSING
                uPortLog("** ");
                printId(&msg);
                uPortLog(" size %d\n", errorCodeOrLength);
#endif
                break;
            } else {
#ifdef U_GNSS_PRIVATE_DEBUG_PARSING
                uPortLog("** DISCARD: wanted ");
                printId(pPrivateMessageId);
                uPortLog(", got ");
                printId(&msg);
                uPortLog(", %d byte(s)\n", errorCodeOrLength);
#endif
                if ((pPrivateMessageId->type == U_GNSS_PROTOCOL_UBX) &&
                    (msg.type == U_GNSS_PROTOCOL_UBX) &&
                    (msg.id.ubx == 0x0500/*ACK-NACK*/) && (errorCodeOrLength == 10)) {
                    uint8_t msg[10];
                    if (10 == uRingBufferReadHandle(pRingBuffer, readHandle, (char *)msg, 10)) {
                        uint16_t ubxId = (msg[6]/*CLS*/ << 8) | msg[7]/*ID*/;
                        if (ubxIdMatch(ubxId, pPrivateMessageId->id.ubx)) {
#ifdef U_GNSS_PRIVATE_DEBUG_PARSING
                            uPortLog("** ...but noting a UBX ACK-NACK for %04x => U_GNSS_ERROR_NACK\n", ubxId);
#endif
                            errorCodeOrLength = U_GNSS_ERROR_NACK;
                            break;
                        }
                    }
                } else {
#ifdef U_GNSS_PRIVATE_DEBUG_PARSING
                    pDiscard = (char *) pUPortMalloc(errorCodeOrLength);
#endif
                    // Discard what is not wanted by the caller
                    uRingBufferReadHandle(pRingBuffer, readHandle, pDiscard, errorCodeOrLength);
#ifdef U_GNSS_PRIVATE_DEBUG_PARSING
                    if (pDiscard != NULL) {
                        uPortLog("** Discarded contents:");
                        uGnssPrivatePrintBuffer(pDiscard, errorCodeOrLength);
                        uPortLog("\n");
                        uPortFree(pDiscard);
                    }
#endif
                }
            }
        };
    }
    return errorCodeOrLength;
}

// Fill the internal ring buffer with data from the GNSS chip.
// IMPORTANT: this function should not do anything that has "global"
// effect on the instance data since it may be called at any time
// by the message receive task over in u_gnss_msg.c.
int32_t uGnssPrivateStreamFillRingBuffer(uGnssPrivateInstance_t *pInstance,
                                         int32_t timeoutMs, int32_t maxTimeMs)
{
    int32_t errorCodeOrLength = (int32_t) U_ERROR_COMMON_INVALID_PARAMETER;
    int32_t startTimeMs;
    int32_t privateStreamTypeOrError;
    int32_t streamHandle = -1;
    int32_t receiveSize;
    int32_t totalReceiveSize = 0;
    int32_t ringBufferAvailableSize;
    char *pTemporaryBuffer;

    if (pInstance != NULL) {
        pTemporaryBuffer = pInstance->pTemporaryBuffer;
        if ((pInstance->pMsgReceive != NULL) &&
            uPortTaskIsThis(pInstance->pMsgReceive->taskHandle)) {
            // If we're being called from the message receive task,
            // which does not lock gUGnssPrivateMutex, we use its
            // temporary buffer in order to avoid clashes with
            // the main application task
            pTemporaryBuffer = pInstance->pMsgReceive->pTemporaryBuffer;
        }
        errorCodeOrLength = (int32_t) U_ERROR_COMMON_NOT_SUPPORTED;
        privateStreamTypeOrError = uGnssPrivateGetStreamType(pInstance->transportType);
        streamHandle = uGnssPrivateGetStreamHandle((uGnssPrivateStreamType_t) privateStreamTypeOrError,
                                                   pInstance->transportHandle);
        if (streamHandle >= 0) {
            errorCodeOrLength = (int32_t) U_ERROR_COMMON_TIMEOUT;
            startTimeMs = uPortGetTickTimeMs();
            // This is constructed as a do()/while() so that
            // it always has one go even with a zero timeout
            do {
                receiveSize = uGnssPrivateStreamGetReceiveSize(pInstance);
                // Don't try to read in more than uRingBufferForceAdd()
                // can put into the ring buffer
                ringBufferAvailableSize = uRingBufferAvailableSizeMax(&(pInstance->ringBuffer));
                if (receiveSize > ringBufferAvailableSize) {
                    receiveSize = ringBufferAvailableSize;
                }
                if (receiveSize > 0) {
                    // Read into a temporary buffer
                    if (receiveSize > U_GNSS_MSG_TEMPORARY_BUFFER_LENGTH_BYTES) {
                        receiveSize = U_GNSS_MSG_TEMPORARY_BUFFER_LENGTH_BYTES;
                    }
                    switch (privateStreamTypeOrError) {
                        case U_GNSS_PRIVATE_STREAM_TYPE_UART:
                            // For UART we ask for as much data as we can, it will just
                            // bring in more if more has arrived between the "receive
                            // size" call above and now
                            receiveSize = uPortUartRead(streamHandle, pTemporaryBuffer,
                                                        U_GNSS_MSG_TEMPORARY_BUFFER_LENGTH_BYTES);
                            break;
                        case U_GNSS_PRIVATE_STREAM_TYPE_I2C:
                            // For I2C we need to ask for the amount we know is there since
                            // the I2C buffer is effectively on the GNSS chip and I2C drivers
                            // often don't say how much they've read, just giving us back
                            // the number we asked for on a successful read
                            receiveSize = uPortI2cControllerSendReceive(streamHandle,
                                                                        pInstance->i2cAddress,
                                                                        NULL, 0,
                                                                        pTemporaryBuffer,
                                                                        receiveSize);
                            break;
                        case U_GNSS_PRIVATE_STREAM_TYPE_SPI:
                            // For the SPI case, we need to pull the data that was
                            // received in uGnssPrivateStreamGetReceiveSize() back
                            // out of the SPI ring buffer and into our temporary buffer
                            receiveSize = (int32_t) uRingBufferRead(pInstance->pSpiRingBuffer,
                                                                    pTemporaryBuffer, receiveSize);
                        default:
                            break;
                    }
                    if (receiveSize >= 0) {
                        totalReceiveSize += receiveSize;
                        errorCodeOrLength = totalReceiveSize;
                        // Now stuff this into the ring buffer; we use a forced
                        // add: it is up to this MCU to keep up, we don't want
                        // to block data from the GNSS chip, after all it has
                        // no UART flow control lines that we can stop it with
                        if (!uRingBufferForceAdd(&(pInstance->ringBuffer),
                                                 pTemporaryBuffer, receiveSize)) {
                            errorCodeOrLength = (int32_t) U_ERROR_COMMON_NO_MEMORY;
                        }
                    } else {
                        // Error case
                        errorCodeOrLength = receiveSize;
                    }
                } else if ((ringBufferAvailableSize > 0) && (timeoutMs > 0)) {
                    // Relax while we're waiting for more data to arrive
                    uPortTaskBlock(10);
                }
                // Exit if we get an error (that is not a timeout), or if we were given zero time,
                // or if there is no room in the ring-buffer for more data, or if we've
                // received nothing and hit the timeout, or if we are not still receiving stuff
                // or were given a maximum time and have exceeded it
            } while (((errorCodeOrLength == (int32_t) U_ERROR_COMMON_TIMEOUT) || (errorCodeOrLength >= 0)) &&
                     (timeoutMs > 0) && (ringBufferAvailableSize > 0) &&
                     // The first condition below is the "not yet received anything case", guarded by timeoutMs
                     // the second condition below is when we're receiving stuff, guarded by maxTimeMs
                     (((totalReceiveSize == 0) && (uPortGetTickTimeMs() - startTimeMs < timeoutMs)) ||
                      ((receiveSize > 0) && ((maxTimeMs == 0) || (uPortGetTickTimeMs() - startTimeMs < maxTimeMs)))));
        }
    }

    if (totalReceiveSize > 0) {
        errorCodeOrLength = totalReceiveSize;
    }

    return errorCodeOrLength;
}

// Read data from the internal ring buffer into the given linear buffer.
int32_t uGnssPrivateStreamReadRingBuffer(uGnssPrivateInstance_t *pInstance,
                                         int32_t readHandle,
                                         char *pBuffer, size_t size,
                                         int32_t maxTimeMs)
{
    return streamGetFromRingBuffer(pInstance, readHandle, pBuffer, size,
                                   0, maxTimeMs, true);
}

// Take a peek at the data in the internal ring buffer.
int32_t uGnssPrivateStreamPeekRingBuffer(uGnssPrivateInstance_t *pInstance,
                                         int32_t readHandle,
                                         char *pBuffer, size_t size,
                                         size_t offset,
                                         int32_t maxTimeMs)
{
    return streamGetFromRingBuffer(pInstance, readHandle, pBuffer, size,
                                   offset, maxTimeMs, false);
}

// Send a UBX format message over UART or I2C.
int32_t uGnssPrivateSendOnlyStreamUbxMessage(uGnssPrivateInstance_t *pInstance,
                                             int32_t messageClass,
                                             int32_t messageId,
                                             const char *pMessageBody,
                                             size_t messageBodyLengthBytes)
{
    int32_t errorCodeOrSentLength = (int32_t) U_ERROR_COMMON_INVALID_PARAMETER;
    int32_t bytesToSend = 0;
    char *pBuffer;

    if (pInstance != NULL) {
        if ((uGnssPrivateGetStreamType(pInstance->transportType) >= 0) &&
            (((pMessageBody == NULL) && (messageBodyLengthBytes == 0)) ||
             (messageBodyLengthBytes > 0))) {
            errorCodeOrSentLength = (int32_t) U_ERROR_COMMON_NO_MEMORY;

            // Allocate a buffer big enough to encode the outgoing message
            pBuffer = (char *) pUPortMalloc(messageBodyLengthBytes + U_UBX_PROTOCOL_OVERHEAD_LENGTH_BYTES);
            if (pBuffer != NULL) {
                bytesToSend = uUbxProtocolEncode(messageClass, messageId,
                                                 pMessageBody, messageBodyLengthBytes,
                                                 pBuffer);

                U_PORT_MUTEX_LOCK(pInstance->transportMutex);

                errorCodeOrSentLength = sendMessageStream(pInstance, pBuffer, bytesToSend,
                                                          pInstance->printUbxMessages);

                U_PORT_MUTEX_UNLOCK(pInstance->transportMutex);

                // Free memory
                uPortFree(pBuffer);
            }
        }
    }

    return errorCodeOrSentLength;
}

// Send a message that has no acknowledgement and check that it was received.
int32_t uGnssPrivateSendOnlyCheckStreamUbxMessage(uGnssPrivateInstance_t *pInstance,
                                                  int32_t messageClass,
                                                  int32_t messageId,
                                                  const char *pMessageBody,
                                                  size_t messageBodyLengthBytes)
{
    int32_t errorCodeOrLength = (int32_t) U_ERROR_COMMON_INVALID_PARAMETER;
    int32_t userMessageSentLength;
    // Message buffer for the 120-byte UBX-MON-MSGPP message
    char message[120] = {0};
    uint64_t y;

    if ((pInstance != NULL) &&
        (uGnssPrivateGetStreamType(pInstance->transportType) >= 0)) {
        // Send UBX-MON-MSGPP to get the number of messages received
        errorCodeOrLength = uGnssPrivateSendReceiveUbxMessage(pInstance,
                                                              0x0a, 0x06,
                                                              NULL, 0,
                                                              message,
                                                              sizeof(message));
        if (errorCodeOrLength == sizeof(message)) {
            // Derive the number of messages received on the port
            y = uUbxProtocolUint64Decode(message + ((size_t) (unsigned) pInstance->portNumber * 16));
            // Now send the message
            errorCodeOrLength = uGnssPrivateSendOnlyStreamUbxMessage(pInstance, messageClass, messageId,
                                                                     pMessageBody, messageBodyLengthBytes);
            if (errorCodeOrLength == messageBodyLengthBytes + U_UBX_PROTOCOL_OVERHEAD_LENGTH_BYTES) {
                userMessageSentLength = errorCodeOrLength;
                // Get the number of received messages again
                errorCodeOrLength = uGnssPrivateSendReceiveUbxMessage(pInstance,
                                                                      0x0a, 0x06,
                                                                      NULL, 0,
                                                                      message,
                                                                      sizeof(message));
                if (errorCodeOrLength == sizeof(message)) {
                    errorCodeOrLength = (int32_t) U_ERROR_COMMON_PLATFORM;
                    y = uUbxProtocolUint64Decode(message + ((size_t) (unsigned) pInstance->portNumber * 16)) - y;
                    // Should be two: UBX-MON-MSGPP and then the send done by
                    // uGnssPrivateSendReceiveUbxMessage().
                    if (y == 2) {
                        errorCodeOrLength = userMessageSentLength;
                    }
                }
            }
        }
    }

    return errorCodeOrLength;
}

// Receive an arbitrary message over UART or I2C.
int32_t uGnssPrivateReceiveStreamMessage(uGnssPrivateInstance_t *pInstance,
                                         uGnssPrivateMessageId_t *pPrivateMessageId,
                                         int32_t readHandle,
                                         char **ppBuffer, size_t size,
                                         int32_t timeoutMs,
                                         bool (*pKeepGoingCallback)(uDeviceHandle_t gnssHandle))
{
    int32_t errorCodeOrLength = (int32_t) U_ERROR_COMMON_INVALID_PARAMETER;
    int32_t receiveSize;
    int32_t ringBufferSize;
    size_t discardSize = 0;
    int32_t startTimeMs;
    int32_t x = timeoutMs > 0 ? U_GNSS_RING_BUFFER_MIN_FILL_TIME_MS : 0;
    int32_t y;

    if ((pInstance != NULL) && (pPrivateMessageId != NULL) &&
        (ppBuffer != NULL) && ((*ppBuffer == NULL) || (size > 0))) {
        errorCodeOrLength = (int32_t) U_ERROR_COMMON_TIMEOUT;
        startTimeMs = uPortGetTickTimeMs();
        // Lock our read pointer while we look for stuff
        uRingBufferLockReadHandle(&(pInstance->ringBuffer), readHandle);
        // This is constructed as a do()/while() so that it always has one go
        // even with a zero timeout
        do {
            // Try to pull some more data in
            receiveSize = uGnssPrivateStreamFillRingBuffer(pInstance, x, 0);
            // Get the number of bytes waiting for us in the ring buffer
            ringBufferSize = uRingBufferDataSizeHandle(&(pInstance->ringBuffer),
                                                       readHandle);
            if (ringBufferSize < 0) {
                errorCodeOrLength = ringBufferSize;
            } else if (ringBufferSize > 0) {
                // Deal with any discard from a previous run around this loop
                discardSize -= uRingBufferReadHandle(&(pInstance->ringBuffer),
                                                     readHandle,
                                                     NULL, discardSize);
                if (discardSize == 0) {
                    // Attempt to decode a message/message header from the ring buffer
                    errorCodeOrLength = uGnssPrivateStreamDecodeRingBuffer(&(pInstance->ringBuffer),
                                                                           readHandle,
                                                                           pPrivateMessageId);
                    if (errorCodeOrLength > 0) {
                        if (*ppBuffer == NULL) {
                            // The caller didn't give us any memory; allocate the right
                            // amount; the caller must free this memory
                            *ppBuffer = pUPortMalloc(errorCodeOrLength);
                        } else {
                            // If the user gave us a buffer, limit the size
                            if (errorCodeOrLength > (int32_t) size) {
                                discardSize += errorCodeOrLength - size;
                                errorCodeOrLength = size;
                            }
                        }
                        if (*ppBuffer != NULL) {
                            // Now read the message data into the buffer,
                            // which will move our read pointer on
                            y = timeoutMs - (uPortGetTickTimeMs() - startTimeMs);
                            if (y < U_GNSS_RING_BUFFER_MIN_FILL_TIME_MS) {
                                // Make sure we give ourselves time to read the messsage out
                                y = U_GNSS_RING_BUFFER_MIN_FILL_TIME_MS;
                            }
                            errorCodeOrLength = uGnssPrivateStreamReadRingBuffer(pInstance,
                                                                                 readHandle,
                                                                                 *ppBuffer,
                                                                                 errorCodeOrLength, y);
                        } else {
                            discardSize = errorCodeOrLength;
                            errorCodeOrLength = (int32_t) U_ERROR_COMMON_NO_MEMORY;
                        }
                    }
                }
            }

            if ((receiveSize <= 0) && (timeoutMs > 0)) {
                // Relax a little while we're waiting for some data
                uPortTaskBlock(10);
            }

            // Continue to loop while we've not received anything (provided
            // there hasn't been a NACK for the UBX-format message we were looking
            // for and we haven't run out of memory) or still need to discard things,
            // but always checking the guard time/callback.
        } while ((((errorCodeOrLength < 0) && (errorCodeOrLength != (int32_t) U_GNSS_ERROR_NACK) &&
                   (errorCodeOrLength != (int32_t) U_ERROR_COMMON_NO_MEMORY)) || (discardSize > 0)) &&
                 (timeoutMs > 0) &&
                 (uPortGetTickTimeMs() - startTimeMs < timeoutMs) &&
                 ((pKeepGoingCallback == NULL) || pKeepGoingCallback(pInstance->gnssHandle)));

        // Read pointer can be unlocked now
        uRingBufferUnlockReadHandle(&(pInstance->ringBuffer), readHandle);
    }

    return errorCodeOrLength;
}

// Add received data to the internal SPI buffer.
int32_t uGnssPrivateSpiAddReceivedData(uGnssPrivateInstance_t *pInstance,
                                       const char *pBuffer, size_t size)
{
    int32_t errorCodeOrLength = (int32_t) U_ERROR_COMMON_INVALID_PARAMETER;
    const char *pTmp = pBuffer;
    int32_t y;

    if ((pInstance != NULL) && (pInstance->pSpiRingBuffer != NULL) &&
        (pBuffer != NULL) && (size > 0)) {
        if ((pInstance->spiFillThreshold > 0) && (size >= (size_t) pInstance->spiFillThreshold)) {
            // Check if all we have is fill and chuck stuff away if so
            for (size_t x = 0; (x < size) && (*pTmp == U_GNSS_PRIVATE_SPI_FILL); x++) {
                pTmp++;
            }
            y = pTmp - pBuffer;
            if (y >= pInstance->spiFillThreshold) {
                pBuffer += y;
                size -= y;
            }
        }
        // Do a forced add so we always keep the most recent data
        uRingBufferForceAdd(pInstance->pSpiRingBuffer, pBuffer, size);
        if (pInstance->spiFillThreshold > 0) {
            // Fill might still have got into the ring buffer, e.g. if
            // we are receiving data in chunks smaller than the fill
            // threshold, so check for any fill in the buffer also
            uRingBufferFlushValue(pInstance->pSpiRingBuffer, U_GNSS_PRIVATE_SPI_FILL,
                                  pInstance->spiFillThreshold);
        }
        errorCodeOrLength = (int32_t) uRingBufferDataSize(pInstance->pSpiRingBuffer);
    }

    return errorCodeOrLength;
}

/* ----------------------------------------------------------------
 * PUBLIC FUNCTIONS THAT ARE PRIVATE TO GNSS: ANY TRANSPORT
 * -------------------------------------------------------------- */

// Send a UBX format message and receive a response of known length.
int32_t uGnssPrivateSendReceiveUbxMessage(uGnssPrivateInstance_t *pInstance,
                                          int32_t messageClass,
                                          int32_t messageId,
                                          const char *pMessageBody,
                                          size_t messageBodyLengthBytes,
                                          char *pResponseBody,
                                          size_t maxResponseBodyLengthBytes)
{
    uGnssPrivateUbxReceiveMessage_t response;

    // Fill the response structure in with the message class
    // and ID we expect to get back and the buffer passed in.
    response.cls = messageClass;
    response.id = messageId;
    response.ppBody = NULL;
    response.bodySize = 0;
    if (pResponseBody != NULL) {
        response.ppBody = &pResponseBody;
        response.bodySize = maxResponseBodyLengthBytes;
    }

    return sendReceiveUbxMessage(pInstance, messageClass, messageId,
                                 pMessageBody, messageBodyLengthBytes,
                                 &response);
}

// Send a UBX format message and receive a response of unknown length.
int32_t uGnssPrivateSendReceiveUbxMessageAlloc(uGnssPrivateInstance_t *pInstance,
                                               int32_t messageClass,
                                               int32_t messageId,
                                               const char *pMessageBody,
                                               size_t messageBodyLengthBytes,
                                               char **ppResponseBody)
{
    uGnssPrivateUbxReceiveMessage_t response;

    // Fill the response structure in with the message class
    // and ID we expect to get back
    response.cls = messageClass;
    response.id = messageId;
    response.ppBody = ppResponseBody;
    response.bodySize = 0;

    return sendReceiveUbxMessage(pInstance, messageClass, messageId,
                                 pMessageBody, messageBodyLengthBytes,
                                 &response);
}

// Send a UBX format message to the GNSS module that only has an
// Ack response and check that it is Acked.
int32_t uGnssPrivateSendUbxMessage(uGnssPrivateInstance_t *pInstance,
                                   int32_t messageClass,
                                   int32_t messageId,
                                   const char *pMessageBody,
                                   size_t messageBodyLengthBytes)
{
    int32_t errorCode;
    uGnssPrivateUbxReceiveMessage_t response;
    char ackBody[2] = {0};
    char *pBody = &(ackBody[0]);

    // Fill the response structure in with the message class
    // and ID we expect to get back and the buffer passed in.
    response.cls = 0x05;
    response.id = -1;
    response.ppBody = &pBody;
    response.bodySize = sizeof(ackBody);

    errorCode = sendReceiveUbxMessage(pInstance, messageClass, messageId,
                                      pMessageBody, messageBodyLengthBytes,
                                      &response);
    if ((errorCode == 2) && (response.cls == 0x05) &&
        (ackBody[0] == (char) messageClass) &&
        (ackBody[1] == (char) messageId)) {
        errorCode = (int32_t) U_GNSS_ERROR_NACK;
        if (response.id == 0x01) {
            errorCode = (int32_t) U_ERROR_COMMON_SUCCESS;
        }
    } else {
        errorCode = (int32_t) U_ERROR_COMMON_UNKNOWN;
    }

    return errorCode;
}

// End of file
