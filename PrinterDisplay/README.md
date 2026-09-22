# EnclosureDisplay

## Overview

This project involves using an ESP32 microcontroller to monitor and display the status of a 3D printer connected to an OctoPrint server. The ESP32 fetches data such as temperature, print progress, and print time from the OctoPrint server over a local network and displays it on a CYD display.

## Features

- Real-time monitoring of 3D printer status
- Displays printer temperature (hotend and bed)
- Shows print progress percentage
- Displays estimated print time remaining
- Connects to OctoPrint server via WiFi

## Hardware Requirements

- ESP32 development board
- CYD (Cheap Yellow Display) compatible display
- USB cable for power and programming
- 3D printer connected to OctoPrint

## Software Requirements

- Arduino IDE
- Required libraries (see Installation)
- OctoPrint server running on your network

## Installation

1. Clone this repository
2. Open the project in Arduino IDE
3. Install the required libraries
4. Configure your WiFi and OctoPrint API settings in the config file
5. Upload to your ESP32

## Configuration

After cloning the repository, you'll need to configure both your WiFi credentials and OctoPrint connection settings.

### WiFi and API Settings

Copy the template and fill in your own values (never commit the real file):

```text
copy include\secrets.h.example include\secrets.h
```

Edit the `secrets.h` file to set:

```cpp
String secrets::getWiFiSSID()
{
    return "WIFI SSID";
}

String secrets::getWiFiPassword()
{
    return "WIFI PASSWORD";
}

String secrets::getOctoprintApiKey()
{
    return "OCTOPRINT API";
}

String secrets::getOTAPassword()
{
    return "OTA PASSWORD";
}
```

`include/secrets.h` is git-ignored. Only `include/secrets.h.example` is tracked.

### OctoPrint Server Settings

Configure the OctoPrint server IP address and port in `const.h`:

```cpp
#define OCTOTPRINT_PORT 80            // Port of the OctoPrint server
#define OCTOTPRINT_IP "192.168.1.xxx" // IP address of the OctoPrint server (change this to your OctoPrint server IP address)

```

> WARNING: Rotate your WiFi password and OctoPrint API key if they were ever committed.

## Usage

Once configured and powered on, the ESP32 will automatically connect to your WiFi network and begin polling the OctoPrint server for status updates, displaying the information on the CYD screen.

## OTA Updates

The first upload must be made over USB so the ESP32 can install the OTA-enabled firmware:

```text
platformio run --target upload --environment esp32dev --upload-port COMx
```

After the board connects to WiFi, upload later builds over OTA using its mDNS hostname (OTA is password-protected via `getOTAPassword()` in `secrets.h`):

```text
platformio run --target upload --environment esp32dev --upload-protocol espota --upload-port enclosure-display.local --upload-flags --auth=YOUR_OTA_PASSWORD
```

If mDNS is unavailable, replace `enclosure-display.local` with the ESP32 IP address shown in the serial monitor. The `esphome run` command is not applicable to this PlatformIO project.
