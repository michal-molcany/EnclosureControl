// Web UI for the EnclosureFanControl (C3): the page itself plus the small JSON
// API it talks to.
//
// See enclosure_ui.h for why this is a separate port from lib/EnclosureApi, and
// enclosure_ui_page.h for why the page is one string constant rather than a file
// on a filesystem.
//
// Wire format, all of it behind HTTP Basic auth (password = ENC_API_TOKEN):
//
//   GET  /                 -> the page (text/html)
//   GET  /api/status       -> everything the page renders, one round trip
//   POST /api/pid          <- {"kp":<float>,"ki":<float>,"kd":<float>}
//                            400 if a value is out of range or not finite, in
//                            which case the running gains are left untouched
//   POST /api/pid/save     -> persists the running gains to NVS
//   POST /api/pid/defaults -> restores the compiled-in defaults and persists them
//   POST /api/calibrate    <- {"action":"start"|"cancel"|"apply"|"discard"}
//                            409 when the board refuses, with the reason
//   POST /api/manual       <- {"mode":0|1,"louvre":0-100,"fan":0-100}
//   POST /api/octoprint     <- {"ip":"...","port":80[,"key":"..."]}
//                            Address and API key together, PROBED together: they
//                            cannot be set independently. "key" is optional and
//                            means "keep the key in force".
//   POST /api/octoprint/defaults -> drops BOTH stored overrides at once
//
// Every response - including the ones to a write - is the same document
// /api/status returns. The page re-renders from it, so a write can never leave
// the display showing a value the board does not actually have: if the board
// clamped or rejected something, what comes back is what is really in force.

#include "enclosure_ui.h"
#include "enclosure_ui_page.h"

#include <WebServer.h>
#include <ArduinoJson.h>
#include <WiFi.h>

namespace
{
    WebServer *server = nullptr;

    // The raw request body, or an explanation of why there isn't one.
    //
    // arg("plain") is only populated for a NON-form Content-Type (see
    // WebServer's Parsing.cpp: a request sent as application/x-www-form-urlencoded
    // has its body handed to _parseArguments() instead, where a JSON string has no
    // '=' and is discarded outright - unrecoverable, not merely empty). A browser
    // always sends application/json, but `curl -d` does not, so the failure this
    // guards is the FIRST thing a command-line user hits. Saying so is worth more
    // than the generic "bad json".
    bool requestBody(JsonDocument &doc)
    {
        const String body = server->arg("plain");
        if (body.length() == 0)
        {
            server->send(400, "text/plain",
                         "empty body: send Content-Type: application/json, otherwise "
                         "the request body is parsed as form fields and discarded");
            return false;
        }
        return !deserializeJson(doc, body);
    }

    const char *token = nullptr;
    const char *fwVersion = nullptr;
    EnclosureUi::StatusProvider provideStatus = nullptr;
    EnclosureUi::PidProvider providePid = nullptr;
    EnclosureUi::PidSetter applyPid = nullptr;
    EnclosureUi::PidPersist persistPid = nullptr;
    EnclosureUi::PidPersist restoreDefaultPid = nullptr;
    EnclosureUi::CalibrationHandler handleCalibration = nullptr;
    EnclosureUi::ManualHandler handleManual = nullptr;
    EnclosureUi::EndpointSetter applyEndpoint = nullptr;
    EnclosureUi::EndpointReset clearEndpointFn = nullptr;

    // The stored key is a credential, so it is never echoed: /api/status reports
    // only whether one is in force and where it came from. This bound exists so
    // that decision cannot be undone by accident later - there is no code path
    // in this file that puts a key into a response.
    static_assert(ENC_OCTO_KEY_MAX <= 128, "key buffer bound");

    // Same rule as lib/EnclosureApi and lib/HttpOta: an empty token disables the
    // check entirely.
    //
    // The username is NOT ignored, despite "cyd" reading like a placeholder.
    // WebServer::authenticate() base64-encodes "<username>:<password>" and
    // compares that WHOLE header against the one the client sent, so the
    // username is part of the credential and must match exactly. Log in as
    // user "Enclosure", password = the token.
    //
    // requestAuthentication() sends the 401 with a WWW-Authenticate header, and
    // that header is what makes the browser prompt.
    bool authorized()
    {
        if (!token || token[0] == '\0')
            return true;
        if (server->authenticate("Enclosure", token))
            return true;
        server->requestAuthentication();
        return false;
    }

