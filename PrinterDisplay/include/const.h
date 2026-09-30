#ifndef const_h
#define const_h

#define OCTOTPRINT_PORT 80            // Port of the OctoPrint server
#define OCTOTPRINT_IP "10.201.93.207" // IP address of the OctoPrint server (change this to your OctoPrint server IP address)

/*Set to your screen resolution and rotation*/
#define TFT_HOR_RES 240
#define TFT_VER_RES 320
#define TFT_ROTATION LV_DISPLAY_ROTATION_90

// Touchscreen pins
#define XPT2046_IRQ 36  // T_IRQ
#define XPT2046_MOSI 32 // T_DIN
#define XPT2046_MISO 39 // T_OUT
#define XPT2046_CLK 25  // T_CLK
#define XPT2046_CS 33   // T_CS

#define API_REFRESH_TIME 3000       // 3 seconds (background poll cycle)
#define DISPLAY_REFRESH_TIME 250    // 0.25 seconds (UI redraw from snapshot)
#define WIFI_SIGNAL_REFRESH_TIME 30000 // 30 seconds (WiFi signal redraw)

#define LABEL1_FONT &FreeSerif8pt7b      // Key label font 1
#define LABEL2_FONT &FreeSerifBold12pt7b // Key label font 2

#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240
#define FONT_SIZE 2
#define BACKGROUND_COL TFT_BLACK
#define BACKGROUND 0x18E3
#define FONT_COL TFT_WHITE

#define SERIAL_DEBUG 1

#define CHAMBER_SETPOINT_MIN 20
#define CHAMBER_SETPOINT_MAX 60
#define CHAMBER_SETPOINT_STEP 1
#define CHAMBER_SETPOINT_DEFAULT 30

#define LOUVRE_MIN 0
#define LOUVRE_MAX 100
#define LOUVRE_STEP 10
#define LOUVRE_DEFAULT 0
#define FAN_MIN 0
#define FAN_MAX 100
#define FAN_STEP 10
#define FAN_DEFAULT 0
#define FAN_LOUVRE_MIN_OPEN 10

// Material presets for the chamber Material tab (AUTO setpoint profiles).
// ASA/ABS target is 50 everywhere (matches C3 TEMP_ASA_CHAMBER_TARGET).
#define MATERIAL_COUNT 6
#define MATERIAL_DEFAULT_INDEX 0 // PLA
#define MATERIAL_PLA_SETPOINT 30
#define MATERIAL_PETG_SETPOINT 30
#define MATERIAL_ASA_SETPOINT 50
#define MATERIAL_ABS_SETPOINT 50
#define MATERIAL_TPU_SETPOINT 35
#define MATERIAL_NYLON_SETPOINT 50

// Telemetry older than this counts as stale (greyed out / "--").
#define ENC_LINK_STALE_MS 5000

// Force the Wi-Fi channel after joining. 0 = follow the access point.
//
// ESP-NOW only works between peers on the same channel, and "the same SSID" does
// not guarantee that. An AP with roaming, band steering, per-client channel
// assignment, or a second radio broadcasting the same SSID will put two boards
// onto different channels, and the link then fails with every send erroring and no
// frame ever arriving.
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
// Set this to that channel in BOTH projects once the APs agree. A pin is best
// effort: the AP re-synchronises the station on every beacon, so one it will not
// honour costs the association, and it is abandoned after 20s rather than fought
// forever - see EnclosureChannelPin.
#define ENC_WIFI_CHANNEL 0

// Firmware update paths (lib/HttpOta). The board serves HTTP on
// OTA_HTTP_PORT; the uploader makes an outbound request, which works behind a
// default Windows Firewall that would block ArduinoOTA's inbound callback.
#define OTA_HTTP_PORT 8080
#define OTA_HTTP_USER "enclosure"
#define OTA_ARDUINO_HOSTNAME "enclosure-display"

// ESP-NOW peer (EnclosureFanControl C3 WiFi MAC). All zeros = receive-only
// until set. Both firmwares log their own MAC at boot - copy them over.
static const uint8_t ENC_PEER_MAC[6] = {0xAC, 0xEB, 0xE6, 0x4B, 0x46, 0x1C};
enum ButtonFunction
{
    PREHEAT_215 = 215,
    PREHEAT_230 = 230,
    PREHEAT_OFF = 0,
    PAUSE_PRINT_BUTTON = -1,
    RESUME_PRINT_BUTTON = -2,
    STOP_PRINT_BUTTON = -3,
};

#endif