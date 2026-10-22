#ifndef ENCLOSURE_API_SERVER_H
#define ENCLOSURE_API_SERVER_H

#include <Arduino.h>
#include <enclosure_proto.h>
#include <enclosure_pid.h>

// HTTP JSON API served by the EnclosureFanControl C3, so the PrinterDisplay CYD
// can exchange enclosure telemetry and commands when ESP-NOW is unavailable.
//
// Why this exists
// ---------------
// ESP-NOW frames never leave the radio, so the two boards only hear each other
// when they share a Wi-Fi channel AND the same AP radio (BSSID). Band steering,
// a split 2.4/5 GHz SSID, or a mesh/extender node will associate the two boards
// to different channels, and the link then fails completely - every send errors
// and nothing arrives. An HTTP request rides the IP network instead, which both
// boards share regardless of channel or BSSID. That makes this the firmware-side
// fallback documented in ESPNOW_PROTOCOL.md.
//
// This is a *fallback*: the CYD keeps using ESP-NOW as its primary transport and
// only queries this API once its telemetry has gone stale.
//
// Every request must carry the shared token as HTTP Basic auth (username
// ignored, password = ENC_API_TOKEN). Without it this endpoint would let any
// device on the LAN drive the fan and louvre, which is a step down from
// lib/HttpOta and ArduinoOTA, both of which authenticate. An empty token
// disables the check, matching the rule lib/HttpOta already uses.
//
// The programme supplies two callbacks so the server stays free of any
// dependency on TemperatureRegulation (which lives in the C3 project, not here):
//   - a telemetry provider, called at request time to build the JSON snapshot;
//   - a command handler, which receives a decoded EnclosureCommand.
// applyRemoteCommand() on the C3 already refreshes remoteLinkAlive(), so routing
// commands through this API keeps the existing "linked" footer, OLED state, and
// command-link loss fallback working identically across both transports.
//
// The server owns its own WebServer on its own port, deliberately distinct from
// the OTA server in lib/HttpOta, so a firmware upload and the enclosure API
// never contend for one socket.

namespace EnclosureApi
{
    // Telemetry values read at request time. chamberCx10 is the chamber
    // temperature in 0.1 C units, using ENC_CHAMBER_INVALID (INT16_MIN) when
    // unknown - the same encoding as EnclosureTelemetry.
    struct Telemetry
    {
        uint8_t louvrePct = 0;
        uint8_t fanPct = 0;
        int16_t chamberCx10 = ENC_CHAMBER_INVALID;
        uint8_t state = ENC_STATE_IDLE;
        uint8_t mode = ENC_MODE_AUTO;
        // Mirrors EnclosureTelemetry::seq so the CYD's loss detection works over
        // this transport too. It is a per-request counter here, not a copy of the
        // ESP-NOW one: these are different transports and their packet streams
        // are independent, so sharing the value would make the CYD's loss
        // detection report phantom gaps (or phantom health) on either one.
        uint8_t seq = 0;
        // The OctoPrint address the C3 is polling, so the CYD can follow it over
        // this transport too when ESP-NOW is unavailable. All-zero means "no
        // override"; see EnclosureTelemetry::octoIp.
        uint8_t octoIp[4] = {0, 0, 0, 0};
        uint16_t octoPort = 0;
        // The OctoPrint API key and its ENC_KEYFLAG_* state, carrying exactly
        // the same meaning as the ESP-NOW fields of the same name.
        //
        // Repeated here so the CYD has ONE apply path for the key regardless of
        // which transport delivered it. Two copies of this logic, one per
        // transport, is exactly how the two would drift - and the drift would
        // show up only while one transport was down, i.e. never in testing.
        //
        // Over HTTP this is a token-protected request rather than a cleartext
        // radio frame, so the flags matter less (a stale key is corrected on the
        // next response), but reusing them keeps the receiver identical.
        char octoKey[ENC_OCTO_KEY_MAX] = {0};
        uint8_t octoKeyFlags = 0;
        // Calibration fields, mirroring EnclosureTelemetry. The CYD needs them
        // on this transport too: its ESP-NOW link can be down for the whole run,
        // and the calibration screen is exactly when losing that state would
        // leave the user with a modal screen and no numbers.
        int16_t bedCx10 = ENC_CHAMBER_INVALID;
        uint16_t calElapsedSec = 0;
        uint16_t calCycles = 0;
        uint8_t calPhase = ENC_CAL_PHASE_NONE;
        uint8_t calBedTargetC = 0;
        uint8_t calSetpointC = 0;
        uint8_t calErrorCode = ENC_CAL_ERR_NONE;
    };

    using TelemetryProvider = Telemetry (*)();

    // Receives a decoded command (magic already stamped by the server). The
    // handler owns validation; the server has only checked the JSON shape.
    using CommandHandler = void (*)(const EnclosureCommand &cmd);

    // Runtime PID gains, read and written over /api/pid.
    //
    // The setter OWNS VALIDATION and returns false to reject; the server turns
    // that into a 400 and leaves the running gains untouched. The setter exists
    // at all so this library stays free of any dependency on
    // TemperatureRegulation, which lives in the C3 project, not here.
    //
    // Both are optional and default to nullptr. With neither supplied the /api/pid
    // routes are simply not registered, so a board that has not been flashed
    // with the gains feature still serves the rest of the API unchanged.
    using PidProvider = PidGains (*)();
    using PidSetter = bool (*)(const PidGains &g);

    // Persistence hooks for the gains. This library has no NVS of its own, so
    // the programme that owns the storage supplies these; null means the
    // /api/pid/save and /api/pid/defaults routes answer 501 instead of
    // pretending to have saved.
    using PidPersist = void (*)();

    // Starts its own WebServer on `port`. Call once after Wi-Fi is up.
    // `token` is the shared secret required on every request; pass "" to disable.
    //
    // The four PID parameters all default to nullptr, so a board that does not
    // wire them up keeps the previous two-argument behaviour and serves the
    // /api/pid routes as 501.
    void begin(uint16_t port, const char *token, TelemetryProvider telemetry,
               CommandHandler onCommand,
               PidProvider pidProvider = nullptr,
               PidSetter pidSetter = nullptr,
               PidPersist persist = nullptr,
               PidPersist toDefaults = nullptr);

    // Handles one pending request. Call from loop(); returns immediately.
    void poll();
} // namespace EnclosureApi

#endif // ENCLOSURE_API_SERVER_H
