# OctoPrint-­EnclosureFan

A **Fan Control** tab inside OctoPrint for the EnclosureFanControl (C3) board: live
enclosure state, runtime PID tuning, and a manual fan/louvre override.

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
4. Open **Settings → Enclosure Fan Control** and set:
   - **Board address** — `enclosure-fan-control.local` (default), or an IP
   - **Port** — `8082`
   - **API token** — `ENC_API_TOKEN` from the firmware's `include/enclosure_api.h`

The username is fixed at `Enclosure` by the plugin. It is not a placeholder:
`WebServer::authenticate()` base64-encodes `"user:password"` and compares the
whole header, so the username is part of the credential.

The token is stored in OctoPrint's `config.yaml`, which is not in version control.
It is deliberately **not** defaulted in the plugin source.

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
- **Both boards report different firmware versions.** The C3 in this repo is on
  0.4.x while the CYD has moved ahead; nothing here depends on them matching.

## Development

```
tools/selftest_plugin.py    # logic tests, no OctoPrint required (stubs the API)
tools/build_package.py      # builds dist/OctoPrint-EnclosureFan-<v>.zip
```

`selftest_plugin.py` covers the CYD-link gate, input validation, poller recovery
from transient failures, and behaviour against a dead board — plus a live check
against the real C3 when one is reachable. It needs no OctoPrint installed: it
stubs `octoprint.plugin` and drives the real plugin class.