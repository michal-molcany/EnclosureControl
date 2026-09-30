#ifndef ENCLOSURE_CONST_H
#define ENCLOSURE_CONST_H

#define SERVO_PIN 0
#define SERVO_PWM_FREQ 50
#define SERVO_MIN 500
#define SERVO_MAX 2400
#define SERVO_OFF_ANGLE 0
#define SERVO_ON_ANGLE 130
// Minimum time between physical servo moves (replaces blocking delay).
#define SERVO_MIN_MOVE_INTERVAL_MS 500

// Fan Control (PWM)
// IMPORTANT: LEDC channel/timer reservation (Arduino maps chan->timer fixed):
//   servo (ESP32Servo) -> channel 0, timer 0 @ 50 Hz
//   fan               -> channel 4, timer 2 @ 5 kHz
// Sharing a channel retunes the shared timer and kills the other output.
#define FAN_PIN 1
#define FAN_PWM_FREQ 5000 // 5kHz PWM frequency
#define FAN_PWM_RESOLUTION 8
#define FAN_PWM_CHANNEL 4

#define TEMP_STANDARD_TARGET 30
#define TEMP_TPU_TARGET 35
#define TEMP_HIGH_TEMP_TARGET 50
#define TEMP_FAN_COOLDOWN_END 30
#define TEMP_PASSIVE_COOLDOWN_START 37
#define TEMP_PASSIVE_COOLDOWN_END 30
#define TEMP_EMERGENCY_TRIGGER 55
#define TEMP_EMERGENCY_CLEAR 50
#define REMOTE_SETPOINT_MIN 20
#define REMOTE_SETPOINT_MAX 60

// ESP-NOW peer (PrinterDisplay CYD WiFi MAC). All zeros = receive-only
// until set. Both firmwares log their own MAC at boot - copy them over.
static const uint8_t ENC_PEER_MAC[6] = {0x5C, 0x01, 0x3B, 0x8A, 0x63, 0x54};

// PID Parameters for Chamber Temperature Control (cooling: error = current - setpoint)
#define PID_KP 1.5
#define PID_KI 0.1
#define PID_KD 0.5
#define PID_SETPOINT TEMP_STANDARD_TARGET
#define PID_UPDATE_INTERVAL 1000 // 1 second

// OctoPrint background poll interval (FreeRTOS task, CYD-style). Loop only
// copies the latest snapshot and never blocks on the network.
#define OCTO_POLL_INTERVAL_MS 5000 // 5 seconds

// Force the Wi-Fi channel after joining. 0 = follow the access point.
//
// ESP-NOW only works between peers on the same channel, and "the same SSID" does
// not guarantee that. An AP with roaming, band steering, per-client channel
// assignment, or a second radio broadcasting the same SSID will put two boards
// onto different channels, and the link then fails with every send erroring and no
// frame ever arriving. On this board that is especially easy to hit, because the
// SSID is chosen through WiFiManager and persisted in NVS rather than compiled in,
// so it can drift away from whatever the CYD is hard-coded to join.
//
// MEASURED ON THIS HARDWARE, both directions dead, 0 of 760 sends acknowledged:
//   C3   bssid C6:6E:2A:01:38:C0  we=ch11  10.201.93.245
//   CYD  bssid C6:6E:2A:01:35:D0  we=ch1   10.201.93.29
// The peer MACs were verified correct, so the channel is the only fault. But note
// the BSSIDs differ: the two boards are on two different AP radios broadcasting the
// same SSID, which is a mesh or extender. A channel pin CANNOT fix that - forcing
// this station off the AP it is associated with just gets it deauthenticated, and
// it re-associates to the same radio it was trying to escape. So the pin stays 0
// (follow the AP) and the fix is network-side: every AP broadcasting this SSID must
// use one 2.4GHz channel, or the two boards must be moved onto a single radio.
//
// Set this to that channel in BOTH projects once the APs agree - the code is ready
// and EnclosureChannelPin will hold it against the per-beacon resync. A pin is best
// effort: the AP re-synchronises the station on every beacon, so one it will not
// honour costs the association, and it is abandoned after 20s rather than fought
// forever so a board reachable only over the air cannot be stranded.
#define ENC_WIFI_CHANNEL 0

// Firmware update paths (lib/HttpOta). The board serves HTTP on
// OTA_HTTP_PORT; the uploader makes an outbound request, which works behind a
// default Windows Firewall. Deliberately not 80: WiFiManager's captive portal
// uses that port.
#define OTA_HTTP_PORT 8080
#define OTA_HTTP_USER "enclosure"
#define OTA_ARDUINO_HOSTNAME "enclosure-fan-control"

// How long a failed ArduinoOTA keeps the OLED before the chamber view returns.
// Only for that path: lib/HttpOta tracks its own hold internally.
#define OTA_ERROR_HOLD_MS 10000

#endif // ENCLOSURE_CONST_H

