// PlatformIO compiles every source in this library into whichever project
// includes it, so without this guard the C3 would drag in HTTPClient + ESPmDNS
// from the client half - and the CYD would drag in WebServer - for code neither
// board calls. Each project defines exactly one of these in build_flags (see
// lib/EnclosureApi/README.md); the other file compiles to an empty translation
// unit and contributes nothing to the link.
//
// The guard wraps the whole file, including the #includes, so the unused half
// does not even pull its headers in.
#if defined(ENCLOSURE_API_SERVER)

#include "enclosure_api_server.h"

#include <WebServer.h>
#include <ArduinoJson.h>

// JSON wire format, shared with enclosure_api_client.h. Keep the key names in
// lockstep with the client; the values mirror EnclosureTelemetry/EnclosureCommand.
//
//   GET /api/enclosure  -> {"louvre":0-100,"fan":0-100,"chamber":int16<- "0.1 C, "
//                          "state":EnclosureState,"mode":EnclosureMode,"seq":0-255}
//   POST /api/enclosure <- {"mode":EnclosureMode,"setpoint":0-60,"louvre":0-100,
//                          "fan":0-100,"material":EnclosureMaterial,"job":0|1}
//
//   GET  /api/pid       -> {"kp":<float>,"ki":<float>,"kd":<float>}
//   POST /api/pid       <- same shape. 400 if any value is out of range or not
//                          finite; the running gains are then left untouched.
//   POST /api/pid/save      -> persists the current gains to NVS
//   POST /api/pid/defaults  -> restores the compiled-in defaults and persists them
//
// Both require HTTP Basic auth with ENC_API_TOKEN as the password.
//
// The /api/pid routes are only registered when the programme supplies both a
// PidProvider and a PidSetter (see enclosure_api_server.h). Persistence is NOT
// the server's business - it has no NVS - so save/defaults are handed to a third
// callback the C3 owns.

namespace
{
    WebServer *server = nullptr;
    String token;
    EnclosureApi::TelemetryProvider provideTelemetry = nullptr;
    EnclosureApi::CommandHandler handleCommand = nullptr;
    EnclosureApi::PidProvider providePid = nullptr;
    EnclosureApi::PidSetter applyPid = nullptr;
    // Persistence hooks. Null on a board that has no NVS wiring for the gains;
    // the routes then answer 501 rather than pretending to have saved.
    EnclosureApi::PidPersist persistPid = nullptr;
    EnclosureApi::PidPersist restoreDefaultPid = nullptr;
    // Per-request counter feeding Telemetry::seq. Incremented on every served
    // GET so the client can tell a fresh answer from a replayed/cached one.
    uint8_t telemetrySeq = 0;

    // HTTP Basic auth, mirroring lib/HttpOta's rule: an empty token disables the
    // check entirely.
    //
    // "cyd" is NOT an ignored placeholder, whatever it looks like.
    // WebServer::authenticate() base64-encodes "<username>:<password>" and
    // compares that whole header against the client's, so the username is part
    // of the credential: any other username is rejected even with the right
    // token. Both clients here - and the browser page on port 8082 - send "cyd"
    // to match.
    bool authorized()
    {
        if (token.length() == 0)
            return true;
        if (server->authenticate("cyd", token.c_str()))
            return true;
        server->requestAuthentication();
        return false;
    }

    void sendTelemetry()
    {
        EnclosureApi::Telemetry t =
            provideTelemetry ? provideTelemetry() : EnclosureApi::Telemetry{};
        t.seq = telemetrySeq++;
        JsonDocument doc;
        doc["louvre"] = t.louvrePct;
        doc["fan"] = t.fanPct;
        doc["chamber"] = t.chamberCx10;
        doc["state"] = t.state;
        doc["mode"] = t.mode;
        doc["seq"] = t.seq;
        doc["octoIp"] = String(t.octoIp[0]) + "." + String(t.octoIp[1]) + "." +
                        String(t.octoIp[2]) + "." + String(t.octoIp[3]);
        doc["octoPort"] = t.octoPort;
        // The key and its flags, same names as the ESP-NOW fields' meaning. Only
        // emitted when there is something to say: an unconditional key field
        // would put the credential in every response body, including any that a
        // browser or a log might end up holding, for no benefit - flags == 0
        // already means "keep what you have".
        if (t.octoKeyFlags & (ENC_KEYFLAG_PRESENT | ENC_KEYFLAG_CLEAR))
        {
            doc["octoKeyFlags"] = t.octoKeyFlags;
            if (t.octoKeyFlags & ENC_KEYFLAG_PRESENT)
                doc["octoKey"] = String(t.octoKey);
        }
        String body;
        serializeJson(doc, body);
        server->send(200, "application/json", body);
    }

