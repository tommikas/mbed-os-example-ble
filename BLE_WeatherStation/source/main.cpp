/* ESP8266 implementation of NetworkInterfaceAPI
 * Copyright (c) 2019 ARM Limited
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <events/mbed_events.h>
#include "mbed.h"
#include "ble/BLE.h"
#include "ble/services/EnvironmentalService.h"
#include "ble/SecurityManager.h"
#if MBED_CONF_APP_FILESYSTEM_SUPPORT
#include "LittleFileSystem.h"
#include "FlashIAPBlockDevice.h"
#include "SlicingBlockDevice.h"
#endif //MBED_CONF_APP_FILESYSTEM_SUPPORT

#define SENSOR_TYPE_FAKE     (0)
#define SENSOR_TYPE_BME280   (1)

#if MBED_CONF_APP_SENSOR_TYPE == SENSOR_TYPE_BME280
#include "bme280-driver/BME280Sensor.h"
BME280Sensor bme280;
#endif

#ifdef TARGET_RUUVITAG
#include "drivers/SerialWireOutput.h"
// On RUUVITAG standard output is done via SWO as the serial UART pins are not connected.
// This function will retarget the stdout to SWO.
FileHandle *mbed::mbed_override_console(int fd) {
    static SerialWireOutput swo_serial;
    return &swo_serial;
}
#endif

#define LED_ON (1)
#define LED_OFF (!LED_ON)

DigitalOut led1(LED1, LED_OFF);


const static char     DEVICE_NAME[]        = MBED_CONF_APP_BLE_DEVICE_NAME;
static const uint16_t uuid16_list[]        = {GattService::UUID_ENVIRONMENTAL_SERVICE};

/* for demonstration purposes we will store the peer device address
 * of the device that connects to us in the first demonstration
 * so we can use its address to reconnect to it later */
static BLEProtocol::AddressBytes_t peer_address;

static EnvironmentalService *environmentalServicePtr;

static EventQueue eventQueue(/* event count */ 16 * EVENTS_EVENT_SIZE);

/** Base class for both peripheral and central. The same class that provides
 *  the logic for the application also implements the SecurityManagerEventHandler
 *  which is the interface used by the Security Manager to communicate events
 *  back to the applications. You can provide overrides for a selection of events
 *  your application is interested in.
 */
