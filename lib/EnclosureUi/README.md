# EnclosureUi

A browser page served by the EnclosureFanControl (C3): live status, PID gain
tuning, and manual fan/louvre control.

Open **`http://enclosure-fan-control.local:8082/`** (or the board's IP). The
browser asks for credentials: username **`Enclosure`**, password
**`ENC_API_TOKEN`** from `include/enclosure_api.h` — the same secret the
enclosure API on port 8081 uses, but *not* the same username (8081 wants `cyd`).
The browser then attaches those credentials to every request on its own, which is
why the token never appears in the page's JavaScript, in a URL, or in browser
storage.

> The username is **`Enclosure`** and must match exactly.
> `WebServer::authenticate()` base64-encodes `"<user>:<password>"` and compares
> the whole header, so the username is part of the credential — any other
> username is rejected even with the right password. It was `cyd` while the CYD
> was the only client; the OctoPrint plugin fixes its username at `Enclosure`,
> which is why the two ports now differ. Changing it again would lock out the
> plugin and every deployed browser client.

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
| `/api/calibrate` | POST | `{action: start / cancel / apply / discard}` — 409 with the reason when refused |
| `/api/cal/trace` | GET | the current/last calibration run's temperature history (see below) |
| `/api/manual` | POST | `{mode, louvre, fan}` |
| `/api/octoprint` | POST | `{ip, port[, key]}` — **probes first**, stores only on success |
| `/api/octoprint/defaults` | POST | drop **both** stored overrides, return to the compiled-in ones |

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

Address **and** API key are one setting, entered together and submitted as one
request, and that is load-bearing rather than tidy. A different OctoPrint server
means a different address *and* a different key, but a probe can only ever use
one stored value plus one candidate:

- save the new address alone → probed with the **old** key → `401` → refused
- save the new key alone → probed against the **old** address → refused

Neither half can ever be applied first, so separating them does not merely make
the form awkward — it makes a moved server unreachable without a serial flash.
`POST /api/octoprint` takes `{ip, port, key}` and probes the candidate pair.

An **omitted or empty `key` means "keep the key already in force"**, which leaves
a plain address change as a one-step operation. The page shows which key is in
force (a badge), so "which key will this submit use?" is always answerable even
though the key itself is not displayed.

Setting the pair is gated for the same reason the address always was — this is
the one action in the page that can stop the enclosure being cooled. **Check and
save** makes the board GET `/api/version` with the candidate credentials first and
stores nothing unless that answers. A wrong address stops the board reading the
printer; a wrong *key* does the same **and looks like a printer fault**, because
OctoPrint answers `401` to every request — so an unverified key would trade a
clear "the key was refused" message for a silent loss of cooling twenty minutes
into a print.

The probe lives in the programme, not here, because only the programme holds the
credentials needed to identify itself. It blocks the request for up to the
programme's timeout — it is never on the 2 s poll path.

This library only does the parts that are purely mechanical: rejecting a malformed
or impossible address before spending a probe on it (`0.0.0.0` is refused as well
as rejected as nonsense — it is also the "no override" sentinel on the ESP-NOW
link, so accepting it would turn "clear the override" into an override to
nowhere), validating the key (length-bounded, printable ASCII), and reporting the
outcome. The CR/LF rejection is not pedantry: the key is interpolated raw into an
`X-Api-Key:` header line, so an embedded newline would end that line early and
turn the rest of the value into headers sent to the user's OctoPrint server.

**The key is never echoed back.** `/api/status` reports only `octoKeySet` and
`octoKeyOverridden` — booleans, so the page can say *where the current key came
from* — and the field is left blank, including after a successful save. The
credential therefore never enters browser memory, the DOM, devtools, or a
screenshot. This matches the rule the page already follows for `ENC_API_TOKEN`,
one step stricter: the token never reaches the page at all, and neither does this.

The address and key are owned by whichever board runs this page and are pushed to
the other board over ESP-NOW; see `ESPNOW_PROTOCOL.md`. The key travels **not in
every telemetry frame** — it rides along just after a change and then on a slow
refresh, so the credential spends as little time on the unencrypted radio as
possible. `ESPNOW_PROTOCOL.md` has the cadence and the `ENC_KEYFLAG_*` states
that make a sparse cadence safe.

Because they are one setting, `POST /api/octoprint/defaults` clears **both** — one
button, rather than two near-identical ones in one card.

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

## PID calibration

`POST /api/calibrate` takes `{"action":"start"|"cancel"|"apply"|"discard"}`:

- `start` runs the bed preheat + relay experiment. It is refused with a **409**
  and a human-readable body when the printer is busy, no valid OctoPrint data is
  available, the chamber is already above 45 °C, a run is active, or a previous
  result has not been applied or discarded.
- `cancel` aborts a run: bed off, outputs into full cooling.
- `apply` writes the measured gains to RAM and consumes the result. Persisting is
  separate, through `/api/pid/save` — the page's **Apply + save to NVS** calls
  both.
- `discard` drops the result without touching the running gains.

The 60-minute experiment window starts when the chamber first reaches 35 °C. A
separate 30-minute cap aborts a run whose chamber never gets there; once the
setpoint is reached, the full window is available however long the climb took.

`/api/status` includes a `cal` object: `active`, `hasResult`, `aborted`, `phase`
(0 none, 1 preheating, 2 settling, 3 relay, 4 done, 5 aborted), `elapsed` (ms,
total run time),
`cycles` (relay phases, each a half-cycle), `ku`, `tu`, `amp`, `delta`, the new
`kp`/`ki`/`kd`, the old `oldKp`/`oldKi`/`oldKd`, `clamped`, `applied`, `zcSym`,
`ampConv`, `warned`, `bedTarget`, `setpoint`, and `error`. The page derives the
confirm, progress and result dialogs from it; the confirmation itself is
browser-only, so the board is not asked to do anything until **Start** is
pressed.

**Calibrate** is also disabled while a job is active, with a note under the
button. While a run is active the page disables it, shows the phase, chamber,
bed, elapsed time and relay phase count, and offers **Cancel**. The result dialog
shows old versus new gains, `Ku`, `Tu`, the measured amplitude, the relay
half-span `d`, and warnings for clamped gains or a failed quality gate. **Calibrate** stays
disabled while a result is waiting and after an abort (dismiss it first).

Manual commands from the page and the CYD are ignored for the duration, because
the relay owns the outputs; the CYD link is still refreshed and reported as
live. A PID write through `/api/pid` still changes the running gains, but the
calibration result replaces them on **Apply** anyway. The OctoPrint address and
key controls are disabled too, and `/api/octoprint` and
`/api/octoprint/defaults` answer **409** during a run: the final bed-off has to
reach the server the run started with.

### The graph, and why the board records it

`GET /api/cal/trace` returns the temperature history of the current (or most
recent) run:

```json
{"interval":5,"n":120,"t":[0,5,...],"ch":[262,265,...],"bed":[251,253,...],
 "out":[0,0,...],"ph":[1,1,...]}
```

`t` is seconds since the run started, `ch`/`bed` are 0.1 °C
(`-32768` = no reading, drawn as a break in the line), `out` is the cooling
demand the run applied (0–200, so the relay's two levels and the moments it
switched are visible), and `ph` is the phase code from `/api/status`. `interval`
is the spacing between points in seconds.

The page does **not** accumulate this itself. A run lasts up to an hour, it is
usually started from the OctoPrint plugin or the CYD so the page is opened long
after the preheat began, a reload would throw the history away, and a
backgrounded tab throttles its timers to about one tick a minute. The board is
the only observer present for the whole experiment, so the regulator samples it
every 5 s into a 780-sample ring (65 minutes, 6.2 KB of static RAM) and this
route serves it.

The response is decimated to at most 320 points, every *k*-th sample plus the
most recent one. It is deliberately **not** part of `/api/status`: that document
is polled every 2 s, and a few thousand numbers folded into it would be a
stall on a server that shares `loop()` with the relay. The page fetches the
trace when the graph appears, every 10 s while a run is active, and once more on
the falling edge.

State stays in `/api/status` (`active`, `aborted`, `elapsed`, ...); this route is
series data only, so there is one authority for what the run is doing. The trace
is RAM only: it survives the end of a run (so the result dialog can show the
oscillation the gains came from) and is cleared when the next run starts, but a
reboot loses it — and a reboot aborts the run, so there is nothing left to show.

## Files

| File | Provides |
| --- | --- |
| `src/enclosure_ui.{h,cpp}` | `begin()` / `poll()`, the JSON routes, Basic auth |
| `src/enclosure_ui_page.h` | the HTML page as one string constant |
| `test/selftest_trace.js` | runs the page's own graph code against a stub canvas |

Callback-based and project-agnostic, like `lib/EnclosureApi`: the library knows
nothing about `TemperatureRegulation`, the OLED or ESP-NOW. Every callback is
optional — without the PID callbacks `/api/pid` answers 501 rather than failing
silently, without the calibration callback `/api/calibrate` does the same, and
without the trace writer `/api/cal/trace` answers 501 (the page then leaves the
graph on its "waiting for samples" message instead of showing an empty axis).