    // The accepted ranges and the compiled-in defaults are reported to the page
    // rather than hard-coded in its HTML: the bounds live in enclosure_pid.h,
    // which both firmwares include, and a second copy in a string literal would
    // be free to drift out of date without anything noticing.
    void writePidBlock(JsonDocument &doc, PidGains live)
    {
        JsonObject pid = doc["pid"].to<JsonObject>();
        pid["kp"] = live.kp;
        pid["ki"] = live.ki;
        pid["kd"] = live.kd;

        JsonObject lo = pid["min"].to<JsonObject>();
        lo["kp"] = PID_KP_MIN;
        lo["ki"] = PID_KI_MIN;
        lo["kd"] = PID_KD_MIN;

        JsonObject hi = pid["max"].to<JsonObject>();
        hi["kp"] = PID_KP_MAX;
        hi["ki"] = PID_KI_MAX;
        hi["kd"] = PID_KD_MAX;

        const PidGains defaults = pidDefaultGains();
        JsonObject def = pid["defaults"].to<JsonObject>();
        def["kp"] = defaults.kp;
        def["ki"] = defaults.ki;
        def["kd"] = defaults.kd;
    }

    void sendStatus()
    {
        EnclosureUi::Status s;
        if (provideStatus)
            s = provideStatus();

        JsonDocument doc;
        doc["chamber"] = s.chamberCx10;
        // The invalid reading is ENC_CHAMBER_INVALID, which the page shows as
        // "no data". This flag says so explicitly rather than making the page
        // compare against INT16_MIN and get the encoding wrong.
        doc["chamberValid"] = (s.chamberCx10 != ENC_CHAMBER_INVALID);
        doc["bed"] = s.bedCx10;
        doc["bedValid"] = (s.bedCx10 != ENC_CHAMBER_INVALID);
        doc["state"] = s.state;
        doc["mode"] = s.mode;
        doc["louvre"] = s.louvrePct;
        doc["fan"] = s.fanPct;
        doc["fanActive"] = s.fanActive;
        doc["manualLouvre"] = s.manualLouvrePct;
        doc["manualFan"] = s.manualFanPct;
        doc["job"] = s.jobActive;
        doc["material"] = s.material;
        doc["setpoint"] = s.setpointC;
        doc["emergency"] = s.emergencyActive;
        doc["peer"] = s.remotePeer;
        doc["remote"] = s.remoteLinkAlive;
        doc["pidSaved"] = s.gainsSaved;
        doc["octoIp"] = s.octoIp.toString();
        doc["octoPort"] = s.octoPort;
        doc["octoOverridden"] = s.octoOverridden;
        doc["octoSentToCyd"] = s.octoSentToCyd;
        doc["octoKeySet"] = s.octoKeySet;
        doc["octoKeyOverridden"] = s.octoKeyOverridden;

        writePidBlock(doc, providePid ? providePid() : pidDefaultGains());

        // Calibration state. Always present so the page never has to guess: an
        // all-default block means "no run, no result", which is also what a
        // board without calibration support reports.
        JsonObject cal = doc["cal"].to<JsonObject>();
        cal["active"] = s.calibration.active;
        cal["hasResult"] = s.calibration.hasResult;
        cal["aborted"] = s.calibration.aborted;
        cal["phase"] = s.calibration.phase;
        cal["elapsed"] = s.calibration.elapsedMs;
        cal["cycles"] = s.calibration.cycles;
        cal["ku"] = s.calibration.ku;
        // "tu" on the wire, not "pu": the page and the ZN literature both call
        // the ultimate period Tu, and only ESPHome's code calls it pu.
        cal["tu"] = s.calibration.pu;
        cal["amp"] = s.calibration.amplitude;
        cal["delta"] = s.calibration.relayDelta;
        cal["kp"] = s.calibration.newGains.kp;
        cal["ki"] = s.calibration.newGains.ki;
        cal["kd"] = s.calibration.newGains.kd;
        cal["oldKp"] = s.calibration.oldGains.kp;
        cal["oldKi"] = s.calibration.oldGains.ki;
        cal["oldKd"] = s.calibration.oldGains.kd;
        cal["clamped"] = s.calibration.clamped;
        cal["applied"] = s.calibration.applied;
        cal["zcSym"] = s.calibration.zcSymmetrical;
        cal["ampConv"] = s.calibration.amplitudeConvergent;
        cal["warned"] = s.calibration.proceededWithWarning;
        cal["bedTarget"] = s.calibration.bedTargetC;
        cal["setpoint"] = s.calibration.setpointC;
        cal["error"] = s.calibration.error;

        // Board facts the library can answer without asking the programme. The
        // heap figure is the reason this page is served from flash: a dropped
        // free-heap number after a few minutes is the first sign of a leak.
        doc["rssi"] = WiFi.RSSI();
        doc["heap"] = ESP.getFreeHeap();
        doc["uptime"] = millis() / 1000;
        doc["ip"] = WiFi.localIP().toString();
        doc["fw"] = fwVersion ? fwVersion : "";

        String body;
        serializeJson(doc, body);
        server->send(200, "application/json", body);
    }

