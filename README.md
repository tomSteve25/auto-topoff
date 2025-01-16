# Aquarium water automatic top-off system

This is a simple, ESP32 based, system for automatically topping off aquarium water. It uses a HC-SR04 ultrasonic distance sensor mounted above the tank facing the water to measure the water level. When the level is too low, i.e. the measured distance exceeds a set threshold, a MOSFET is used to turn on a pump.

## Software design
The system connects to a WIFI network and hosts a webpage. This allows the user to configure the water trigger level, and the time that the system should check if a top up needs to occur. The current water level, system time and pump status are displayed and the information is refreshed every 10s.

The topup procedure turns on the pump and continously monitors the water level until it is below the trigger level again. There is a safety mechanism where the pump will turn off after 15s to prevent a sensor issue causing an overflow.

### Hardware Required

* A WIFI enabled ESP32. I used an [ESP32-C6-Zero](https://www.waveshare.com/wiki/ESP32-C6-Zero) from Waveshare,
* A USB cable for power supply and programming,
* A HC-SR04 ultrasonic distance sensor,
* A pump.

Ultimately this project is an ESP32 which reads an ultrasonic sensor and turns on a pump. There are multiple ways to do this, but my way is described in this [README](hardware/README.md).

### Configure the project

```
idf.py menuconfig
```

The `CONFIG_EXAMPLE_WIFI_SSID` and `CONFIG_EXAMPLE_WIFI_PASSWORD` fields need to be set for the device to connect to WIFI.
Ensure that the watchdog timer is not enabled by not setting `ESP_TASK_WDT_EN`.
### Build and Flash

Build the project and flash it to the board, then run monitor tool to view serial output:

```
idf.py -p PORT flash monitor
```

(Replace PORT with the name of the serial port to use.)

(To exit the serial monitor, type ``Ctrl-]``.)

See the Getting Started Guide for full steps to configure and use ESP-IDF to build projects.

The system will output its IP address. It is recommended to give the device a static IP in your router settings so that you do not need to monitor the serial logs for the address on each boot. Navigate to this address in your browser and configure the trigger level and topup times.

## Potential improvements
### Customisability
There are a couple things that could be added to make the system more customisable. The system assumes a HC-SR04 distance sensor mounted above the tank, facing the water, connected to certain GPIO. The GPIO could be made configurable through the web interface. The nature of the distance sensor meant that the system would trigger when the measured distance was greater than the trigger distance. For another type of sensor, a user might want the trigger to happen if the measured distance was less than the trigger distance. In addition, the user may want an entirely different sensor, requiring different control logic. Ultimately, these features were not added because it had specific design goals in mind and if you were looking for something more general  it is probably better to use ESP Home or to just implement it yourself.
### Power
It was assumed that the system was wall powered and as such no effort was made for the ESP to enter any sleep states. In addition, the intention was for the device to always be connected to WIFI, which is not possible in any sleep state.

A potential improvement could be that a hardware switch is added which enables the webpage. When the switch is not active, the wireless peripherals can be disabled and the high power processor can be put into deep sleep. The entire program could then be run on the LP processor since it has access to the GPIO and RTC timer peripherals. This wasn't needed for my use case so it was not implemented.

### Code
The system uses the [Protocol Examples Common](https://github.com/espressif/esp-idf/tree/master/examples/common_components/protocol_examples_common) component provided by Espressif. This could be cut down to only require the necessary functionlity for WIFI, but it didn't really matter.

The webpage files are hardcoded into the C code. It would be better if the files were stored in flash and read in during runtime, but I didn't have time to investigate how to do that. The current solution works fine, its just annoying when those files need to change.