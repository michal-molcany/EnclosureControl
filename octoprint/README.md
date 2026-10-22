# OctoPrint-­EnclosureFan

A **Fan Control** tab inside OctoPrint for the EnclosureFanControl (C3) board: live
enclosure state, runtime PID tuning, PID calibration, and a manual fan/louvre
override.

## It is a client, not a new device

**No firmware changes are required.** The C3 has served a complete JSON API on
port 8082 since before this plugin existed (see `lib/EnclosureUi/README.md`). This
plugin is entirely OctoPrint-side: it adds a tab, a settings dialog and a
background poller.

## Why port 8082 and not 8081

Both of the C3's JSON APIs come from the same board. 8082 is used because it is a
strict superset of what this tab needs:

| | 8081 (`/api/enclosure`) | 8082 (`/api/status`) |
|---|---|---|
| CYD link state | absent | **`remote`** |
| Accepted PID ranges | absent | **`pid.min` / `pid.max`** |
| Compiled-in PID defaults | absent | **`pid.defaults`** |
| Chamber validity | `INT16_MIN` sentinel | **`chamberValid`** |
| Response to a write | the written thing | **the whole status** |

Every one of those matters here. `remote` is what gates manual control; the ranges
come from the board so they cannot drift out of sync with `enclosure_pid.h`;
`chamberValid` avoids guessing at a sentinel; and the "write returns full status"
property means a write is also a refresh, so the UI can never show a value the
board does not have.

Port 8081 is the CYD's own machine-to-machine channel and uses username `cyd`
(8082 uses `Enclosure`).

## PID calibration

The tab can run the board's relay-feedback auto-tune and follow it live, with
the same data the board's own web page shows: phase, chamber and bed
temperature, elapsed time and relay cycles.

- **Start** sits behind a confirmation panel that spells out what the run
  does: the bed goes to 90 °C, the louvre and fan oscillate around a 35 °C
  chamber setpoint for up to 60 minutes after the setpoint is reached, and the
  run aborts above 50 °C. The printer must be idle, and the board refuses to
  start otherwise - its reason is shown verbatim.
- While a run is active, the tab shows the live values and a **Cancel
  calibration** button. The PID gains and the manual controls are locked: the
  board owns the outputs for the duration and ignores every other command.
- When the run stops, the board's own reason is shown ("cancelled", "chamber
  exceeded the 50 C abort limit", ...).
- When the run finishes, the result panel shows the old and the new gains, the
  measured Ku/Tu/amplitude, and any clamping or convergence warnings, with
  **Apply**, **Apply + save to NVS** and **Discard**.

This is still only a client: the plugin posts to the board's existing
`/api/calibrate` route and reads the `cal` block of `/api/status`. **The C3
firmware must be 0.5.7 or newer** - older boards report no `cal` block at all,
and the tab then says so instead of offering a button that cannot work.

## Manual control is disabled while the CYD is linked

This is deliberate, and it is the one design decision worth explaining.

The CYD re-sends its own command **every 2 seconds**
(`ENC_CMD_HEARTBEAT_INTERVAL_MS`), and the C3 treats *any* received command as
proof the link is alive. So a manual override set while a CYD is linked is
reverted within about two seconds — silently, with the sliders snapping back.

Rather than let that be discovered by watching numbers move, the plugin:

- greys out the manual controls and shows an explanation,
- and **refuses the write server-side**, checked against the live board rather
  than the cache, so it cannot be bypassed by calling the API directly.

To take control from OctoPrint, unplug the display board. The gate re-evaluates on
every poll, so the controls return on their own once the CYD drops off.

> Verified on hardware 2026-10-19: with the CYD linked, a manual write of
> `mode=1, louvre=40, fan=55` came back echoing `louvre=0, fan=0` — the CYD had
> already won. That is the behaviour the gate exists to prevent.

## Installation

1. In OctoPrint: **Settings → Plugin Manager → Install from URL**
2. Paste the URL of `OctoPrint-EnclosureFan-<version>.zip` and install
3. **Restart OctoPrint** (required)
4. Open **Settings → Enclosure-Fan-Control** and set:
   - **Board address** — `enclosure-fan-control.local` (default), or an IP
   - **Port** — `8082`
   - **API token** — `ENC_API_TOKEN` from the firmware's `include/enclosure_api.h`
5. Press **Test connection**. It probes what is currently typed, unsaved, and says
   which of the four things went wrong: unreachable, wrong port, token rejected,
   or connected (and then shows the firmware version).

The username is fixed at `Enclosure` by the plugin. It is not a placeholder:
`WebServer::authenticate()` base64-encodes `"user:password"` and compares the
whole header, so the username is part of the credential.

The token is stored in OctoPrint's `config.yaml`, which is not in version control.
It is deliberately **not** defaulted in the plugin source. It is also never sent
back to the browser: the settings API masks it, so an empty token box after a
reload means "a token is set", and saving with the box empty keeps the stored one.

## Troubleshooting

**The tab is empty / says the credentials were rejected.** Check the port first.
8082 is the status API; **8081 is the CYD's channel** and uses a different
username, so it answers and rejects the plugin's credentials — the symptom looks
exactly like a wrong token. 8080 is the OTA uploader. *Test connection* in the
settings dialog distinguishes the two.

## Known limitations

- **Manual → Automatic sets the conservative 50 °C fallback target.** The
  `/api/manual` endpoint hardcodes `material = UNKNOWN` and `jobActive = 0` on
  the board's side ("a browser has no business claiming a job is running"). Since
  OctoPrint knows both, this is fixable: port 8081's `/api/enclosure` accepts
  `material` and `job`. Not done here because it would mean a second port and a
  second username for one field, and the effect only matters while the CYD is
  unplugged.
- **mDNS can be flaky** on some networks. An IP address in the settings works
  identically and is the fallback.
- **PID calibration needs C3 firmware 0.5.7 or newer.** Older boards do not
  report calibration state, so the section is hidden and an explanation is
  shown instead. Everything else in the plugin works on older firmware.

## Development

```
tools/selftest_plugin.py    # logic tests, no OctoPrint required (stubs the API)
tools/selftest_viewmodel.js  # front-end test, needs Node but no npm packages
tools/build_package.py      # builds dist/OctoPrint-EnclosureFan-<v>.zip
```

`selftest_plugin.py` covers the CYD-link gate, input validation, poller recovery
from transient failures, behaviour against a dead board, the token round-trip
through a settings save, the connection-test classifications, and the three
front-end bugs that once made the page look dead: a settings template bound
against the wrong Knockout root, a promise-returning API called with a callback,
and — the decisive one — the view model registered on a `DOMReady` event that
OctoPrint never fires, so none of the other work ever ran. Plus a live check
against the real C3 when one is reachable. It needs no OctoPrint installed: it
stubs `octoprint.plugin` and drives the real plugin class.

`selftest_viewmodel.js` goes one step further and actually RUNS the shipped
JavaScript: it stubs Knockout, jQuery and OctoPrint, constructs the real view
model from the real asset file and drives a whole calibration through it —
confirmation, start, live values, cancel, the board's reason, dismiss, apply.
String checks can only prove that the right names are present; this proves the
behaviour. It needs Node and no npm packages.