    void sendPage()
    {
        // send_P streams the page straight out of flash. send() would first
        // build a String copy of it in RAM - roughly 10 KB of heap for a page
        // that is never modified. The length is sizeof()-derived so it is
        // computed at compile time rather than by strlen() at runtime.
        server->send_P(200, "text/html", EnclosureUi::page::kPage, sizeof(EnclosureUi::page::kPage) - 1);
    }

    // Reads one gain out of the decoded body. Rejects anything that is not a
    // JSON number, which covers the three cases that must not be silently
    // accepted: an absent key, an explicit null, and a quoted string like "6.0".
    //
    // Integers are accepted alongside reals on purpose: {"kp":8} is a natural
    // thing to type at a shell or in a browser field, and whether ArduinoJson's
    // is<float>() coerces it is a library implementation detail.
    bool readGain(JsonVariant value, float &out)
    {
        if (value.is<float>())
        {
            out = value.as<float>();
            return true;
        }
        if (value.is<int>())
        {
            out = (float)value.as<int>();
            return true;
        }
        return false;
    }

    void receivePid()
    {
        if (!providePid || !applyPid)
        {
            server->send(501, "text/plain", "pid not supported on this build");
            return;
        }
        JsonDocument doc;
        if (!requestBody(doc))
        {
            return;
        }
        PidGains g;
        if (!readGain(doc["kp"], g.kp) || !readGain(doc["ki"], g.ki) || !readGain(doc["kd"], g.kd))
        {
            server->send(400, "text/plain", "kp, ki and kd are all required numbers");
            return;
        }
        if (!applyPid(g))
        {
            server->send(400, "text/plain", "rejected: out of range or not finite");
            return;
        }
        // Echo what is actually in force: the programme may clamp, and showing
        // what is live beats showing what was asked for.
        sendStatus();
    }

    void savePid()
    {
        if (!persistPid)
        {
            server->send(501, "text/plain", "no persistence on this build");
            return;
        }
        persistPid();
        sendStatus();
    }

    void defaultPid()
    {
        if (!restoreDefaultPid || !applyPid)
        {
            server->send(501, "text/plain", "no persistence on this build");
            return;
        }
        restoreDefaultPid();
        sendStatus();
    }

    // POST /api/calibrate - {"action":"start"|"cancel"|"apply"|"discard"}
    //
    // One route for the whole lifecycle because the actions are steps in a
    // single state machine, and splitting them into /start, /cancel, ... would
    // scatter the "which action is legal now" question across four handlers.
    // The programme owns that question and answers it with the reason text.
    //
    // 409 rather than 400 for a refusal: the request is well-formed and the
    // board's state says no (a print is running, there is no result to apply).
    // The page shows the reason verbatim.
    void receiveCalibration()
    {
        if (!handleCalibration)
        {
            server->send(501, "text/plain", "calibration not supported on this build");
            return;
        }
        JsonDocument doc;
        if (!requestBody(doc))
        {
            return;
        }

        // An explicit action is REQUIRED. Defaulting to "start" would turn a
        // hand-written POST with an empty object into a 20-minute bed preheat,
        // which is not a thing an omitted field should ever be able to do.
        //
        // Copied into a String rather than used through `doc["action"] | ""`:
        // with a bare string literal ArduinoJson's operator| selects the array
        // overload and hands back something that is not a usable string. Same
        // trap documented in receiveEndpoint().
        JsonVariantConst actionValue = doc["action"];
        if (!actionValue.is<const char *>())
        {
            server->send(400, "text/plain", "action is required (start, cancel, apply or discard)");
            return;
        }
        const String action = String(actionValue.as<const char *>());

        if (action != "start" && action != "cancel" && action != "apply" && action != "discard")
        {
            server->send(400, "text/plain", "action must be start, cancel, apply or discard");
            return;
        }

        String reason;
        if (!handleCalibration(action.c_str(), reason))
        {
            server->send(409, "text/plain", reason.length() ? reason : "calibration refused");
            return;
        }
        sendStatus();
    }

