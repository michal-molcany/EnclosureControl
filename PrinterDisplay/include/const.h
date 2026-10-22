#ifndef const_h
#define const_h

// OctoPrint server address. SHARED with the C3 (EnclosureFanControl), so it
// lives in one git-ignored header at the repo root rather than being duplicated
// here - the two boards used to be configured separately and could silently
// drift apart.
#if defined(__has_include)
#  if !__has_include("octoprint_endpoint.h")
#    error "Missing ../include/octoprint_endpoint.h - copy include/octoprint_endpoint.example.h to include/octoprint_endpoint.h (at the repo root) and set your OctoPrint address."
#  endif
#endif
#include "octoprint_endpoint.h"

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

// Bed target presets for the bed view, one per entry in MATERIAL_PRESETS so the
// two views agree on what a material wants. Bed temps differ from chamber
// setpoints above.
#define BED_PLA_TEMP 60
#define BED_PETG_TEMP 70
#define BED_ASA_TEMP 100
#define BED_ABS_TEMP 100
#define BED_TPU_TEMP 30
#define BED_NYLON_TEMP 70

// Stepper bounds on the bed view. MAX is a per-printer cap: lower it if your
// bed heater cannot reach 120C (many cannot).
#define BED_TARGET_MIN 0
#define BED_TARGET_MAX 120
#define BED_TARGET_STEP 5
#define BED_TARGET_DEFAULT BED_PLA_TEMP

// Nozzle target presets for the nozzle view, one per entry in MATERIAL_PRESETS so
// the two views agree on what a material wants.
#define TOOL_PLA_TEMP 215
#define TOOL_PETG_TEMP 235
#define TOOL_ASA_TEMP 260
#define TOOL_ABS_TEMP 250
#define TOOL_TPU_TEMP 230
#define TOOL_NYLON_TEMP 270

// Stepper bounds on the nozzle view. MAX is a per-printer cap: lower it if your
// hotend cannot reach it (many stock hotends max out at 250-260, the V6 at 285).
#define TOOL_TARGET_MIN 0
#define TOOL_TARGET_MAX 285
#define TOOL_TARGET_STEP 5
#define TOOL_TARGET_DEFAULT TOOL_PLA_TEMP

// How long a preheat view keeps showing the value it just sent, instead of
// adopting the polled target. OctoPrint polls every API_REFRESH_TIME, so a
// snapshot taken before the press would otherwise drag the label backwards.
#define PREHEAT_TARGET_SETTLE_MS 6000

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

// Modal calibration screen timing. The result stays up long enough to see what
// happened, then returns to the main view rather than trapping the display;
// the user can also close it immediately.
#define CAL_RESULT_HOLD_MS 15000UL
// A C3 link that disappears mid-run cannot be cancelled from the display. The
// modal holds the screen while the link is only briefly late, then releases it
// so a powered-off C3 cannot leave the touchscreen permanently unusable. Fresh
// telemetry reporting an active run forces the modal straight back on.
#define CAL_LINK_LOST_UNLOCK_MS 20000UL

// HTTP API fallback for the enclosure link (lib/EnclosureApi). ESP-NOW is primary
// and stays that way; this only engages once telemetry has been stale for
// ENC_LINK_STALE_MS, which is exactly the channel/AP split that kills ESP-NOW
// (see ENC_WIFI_CHANNEL below). An HTTP request rides the IP network, which both
// boards share regardless of channel or BSSID, so it bridges the split that no
// firmware channel pin can. The C3 SERVES this; the CYD only queries it.
//
// ENC_API_PORT and ENC_API_TOKEN are shared with the C3 and live in the
// git-ignored ../../include/enclosure_api.h, so the two cannot drift apart.
#if defined(__has_include)
#  if !__has_include("enclosure_api.h")
#    error "Missing ../include/enclosure_api.h - copy include/enclosure_api.example.h to include/enclosure_api.h (at the repo root) and set the API port + token."
#  endif
#endif
#include "enclosure_api.h"

// ESP-NOW link encryption key. SHARED with the C3, for the same reason the
// header above is: two copies of a secret can disagree, and when they do the
// symptom is a dead radio link with no error anywhere.
//
// Enforced here because the failure mode is otherwise very expensive to
// diagnose: each board encrypts to the key it holds and the other cannot read
// it, so a mismatch is indistinguishable from a channel split or a missing peer.
#if defined(__has_include)
#  if !__has_include("espnow_key.h")
#    error "Missing ../include/espnow_key.h - copy include/espnow_key.example.h to include/espnow_key.h (at the repo root). Both boards must use the SAME 16-byte key or the ESP-NOW link will not come up."
#  endif
#endif
#include "espnow_key.h"

