#include "HttpOta.h"

#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>

// Firmware version echoed by /status, shared by both boards via lib/EnclosureProto.
#include <enclosure_version.h>

// All mutable state lives in one struct. It deliberately does NOT use a member
// named "busy": the public API is HttpOta::busy(), and a file-scope variable of
// the same name would be shadowed by that function inside namespace HttpOta,
// where the identifier would decay to a function pointer (always truthy). That
// silently made busy() return true forever and called Update::abort() on every
// loop iteration, which rebooted the board.
namespace
{
    struct State
    {
        WebServer *server = nullptr;
        // Copied in begin() so callers can pass a temporary.
        String username = "enclosure";
        String password;
        // Short board label echoed by /status. Defaults to "?" so a caller that
        // does not supply one still gets well-formed output.
        const char *boardRole = "?";
        bool verbose = false;
        HttpOta::StatusFn onStatus = nullptr;

        // True while chunks are arriving, and briefly after a failure so the
        // reason stays readable instead of the UI overwriting it.
        bool uploading = false;
        // Outcome of the upload that just finished, reported in the POST
        // response so the uploader can tell a real flash from a failure.
        bool uploadOk = false;
        char errName[16] = "";
        unsigned long failMs = 0;

        // The reboot cannot happen inside the upload callback: the POST
        // response is only sent afterwards, and restarting there would
        // truncate it. Deferred to poll(), which runs outside the handler.
        bool rebootPending = false;

        unsigned long lastWriteMs = 0;
        // Diagnostics: counts upload starts, so a spontaneous one is visible
        // in /status instead of looking like a stuck flag.
        unsigned int starts = 0;
        unsigned long lastPctMs = 0;
        unsigned long lastLogMs = 0;
        unsigned int lastPct = 101;
        size_t written = 0;
        // Size of the whole multipart body, for display only. It is larger
        // than the firmware image because of the form boundaries.
        size_t contentTotal = 0;
    };

    State st;
    const unsigned long failHoldMs = 10000;
    const unsigned long stallMs = 30000;

    const char *const PAGE =
        "<!doctype html><meta charset=utf-8><title>Enclosure OTA</title>"
        "<h1>Firmware update</h1>"
        "<form method='POST' action='/update' enctype='multipart/form-data'>"
        "<input type='file' name='firmware'><input type='submit' value='Flash'>"
        "</form>";

    void status(const char *text)
    {
        if (st.onStatus)
            st.onStatus(text);
    }

    // HTTP Basic auth. An empty password disables auth entirely, mirroring the
    // rule the firmware uses for the ArduinoOTA path.
    bool authorized()
    {
        if (st.password.length() == 0)
            return true;
        if (st.server->authenticate(st.username.c_str(), st.password.c_str()))
            return true;
        st.server->requestAuthentication();
        return false;
    }

    void failed(const char *what)
    {
        st.uploading = false;
        st.uploadOk = false;
        strlcpy(st.errName, what, sizeof(st.errName));
        st.failMs = millis();
        if (st.verbose)
        {
            Serial.printf("[HOTA] FAILED: %s (%s)\n", what, Update.errorString());
        }
        status(what);
    }

    void onUpload()
    {
        HTTPUpload &up = st.server->upload();

        switch (up.status)
        {
        case UPLOAD_FILE_START:
            // Reject a genuinely concurrent upload here. It cannot be detected
            // in the POST completion handler, because the upload callback has
            // already run by then and uploading reflects the finished upload.
            if (st.uploading)
            {
                Update.abort();
                strlcpy(st.errName, "busy", sizeof(st.errName));
                st.uploadOk = false;
                return;
            }
            st.uploading = true;
            st.uploadOk = false;
            st.errName[0] = '\0';
            st.failMs = 0;
            st.starts++;
            st.lastWriteMs = millis();
            st.lastPct = 101;
            st.lastPctMs = millis();
            st.lastLogMs = millis();
            st.written = 0;
            st.contentTotal = up.totalSize;
            if (st.verbose)
            {
                Serial.printf("[HOTA] start #%u %s (%u byte body)\n", st.starts,
                              up.filename.c_str(), static_cast<unsigned>(up.totalSize));
            }
            // UPDATE_SIZE_UNKNOWN, not up.totalSize: the multipart body is
            // larger than the image, and Update.end() rejects a size mismatch.
            if (!Update.begin(UPDATE_SIZE_UNKNOWN))
            {
                failed("begin");
                return;
            }
            status("starting");
            break;

        case UPLOAD_FILE_WRITE:
        {
            const size_t n = Update.write(up.buf, up.currentSize);
            if (n != up.currentSize)
            {
                Update.abort();
                failed("write");
                return;
            }
            st.written += n;
            st.lastWriteMs = millis();
            // Logged unconditionally (not only when the displayed text changes)
            // and throttled, since a ~1 MB image arrives as hundreds of chunks.
            if (st.verbose && (millis() - st.lastLogMs) >= 1000)
            {
                st.lastLogMs = millis();
                Serial.printf("[HOTA] %u/%u written\n", static_cast<unsigned>(st.written),
                              static_cast<unsigned>(st.contentTotal));
            }
            // Small screens cannot show a fine-grained bar, and throttling to
            // 250 ms keeps redraw cost down during a fast upload. curl sends a
            // chunked body, so totalSize can be 0 here and the only honest
            // thing to show then is a running byte count.
            if (millis() - st.lastPctMs >= 250)
            {
                st.lastPctMs = millis();
                char txt[12];
                if (st.contentTotal > 0 && st.contentTotal != static_cast<size_t>(-1))
                {
                    const unsigned int pct = static_cast<unsigned int>(
                        (static_cast<unsigned long>(st.written) * 100UL) / st.contentTotal);
                    if (pct == st.lastPct)
                        return; // whole-percent only; nothing changed
                    st.lastPct = pct;
                    snprintf(txt, sizeof(txt), "%u%%", pct);
                }
                else
                {
                    snprintf(txt, sizeof(txt), "%luk",
                             static_cast<unsigned long>(st.written) / 1024UL);
                }
                status(txt);
            }
            break;
        }

        case UPLOAD_FILE_END:
            if (Update.end(true))
            {
                st.uploading = false;
                st.uploadOk = true;
                // Applied only once the response has been sent - see poll().
                st.rebootPending = true;
                if (st.verbose)
                {
                    Serial.printf("[HOTA] OK, %u bytes written, rebooting\n",
                                  static_cast<unsigned>(st.written));
                }
                status("done reboot");
            }
            else
            {
                failed("end");
            }
            break;

        case UPLOAD_FILE_ABORTED:
        default:
            Update.abort();
            failed("abort");
            break;
        }
    }
} // namespace