class SMDevice : private mbed::NonCopyable<SMDevice>,
                 public SecurityManager::EventHandler
{
public:
    SMDevice(BLE &ble, events::EventQueue &event_queue, BLEProtocol::AddressBytes_t &peer_address) :
        ble(ble),
        _event_queue(event_queue),
        _peer_address(peer_address),
        _handle(0),
        _is_connected(false),
        _is_secure(false) { };

    virtual ~SMDevice()
    {
        if (ble.hasInitialized()) {
            ble.shutdown();
        }
    };

    /* event handler functions */

    /** Respond to a pairing request. This will be called by the stack
     * when a pairing request arrives and expects the application to
     * call acceptPairingRequest or cancelPairingRequest */
    virtual void pairingRequest(
        ble::connection_handle_t connectionHandle
    ) {
        printf("Pairing requested - authorising\r\n");
        ble.securityManager().acceptPairingRequest(connectionHandle);
    }

    /** Inform the application of a successful pairing. */
    virtual void pairingResult(
        ble::connection_handle_t connectionHandle,
        SecurityManager::SecurityCompletionStatus_t result
    ) {
        if (result == SecurityManager::SEC_STATUS_SUCCESS) {
            printf("Pairing successful\r\n");
        } else {
            printf("Pairing failed\r\n");
        }
    }

    /** Inform the application of change in encryption status. This will be
     * communicated through the serial port */
    virtual void linkEncryptionResult(
        ble::connection_handle_t connectionHandle,
        ble::link_encryption_t result
    ) {
        if (result == ble::link_encryption_t::ENCRYPTED) {
            _is_secure = true;
            printf("Link ENCRYPTED\r\n");
        } else if (result == ble::link_encryption_t::ENCRYPTED_WITH_MITM) {
            _is_secure = true;
            printf("Link ENCRYPTED_WITH_MITM\r\n");
        } else if (result == ble::link_encryption_t::NOT_ENCRYPTED) {
            _is_secure = false;
            printf("Link NOT_ENCRYPTED - terminating connection\r\n");
            _event_queue.call(&ble.gap(), &Gap::disconnect, _handle, Gap::REMOTE_USER_TERMINATED_CONNECTION);
        }

    }

    /** This is called when BLE interface is initialised and starts the demonstration */
    void initComplete(BLE::InitializationCompleteCallbackContext *event)
    {
        ble_error_t error;

        if (event->error) {
            printf("Error during the initialisation\r\n");
            return;
        }

        /* This path will be used to store bonding information but will fallback
         * to storing in memory if file access fails (for example due to lack of a filesystem) */
        const char* db_path = "/fs/bt_sec_db";
        /* If the security manager is required this needs to be called before any
         * calls to the Security manager happen. */
        error = ble.securityManager().init(
            true,
            false,
            SecurityManager::IO_CAPS_NONE,
            NULL,
            false,
            db_path
        );

        if (error) {
            printf("Error during init %d\r\n", error);
            return;
        }

        error = ble.securityManager().preserveBondingStateOnReset(true);

        if (error) {
            printf("Error during preserveBondingStateOnReset %d\r\n", error);
        }

#if MBED_CONF_APP_FILESYSTEM_SUPPORT && MBED_CONF_APP_BLE_PRIVACY
        /* Enable privacy so we can find the keys */
        error = ble.gap().enablePrivacy(false);

        if (error) {
            printf("Error enabling privacy\r\n");
        }

        Gap::PeripheralPrivacyConfiguration_t configuration_p = {
            /* use_non_resolvable_random_address */ false,
            Gap::PeripheralPrivacyConfiguration_t::REJECT_NON_RESOLVED_ADDRESS
        };
        ble.gap().setPeripheralPrivacyConfiguration(&configuration_p);
#endif

        /* Tell the security manager to use methods in this class to inform us
         * of any events. Class needs to implement SecurityManagerEventHandler. */
        ble.securityManager().setSecurityManagerEventHandler(this);

        /* when scanning we want to connect to a peer device so we need to
         * attach callbacks that are used by Gap to notify us of events */
        ble.gap().onConnection(this, &SMDevice::on_connect);
        ble.gap().onDisconnection(this, &SMDevice::on_disconnect);

        /** This tells the stack to generate a pairingRequest event
         * which will require this application to respond before pairing
         * can proceed. Setting it to false will automatically accept
         * pairing. */
        ble.securityManager().setPairingRequestAuthorisation(true);
    };

    bool connectionActive()
    {
        return _is_connected && _is_secure;
    }

private:
    /** This is called by Gap to notify the application we connected,
     *  in our case it immediately requests a change in link security */
    void on_connect(const Gap::ConnectionCallbackParams_t *connection_event)
    {
        ble_error_t error;

        /* remember the device that connects to us now so we can connect to it
         * during the next demonstration */
        memcpy(_peer_address, connection_event->peerAddr, sizeof(_peer_address));

        printf("Connected to: %02x:%02x:%02x:%02x:%02x:%02x\r\n",
                _peer_address[5], _peer_address[4], _peer_address[3],
                _peer_address[2], _peer_address[1], _peer_address[0]);

        _is_connected = true;
        _is_secure = false; // not secured yet

        /* store the handle for future Security Manager requests */
        _handle = connection_event->handle;

        /* Request a change in link security. This will be done
         * indirectly by asking the master of the connection to
         * change it. Depending on circumstances different actions
         * may be taken by the master which will trigger events
         * which the applications should deal with. */
        error = ble.securityManager().setLinkSecurity(
            _handle,
            SecurityManager::SECURITY_MODE_ENCRYPTION_NO_MITM
        );

        if (error) {
            printf("Error during SM::setLinkSecurity %d\r\n", error);
            return;
        }
    };

    /** This is called by Gap to notify the application we disconnected,
     *  in our case it ends the demonstration. */
    void on_disconnect(const Gap::DisconnectionCallbackParams_t *event)
    {
        _is_connected = false;
        _is_secure = false;
        printf("Disconnected. Advertising...\r\n");
        BLE::Instance().gap().startAdvertising();
    };

private:
    BLE &ble;
    events::EventQueue &_event_queue;
    BLEProtocol::AddressBytes_t &_peer_address;
    ble::connection_handle_t _handle;
    bool _is_connected;
    bool _is_secure;
};

