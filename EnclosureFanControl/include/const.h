#ifndef ENCLOSURE_CONST_H
#define ENCLOSURE_CONST_H

// OctoPrint server address. SHARED with the CYD (PrinterDisplay), so it lives in
// one git-ignored header at the repo root rather than being duplicated here -
// the two boards used to be configured separately and could silently drift
// apart.
#if defined(__has_include)
#  if !__has_include("octoprint_endpoint.h")
#    error "Missing ../include/octoprint_endpoint.h - copy include/octoprint_endpoint.example.h to include/octoprint_endpoint.h (at the repo root) and set your OctoPrint address."
#  endif
#endif
#include "octoprint_endpoint.h"

#define SERVO_PIN 0
#define SERVO_PWM_FREQ 50
#define SERVO_MIN 500
#define SERVO_MAX 2400
#define SERVO_OFF_ANGLE 0
#define SERVO_ON_ANGLE 120
// Minimum time between physical servo moves (replaces blocking delay).
#define SERVO_MIN_MOVE_INTERVAL_MS 500

// Fan Control (PWM)
// IMPORTANT: LEDC channel/timer reservation (Arduino maps chan->timer fixed):
//   servo (ESP32Servo) -> channel 0, timer 0 @ 50 Hz
//   fan               -> channel 4, timer 2 @ 25 kHz
// Sharing a channel retunes the shared timer and kills the other output.
#define FAN_PIN 1
// 25 kHz, not the previous 5 kHz. 5 kHz is squarely inside human hearing (20Hz -
// 20kHz) and was the source of a tonal whine on this fan. The driver picks the
// nearest integer divider from the 80MHz APB clock, so this lands at ~24.0kHz
// (80e6 / (256 * 13)), which is above the audible band.
#define FAN_PWM_FREQ 25000
#define FAN_PWM_RESOLUTION 8
#define FAN_PWM_CHANNEL 4

// Minimum duty for any nonzero fan request.
//
// A 2/3-wire fan on a low-side switch cannot break away from rest below roughly
// half duty, and between half duty and full speed it spins unevenly so blade-pass
// becomes tonal. Both were observed on this hardware: nothing spun below ~50%
// and it resonated below ~60%. Commanding a percentage that the rotor cannot
// actually achieve is worse than useless - it looks like control while the fan
// sits dead or screams.
//
// The floor costs nothing in usable control resolution, because the louvre is
// already commanded at 1% steps across 0-100 and saturates before the fan is
// even engaged (the fan only runs when PID demand exceeds 100).
#define FAN_MIN_RUN_PCT 60

// Break-away boost. Static friction is highest at rest and worse when the fan is
// cold, so a step straight from 0 to FAN_MIN_RUN_PCT can still fail to start.
// Full power briefly, then settle to the target.
#define FAN_START_BOOST_PCT 100
#define FAN_START_BOOST_MS 300

// Anti-chatter. update() is gated to 1Hz, so a PID demand hovering at the
// engage threshold can toggle the fan every second - and each start re-triggers
// the boost above, which would be far more audible than the original fault. Once
// running, the fan is held on at least this long.
//
// Fail-safe shutdowns (invalid OctoPrint data, leaving the active-auto state)
// bypass this via forceFanOff(), so a safety action is never delayed.
#define FAN_MIN_ON_MS 3000

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

// PID parameters for chamber temperature control
// (cooling: error = current - setpoint).
//
// The gains themselves live in lib/EnclosureProto/src/enclosure_pid.h and are
// tunable at runtime over the enclosure API; the values below are only the
// compiled-in defaults that seed a blank NVS.
//
// There is deliberately no PID_SETPOINT here. One used to sit here next to the
// gains and did nothing: applyActiveSetpoint() overwrote the setpoint on every
// regulation cycle with the material target or the CYD's remote setpoint, so the
// constant was live for a few milliseconds after boot and never again. The
// setpoint is chosen per print (materialTarget / remoteSetpoint) and lives
// nowhere it can be edited by accident.
#define PID_UPDATE_INTERVAL 1000 // 1 second