#define ENC_API_MDNS_NAME "enclosure-fan-control"
#define ENC_API_STATIC_IP "0.0.0.0" // dotted-quad fallback; 0.0.0.0 = disabled
#define ENC_API_POLL_MS 1000        // telemetry cadence while the fallback runs
#define ENC_API_RESOLVE_MS 30000// how often to re-resolve a working C3
#define ENC_API_UNREACHABLE_RETRY_MS 30000 // re-resolve cadence once a known
//                                           address starts refusing. Deliberately
//                                           the same order as ENC_API_RESOLVE_MS:
//                                           a cached address that is not answering
//                                           is retried on a slow loop, not per
//                                           tick, so an unreachable C3 cannot
//                                           turn the fallback into an mDNS busy
//                                           loop while ESP-NOW is down.

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

// Job control codes, carried as PollerCommand::value when target == Job.
enum JobCommand
{
    PAUSE_JOB = 1,
    STOP_JOB,
    RESUME_JOB,
    // File list: REFRESH_FILES re-fetches the newest files into the poller
    // cache; PRINT_FILE starts the staged path; FILE_INFO fetches one file's
    // details for the info panel (see pollerRequestFilePrint/FileInfo).
    REFRESH_FILES = 4,
    PRINT_FILE = 5,
    FILE_INFO = 6
};

// How long a fetched file list stays usable. Opening the view inside this window
// shows the cache immediately; once it expires the view refreshes in place every
// five minutes while it is open. There is no background polling: the list is
// large enough that doing it while the user is elsewhere would just spend Wi-Fi
// and OctoPrint time for a screen nobody is looking at.
#define FILES_CACHE_TTL_MS (5UL * 60UL * 1000UL)
// After a failed fetch, do not hammer the server when the user keeps reopening
// the view. A later attempt is allowed once this has elapsed.
#define FILES_RETRY_MS 30000

// Newest-first printable files shown by the files view (opened from the idle
// no-active-print panel).
//
// Two different numbers, deliberately not one:
//
//   PRINT_FILES_VISIBLE  rows that FIT on screen. Emergent from the geometry,
//                        not a cap: a 146 px list at a 48 px pitch is exactly
//                        three rows. The list is scrollable, so this is only what
//                        you see before scrolling.
//   MAX_PRINT_FILES      rows FETCHED, STORED and given a label - the number
//                        reachable by scrolling. Ten, not four, because the point
//                        of the view is choosing something to print: having the
//                        5th newest file one scroll away beats not having it.
//
// The count is NOT what makes opening this view expensive - the full recursive
// listing from OctoPrint is ~484 KB on the live server whatever these say, and
// that body still has to cross Wi-Fi even though the client now keeps only the
// lightweight fields. The parsed document is no longer the bottleneck (see the
// filter in OctoPrinter::fetchFiles); the cache and the longer file deadline are
// what keep the wait off the common path.
//
// Both are coupled to the row geometry: three 46 px rows at a 48 px pitch is
// what fills the list on a 320x240 panel while leaving room for the status line
// and the print button.
// Timestamps (upload date, print times) are rendered in Europe/Bratislava local
// time, computed from the timestamp itself - see formatUploadDate() in display.cpp.
//
// Done arithmetically rather than with the C library on purpose: this board has no
// clock. Nothing calls configTime(), there is no TZ and no NTP sync anywhere in the
// firmware, so mktime()/localtime() would decide whether daylight saving is in
// effect by comparing against a system clock stuck in 1970 - and would then be
// wrong for every summer timestamp. Applying the EU rule straight to the value
// being formatted needs neither a clock nor a network round trip.
//
// Slovakia follows the EU rule exactly: CET (UTC+1) in winter, CEST (UTC+2) between
// the last Sunday in March 01:00 UTC and the last Sunday in October 01:00 UTC.
#define PRINT_FILES_VISIBLE 3
#define MAX_PRINT_FILES 10 
#define PRINT_FILE_PATH_LEN 120
#define PRINT_FILE_NAME_LEN 64

#endif