SMDevice *bleSecurityManager;

static void updateSensorValues(void) {
    static double currentTemperature    = 20;
    static double currentHumidity       = 100100;
    static double currentPressure       = 15;

    /* Read new values from the sensor */
#if MBED_CONF_APP_SENSOR_TYPE == SENSOR_TYPE_BME280
    bme280.get_readings(&currentTemperature, &currentPressure, &currentHumidity);
#elif MBED_CONF_APP_SENSOR_TYPE == SENSOR_TYPE_FAKE
    currentTemperature = (currentTemperature + 0.1 > 30) ? 20 : currentTemperature + 0.1;
    currentPressure = (currentPressure + 0.1 > 100123) ? 100100 : currentPressure + 0.1;
    currentHumidity = (currentHumidity + 0.1 > 25) ? 15 : currentHumidity + 0.1;
#endif

    /* Update the characteristics if connected */
    if (bleSecurityManager->connectionActive()) {
        environmentalServicePtr->updateTemperature((EnvironmentalService::TemperatureType_t)(currentTemperature));
        environmentalServicePtr->updateHumidity((EnvironmentalService::HumidityType_t)(currentHumidity));
        environmentalServicePtr->updatePressure((EnvironmentalService::PressureType_t)(currentPressure));
        printf("Updated sensor values temp %.02f, p %.02f, hum %.02f\r\n", currentTemperature, currentPressure, currentHumidity);
    }
}

static void blinky(void)
{
    static const uint16_t idle_pattern[] = {200, 1800};
    static const uint16_t connected_pattern[] = {100, 900};
    static int i = 1;

    led1 = !led1;

    if (bleSecurityManager->connectionActive()) {
        eventQueue.call_in(connected_pattern[i++%2], blinky);
    } else {
        eventQueue.call_in(idle_pattern[i++%2], blinky);
    }
}

void onBleInitError(BLE &ble, ble_error_t error)
{
   /* Initialization error handling should go here */
}

void printMacAddress()
{
    /* Print out device MAC address to the console*/
    Gap::AddressType_t addr_type;
    Gap::Address_t address;
    BLE::Instance().gap().getAddress(&addr_type, address);
    printf("DEVICE MAC ADDRESS: ");
    for (int i = 5; i >= 1; i--){
        printf("%02x:", address[i]);
    }
    printf("%02x\r\n", address[0]);
}

/** End demonstration unexpectedly. Called if timeout is reached during advertising,
     * scanning or connection initiation */
void bleTimeout(const Gap::TimeoutSource_t source)
{
    printf("Unexpected timeout - aborting\r\n");
    eventQueue.break_dispatch();
};

