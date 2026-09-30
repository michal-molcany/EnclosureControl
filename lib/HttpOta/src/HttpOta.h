#ifndef HTTP_OTA_H
#define HTTP_OTA_H

#include <Arduino.h>

// HTTP OTA endpoint (POST /update), shared by PrinterDisplay (CYD) and
// EnclosureFanControl (C3).
//
// Why not just ArduinoOTA? ArduinoOTA always makes the *board* dial out to
// the uploader, so the uploader's PC has to accept an inbound connection. A
// default Windows Firewall blocks that, and the board reports:
//
//   [OTA] failed: connect (2)          <- OTA_CONNECT_ERROR
//   [OTA] failed: end (4)             <- OTA_END_ERROR
//   [E][ArduinoOTA.cpp:360] _runUpdate(): Update ERROR: Aborted
//
// The UDP handshake still shows "Authenticating...OK" because the reply is part
// of the uploader's own outbound flow, which is why the failure looks like a
// mystery. Changing the port does not help: the default policy is
// block-all-inbound, not block-by-port, and port 80 fails identically.
//
// This endpoint inverts the direction - the board listens and the uploader
// makes an ordinary outbound request:
//
//   curl -u <user>:<password> -F "firmware=@firmware.bin" http://<board>:8080/update
//
// The response body is "OK" on success (HTTP 200) or the short failure name
// such as "end" / "write" / "stalled" / "busy" (HTTP 500). GET /status reports
// the same state without uploading, for diagnosis without a serial cable.
//
// On success the board reboots itself once the response has been sent, so the
// new image is the one actually running. A stalled transfer is abandoned after
// 30s so a half-open connection cannot wedge the endpoint.
//
// Call begin() once after WiFi is up and poll() from loop(). The image is
// written to the inactive OTA slot and validated before the reboot.

namespace HttpOta
{
    // Short status text for the project's own UI, e.g. "starting", "42%",
    // "512k", "done reboot", "begin", "write", "end", "abort". May be null.
    using StatusFn = void (*)(const char *text);

    // password may be null or empty, which disables auth. It is copied, so the
    // caller does not have to keep the string alive. verbose adds a per-chunk
    // serial progress line (about once a second).
    void begin(uint16_t port, const char *username, const char *password,
               bool verbose, StatusFn onStatus);

    // Call from loop(). Handles one request; returns immediately.
    void poll();

    // True while an upload is in progress, or for ~10s after one failed, so the
    // caller can stop the normal UI repaint from overwriting the status. The
    // hold keeps a wrong password (or an aborted transfer) from leaving the
    // screen dead until a manual power cycle.
    bool busy();
} // namespace HttpOta

#endif
