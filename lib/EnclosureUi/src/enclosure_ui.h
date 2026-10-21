#ifndef ENCLOSURE_UI_H
#define ENCLOSURE_UI_H

#include <Arduino.h>
#include <enclosure_proto.h>
#include <enclosure_pid.h>

// A browser UI for the EnclosureFanControl (C3), served from the board itself.
//
// Why a page instead of more curl
// -------------------------------
// Tuning the PID gains over curl means remembering the token, the port and the
// JSON shapes, and it gives no feedback until after the fact: you cannot see the
// gains that are actually in force, whether they match what is in NVS, or what
// the chamber is doing while you change them. The API already existed - this is
// only a face for it, served from the same class of device that owns the values.
//
// OWN PORT, deliberately
// -----------------------
// lib/EnclosureApi already runs a WebServer on ENC_API_PORT, and this one runs on
// ENC_UI_PORT instead of sharing it. The API is the CYD's fallback transport:
// the CYD queries it only when ESP-NOW is stale, and it does so on a deadline -
// every 1.2 s timeout, then a backoff. A browser page on the same synchronous
// WebServer (one client at a time, a request served inline from poll()) would
// sit in front of that client for the length of a page load plus every poll
// interval. Separate servers mean a browser tab open on a phone cannot delay a
// single API call.
//
// Auth is the same rule as the API's: HTTP Basic, username ignored, password =
// ENC_API_TOKEN, and an empty token disables the check. So the page is opened by
// letting the browser prompt for credentials - the browser then attaches them to
// every same-origin fetch on its own, which means the token never appears in the
// page's JavaScript, in a URL, or in browser storage.
//
// Nothing here depends on a filesystem: the page is a single string constant in
// flash. That matters because neither board has a LittleFS partition, and adding
// one to upload an HTML file would cost more than it saves.

namespace EnclosureUi
{
    // PID calibration, as the page renders it.
    //
    // The numeric phase codes match PidCalibration::Phase in the programme; this
    // library only passes them through, deliberately, so the UI does not have to
    // depend on a header owned by the board firmware.
    struct CalibrationStatus
    {
        bool active = false;   // a run is in progress
        bool hasResult = false; // finished with a result waiting for apply/discard
        bool aborted = false;   // stopped without a result
        uint8_t phase = 0;
        unsigned long elapsedMs = 0;
        uint16_t cycles = 0;
        float ku = 0.0f;
        float pu = 0.0f;
        float amplitude = 0.0f;
        float relayDelta = 0.0f;
        PidGains oldGains = {0.0f, 0.0f, 0.0f};
        PidGains newGains = {0.0f, 0.0f, 0.0f};
        bool clamped = false;
        bool applied = false;
        bool zcSymmetrical = false;
        bool amplitudeConvergent = false;
        bool proceededWithWarning = false;
        float bedTargetC = 0.0f;
        float setpointC = 0.0f;
        String error;
    };

    // Snapshot the page renders. Filled by the programme at request time, so this
    // library knows nothing about TemperatureRegulation, the OLED, or ESP-NOW.
    struct Status
    {
        // Chamber temperature in 0.1 C. ENC_CHAMBER_INVALID means "no reading",
        // which the page must show as unavailable rather than as -3276.8 C.
        int16_t chamberCx10 = ENC_CHAMBER_INVALID;
        // Bed temperature in 0.1 C, same encoding. Shown by the calibration
        // progress dialog so the preheat phase is observable.
        int16_t bedCx10 = ENC_CHAMBER_INVALID;
        // ENC_STATE_* / ENC_MODE_* from enclosure_proto.h, passed through as
        // codes so this header stays the single source of those enumerations.
        uint8_t state = ENC_STATE_IDLE;
        uint8_t mode = ENC_MODE_AUTO;
        uint8_t louvrePct = 0;
        uint8_t fanPct = 0;
        bool fanActive = false;
        // What the manual sliders currently hold, so the page can show the
        // committed values rather than its own unsent ones.
        uint8_t manualLouvrePct = 0;
        uint8_t manualFanPct = 0;
        bool jobActive = false;
        uint8_t material = ENC_MATERIAL_UNKNOWN;
        // Setpoint in force, in whole C. Negative when the regulator has not
        // applied one yet (no valid data since boot).
        int8_t setpointC = -1;
        bool emergencyActive = false;
        // Whether the CYD is reachable: peer MAC configured, and commands
        // recently received. The page uses this to warn that a manual override
        // made here will be overwritten by the CYD's next heartbeat.
        bool remotePeer = false;
        bool remoteLinkAlive = false;
        // True when the NVS copy of the gains still matches the running ones,
        // i.e. nothing applied through the UI is one reboot away from being lost.
        bool gainsSaved = true;

        // PID calibration state. Empty/inactive for a board that never runs one.
        CalibrationStatus calibration;

        // The OctoPrint address this board is polling, and whether it came from
        // a stored override rather than the compiled-in header. The page shows
        // both so "the page says .88 but the firmware header says .80" is
        // answerable instead of mysterious.
        IPAddress octoIp;
        uint16_t octoPort = 0;
        bool octoOverridden = false;
        // Whether the CYD is currently applying an override of its own. Purely
        // informational: the C3 cannot confirm what the CYD stored, because
        // telemetry is one-way. Reported as "the CYD has been told", never as
        // "the CYD has it".
        bool octoSentToCyd = false;

