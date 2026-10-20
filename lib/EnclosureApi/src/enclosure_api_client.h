#ifndef ENCLOSURE_API_CLIENT_H
#define ENCLOSURE_API_CLIENT_H

#include <Arduino.h>
#include <IPAddress.h>
#include <WiFiClient.h>
#include <enclosure_proto.h>

// HTTP JSON API client used by the PrinterDisplay CYD to talk to the C3 through
// the EnclosureApi server when ESP-NOW is unavailable (see enclosure_api_server.h
// for why a channel/AP split forces this fallback).
//
// Every call is best-effort: a failure simply returns false and the caller keeps
// its current state. Nothing here blocks for long - resolve() is bounded by its
// mDNS timeout, and the GET/POST use HTTPClient's own timeouts.
//
// `token` is the shared secret both sides agree on (ENC_API_TOKEN). The server
// rejects a request without it; pass "" only if the server also has auth off.
//
// WHY THE CALLER PASSES A WiFiClient
// ----------------------------------
// HTTPClient is given an already-connected client rather than a URL string.
// begin(url) unconditionally constructs a WiFiClientSecure for the "https"
// case, which drags the whole mbedTLS stack and the CA bundle into the link.
// On the CYD that measured at ~168 KB of flash against a partition that had
// only ~100 KB spare - the build did not fit at all. This API is plain HTTP
// between two boards on the same LAN, so the caller hands over a plain
// WiFiClient and TLS is never referenced.
//
// The cost of that choice is stated plainly: an https:// URL cannot be used
// with these entry points. If this ever needs TLS, the trade has to be made
// deliberately (and the partition budget revisited), not inherited silently.

namespace EnclosureApi
{
    // Why the last request() failed. A bare bool is not enough to debug this
    // link: "no telemetry" looks identical whether the socket never opened, the
    // server answered 401, or the JSON was garbage - and the last of those is a
    // protocol bug while the first is a network one. Set by the most recent
    // getTelemetry()/postCommand() call.
    enum class Failure
    {
        None = 0,
        Connect,     // TCP connect() failed - wrong address, C3 down, or firewalled
        HttpStatus,  // server answered, but not 200 (401 = bad token)
        BadJson,     // 200 but the body did not parse
    };

    // Human-readable reason for the most recent failure, for the serial log.
    const char *failureText(Failure f);
    // Failure from the most recent getTelemetry()/postCommand() call.
    Failure lastFailure();
    // HTTP status from the most recent non-200 response, else 0. 401 means the
    // shared token did not match.
    int lastHttpStatus();

    // Resolves the C3's address.
    //   mdnsName  - mDNS host the C3 advertises (e.g. "enclosure-fan-control").
    //               Resolved via an mDNS A-record query, so a DHCP lease change
    //               does not break the link. May be null/empty to skip mDNS.
    //   staticIp  - dotted-quad used only if the mDNS query fails; may be
    //               null/empty (or "0.0.0.0") to disable the fallback.
    // Returns true and writes `out` on success.
    bool resolve(IPAddress &out, const char *mdnsName, const char *staticIp);

    // GET /api/enclosure. Returns true and fills `out` on HTTP 200 + valid JSON.
    bool getTelemetry(const IPAddress &host, uint16_t port, const char *token,
                      WiFiClient &client, EnclosureTelemetry &out);

    // POST /api/enclosure with `cmd` encoded as JSON. Returns true on HTTP 200.
    bool postCommand(const IPAddress &host, uint16_t port, const char *token,
                     WiFiClient &client, const EnclosureCommand &cmd);
} // namespace EnclosureApi

#endif // ENCLOSURE_API_CLIENT_H