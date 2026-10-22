# EnclosureControl

Enclosure climate control for a 3D printer: two ESP32 boards and an OctoPrint
plugin. They work together, but none of them depends on the others to boot.

- **EnclosureFanControl (C3)** - firmware for the ABRobot ESP32-C3 enclosure
  board. PID chamber regulation with a servo louvre and a PWM fan, a small OLED,
  and a web UI plus JSON API on port **8082** (live status, runtime PID tuning,
  and a relay-feedback PID auto-calibration).
- **PrinterDisplay (CYD)** - firmware for a "Cheap Yellow Display" (ESP32 + 2.8"
  TFT). Shows printer status from OctoPrint, offers enclosure controls, and keeps
  its own copy of the enclosure state so it stays useful when OctoPrint is down.
- **OctoPrint plugin** - adds an **Enclosure-Fan-Control** tab and settings page:
  live state, runtime PID tuning, manual fan/louvre override, and a connection
  test.

The C3 keeps regulating if both the CYD and OctoPrint disappear, falling back to
its own conservative policy, so a print is never left with a closed louvre
because the network dropped.

## Components

| Component | Hardware | Source |
|---|---|---|
| EnclosureFanControl (C3) | ABRobot ESP32-C3 board, SSD1306 OLED, servo + fan | [`EnclosureFanControl/`](EnclosureFanControl/README.md) |
| PrinterDisplay (CYD) | ESP32-2432S028 "Cheap Yellow Display" | [`PrinterDisplay/`](PrinterDisplay/README.md) |
| OctoPrint plugin | OctoPrint 1.11 | [`octoprint/`](octoprint/README.md) |

Both boards poll OctoPrint independently: the C3 needs the job and its material
to pick a regulation policy (chamber temperature comes from its own sensor), the
CYD needs printer status for its screen.

## How the pieces talk

| Link | Transport | Credentials |
|---|---|---|
| CYD <-> C3 | ESP-NOW; C3 telemetry about once a second, CYD command every 2 s | link key in `include/espnow_key.h` |
| CYD -> C3, fallback | HTTP `POST /api/enclosure` on port **8081** | user `cyd` |
| OctoPrint -> C3 | HTTP on port **8082** (`/api/status`, `/api/pid`, `/api/manual`) | user `Enclosure` |
| Browser -> C3 | web UI on port **8082** | user `Enclosure` |
| OTA upload -> either board | HTTP on port **8080** | user `enclosure` |

Two usernames on one board is the usual source of confusion: **8082 is the
status/PID API and uses `Enclosure`; 8081 belongs to the CYD and uses `cyd`.**
Both take `ENC_API_TOKEN` as the password. The username is compared as part of
the credential, so it cannot be anything else.

ESP-NOW frames never leave the radio, so the boards must be on the same Wi-Fi
channel and the same AP radio. When they are not, the CYD falls back to the HTTP
API. The packet layout, timing and fallback rules are in
[`ESPNOW_PROTOCOL.md`](ESPNOW_PROTOCOL.md).

## Quick start

1. **Create the configuration headers.** The tracked files are examples; the real
   ones are git-ignored and must be copied:

   ```
   include/enclosure_api.example.h       -> include/enclosure_api.h
   include/espnow_key.example.h          -> include/espnow_key.h
   include/octoprint_endpoint.example.h  -> include/octoprint_endpoint.h

   EnclosureFanControl/include/secrets.example.h -> .../include/secrets.h
   PrinterDisplay/include/secrets.h.example      -> .../include/secrets.h
   ```

   Fill in the Wi-Fi credentials, the OctoPrint API key, the ESP-NOW link key,
   and `ENC_API_TOKEN`. Use the *same* link key and token on both boards.