        // The OctoPrint API key: reported as PROVENANCE ONLY, never as a value.
        //
        // The page does not receive the credential, ever - not prefilled into a
        // field, not in /api/status, not in the DOM. So it cannot be copied out
        // of the page, out of browser devtools, or out of a screenshot. What it
        // does get is enough to answer "is a key in force, and where did it come
        // from", which is the question a user actually has.
        //
        // octoKeySet is true when a usable key exists at all; a board whose
        // secrets.h has none configured has nothing to poll OctoPrint with.
        bool octoKeySet = false;
        bool octoKeyOverridden = false;
    };

    using StatusProvider = Status (*)();

    // Runtime PID gains. Mirrors the EnclosureApi callbacks, including the rule
    // that the SETTER OWNS VALIDATION: it returns false to reject, the server
    // turns that into a 400, and the running gains are left exactly as they were.
    using PidProvider = PidGains (*)();
    using PidSetter = bool (*)(const PidGains &g);

    // Persistence hooks. This library has no NVS of its own, so the programme
    // that owns the storage supplies these; null means save/defaults answer 501
    // rather than pretending to have saved.
    using PidPersist = void (*)();

    // Runs one calibration action: "start", "cancel", "apply" or "discard".
    //
    // Returns false with a human-readable `reason` when the board refuses - a
    // print is running, there is no result to apply, a run is already active.
    // The server turns that into a 409 and the page shows the text verbatim,
    // which is more useful than a generic "conflict".
    using CalibrationHandler = bool (*)(const char *action, String &reason);

    // Receives a decoded EnclosureCommand from POST /api/manual. This is a LOCAL
    // command, not one from the CYD, so the handler must apply it WITHOUT
    // refreshing the remote-link liveness - otherwise a browser click would make
    // the OLED claim the display is connected and suppress the command-link-loss
    // fallback that exists to catch a real disconnection.
    using ManualHandler = void (*)(const EnclosureCommand &cmd);

    // Probes and stores a new OctoPrint address, and optionally a new API key
    // alongside it. Must return false with a human-readable `reason` when the
    // combination does not work, and in that case change NOTHING: the board
    // keeps running on the address and key it already has.
    //
    // ADDRESS AND KEY ARE ONE SETTING, not two. They are submitted together
    // because separating them deadlocks: moving to a different OctoPrint server
    // means a new address AND a new key, but each probe can only use one stored
    // value and one candidate. Setting the address first probes the new server
    // with the old key (401); setting the key first probes the old server with
    // the new key. Neither can ever be applied first, and the only way out would
    // be a serial flash.
    //
    // `key` is nullptr or "" when the user left the field blank, which means
    // "keep the key already in force". That keeps a plain address change a
    // one-step operation, without ever making the submitted set ambiguous - the
    // page shows which key is in force, just never its value.
    //
    // The probe belongs to the programme, not to this library, because only the
    // programme holds the credentials needed to identify itself to the server.
    // Getting this wrong is not cosmetic - the C3 treats unreachable or
    // unauthorised OctoPrint data as invalid, which trips the cooling fail-safe.
    using EndpointSetter = bool (*)(IPAddress ip, uint16_t port, const char *key, String &reason);

    // Drops every stored override - address AND key - returning both to their
    // compiled-in defaults. One button, because they are configured as a pair.
    // Returns false with a reason when the reset is not safe right now (the
    // programme refuses it during a PID calibration, where the final bed-off
    // still has to reach the server the run started with).
    using EndpointReset = bool (*)(String &reason);

    struct Config
    {
        uint16_t port = 8082;
        // Shared secret. nullptr or "" disables auth, matching lib/HttpOta and
        // lib/EnclosureApi. Pass ENC_API_TOKEN - the same secret the API uses.
        const char *token = nullptr;
        // Firmware version shown in the page header. Null omits it.
        const char *fwVersion = nullptr;
        StatusProvider status = nullptr;
        PidProvider pid = nullptr;
        PidSetter setPid = nullptr;
        PidPersist savePid = nullptr;
        PidPersist restoreDefaultPid = nullptr;
        CalibrationHandler calibrate = nullptr;
        ManualHandler manual = nullptr;
        EndpointSetter setEndpoint = nullptr;
        EndpointReset clearEndpoint = nullptr;
    };

    // Starts its own WebServer on cfg.port. Call once after Wi-Fi is up.
    //
    // Every callback is optional. Without status there is no /api/status; without
    // the PID callbacks the /api/pid routes answer 501; without calibrate the
    // /api/calibrate route answers 501; without manual there is no /api/manual;
    // without setEndpoint there is no /api/octoprint. A board that wires up only
    // part of this still serves the page, with the missing sections reporting why
    // rather than failing silently.
    void begin(const Config &cfg);

    // Handles one pending request. Call from loop(); returns immediately.
    void poll();
} // namespace EnclosureUi

#endif // ENCLOSURE_UI_H