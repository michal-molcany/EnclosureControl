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
// Both require HTTP Basic auth with ENC_API_TOKEN as the password.

namespace
{
    WebServer *server = nullptr;
    String token;
    EnclosureApi::TelemetryProvider provideTelemetry = nullptr;
    EnclosureApi::CommandHandler handleCommand = nullptr;
    // Per-request counter feeding Telemetry::seq. Incremented on every served
    // GET so the client can tell a fresh answer from a replayed/cached one.
    uint8_t telemetrySeq = 0;

    // HTTP Basic auth, mirroring lib/HttpOta's rule: an empty token disables the
    // check entirely. The username is ignored - only the password is compared -
    // so both sides can use a fixed placeholder for it.
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
        server->send(200, "text/plain",
                     "enclosure api: GET/POST /api/enclosure");
    }
} // namespace

namespace EnclosureApi
{
    void begin(uint16_t port, const char *tokenIn, TelemetryProvider telemetry,
               CommandHandler onCommand)
    {
        provideTelemetry = telemetry;
        handleCommand = onCommand;
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