void bleInitComplete(BLE::InitializationCompleteCallbackContext *params)
{
    BLE&        ble   = params->ble;
    ble_error_t error = params->error;

    if (error != BLE_ERROR_NONE) {
        onBleInitError(ble, error);
        return;
    }

    if (ble.getInstanceID() != BLE::DEFAULT_INSTANCE) {
        return;
    }

    bleSecurityManager->initComplete(params);

    /* Setup primary service. */
    environmentalServicePtr = new EnvironmentalService(ble);

    /* Init sensor values */
    updateSensorValues();

    /* setup advertising */
    ble.gap().accumulateAdvertisingPayload(GapAdvertisingData::BREDR_NOT_SUPPORTED | GapAdvertisingData::LE_GENERAL_DISCOVERABLE);
#if MBED_CONF_APP_BLE_ADVERTISE_ENVIRONMENTAL_SERVICE
    ble.gap().accumulateAdvertisingPayload(GapAdvertisingData::COMPLETE_LIST_16BIT_SERVICE_IDS, (uint8_t *)uuid16_list, sizeof(uuid16_list));
#endif
    ble.gap().accumulateAdvertisingPayload(GapAdvertisingData::COMPLETE_LOCAL_NAME, (uint8_t *)DEVICE_NAME, sizeof(DEVICE_NAME));
    ble.gap().setAdvertisingType(GapAdvertisingParams::ADV_CONNECTABLE_UNDIRECTED);
    ble.gap().setAdvertisingInterval(1000); /* ms */
    ble.gap().startAdvertising();
    printf("BLE init done. Advertising as %.*s\r\n", sizeof(DEVICE_NAME), DEVICE_NAME);

    printMacAddress();
}

void bleScheduleEventsProcessing(BLE::OnEventsToProcessCallbackContext* context) {
    BLE &ble = BLE::Instance();
    eventQueue.call(Callback<void()>(&ble, &BLE::processEvents));
}

#if MBED_CONF_APP_FILESYSTEM_SUPPORT
bool create_filesystem()
{
    static LittleFileSystem fs("fs");

    ///* replace this with any physical block device your board supports (like an SD card) */
    printf("Initializing flash block device\r\n");
    static FlashIAPBlockDevice flash;
    int err = flash.init();
    if (err) {
        return false;
    }
    printf("Initializing the last %d bytes of the flash as a SliceBlockDevice\r\n", MBED_CONF_APP_FLASH_BLOCKDEVICE_SIZE);
    static SlicingBlockDevice bd(&flash, -1 * MBED_CONF_APP_FLASH_BLOCKDEVICE_SIZE);
    err = bd.init();
    if (err) {
        return false;
    }

mount_fs:
    printf("Mounting filesystem\r\n");
    err = fs.mount(&bd);

    if (err) {
        /* Reformat if we can't mount the filesystem */
        printf("No filesystem found, formatting...\r\n");

        err = fs.reformat(&bd);

        if (err) {
            printf("Couldn't create filesystem\r\n");
            goto erase_bd;
        }
    }
    return true;

erase_bd:
    printf("Erase SlicingBlockDevice\r\n");
    err = bd.erase(0, MBED_CONF_APP_FLASH_BLOCKDEVICE_SIZE);
    if (err) {
        printf("Couldn't erase blockdevice\r\n");
        return false;
    }
    goto mount_fs;
}
#endif //MBED_CONF_APP_FILESYSTEM_SUPPORT

void ble_init() {
    eventQueue.call_every(MBED_CONF_APP_SENSOR_VALUE_UPDATE_INTERVAL, updateSensorValues);

    BLE &ble = BLE::Instance();

    bleSecurityManager = new SMDevice(ble, eventQueue, peer_address);
    MBED_ASSERT(bleSecurityManager != NULL);

    ble.onEventsToProcess(bleScheduleEventsProcessing);
    /* handle timeouts, for example when connection attempts fail */
    ble.gap().onTimeout(bleTimeout);

    ble.init(bleInitComplete);

    //bleSecurityManager.run();
}

int main()
{
    printf("Built at: " __DATE__ " " __TIME__ "\r\n");
#if MBED_CONF_APP_SENSOR_TYPE == SENSOR_TYPE_BME280
    int ret = bme280.init();
    if (ret != 0) {
        for(;;) {
            printf("Failed to initialize sensor\r\n");
            led1 = !led1;
            wait_ms(200);
        }
    }
#endif

#if MBED_CONF_APP_FILESYSTEM_SUPPORT
    /* if filesystem creation fails or there is no filesystem the security manager
        * will fallback to storing the security database in memory */
    if (!create_filesystem()) {
        printf("Filesystem creation failed, will use memory storage\r\n");
    }
#endif

    eventQueue.call(blinky);

    ble_init();

    eventQueue.dispatch_forever();

    return 0;
}
