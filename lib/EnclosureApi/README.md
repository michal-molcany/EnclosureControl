# EnclosureApi

A second transport between the two boards, used only when ESP-NOW cannot work at
all. See `ESPNOW_PROTOCOL.md` → *HTTP fallback transport* for the full rationale.

## Why

ESP-NOW frames never leave the radio, so the two boards only hear each other when
they share a Wi-Fi channel **and** the same AP radio. Band steering, a split
2.4/5 GHz SSID, or a mesh node will put them on different radios, and the link
then fails completely. An HTTP request rides the IP network instead, which both
boards share regardless of channel or BSSID.

This is a **fallback**. ESP-NOW stays primary; the CYD only queries this API once
its ESP-NOW telemetry has gone stale.

## Two halves, built separately

| File | Provides | Used by |
| --- | --- | --- |
| `enclosure_api_server.{h,cpp}` | `GET`/`POST /api/enclosure` on a `WebServer` | C3 (`EnclosureFanControl`) |
| `enclosure_api_client.{h,cpp}` | `resolve()` / `getTelemetry()` / `postCommand()` over `HTTPClient` | CYD (`PrinterDisplay`) |

PlatformIO compiles every source in a library into whichever project includes
it, so each half is gated behind a build flag and the unused one compiles to an
empty translation unit. Without this the C3 links in `HTTPClient` + `ESPmDNS` for
a client it never calls, and the CYD links in `WebServer`.

Each project therefore defines exactly one flag in `platformio.ini`:

```ini
; EnclosureFanControl -> serves the API
build_flags = ... -D ENCLOSURE_API_SERVER=1

; PrinterDisplay -> queries the API
build_flags = ... -D ENCLOSURE_API_CLIENT=1
```

Omitting both is a compile error rather than a silently dead library.

## Configuration

Port and shared token are **not** defined here. They are shared by both firmwares
and live in the git-ignored `include/enclosure_api.h` at the repo root (copy from
`include/enclosure_api.example.h`), so the two sides cannot drift apart.

Both request types require HTTP Basic auth with `ENC_API_TOKEN` as the password.
An empty token disables the check, matching the rule `lib/HttpOta` already uses.

## Callback-based, project-agnostic

The server knows nothing about `TemperatureRegulation`; the programme supplies a
telemetry provider and a command handler. Routing API commands through
`applyRemoteCommand()` is what makes the "linked" footer, the OLED state, and the
command-link-loss fallback behave identically across both transports.