2. **Flash the boards.** Each project is its own PlatformIO project:

   ```
   cd EnclosureFanControl   # ESP32-C3
   cd PrinterDisplay        # ESP32 "CYD"
   ```

   - The **C3 is updated over the air** (HTTP upload on port 8080, plus
     ArduinoOTA) and already ships OTA-capable, so USB is only for recovery.
     See [`EnclosureFanControl/README.md`](EnclosureFanControl/README.md#this-board-is-updated-over-the-air).
   - The **CYD needs one USB flash** for the first OTA-capable image; after that
     it updates over the air as well. See
     [`PrinterDisplay/README.md`](PrinterDisplay/README.md#ota-updates).

3. **Install the OctoPrint plugin.** Settings -> Plugin Manager -> Install from
   URL:

   ```
   https://github.com/michal-molcany/EnclosureControl/archive/octoprint-plugin.zip
   ```

   Restart OctoPrint, then open **Settings -> Enclosure-Fan-Control** and set the
   board address (an IP, or `enclosure-fan-control.local`), port **8082**, and the
   API token. Press **Test connection** - it probes what is typed, unsaved, and
   says whether the address, the port or the token is wrong.

## Repository layout

| Path | What is in it |
|---|---|
| `EnclosureFanControl/` | C3 firmware, its web UI and regulation docs |
| `PrinterDisplay/` | CYD firmware, screens and the OctoPrint poller |
| `lib/EnclosureProto/` | Shared structs, enums and firmware version |
| `lib/EnclosureUi/` | The C3's port-8082 web UI and status/PID API |
| `lib/EnclosureApi/` | Port-8081 API server (C3) and client (CYD) |
| `lib/OctoPrinter/` | OctoPrint HTTP client used by both boards |
| `lib/HttpOta/` | The port-8080 firmware uploader used by both boards |
| `include/` | Configuration headers shared by both firmwares |
| `octoprint/` | Plugin source, its README, package builder and selftest |
| `tools/` | `ota-upload.ps1` |
| `ESPNOW_PROTOCOL.md` | The wire protocol between the boards |
| `platformio.ini` | Intentionally empty; each board has its own project file |

## What this repository does not contain

Secrets. Every `*.h` that holds a password, key, token or MAC address is
git-ignored; only the `*.example.h` files are tracked. That includes
`ENC_API_TOKEN`, which the OctoPrint plugin also needs - it is entered in the
plugin's settings and stored in OctoPrint's `config.yaml`, never in this repo.

## Known caveats

- **Manual control from OctoPrint only holds while the CYD is unlinked.** The CYD
  re-sends its own command every 2 seconds, so the C3 would silently revert an
  override. The plugin refuses the write server-side instead, and the controls
  come back on their own once the display is unplugged. See
  [`octoprint/README.md`](octoprint/README.md#manual-control-is-disabled-while-the-cyd-is-linked).
- **ESP-NOW needs the same channel and AP radio.** Band steering or a mesh node
  can put the boards on different radios; the HTTP fallback covers it.
- **mDNS is not guaranteed.** `enclosure-fan-control.local` works on most home
  networks; an IP address always works.
- **PID calibration needs an idle printer and takes up to 30 minutes.** The C3
  preheats the bed to 90 °C, runs for up to 20 minutes after the chamber reaches
  40 °C, ignores manual and CYD commands while the relay runs, and switches the
  bed off at the end. See
  [`EnclosureFanControl/README.md`](EnclosureFanControl/README.md#pid-calibration).
- **The two boards do not need matching firmware versions.** Nothing depends on
  them being equal.

## Development

- PlatformIO, `esp32-c3-devkitm-1` for the C3 and `esp32dev` for the CYD. Build
  from each project directory; VS Code tasks exist for the CYD.
- `python tools/selftest_plugin.py` in `octoprint/` exercises the plugin logic
  without needing OctoPrint installed (it stubs the API), plus a live check when
  a board and `ENC_API_TOKEN` are available.
- `python tools/build_package.py` in `octoprint/` builds the
  `dist/OctoPrint-EnclosureFan-<version>.zip` that the Plugin Manager installs.
