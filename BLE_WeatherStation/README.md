# BLE WeatherStation - example application implementing Environmental Sensing Service with Security Manager

Demonstration of Environmental Sensing Service (ESS) with Security Manager (SM).
SM deals with pairing, authentication and encryption, and ESS provides sensor data.

The application is used as a peripheral. Upon starting the application will start advertising. Once connected to it will
request pairing. Once paired it will update ESS temperature, relative humidity and atmospheric pressure characteristics.
If a secure connection can't be established the application will disconnect and resume advertising.


# Running the application

## Requirements

The sample application can be seen on any BLE scanner on a smartphone. If you don't have a scanner on your phone, please install :

- [nRF Master Control Panel](https://play.google.com/store/apps/details?id=no.nordicsemi.android.mcp) for Android.

- [LightBlue](https://itunes.apple.com/gb/app/lightblue-bluetooth-low-energy/id557428110?mt=8) for iPhone.

Information about activity is printed over the serial connection - please have a client open. You may use:

- [Tera Term](https://ttssh2.osdn.jp/index.html.en)

Hardware requirements are in the [main readme](https://github.com/ARMmbed/mbed-os-example-ble/blob/master/README.md).

## Building instructions

Building instructions for all samples are in the [main readme](https://github.com/ARMmbed/mbed-os-example-ble/blob/master/README.md).

Note: this example currently is currently not supported on ST BLUENRG targets.

## Configuration options

The application's configuration options are defined in the file `mbed_lib.json`. The following options are defined:

|Name|Description|Options|Default|
|---|---|---|---|
|`sensor-type`|Sensor type|SENSOR_TYPE_BME280, SENSOR_TYPE_FAKE|`"SENSOR_TYPE_BME280"`|
|`sensor-value-update-interval`|Interval between sensorvalue updates|integer milliseconds|`60000`|
|`filesystem-support`|Enable filesystem for static storage of pairing information|true or false|`true`|
|`flash-blockdevice-size`|Filesystem flash block device size|integer bytes|`(32 * 1024)`|
|`ble-device-name`|BLE device name|string|`"\"BLE-WeatherStation\""`|
|`ble-privacy`|Use Private Resolvable address if filesystem support is enabled|true or false|`false`|
|`ble-advertise-environmental-service`|Include ESS in advertisement|true or false|`true`|

These options can be overwritten in the file `mbed_app.json`. For example to set the BLE device name add `app.ble-device-name": "\"MY-BLE-DEVICE\""` for your target under `"target overrides"`.