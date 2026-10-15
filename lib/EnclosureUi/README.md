# EnclosureUi

A browser page served by the EnclosureFanControl (C3): live status, PID gain
tuning, and manual fan/louvre control.

Open **`http://enclosure-fan-control.local:8082/`** (or the board's IP). The
browser asks for credentials: username **`cyd`**, password **`ENC_API_TOKEN`** from
`include/enclosure_api.h` — the same secret the enclosure API uses. The browser
then attaches those credentials to every request on its own, which is why the
token never appears in the page's JavaScript, in a URL, or in browser storage.

> The username is **`cyd`** and must match exactly. `WebServer::authenticate()`
> base64-encodes `"<user>:<password>"` and compares the whole header, so the
> username is part of the credential — any other username is rejected even with
> the right password. (`cyd` is a leftover from when the CYD was the only
> client; it is no longer descriptive, but changing it would lock out every
> deployed browser and both firmwares' clients.)

## Why a page

Tuning over `curl` means remembering the token, the port and the JSON shapes,
and it gives no feedback until after the fact: you cannot see which gains are
actually in force, whether they match what is in NVS, or what the chamber is
doing while you change them. The JSON API already existed; this is only a face
for it.

## Why its own port

`lib/EnclosureApi` already runs a `WebServer` on `ENC_API_PORT` (8081). That one
is the CYD's **fallback transport** — the CYD queries it only when ESP-NOW is
stale, and it does so on a deadline (1.2 s timeout, then a backoff).

`WebServer` is synchronous: one client at a time, each request served inline
from `poll()`. A browser page sharing that server would sit in front of the CYD
for the length of a page load plus every poll interval. So this library starts a
**second** server on `ENC_UI_PORT` (8082, in `EnclosureFanControl/include/const.h`).
A phone left open on the page cannot delay a single API call.

Ports in use on the C3: 80 WiFiManager, 8080 HttpOta, 8081 enclosure API, 8082
this page.

## Why the page is a string constant

Neither board has a LittleFS partition, and adding one (partition table, data
upload step, flash offset to get wrong) costs far more than embedding the page.
It is also more reliable: with no filesystem there is no state in which the page
is missing and the firmware has to decide what to serve instead.

`PROGMEM` is deliberately absent from the page array. On ESP32 it expands to
nothing and `pgm_read_byte()` is a plain dereference — `.rodata` is
memory-mapped flash — so a `const char[]` is already readable in place. What does
matter is that the page is never **copied** into RAM, which is why it is served
with `send_P()` (streams from flash) rather than `send()` (builds a `String`).

## Routes

| Route | Method | Purpose |
| --- | --- | --- |
| `/` | GET | the page |
| `/api/status` | GET | everything the page renders, one round trip |
| `/api/pid` | POST | apply gains (RAM only) |
| `/api/pid/save` | POST | persist the running gains to NVS |
| `/api/pid/defaults` | POST | restore compiled-in defaults and persist |
| `/api/manual` | POST | `{mode, louvre, fan}` |
| `/api/octoprint` | POST | `{ip, port}` — **probes first**, stores only on success |
| `/api/octoprint/defaults` | POST | drop the stored address, return to the compiled default |

Every response — including the ones to a write — is the same document
`/api/status` returns, so the page can never end up showing a value the board
does not have.

### `Content-Type: application/json` is required on POSTs with a body

```bash
curl -u "$AUTH" -H 'Content-Type: application/json' -X POST -d '{"kp":6,"ki":0.1,"kd":0}' "$UI/api/pid"
```

`curl -d` alone sends `application/x-www-form-urlencoded`, which makes
`WebServer` hand the body to its form parser instead of `arg("plain")`. A JSON
string contains no `=`, so the parser drops it and the body is **gone** before the
handler runs — the request fails with `bad json` and nothing was applied. The
browser always sets the header, so the page is unaffected; this only bites
command-line callers. `/api/pid/save` and `/api/pid/defaults` send no body and
need no header.

The same applies to `/api/pid` on port 8081 (`lib/EnclosureApi`), which shares
the framework behaviour.

`/api/status` also reports the accepted gain ranges and the compiled-in
defaults, read from `lib/EnclosureProto/src/enclosure_pid.h` rather than
duplicated in the HTML, plus `pidSaved` (does NVS still match the running gains?)
and board facts: RSSI, free heap, uptime, IP, firmware version.

## The OctoPrint card

Setting the OctoPrint address is the one action in this page that can stop the
enclosure being cooled, so it is gated: **Check and save** makes the board GET
`/api/version` on the candidate address first, and stores nothing unless that
answers. A mistyped address is refused with the reason rather than becoming a
fail-safe fan-off twenty minutes into a print.

The probe lives in the programme, not here, because only the programme holds the
OctoPrint API key needed to identify itself. It blocks the request for up to the
programme's timeout — it is never on the 2 s poll path.

This library only does the parts that are purely mechanical: rejecting a malformed
or impossible address before spending a probe on it (`0.0.0.0` is refused as well
as rejected as nonsense — it is also the "no override" sentinel on the ESP-NOW
link, so accepting it would turn "clear the override" into an override to
nowhere), and reporting the outcome.

The address is owned by whichever board runs this page and is pushed to the other
board over ESP-NOW; see `ESPNOW_PROTOCOL.md`.

## Manual control, and the CYD

`POST /api/manual` is a **local** command. It goes through
`TemperatureRegulation::applyLocalCommand()`, not `applyRemoteCommand()`, because
the latter stamps the remote-link liveness — routing a browser click through it
would make the OLED claim the display is connected and would suppress the
command-link-loss fallback that exists to catch a real disconnection.

Even so, a manual change made in the page only **holds** while the CYD is
unlinked: the display re-sends its own mode every 2 s, and that wins. The page
shows live link state and says so, rather than letting the override vanish
quietly.

## Files

| File | Provides |
| --- | --- |
| `src/enclosure_ui.{h,cpp}` | `begin()` / `poll()`, the JSON routes, Basic auth |
| `src/enclosure_ui_page.h` | the HTML page as one string constant |

Callback-based and project-agnostic, like `lib/EnclosureApi`: the library knows
nothing about `TemperatureRegulation`, the OLED or ESP-NOW. Every callback is
optional — without the PID callbacks `/api/pid` answers 501 rather than failing
silently.