    void receiveCommand()
    {
        if (!handleCommand)
        {
            server->send(500, "text/plain", "no handler");
            return;
        }
        JsonDocument doc;
        // arg("plain") is the raw request body for a POST, populated by WebServer
        // even without an upload handler as long as we did not register one.
        const DeserializationError err = deserializeJson(doc, server->arg("plain"));
        if (err)
        {
            server->send(400, "text/plain", "bad json");
            return;
        }
        EnclosureCommand cmd = {};
        cmd.magic = ENC_PROTO_MAGIC; // the link stamps this, not the caller
        cmd.mode = doc["mode"] | ENC_MODE_AUTO;
        cmd.setpoint = doc["setpoint"] | 0;
        cmd.louvrePct = doc["louvre"] | 0;
        cmd.fanPct = doc["fan"] | 0;
        cmd.material = doc["material"] | ENC_MATERIAL_UNKNOWN;
        cmd.jobActive = doc["job"] | 0;
        handleCommand(cmd);
        server->send(200, "text/plain", "OK");
    }

    void sendRoot()
    {
        String body = "enclosure api: GET/POST /api/enclosure";
        if (providePid && applyPid)
            body += ", GET/POST /api/pid, POST /api/pid/save, POST /api/pid/defaults";
        server->send(200, "text/plain", body);
    }

    void sendPid()
    {
        if (!providePid || !applyPid)
        {
            server->send(501, "text/plain", "pid not supported on this build");
            return;
        }
        const PidGains g = providePid();
        JsonDocument doc;
        doc["kp"] = g.kp;
        doc["ki"] = g.ki;
        doc["kd"] = g.kd;
        String body;
        serializeJson(doc, body);
        server->send(200, "application/json", body);
    }

    // Reads one gain out of the decoded body. Rejects anything that is not a JSON
    // number, which covers the three cases that must not be silently accepted:
    // an absent key, an explicit null, and a quoted string like "6.0".
    //
    // Integers are accepted alongside reals on purpose: `{"kp":8}` is a natural
    // thing to type at a shell, and whether ArduinoJson's is<float>() coerces it
    // is a library implementation detail. Testing both directions here keeps it
    // from becoming a user-visible surprise.
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

    // Decodes {"kp":..,"ki":..,"kd":..} and hands it to the programme's setter.
    // A missing key is a 400 rather than a silent "keep the current value" -
    // a tuning request that quietly applies two gains out of three is worse than
    // one that is refused.
    void receivePid()
    {
        if (!providePid || !applyPid)
        {
            server->send(501, "text/plain", "pid not supported on this build");
            return;
        }
        JsonDocument doc;
        const DeserializationError err = deserializeJson(doc, server->arg("plain"));
        if (err)
        {
            server->send(400, "text/plain", "bad json");
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
        // Echo the values actually in force. They may differ from the request if
        // the programme clamps; showing what is live beats showing what was asked.
        sendPid();
    }

    void savePid()
    {
        if (!persistPid)
        {
            server->send(501, "text/plain", "no persistence on this build");
            return;
        }
        persistPid();
        sendPid();
    }

    void defaultPid()
    {
        if (!restoreDefaultPid || !applyPid)
        {
            server->send(501, "text/plain", "no persistence on this build");
            return;
        }
        restoreDefaultPid();
        sendPid();
    }
} // namespace

namespace EnclosureApi
{
    void begin(uint16_t port, const char *tokenIn, TelemetryProvider telemetry,
               CommandHandler onCommand,
               PidProvider pidProvider, PidSetter pidSetter,
               PidPersist persist, PidPersist toDefaults)
    {
        provideTelemetry = telemetry;
        handleCommand = onCommand;
        providePid = pidProvider;
        applyPid = pidSetter;
        persistPid = persist;
        restoreDefaultPid = toDefaults;
        token = tokenIn ? tokenIn : "";

        if (server == nullptr)
            server = new WebServer(port);
        else
        {
            // Re-arm an existing server (e.g. begin() called twice): routes were
            // registered on the first pass, so just rebind the port.
            server->stop();
            server->begin(port);
            return;
        }

        // Auth runs before every handler, so an unauthenticated request never
        // reaches the telemetry provider or the command handler. Returning 401
        // from the handler (rather than a bare authenticate() call) keeps the
        // favicon and root reachable for a human poking at the port.
        server->on("/api/enclosure", HTTP_GET, []() {
            if (authorized())
                sendTelemetry();
        });
        server->on("/api/enclosure", HTTP_POST, []() {
            if (authorized())
                receiveCommand();
        });

        // Registered unconditionally: a request against a build without the
        // callbacks gets a 501 explaining why, which beats a 404 that looks
        // like a typo in the URL.
        server->on("/api/pid", HTTP_GET, []() {
            if (authorized())
                sendPid();
        });
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

        server->on("/", HTTP_GET, sendRoot);
        server->on("/favicon.ico", HTTP_GET, []() { server->send(204); });
        server->begin();
    }

    void poll()
    {
        if (server)
            server->handleClient();
    }
} // namespace EnclosureApi

#endif // ENCLOSURE_API_SERVER