    void receiveManual()
    {
        if (!handleManual)
        {
            server->send(501, "text/plain", "manual control not supported on this build");
            return;
        }
        JsonDocument doc;
        if (!requestBody(doc))
        {
            return;
        }
        const int mode = doc["mode"] | -1;
        if (mode != ENC_MODE_AUTO && mode != ENC_MODE_MANUAL)
        {
            server->send(400, "text/plain", "mode must be 0 (auto) or 1 (manual)");
            return;
        }
        // Percentages are validated here rather than clamped silently. A slider
        // can only produce 0-100, so anything else is a hand-written request -
        // and a manual command is the one path that bypasses the regulator, so
        // it is worth refusing a nonsense value instead of quietly repairing it.
        const int louvre = doc["louvre"] | -1;
        const int fan = doc["fan"] | -1;
        if (louvre < 0 || louvre > 100 || fan < 0 || fan > 100)
        {
            server->send(400, "text/plain", "louvre and fan must be 0-100");
            return;
        }
        EnclosureCommand cmd = {};
        cmd.magic = ENC_PROTO_MAGIC; // stamped here, not by the caller
        cmd.mode = (uint8_t)mode;
        cmd.louvrePct = (uint8_t)louvre;
        cmd.fanPct = (uint8_t)fan;
        // Material and job are left UNKNOWN/false on purpose: they belong to the
        // printer, not to this page, and a browser has no business claiming a
        // job is running. The handler decides what to do with an absent material.
        cmd.material = ENC_MATERIAL_UNKNOWN;
        cmd.jobActive = 0;
        handleManual(cmd);
        sendStatus();
    }

    // POST /api/octoprint - {"ip":"192.168.1.88","port":80,"key":"..."}
    //
    // Address and optional API key in ONE request, and that is load-bearing
    // rather than convenient. They have to be applied together because moving to
    // a different server means a new address AND a new key: probing the new
    // address with the old key gets 401, and probing the old address with the
    // new key fails too, so either one set on its own can never be applied first
    // and the pair could only be fixed by a serial flash.
    //
    // An absent or empty "key" means "keep the key already in force", which
    // leaves a plain address change as a one-step operation.
    //
    // Both values are validated HERE before the programme sees them, so an
    // obviously impossible one is refused without spending a probe on it, and so
    // the caller gets a precise reason rather than a generic failure.
    void receiveEndpoint()
    {
        if (!applyEndpoint)
        {
            server->send(501, "text/plain", "octoprint address not supported on this build");
            return;
        }
        JsonDocument doc;
        if (!requestBody(doc))
        {
            return;
        }
        // Read the address as a String, deliberately NOT as `doc["ip"] | ""`.
//
// With a bare string literal, ArduinoJson's operator| deduces T to char[1],
// which fails the !is_array<T> constraint on the value-returning overload and
// selects the ARRAY one instead - handing back something that converts to
// const char* without being a usable string. The existing `doc["port"] | 80`
// calls elsewhere are safe only because their defaults are typed values.
// Copying into a String also takes the pointer off the JSON buffer, so the
// value cannot be invalidated while the probe runs.
        JsonVariantConst ipValue = doc["ip"];
        String ipText = ipValue.is<const char *>() ? ipValue.as<const char *>() : String();
        IPAddress ip;
        if (!ipText.length() || !ip.fromString(ipText))
        {
            server->send(400, "text/plain",
                         "ip must be a dotted IPv4 address, e.g. 192.168.1.88");
            return;
        }
        // Two addresses that parse but cannot be an OctoPrint. 0.0.0.0 is also
        // the "no override" sentinel on the ESP-NOW link, so accepting it here
        // would turn "clear the override" into an override to nowhere.
        if (ip == IPAddress(0, 0, 0, 0) || ip == IPAddress(255, 255, 255, 255))
        {
            server->send(400, "text/plain",
                         "that address cannot be a server (0.0.0.0 / 255.255.255.255). "
                         "To go back to the firmware default use Reset instead.");
            return;
        }
        const int port = doc["port"] | 80;
        if (port < 1 || port > 65535)
        {
            server->send(400, "text/plain", "port must be 1-65535");
            return;
        }

        // The API key, if one was supplied. An absent or empty value is not an error -
        // it means "keep the key in force" - but a supplied one is validated hard.
        //
        // The CR/LF rejection is not pedantry. The key is interpolated raw into
        // an "X-Api-Key:" header line by the probe; an embedded CR or LF ends
        // that line early and turns the rest of the value into headers of its
        // own, sent to the user's OctoPrint server. That is request splitting,
        // reachable by anyone who can reach this page.
        JsonVariantConst keyValue = doc["key"];
        const String key = keyValue.is<const char *>() ? String(keyValue.as<const char *>()) : String();
        if (key.length() > ENC_OCTO_KEY_MAX - 1)
        {
            server->send(400, "text/plain",
                         "key is too long (" + String(key.length()) + " chars, max " +
                             String(ENC_OCTO_KEY_MAX - 1) + ")");
            return;
        }
        for (size_t i = 0; i < key.length(); ++i)
        {
            const unsigned char c = (unsigned char)key[i];
            // Printable ASCII only: rejects CR, LF, NUL, DEL and every other
            // control byte, so the value cannot break out of the header line it
            // is interpolated into. Spaces are excluded too - no legitimate
            // OctoPrint key contains one.
            if (c < 0x20 || c > 0x7E || c == ' ')
            {
                server->send(400, "text/plain",
                             "key must be printable ASCII with no spaces or control "
                             "characters");
                return;
            }
        }

        String reason;
        // Null when the field was left blank, so the programme can tell "no new
        // key" from "the empty key" and probe with the one already in force.
        if (!applyEndpoint(ip, (uint16_t)port, key.length() ? key.c_str() : nullptr, reason))
        {
            // 502: the board is fine, the thing it was asked to talk to is not.
            server->send(502, "text/plain",
                         reason.length() ? reason : "that address and key did not answer");
            return;
        }
        sendStatus();
    }