void HttpOta::begin(uint16_t port, const char *user, const char *pass,
                    bool verboseLog, StatusFn sink, const char *role)
{
    st.username = (user && *user) ? user : "enclosure";
    st.password = pass ? pass : "";
    st.verbose = verboseLog;
    st.onStatus = sink;
    // Points at the caller's string literal, which outlives the process.
    st.boardRole = (role && *role) ? role : "?";

    if (st.server == nullptr)
    {
        st.server = new WebServer(port);
    }
    else
    {
        st.server->stop();
        st.server->begin(port);
    }

    // The 3rd callback runs after the body is fully received; the 4th runs per
    // chunk while it is still arriving. By the time the 3rd runs, the upload
    // callback has already seen UPLOAD_FILE_END, so the outcome is known.
    st.server->on(
        "/update", HTTP_POST,
        []()
        {
            if (!authorized())
                return;
            st.server->send(st.uploadOk ? 200 : 500, "text/plain",
                            st.uploadOk ? "OK" : st.errName);
        },
        onUpload);

    // Lets an update be diagnosed without a serial cable, which matters on the
    // CYD where SERIAL_DEBUG is off.
    st.server->on(
        "/status", HTTP_GET,
        []()
        {
            if (!authorized())
                return;
            // Board role and firmware version lead the line, so a single
            // GET /status answers "which board is this, and what is running on
            // it?" without needing the serial port - which is the only way to
            // check a board that is mounted out of reach.
            char b[192];
            snprintf(b, sizeof(b),
                     "board=%s fw=v%s proto=v%d busy=%u ok=%u starts=%u written=%u total=%u age=%lums err=%s",
                     st.boardRole, ENC_FW_VERSION_STR, ENC_PROTO_VERSION,
                     st.uploading ? 1u : 0u, st.uploadOk ? 1u : 0u, st.starts,
                     static_cast<unsigned>(st.written),
                     static_cast<unsigned>(st.contentTotal),
                     static_cast<unsigned long>(millis() - st.lastWriteMs), st.errName);
            st.server->send(200, "text/plain", b);
        });

    st.server->on(
        "/", HTTP_GET,
        []()
        {
            if (!authorized())
                return;
            st.server->send(200, "text/html", PAGE);
        });

    st.server->on(
        "/favicon.ico", HTTP_GET,
        []()
        { st.server->send(204); });

    st.server->begin();
    if (st.verbose)
    {
        Serial.printf("[HOTA] listening on port %u (POST /update, auth=%u)\n",
                      static_cast<unsigned>(port), st.password.length() > 0 ? 1 : 0);
    }
}

void HttpOta::poll()
{
    if (st.server)
        st.server->handleClient();

    // Self-heal a half-open connection that started an upload and then stopped
    // sending, which would otherwise latch the flag and wedge both the endpoint
    // and the on-screen status.
    if (st.uploading && (millis() - st.lastWriteMs) > stallMs)
    {
        Update.abort();
        failed("stalled");
    }

    // Deferred so the POST response is fully sent first. Without this the new
    // image is written and marked bootable but the old code keeps running
    // until someone power-cycles the board.
    if (st.rebootPending)
    {
        st.rebootPending = false;
        delay(250); // let the response and any serial output flush
        ESP.restart();
    }
}

bool HttpOta::busy()
{
    if (st.uploading)
        return true;
    if (st.failMs != 0 && (millis() - st.failMs) < failHoldMs)
        return true;
    st.failMs = 0;
    return false;
}
