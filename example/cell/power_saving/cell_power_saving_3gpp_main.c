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

/** @brief This example demonstrates how to configure a module
 * for 3GPP power saving.
 *
 * The choice of module and the choice of platform on which this
 * code runs is made at build time, see the README.md for
 * instructions.
 */

#ifdef U_CFG_TEST_CELL_MODULE_TYPE

// Bring in all of the ubxlib public header files
# include "ubxlib.h"

// Bring in the application settings
# include "u_cfg_app_platform_specific.h"

# ifndef U_CFG_DISABLE_TEST_AUTOMATION
// This purely for internal u-blox testing
#  include "u_cell_test_cfg.h"
#  include "u_cfg_test_platform_specific.h"
# endif

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS
 * -------------------------------------------------------------- */

// The requested "active time" for 3GPP power saving.  This is the
// period of inactivity after which the module may enter deep sleep.
# ifndef ACTIVE_TIME_SECONDS
#  define ACTIVE_TIME_SECONDS 60
# endif

// The requested period at which the module will wake up to inform
// the cellular network that it is still connected; this should
// be set to around 1.5 times your application's natural periodicity,
// as a safety-net; the wake-up only occurs if the module has not already
// woken up for other reasons in time.
# ifndef PERIODIC_WAKEUP_SECONDS
#  define PERIODIC_WAKEUP_SECONDS (3600 * 4)
# endif

// The RAT the module will use.  While it is not a requirement
// to set this explicitly (you could, for instance just register
// with the network and then call uCellNetGetActiveRat() to find
// out which RAT you are registered on), power saving is only
// supported on an EUTRAN RAT (Cat-M1 or NB1) and some modules
// require a re-boot to apply new 3GPP power saving settings, so
// rather than messing about registering and then rebooting if
// required, for this example code we set the RAT explicitly.
# ifndef MY_RAT
#  define MY_RAT U_CELL_NET_RAT_CATM1
# endif

// For u-blox internal testing only
# ifdef U_PORT_TEST_ASSERT
#  define EXAMPLE_FINAL_STATE(x) U_PORT_TEST_ASSERT(x);
# else
#  define EXAMPLE_FINAL_STATE(x)
# endif

# ifndef U_PORT_TEST_FUNCTION
#  error if you are not using the unit test framework to run this code you must ensure that the platform clocks/RTOS are set up and either define U_PORT_TEST_FUNCTION yourself or replace it as necessary.
# endif

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * VARIABLES
 * -------------------------------------------------------------- */

// Cellular configuration.
// Set U_CFG_TEST_CELL_MODULE_TYPE to your module type,
// chosen from the values in cell/api/u_cell_module_type.h
//
// Note that the pin numbers are those of the MCU: if you
// are using an MCU inside a u-blox module the IO pin numbering
// for the module is likely different that from the MCU: check
// the data sheet for the module to determine the mapping.

// DEVICE i.e. module/chip configuration: in this case a cellular
// module connected via UART
static const uDeviceCfg_t gDeviceCfg = {
    .deviceType = U_DEVICE_TYPE_CELL,
    .deviceCfg = {
        .cfgCell = {
            .moduleType = U_CFG_TEST_CELL_MODULE_TYPE,
            .pSimPinCode = NULL, /* SIM pin */
            .pinEnablePower = U_CFG_APP_PIN_CELL_ENABLE_POWER,
            .pinPwrOn = U_CFG_APP_PIN_CELL_PWR_ON,
            .pinVInt = U_CFG_APP_PIN_CELL_VINT,
            .pinDtrPowerSaving = U_CFG_APP_PIN_CELL_DTR
        },
    },
    .transportType = U_DEVICE_TRANSPORT_TYPE_UART,
    .transportCfg = {
        .cfgUart = {
            .uart = U_CFG_APP_CELL_UART,
            .baudRate = U_CELL_UART_BAUD_RATE,
            .pinTxd = U_CFG_APP_PIN_CELL_TXD,
            .pinRxd = U_CFG_APP_PIN_CELL_RXD,
            .pinCts = U_CFG_APP_PIN_CELL_CTS,
            .pinRts = U_CFG_APP_PIN_CELL_RTS
        },
    },
};
// NETWORK configuration for cellular
static const uNetworkCfgCell_t gNetworkCfg = {
    .type = U_NETWORK_TYPE_CELL,
    .pApn = NULL, /* APN: NULL to accept default.  If using a Thingstream SIM enter "tsiot" here */
    .timeoutSeconds = 240 /* Connection timeout in seconds */
    // There is an additional field here "pKeepGoingCallback",
    // which we do NOT set, we allow the compiler to set it to 0
    // and all will be fine. You may set the field to a function
    // of the form "bool keepGoingCallback(uDeviceHandle_t devHandle)",
    // e.g.:
    // .pKeepGoingCallback = keepGoingCallback
    // ...and your function will be called periodically during an
    // abortable network operation such as connect/disconnect;
    // if it returns true the operation will continue else it
    // will be aborted, allowing you immediate control.  If this
    // field is set, timeoutSeconds will be ignored.
};

// Flag that allows us to check if power saving has been set.
static volatile bool g3gppPowerSavingSet = false;

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS
 * -------------------------------------------------------------- */