// ---------------------------------------------------------------------------
// PID calibration (web UI "Calibrate" button)
//
// The experiment is a relay-feedback auto-tune: the bed is heated to 90 C to
// turn the enclosure into a repeatable heat load, then the louvre/fan are
// switched between full cooling (200) and no cooling (0) around a 35 C chamber
// setpoint. The resulting oscillation gives the ultimate gain and period, from
// which Ziegler-Nichols produces the gains.
//
// 35 C is deliberately BELOW the 50 C abort and the 55 C emergency trigger: the
// no-cooling half of the relay lets the chamber rise, so the setpoint has to
// leave headroom for that overshoot. Do not raise it without re-checking those
// two limits.
// ---------------------------------------------------------------------------
#define CAL_BED_TARGET_C 90
// How close the bed has to get before the relay is allowed to start. The
// sensor is a 1 Hz sample and the bed overshoots slightly; 2 C is inside that.
#define CAL_BED_TOLERANCE_C 2.0f
// A cold enclosure takes several minutes to reach a 90 C bed. Past this the
// run is aborted rather than left heating indefinitely.
#define CAL_PREHEAT_TIMEOUT_MS (5UL * 60UL * 1000UL)
// Experiment window. It starts when the chamber first reaches CAL_SETPOINT_C,
// not at the button press: the preheat and the initial climb to 35 C must not
// eat into the oscillation time. Ziegler-Nichols needs a handful of periods; if
// that has not happened by now the process is not oscillating the way the
// method assumes.
#define CAL_MAX_DURATION_MS (60UL * 60UL * 1000UL)
// Absolute backstop for a chamber that never reaches the setpoint, so the
// window above never starts. Covers the 5-minute preheat and leaves 25 minutes
// for the chamber to reach the setpoint. It no longer applies once the setpoint
// has been reached: the full experiment window is then available.
#define CAL_ABSOLUTE_TIMEOUT_MS (30UL * 60UL * 1000UL)
// New OctoPrint data arrives every OCTO_POLL_INTERVAL_MS. If the timestamp
// stops moving for this long the link is gone (or OctoPrint is stuck), and
// continuing to drive the outputs on stale readings would be guesswork.
#define CAL_DATA_STALE_MS 30000UL
// Short confirmation that the bed is holding target before the relay starts.
#define CAL_SETTLE_MS 5000UL
// During preheat the chamber is only heated by the bed; if it gets this warm
// the louvre/fan are opened to keep it away from the abort limit.
#define CAL_PREHEAT_COOL_C 45.0f
// Abort before the 55 C emergency trigger. The emergency path itself stays
// armed and wins regardless.
#define CAL_ABORT_CHAMBER_C 50.0f
#define CAL_SETPOINT_C 35.0f
// Relay hysteresis in C. Above the chamber sensor's noise, below the
// oscillation amplitude the relay is meant to produce.
#define CAL_NOISEBAND_C 0.5f
// The two relay levels, in the regulator's 0-200 span: 0 = louvre closed and fan
// off, 200 = louvre 100% and fan 100%.
#define CAL_RELAY_LOW 0.0f
#define CAL_RELAY_HIGH 200.0f
// How long the board keeps treating a "paused" printer as paused BY US after
// the M84 that was supposed to end the pause.
//
// Long enough for the command to reach the printer and for the printer to
// come out of the pause; short enough that a printer which ignores M84 does
// not stay mis-flagged for ever, which would hide a real pause later.
#define CAL_PAUSE_GRACE_MS 30000UL

// OctoPrint background poll interval (FreeRTOS task, CYD-style). Loop only
// copies the latest snapshot and never blocks on the network.
#define OCTO_POLL_INTERVAL_MS 5000 // 5 seconds

// Longest gap between two PID samples the controller will integrate across,
// in seconds. A gap longer than this means the readings behind it were stale
// rather than a real time step - the poll task stalled, or the board was busy -
// and integrating a stale error across it would wind the integral on
// information that was never true. Skipping leaves the regulator holding its
// last demand, which is the honest response.
//
// Sized at 3x OCTO_POLL_INTERVAL_MS: tolerant of the two or three missed polls
// a congested Wi-Fi link produces routinely, but bounded, so a long stall cannot
// dump a minute of imaginary error into the integrator in one step.
#define PID_MAX_TIMESTEP_S 15.0f

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

// Enclosure API fallback (lib/EnclosureApi). This board SERVES it; the CYD
// queries it only when ESP-NOW is dead. ESP-NOW frames never leave the radio, so
// a channel/AP split (band steering, a mesh node) takes the link down completely;
// an HTTP request rides the IP network instead.
//
// The port and shared token are SHARED with the CYD and live in the git-ignored
// ../../include/enclosure_api.h, so the two firmwares cannot drift apart. The
// port is deliberately distinct from the OTA server (OTA_HTTP_PORT) so a firmware
// upload and the API never share a socket.
#if defined(__has_include)
#  if !__has_include("enclosure_api.h")
#    error "Missing ../include/enclosure_api.h - copy include/enclosure_api.example.h to include/enclosure_api.h (at the repo root) and set the API port + token."
#  endif
#endif
#include "enclosure_api.h"

// ESP-NOW link encryption key. SHARED with the CYD, for the same reason the
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

// Web UI (lib/EnclosureUi): status, PID tuning, manual control from a browser.
//
// Its OWN port, deliberately, not a path on the enclosure API's server. WebServer
// is synchronous - one client at a time, served inline from poll() - and the
// enclosure API is the CYD's fallback transport, which it queries on a deadline.
// A browser page sharing that server would sit in front of the CYD for the length
// of a page load plus every poll interval. A second server cannot do that.
//
// Also distinct from the OTA server (OTA_HTTP_PORT), so opening the page on a
// phone cannot interfere with a firmware upload, and from WiFiManager's captive
// portal on 80.
//
// The port is NOT in the shared ../../include/enclosure_api.h: only this board
// serves it, and the CYD never has to agree about anything.
#define ENC_UI_PORT 8082

#endif // ENCLOSURE_CONST_H

