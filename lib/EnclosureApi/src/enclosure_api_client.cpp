#if defined(ENCLOSURE_API_CLIENT)

#include "enclosure_api_client.h"

#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>

namespace
{
    // ArduinoHttpClient's default User-Agent identifies the library and version
    // on every request, which tells anyone on the LAN exactly what firmware is
    // answering and invites a version search. The enclosure API is not a public
    // service, so send a generic one.
    const char *const kUserAgent = "EnclosureDisplay/1";

    // Named g_ to avoid colliding with the lastFailure() accessor below, and
    // fully qualified because Failure is declared inside namespace EnclosureApi,
    // which is not yet open at this point.
    EnclosureApi::Failure gLastFailure = EnclosureApi::Failure::None;
    // HTTP status from the most recent non-200 response; 401 is the one that
    // matters (bad token), so it is kept for the serial log.
    int lastStatus = 0;

    void applyAuth(HTTPClient &http, const char *token)
    {
        if (token && token[0])
        {
            // The token goes in the PASSWORD slot, matching the server's
            // authenticate("cyd", token). HTTPClient::setAuthorization(user, pw)
            // builds "user:pw" and base64s it, so passing the token as `user`
            // with an empty password yields base64("<token>:") - which the
            // server reads as the wrong credentials and answers 401.
            http.setAuthorization("cyd", token);
        }
    }

    // Opens `client` to host:port and hands it to `http`. Returns false if the
    // connection could not be established, leaving `http` untouched.
    //
    // Using the caller's WiFiClient (rather than http.begin(url)) is what keeps
    // mbedTLS out of the link - see the note in enclosure_api_client.h.
    bool openTunnel(HTTPClient &http, WiFiClient &client, const IPAddress &host,
                    uint16_t port)
    {
        if (!client.connect(host, port))
            return false;
        return http.begin(client, host.toString(), port, "/api/enclosure", false);
    }
} // namespace

namespace EnclosureApi
{
    const char *failureText(Failure f)
    {
        switch (f)
        {
        case Failure::Connect:
            return "connect";
        case Failure::HttpStatus:
            return "http-status";
        case Failure::BadJson:
            return "bad-json";
        case Failure::None:
        default:
            return "none";
        }
    }

    Failure lastFailure()
    {
        return gLastFailure;
    }

    int lastHttpStatus()
    {
        return lastStatus;
    }

    bool resolve(IPAddress &out, const char *mdnsName, const char *staticIp)
    {
        // mDNS first: the C3 registers this host (via ArduinoOTA with the same
        // name), so queryHost() returns its current address even after a DHCP
        // lease change. queryHost() blocks up to the timeout only when the query
        // fails; callers do it on a slow cadence, not per poll.
        if (mdnsName && mdnsName[0])
        {
            const IPAddress ip = MDNS.queryHost(const_cast<char *>(mdnsName), 1500);
            if ((uint32_t)ip != 0)
            {
                out = ip;
                return true;
            }
        }
        // Static fallback. An empty string or 0.0.0.0 disables it, so a plain
        // mDNS-only setup does not accidentally connect to the wildcard.
        if (staticIp && staticIp[0])
        {
            IPAddress ip;
            if (ip.fromString(staticIp) && (uint32_t)ip != 0)
            {
                out = ip;
                return true;
            }
        }
        return false;
    }

    bool getTelemetry(const IPAddress &host, uint16_t port, const char *token,
                      WiFiClient &client, EnclosureTelemetry &out)
    {
        HTTPClient http;
        http.setConnectTimeout(1200);
        http.setTimeout(1200);
        http.setUserAgent(kUserAgent);
        http.setReuse(false); // this HTTPClient is short-lived; no keep-alive
        if (!openTunnel(http, client, host, port))
        {
            client.stop();
            gLastFailure = Failure::Connect;
            return false;
        }
        applyAuth(http, token);

        const int code = http.GET();
        if (code != 200)
        {
            // Record the status too: 401 means the token is wrong, which is a
            // very different fault from a dead socket and worth naming.
            lastStatus = code;
            http.end();
            gLastFailure = Failure::HttpStatus;
            return false;
        }
        // Stream-parse straight off the socket: the response is ~80 bytes, so
        // this never needs a big buffer on the flash-tight CYD.
        JsonDocument doc;
        const DeserializationError err = deserializeJson(doc, http.getStream());
        http.end();
        if (err)
        {
            gLastFailure = Failure::BadJson;
            return false;
        }
        gLastFailure = Failure::None;

        out = EnclosureTelemetry{};
        out.magic = ENC_PROTO_MAGIC;
        out.louvrePct = doc["louvre"] | (uint8_t)0;
        out.fanPct = doc["fan"] | (uint8_t)0;
        out.chamberCx10 = (int16_t)(doc["chamber"] | (int)ENC_CHAMBER_INVALID);
        out.state = doc["state"] | (uint8_t)ENC_STATE_IDLE;
        out.mode = doc["mode"] | (uint8_t)ENC_MODE_AUTO;
        out.seq = doc["seq"] | (uint8_t)0;
        return true;
    }

    bool postCommand(const IPAddress &host, uint16_t port, const char *token,
                     WiFiClient &client, const EnclosureCommand &cmd)
    {
        HTTPClient http;
        http.setConnectTimeout(1200);
        http.setTimeout(1200);
        http.setUserAgent(kUserAgent);
        http.setReuse(false);
        if (!openTunnel(http, client, host, port))
        {
            client.stop();
            gLastFailure = Failure::Connect;
            return false;
        }
        applyAuth(http, token);
        http.addHeader("Content-Type", "application/json");

        JsonDocument doc;
        doc["mode"] = cmd.mode;
        doc["setpoint"] = cmd.setpoint;
        doc["louvre"] = cmd.louvrePct;
        doc["fan"] = cmd.fanPct;
        doc["material"] = cmd.material;
        doc["job"] = cmd.jobActive;
        String body;
        serializeJson(doc, body);

        const int code = http.POST(body);
        http.end();
        if (code != 200)
        {
            lastStatus = code;
            gLastFailure = Failure::HttpStatus;
            return false;
        }
        gLastFailure = Failure::None;
        return true;
    }
} // namespace EnclosureApi

#endif // ENCLOSURE_API_CLIENT