// Callback that will be called when the network indicates what 3GPP
// power saving settings have been applied
static void callback(uDeviceHandle_t devHandle, bool onNotOff,
                     int32_t activeTimeSeconds, int32_t periodicWakeupSeconds,
                     void *pParameter)
{
    (void)devHandle;
    (void)pParameter;
    uPortLog("## 3GPP power saving is %s, active time %d seconds,"
             " periodic wake-up %d seconds.\n", onNotOff ? "on" : "off",
             activeTimeSeconds, periodicWakeupSeconds);

    // Check if the settings are as we expect.  Note that the 3GPP
    // encoding does not support all values, hence the check is >=
    // rather than ==
    if (onNotOff && (activeTimeSeconds >= ACTIVE_TIME_SECONDS) &&
        (periodicWakeupSeconds >= PERIODIC_WAKEUP_SECONDS)) {
        g3gppPowerSavingSet = true;
    }
}

/* ----------------------------------------------------------------
 * PUBLIC FUNCTIONS: THE EXAMPLE
 * -------------------------------------------------------------- */

// The entry point, main(): before this is called the system
// clocks must have been started and the RTOS must be running;
// we are in task space.
U_PORT_TEST_FUNCTION("[example]", "exampleCellPowerSaving3gpp")
{
    uDeviceHandle_t devHandle = NULL;
    bool onMyRat = true;
    int32_t x = -1;
    int32_t returnCode;

    // Initialise the APIs we will need
    uPortInit();
    uDeviceInit();

    // Add a cellular network instance.
    // Open the device
    returnCode = uDeviceOpen(&gDeviceCfg, &devHandle);
    uPortLog("### Opened device with return code %d.\n", returnCode);

    // Set a callback for when the 3GPP power saving parameters are
    // agreed by the network
    uCellPwrSet3gppPowerSavingCallback(devHandle, callback, NULL);

    // Set the primary RAT to MY_RAT
    if (uCellCfgGetRat(devHandle, 0) != MY_RAT) {
        if (uCellCfgSetRatRank(devHandle, MY_RAT, 0) != 0) {
            onMyRat = false;
        }
    }

    if (onMyRat) {
        // Set the requested 3GPP power saving values
        uPortLog("## Requesting 3GPP power saving with active time"
                 " %d seconds, periodic wake-up %d seconds...\n",
                 ACTIVE_TIME_SECONDS, PERIODIC_WAKEUP_SECONDS);
        x = uCellPwrSetRequested3gppPowerSaving(devHandle, MY_RAT, true,
                                                ACTIVE_TIME_SECONDS,
                                                PERIODIC_WAKEUP_SECONDS);
        if (x == 0) {
            // Reboot the module, if required, to apply the settings
            if (uCellPwrRebootIsRequired(devHandle)) {
                uCellPwrReboot(devHandle, NULL);
            }

            // Bring up the network
            uPortLog("### Bringing up the network...\n");
            if (uNetworkInterfaceUp(devHandle, U_NETWORK_TYPE_CELL,
                                    &gNetworkCfg) == 0) {

                // Here you would normally do useful stuff; for the
                // purposes of this simple power-saving example,
                // we just wait for our requested 3GPP power
                // saving settings to be agreed by the network.
                while (!g3gppPowerSavingSet && (x < 30)) {
                    uPortTaskBlock(1000);
                    x++;
                }

                if (g3gppPowerSavingSet) {
                    uPortLog("### The 3GPP power saving settings have been agreed.\n");
                } else {
                    uPortLog("### Unable to switch 3GPP power saving on!\n");
                }

                // When finished with the network layer
                uPortLog("### Taking down network...\n");
                uNetworkInterfaceDown(devHandle, U_NETWORK_TYPE_CELL);
            } else {
                uPortLog("### Unable to bring up the network!\n");
            }
        } else {
            uPortLog("### 3GPP power saving is not supported in this configuration!\n");
        }
    } else {
        uPortLog("### Unable to set primary RAT to %d!\n", MY_RAT);
    }

    // Close the device
    // Note: we don't power the device down here in order
    // to speed up testing; you may prefer to power it off
    // by setting the second parameter to true.
    uDeviceClose(devHandle, false);

    // Tidy up
    uDeviceDeinit();
    uPortDeinit();

    uPortLog("### Done.\n");

# ifndef U_CFG_CELL_DISABLE_UART_POWER_SAVING
    // For u-blox internal testing only
    EXAMPLE_FINAL_STATE((x < 0) || g3gppPowerSavingSet);
#  ifdef U_PORT_TEST_ASSERT
    // We don't want 3GPP power saving left on for our internal testing,
    // we need the module to stay awake, so switch it off again here
    if (g3gppPowerSavingSet) {
        uPortInit();
        uDeviceInit();
        returnCode = uDeviceOpen(&gDeviceCfg, &devHandle);
        uPortLog("### Added network with return code %d.\n", returnCode);
        uCellPwrSetRequested3gppPowerSaving(devHandle, MY_RAT, false, -1, -1);
        // Reboot the module, if required, to apply the settings
        if (uCellPwrRebootIsRequired(devHandle)) {
            uCellPwrReboot(devHandle, NULL);
        }
        // Close the device
        // Note: we don't power the device down here in order
        // to speed up testing.
        uDeviceClose(devHandle, false);
        uDeviceDeinit();
        uPortDeinit();
    }
#  endif
# endif
}

#endif // #ifdef U_CFG_TEST_CELL_MODULE_TYPE

// End of file