    void resetEndpoint()
    {
        if (!clearEndpointFn)
        {
            server->send(501, "text/plain", "octoprint address not supported on this build");
            return;
        }
        String reason;
        if (!clearEndpointFn(reason))
        {
            server->send(409, "text/plain",
                         reason.length() ? reason.c_str() : "octoprint endpoint reset refused");
            return;
        }
        sendStatus();
    }

    void notFound()
    {
        server->send(404, "text/plain", "no such page");
    }
} // namespace

namespace EnclosureUi
{
    void begin(const Config &cfg)
    {
        token = cfg.token;
        fwVersion = cfg.fwVersion;
        provideStatus = cfg.status;
        providePid = cfg.pid;
        applyPid = cfg.setPid;
        persistPid = cfg.savePid;
        restoreDefaultPid = cfg.restoreDefaultPid;
        handleCalibration = cfg.calibrate;
        handleManual = cfg.manual;
        applyEndpoint = cfg.setEndpoint;
        clearEndpointFn = cfg.clearEndpoint;

        if (server)
            return;

        server = new WebServer(cfg.port);

        server->on("/", HTTP_GET, []() {
            if (authorized())
                sendPage();
        });

        server->on("/api/status", HTTP_GET, []() {
            if (authorized())
                sendStatus();
        });

        // Registered unconditionally: a request against a build without the
        // callbacks gets a 501 explaining why, which beats a 404 that looks
        // like a typo in the URL.
        server->on("/api/pid", HTTP_POST, []() {
            if (authorized())
                receivePid();
        });
        server->on("/api/pid/save", HTTP_POST, []() {
            if (authorized())
                savePid();
        });
        server->on("/api/pid/defaults", HTTP_POST, []() {
            if (authorized())
                defaultPid();
        });

        server->on("/api/calibrate", HTTP_POST, []() {
            if (authorized())
                receiveCalibration();
        });

        server->on("/api/manual", HTTP_POST, []() {
            if (authorized())
                receiveManual();
        });

        server->on("/api/octoprint", HTTP_POST, []() {
            if (authorized())
                receiveEndpoint();
        });
        server->on("/api/octoprint/defaults", HTTP_POST, []() {
            if (authorized())
                resetEndpoint();
        });

        // Answered before the auth check, and without auth: a browser requests
        // the favicon on its own, and a 401 on it makes some browsers show an
        // auth prompt nobody asked for.
        server->on("/favicon.ico", HTTP_GET, []() {
            server->send(204);
        });

        server->onNotFound(notFound);
        server->begin();
    }

    void poll()
    {
        if (server)
            server->handleClient();
    }
} // namespace EnclosureUi