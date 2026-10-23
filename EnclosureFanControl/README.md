# EnclosureFanControl (C3)

Firmware for the **ABRobot ESP32-C3** enclosure board. It regulates the print
enclosure by driving a servo louvre and a PWM fan, and it is the "C3" half of
the ESP-NOW link with the `PrinterDisplay` (CYD) board.

- ESP32-C3 (160 MHz, 320 KB RAM, 4 MB flash), single core
- Arduino framework, PlatformIO (`esp32-c3-devkitm-1`)
- 0.42" SSD1306 OLED on I2C SDA=GPIO5 / SCL=GPIO6, address `0x3C`
- OctoPrint polled from a background FreeRTOS task; the main loop never blocks
  on a socket

Regulation behaviour and tuning are documented in
[`TEMPERATURE_REGULATION_GUIDE.md`](TEMPERATURE_REGULATION_GUIDE.md). The
CYD <-> C3 packet layout, timing and fallback rules are in
[`../ESPNOW_PROTOCOL.md`](../ESPNOW_PROTOCOL.md).

## Features

- PID enclosure regulation with material profiles (PLA/TPU/ASA/ABS) and
  emergency cooling
- Servo louvre with a minimum move interval, plus a power-on self-test
- ESP-NOW telemetry to the CYD and inbound CYD control commands
- WiFi captive portal via WiFiManager for first-time network setup
- **OTA firmware updates over WiFi** (HTTP on port 8080, plus ArduinoOTA), with
  progress shown on the OLED
- **Web UI on port 8082** — live status, PID tuning and manual fan/louvre control
  from any browser on the network
- **Relay-feedback PID auto-calibration** — preheat the bed, measure the
  chamber's ultimate gain and period, and get Ziegler-Nichols gains from the web
  UI instead of guessing

## Web UI

Open **`http://enclosure-fan-control.local:8082/`** from any browser on the same
network. Log in as user **`Enclosure`** with `ENC_API_TOKEN` as the password — the
same secret the enclosure API on 8081 uses, though that port wants the username
`cyd`. The username must be exactly `Enclosure` here: `WebServer` compares the
whole `user:password` credential, so any other username is rejected even with the
right token.

The page shows what the board is doing (chamber temperature, state, louvre and
fan, material, setpoint, whether the CYD is linked), and lets you:

- read and change the PID gains, then **Save** them to NVS or restore the
  compiled-in defaults. The header shows whether the running gains still match
  what is stored, so an unsaved tuning cannot quietly disappear on reboot;
- run a **PID calibration** (see below);
- take manual control of the fan and louvre.

Two things worth knowing before using it:

- **Its own port is deliberate.** 8081 is the CYD's fallback transport and
  `WebServer` serves one client at a time, so a browser page sharing that server
  could delay a CYD request. A second server cannot.
- **Manual control only holds while the CYD is unlinked.** The CYD re-sends its
  own mode every 2 s and wins. The page shows live link state rather than letting
  the override vanish quietly. A web command is also applied as *local*, so it
  never makes the OLED claim the display is connected.

Ports on this board: 80 WiFiManager portal, 8080 OTA, 8081 enclosure API, 8082 web
UI. See [`../lib/EnclosureUi/README.md`](../lib/EnclosureUi/README.md).

## PID calibration

The **Calibrate** button in the web UI runs a relay-feedback auto-tune — the
method ESPHome uses — and turns the measurement into Ziegler-Nichols gains.

What it does, in order:

1. Asks for confirmation, then preheats the bed to **90 °C** through OctoPrint
   and sends **M25** to the printer, which pauses it. The pause is what keeps a
   firmware-side heating timeout from switching the bed off again a few
   minutes into the run - the same timeout that would otherwise abort it.
2. Waits for the bed to reach 88 °C (5-minute timeout), then lets it settle
   briefly.
3. Drives the louvre and fan between **no cooling** and **full cooling** (output
   0 and 200) with a 0.5 °C hysteresis, around a fixed **35 °C** chamber
   setpoint.
4. Measures the resulting oscillation: the ultimate gain `Ku = 4d / (pi*a)` and
   the period `Tu`, then computes `kp = 0.6*Ku`, `ki = 1.2*Ku/Tu`,
   `kd = 0.075*Ku*Tu`.
5. Switches the bed off, sends **M84** - which ends the pause and turns the
   heaters off, so the printer is left ready for the next print - and shows the
   old and new gains, `Ku`, `Tu`, the measured amplitude, and any quality
   warnings.
6. Offers **Apply** (RAM only), **Apply + save to NVS**, or **Discard**.

Safety and limits:

- The printer must be **idle** (no print, no pause) and the chamber below 45 °C
  at the start. The **Calibrate** button is disabled while a job is active.
- The 60-minute experiment window starts when the chamber first reaches 35 °C,
  not at the button press; a separate 30-minute backstop aborts a run whose
  chamber never gets there. Once the setpoint is reached, the full window is
  available however long the climb took. The run aborts above **50 °C** chamber,
  if the bed is not at temperature within 5 minutes, if OctoPrint data goes stale
  for 30 s, if a print starts while it runs, or on **Cancel**.
- Every exit path switches the bed off, sends M84 and enters the normal
  full-cooling state. The 55 °C emergency path stays armed and wins.
- The pause is OURS, and the board knows it: it does not read its own M25 as
  "a print started" (which would abort the run that sent it), and it does not
  refuse the next run as "not idle" because of it. The pause stops counting as
  ours as soon as the printer reports that it is not paused any more.
- A print that starts DURING a run aborts it - and then the bed-off and the M84
  are skipped, because the printer belongs to that print now: it set its own
  bed target when it started, and M84 would switch its heaters off under it.
- The CYD latches a modal calibration screen as soon as it sees an active run.
  It shows live chamber/bed readings, phase, elapsed time and cycles, blocks
  navigation, and offers one confirmed **Cancel calibration** action. This works
  over ESP-NOW and the HTTP fallback alike.
- The web UI draws the run: bed and chamber curves, the 90 °C bed target, the
  35 °C setpoint with its ±0.5 °C noise band, the relay's cooling strip, and a
  tinted band per phase. The history comes from the board, not the browser
  (`GET /api/cal/trace`, one sample every 5 s), so it is complete even when the
  page is opened late, reloaded, or left in a background tab. The same graph
  stays on screen in the result dialog, next to the gains it produced.
- While a run is active, manual controls and the CYD's 2-second command stream
  are **ignored** (the CYD link is still reported as live), except for that
  dedicated cancel action; they resume when the run ends. The OctoPrint address,
  key and reset controls are also locked, because the bed-off has to reach the
  server the run started with.
- The computed `kd` is usually larger than `PID_KD_MAX` (50) and is clamped; `kp`
  can clamp too on a small oscillation. The result dialog says so.
- The bed-off is sent through OctoPrint like the preheat. If the link is down at
  that moment, the request is queued and sent when it returns. If the **board
  loses power** mid-run, OctoPrint keeps the last bed target it received; check
  the printer when the board comes back.

The derivation, the quality gates and a host-side simulation are in
[`TEMPERATURE_REGULATION_GUIDE.md`](TEMPERATURE_REGULATION_GUIDE.md#pid-auto-calibration)
and `test/pid_calibration_sim.py`.

## Changing the OctoPrint address

The **OctoPrint address** card in the web UI, or:

```bash
C3=http://<c3-host>:8082
AUTH=Enclosure:$ENC_API_TOKEN           # 8082 is the "Enclosure" user; 8081 is "cyd"
JSON='Content-Type: application/json'   # required on POSTs with a body

curl -u "$AUTH" $C3/api/status                      # current address + whether it is overridden
curl -u "$AUTH" -H "$JSON" -X POST -d '{"ip":"192.168.1.88","port":80}' $C3/api/octoprint
curl -u "$AUTH" -X POST $C3/api/octoprint/defaults  # back to include/octoprint_endpoint.h
```

- **Checked before it is saved.** The board GETs `/api/version` on the candidate
  address and stores nothing unless it answers. This is not fussiness: unreachable
  OctoPrint data is treated as invalid, which trips the cooling fail-safe — so a
  typo would silently stop cooling rather than fail loudly at the keyboard.
- **Stored in NVS**, so it survives a power cycle and can be replaced whenever you
  like. `api/status` reports `octoOverridden` so it is always clear whether the
  board is on the saved address or the compiled-in `OCTOPRINT_IP`.
- **Pushed to the CYD**, which stores it in its own NVS and repoints its poller.
  Telemetry carries the address every second, so a CYD that was off during the
  change catches up within a second. Clearing the override here clears it there
  too. Both boards must be flashed together when the protocol revision changes -
  see [`../ESPNOW_PROTOCOL.md`](../ESPNOW_PROTOCOL.md) → *Protocol revision 4*.
- An IPv4 address only, not a hostname: the OctoPrint client connects to an
  `IPAddress`, so a name would need DNS resolution added first.

## This board is updated over the air

**USB is not used to update this board, and is not needed for a first flash** — it
already runs OTA-capable firmware. Over-the-air is the normal path; USB exists only
for recovery, and is described as such below.

Consequences worth knowing up front:

- Reach the board by its **mDNS name** (`enclosure-fan-control.local`), not by IP.
  A DHCP address change then needs no action. See the warning about mDNS below
  before changing anything about `ArduinoOTA.begin()`.
- There is no serial cable in the loop, so **`/status` replaces the serial log**
  for checking reachability and diagnosing an update.
- An interrupted update is safe: the image is written to the *inactive* OTA slot,
  so the running firmware stays bootable. It takes two consecutive failed updates
  to strand the board.

Check the board is reachable and idle before pushing anything:

```text
curl -u enclosure:YOUR_OTA_PASSWORD http://enclosure-fan-control.local:8080/status
```

```text
busy=0 ok=0 starts=0 written=0 total=0 age=5396946ms err=
```

`busy=0` means nothing is in flight. `ok=1` means the last completed upload
succeeded, and `err=` names the reason if one failed. `starts=0` means the board
has not been updated since boot.

One trap: `age=` is time since the last chunk was written, so **before any upload
it is just board uptime**, not the time since a last update. A large `age` on a
board reporting `starts=0` is a board that has been running a while, not one that
is stuck.

## Configuration

Copy `include/secrets.example.h` to `include/secrets.h` and fill in the real
values:

```text
copy include\secrets.example.h include\secrets.h   (Windows)
cp include/secrets.example.h include/secrets.h     (Unix)
```

Set the OctoPrint API key and the OTA password:

```cpp
inline String secrets::getOctoprintApiKey() { return "OCTOPRINT API"; }
inline String secrets::getOTAPassword()     { return "OTA PASSWORD"; }
```

The OctoPrint server address is **not** in `secrets.h`. It is shared with the
CYD (PrinterDisplay) firmware and lives in one git-ignored header at the repo
root, so both boards cannot drift to different servers:

```text
copy include\octoprint_endpoint.example.h include\octoprint_endpoint.h
```

```cpp
#define OCTOPRINT_IP "192.168.1.xxx" // OctoPrint server IP address
#define OCTOPRINT_PORT 80            // OctoPrint server port
```

`include/secrets.h` is git-ignored; only `include/secrets.example.h` is tracked.

### The address and API key are editable at runtime

Both are set **together**, from the web UI on port 8082, and verified with a
`/api/version` probe before either is stored.

`getOctoprintApiKey()` is the **fallback**, not the only place the key lives.
Precedence is **stored override → `secrets.h`**, so you can leave it holding a
placeholder and configure the real key in the browser — useful when the key
changes often enough that recompiling is annoying, or when flashing a board for
someone else. Leaving the key field blank on a submit keeps the key already in
force, so an address-only change is still one step. The page shows which of the
two is in force; *Reset both to firmware defaults* drops the overrides and puts
`octoEndpoint.h` and `secrets.h` back in charge.

They have to be one operation. Moving to a different OctoPrint server means a new
address *and* a new key, and a probe can only use one stored value plus one
candidate — so setting either half on its own tests it against the other half and
fails. That would leave a moved server unreachable without a serial flash.

The key is stored separately from the address on purpose: `octoEndpointClear()`
wipes the whole `octoend` namespace, so sharing one would mean resetting the
address silently discarded the key.

Setting it in the page also sends it to the CYD, so the display stops holding its
own compiled-in copy. That is a credential crossing the radio link, so both boards
encrypt ESP-NOW with a shared LTK from `include/espnow_key.h` — see below for why
that matters and what it still does not cover.

### The ESP-NOW link key (LTK)

A git-ignored header at the repo root, **shared with the CYD and required by both
builds**:

```text
copy include\espnow_key.example.h include\espnow_key.h
```

ESP-NOW frames never pass through the access point, so WPA2 never protected them —
with `peer.encrypt = false` every telemetry frame was plaintext on the air, which
matters now that telemetry carries the OctoPrint API key. Both boards set
`peer.encrypt = true` with the 16-byte key from that header.

**If the two boards hold different keys the link is simply dead**, with no error
anywhere, and it looks exactly like a channel split or a missing peer. Both boards
therefore print `espnow encryption ON` at boot outside `SERIAL_DEBUG` — check that
first when the link will not come up. Changing the key requires reflashing both
boards.

Generate it with `python -c "import secrets;print(secrets.token_bytes(16).hex())"`.
See [`../include/espnow_key.example.h`](../include/espnow_key.example.h) for what
link encryption does *not* protect: it is configured on the send side, so inbound
command forgery is still possible.

### The shared API token

`ENC_API_TOKEN` in `include/enclosure_api.h` guards both this board's enclosure API
(port 8081) and its browser page (port 8082). It is a bearer token sent in the
clear on every request, so use a long random value:

```text
python -c "import secrets;print(secrets.token_hex(32))"
```

Changing it requires reflashing both boards, and the browser prompt asks for the
new value.
Never commit real credentials.

WiFi credentials are not stored in `secrets.h`. The board opens a WiFiManager
captive portal on boot (AP `EnclosureFanControl`, password protected) if it
cannot join a known network, and the entered SSID/password are persisted in NVS.

`ENC_PEER_MAC` in `include/const.h` is the CYD's Wi-Fi MAC. It is only the
*starting* value: both boards also exchange a broadcast hello every 2 s and
adopt each other automatically, so a wrong or stale MAC here repairs itself
within seconds instead of leaving a permanently dead link. When it is all zeros
the board has no initial peer and relies purely on discovery.

Note that the OLED only shows the pairing screen for an *all-zero* peer MAC. A
MAC that is merely **wrong** looks identical to a healthy board on screen, which
is why the serial log is the place to check the link.

### Pairing with the CYD

ESP-NOW only works between two boards on the **same AP and the same channel**.
The link prints a status line every 5 s and a discovery summary every 10 s:

```text
[LINK] own MAC: 7C:9E:BD:12:34:56
[LINK] ap ExampleNet ch 6 peer AA:BB:CC:DD:EE:FF
[LINK] peer added on channel 6
[LINK] discovery hellosRx=0 we=ch6 NO PEER HEARD - check both boards are on the same AP and channel
[LINK] discovery: peer learned AA:BB:CC:DD:EE:FF
[LINK] ch=6 rx=14 tx=61/0 drop=0 last=- age=1002ms
```

- `ap` / `ch` — the network and channel this board actually settled on. The SSID
  is printed because it comes from NVS via WiFiManager and is otherwise never
  shown, so an AP mismatch is otherwise invisible.
- **`we=ch` is the single most important number here.** ESP-NOW frames never
  leave the radio, so if the CYD prints a different `we=ch`, the boards are on
  different channels and nothing will ever cross. **This is the failure that
  actually occurred**: the C3 came up on channel 11 and the CYD on channel 1, and
  the signature was `tx=0/1120` — 1120 sends, zero acknowledged.
  `tx=0/N` with a large N always means a channel split. The fix is network-side:
  one 2.4 GHz AP on a single fixed channel, move the boards closer together so
  they associate to the same AP, or re-provision this board through the captive
  portal onto an SSID on the same channel. See
  [`../ESPNOW_PROTOCOL.md`](../ESPNOW_PROTOCOL.md) for the full decision tree.
- `hellosRx=0 ... NO PEER HEARD` for a minute means not paired at all. Once it
  climbs, the link comes up on its own.
- `ch=`, `rx=`, `drop=`, `last=` describe the unicast link. `last=sender` means
  frames are arriving from a board that is not the current peer.

`ENC_WIFI_CHANNEL` in `include/const.h` can pin the channel, but it is best
effort and off by default: the AP re-synchronises the station on every beacon,
so a pin it will not honour either does nothing or drops Wi-Fi entirely. If you
do set it, set it to the same value as in `PrinterDisplay/include/const.h`.


The OLED footer (`+CYD OK` / `NO CYD`) tracks the same `rx=` counter, so it is a
quick way to confirm the CYD→C3 direction without a serial cable.

Full troubleshooting matrix: [`../ESPNOW_PROTOCOL.md`](../ESPNOW_PROTOCOL.md).

Both OTA paths need the board on your network, reached by its mDNS name
(`enclosure-fan-control.local`) rather than by IP. The captive portal is a
first-time-only concern: if the C3 has no stored credentials, the WiFiManager
portal opens (AP `EnclosureFanControl`, password protected) and you must enter your
WiFi details once through it. Without a serial cable, the way to tell the two
states apart is whether `/status` answers at all — a board still sitting in the
portal will not be on the network to serve it.

## Building

```text
pio run -e esp32-c3-devkitm-1
```

The firmware is roughly 975 KB and the default partition table provides two
1.25 MB app slots, so images up to ~1.25 MB can be sent OTA. The current build
sits at 74.4% of its slot, leaving about 335 KB of headroom.

## Uploading from VS Code

`Ctrl+Shift+B` runs the default task, **C3: Build + OTA**, which builds and
then pushes the image to the board over HTTP. Pick the others via
**Terminal → Run Task…**:

| Task | What it does |
| --- | --- |
| `C3: Build + OTA` | build, then upload over the air (default) |
| `C3: OTA (skip build)` | upload whatever is already in `.pio/build` |
| `C3: Build only` | compile, no upload; errors land in the Problems panel |
| `C3: Flash over USB` | USB **recovery** flash on `COM11` |
| `C3: Monitor serial` | serial output until `Ctrl+C` |
| `C3: OTA web page` | open the board's upload form in a browser |

`C3: Monitor serial` and `C3: Flash over USB` need the board cabled to the PC.
While it is not, use the `/status` endpoint instead — it reports the same
reachability and upload outcome without a cable.

> **The PlatformIO toolbar's "Upload" button only ever flashes over USB.** It
> cannot do OTA, which is the most likely way to accidentally reach for the
> wrong control. Use the tasks above for over-the-air updates.

These are defined in `.vscode/tasks.json` and drive the shared
[`../tools/ota-upload.ps1`](../tools/ota-upload.ps1) — the same script serves
the CYD, parameterised by project. It can also be run directly:

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools\ota-upload.ps1 -Project EnclosureFanControl -Env esp32-c3-devkitm-1 -Device enclosure-fan-control.local
powershell -NoProfile -ExecutionPolicy Bypass -File tools\ota-upload.ps1 -Project EnclosureFanControl -Env esp32-c3-devkitm-1 -Device enclosure-fan-control.local -SkipBuild
powershell -NoProfile -ExecutionPolicy Bypass -File tools\ota-upload.ps1 -Project EnclosureFanControl -Env esp32-c3-devkitm-1 -Device 10.201.93.245
```

The script reads the OTA password out of the git-ignored `include/secrets.h`, so
no credential is ever stored in a tracked file. It resolves the board by mDNS
name, so a DHCP address change needs no action, and it **refuses to upload if the
build failed** rather than pushing the previous binary.

> **mDNS is a side effect of `ArduinoOTA.begin()`, and nothing else starts it.**
> No code in this firmware calls `MDNS.begin()` directly; the name is registered
> because `ArduinoOTA.begin()` calls it internally with the hostname set by
> `ArduinoOTA.setHostname(OTA_ARDUINO_HOSTNAME)`.
>
> Two consequences, both silent:
>
> - Removing `ArduinoOTA.begin()` — an easy thing to do while chasing a few KB of
>   flash — breaks OTA-by-**name** on this board *and* on the CYD. The uploader
>   would fall back to needing `-Device <ip>`, which then breaks on any DHCP
>   change. If ArduinoOTA ever goes, mDNS has to be started explicitly with
>   `MDNS.begin(OTA_ARDUINO_HOSTNAME)` first.
> - This is why the board should always be addressed by
>   `enclosure-fan-control.local` and not by IP.

## Flashing over USB (recovery only)

**Not the normal path, and not needed for a first flash.** The board already runs
OTA-capable firmware, so there is no bootstrap step that requires USB. Use it only
when the board cannot be reached over the network, or when both app slots are bad.

The C3 uses native USB, and `ARDUINO_USB_CDC_ON_BOOT=1` routes `Serial` to the USB
port; the board enumerates as `COM11` when it is cabled.

```text
pio run -t upload -e esp32-c3-devkitm-1 --upload-port COM11
```

Two independent quirks of this board's C3 native USB are handled in
`platformio.ini`. Both must stay in place, and neither is a sign of a broken
board.

**1. `upload_speed = 115200`.** The C3 boots its ROM loader at 115200. Any
higher requested speed makes esptool send a `CHANGE_BAUDRATE` (0x0F) command
after the stub loads, which the USB-Serial-JTAG peripheral does not survive:

```text
Stub running...
Changing baud rate to 460800
A fatal error occurred: No serial data received.
```

At 115200 esptool syncs at 115200 and skips the command entirely.

**2. `upload_flags = --no-stub`.** Fixing the baud rate is not enough. The
esptool flasher stub loads and then immediately dies reading the SPI flash:

```text
Stub running...
A fatal error occurred: Unable to verify flash chip connection (No serial data received.)
```

The flash itself is healthy — `--no-stub flash_id` reports `Manufacturer: 46`,
`Device: 4016`, `Detected flash size: 4MB` from the same board. Only the stub
fails, so `--no-stub` talks to the ROM bootloader directly and the write
succeeds and verifies. Because `--no-stub` disables `-z` compression, expect a
~970 KB image to take roughly **40 seconds** at 115200.

`upload_flags` is *prepended* to `UPLOADERFLAGS`, which places `--no-stub`
before the `write_flash` subcommand where esptool's parser requires it. This
matters if you ever move the setting into a `platformio.ini` for another
toolchain: appending it after the subcommand makes esptool reject the argument.

`monitor_dtr`/`monitor_rts` are `0` on purpose so opening the serial monitor
does not toggle the C3 into bootloader mode.

### If the upload still fails

Work down this list:

1. Force download mode manually: hold **BOOT**, tap **RST**, release **BOOT**,
   then upload with `--upload-flags --before no_reset`.
2. Skip chip autodetection: `--upload-flags --chip esp32c3`.
3. Confirm the board is alive before retrying — open the monitor on `COM11` at
   115200 and press **RST**; you should see the boot log.

## OTA Updates

Two paths are available. **Use HTTP** unless you know the uploader can accept
inbound connections. HTTP is also the only one that works from a PC behind a
default Windows Firewall, because the board listens and the PC connects outbound
rather than the other way round.

### If OTA is the only path to this board

With no serial cable and no USB, three things are worth stating explicitly:

- **An interrupted update is recoverable.** The image is written to the inactive
  OTA slot, so the partition the board is currently running from is untouched. A
  push that dies halfway leaves a still-bootable board on its old firmware; it
  takes two consecutive failed updates to strand it. Do not panic halfway through
  a transfer and reset the board — let it finish or let it fail.
- **The credential crosses the network in cleartext.** The password is HTTP Basic
  auth over plain HTTP, and so is the firmware image. That is a deliberate
  trade-off for a trusted LAN, but it means anyone on the network can *attempt* a
  push; the password is the only thing stopping them. Rotating
  `secrets::getOTAPassword()` is the only control available.
- **Reachability depends on the name, not the address.** If `mDNS` stops working —
  see the warning above — the board can only be addressed by IP, and a DHCP change
  then makes it unreachable until one is rediscovered.

### HTTP (recommended — no firewall changes needed)

The board listens on port **8080** and the uploader makes an ordinary *outbound*
request, so a default Windows Firewall does not interfere. A browser form is
served at `/`, or upload directly:

```text
curl -u enclosure:YOUR_OTA_PASSWORD -F "firmware=@firmware.bin" http://<board-ip>:8080/update
```

The password is HTTP Basic auth, reusing `secrets::getOTAPassword()` — the same
value that guards the ArduinoOTA path. An empty password disables auth entirely.
`enclosure-fan-control.local:8080` works too if mDNS resolves. The board writes
the image to the inactive OTA slot, validates it, and reboots. A ~1 MB image
takes about **5 seconds** on a normal WiFi network:

```text
[HOTA] start firmware.bin (0 byte body)
[HOTA] 180936/0 written
[HOTA] 380540/0 written
[HOTA] 575836/0 written
[HOTA] 771132/0 written
[HOTA] 954940/0 written
[HOTA] OK, 1020144 bytes written, rebooting
```

The trailing total reads `0` because `curl` sends a chunked body and the total
size is not known up front — which is also why the OLED shows a running KB count
rather than a percentage in that case.

The response body is `OK` (HTTP 200) or the short failure name — `end`,
`write`, `begin`, `stalled`, `busy` (HTTP 500) — so the uploader can tell a real
flash from a failed one. On success the board **reboots itself** once the
response has been sent, so the new image is the one actually running.

`GET /status` reports the same state without uploading, which is useful
because the display cannot be read over the network:

```text
curl -u enclosure:YOUR_OTA_PASSWORD http://<board>:8080/status
busy=0 ok=0 starts=0 written=0 total=0 age=54905ms err=
```

`starts` counts upload attempts since boot and `age` is time since the last
chunk, so a rising `age` means the board is up and idle rather than wedged. A
transfer that stops mid-stream is abandoned after 30s (`err=stalled`), so a
half-open connection cannot freeze the OLED.

The endpoint lives in the shared [`../lib/HttpOta`](../lib/HttpOta) library,
which the CYD uses too, so the auth and flash logic exist once.

The OLED shows `HOTA / starting`, then a running KB count or percentage
(whichever the transfer supports), then `HOTA / done reboot`. The same progress
is printed on serial about once a second, so a headless update is observable.

### ArduinoOTA (may be blocked by the host firewall)

The board also serves ArduinoOTA at mDNS hostname **`enclosure-fan-control.local`**
on port **3232**:

```text
pio run -t upload -e esp32-c3-devkitm-1 --upload-protocol espota --upload-port enclosure-fan-control.local --upload-flags --auth=YOUR_OTA_PASSWORD
```

If mDNS is unavailable, use the IP address printed at boot.

**This path often fails on Windows.** ArduinoOTA always makes the *board* dial
out to the uploader, so the uploader's PC must accept an inbound connection.
A default Windows Firewall blocks that, and the device reports:

```text
[OTA] failed: connect (2)      <- OTA_CONNECT_ERROR
[OTA] failed: end (4)         <- OTA_END_ERROR
[E][ArduinoOTA.cpp:360] _runUpdate(): Update ERROR: Aborted
```

The `Authenticating...OK` line still appears, because the UDP handshake is
allowed as a reply to the uploader's own outbound flow — only the new inbound
TCP connection is dropped. Changing the port does **not** help: the default
policy is block-all-inbound, not block-by-port, and port 80 fails identically.
Allowing a port is a per-machine decision, so the HTTP path above avoids it
entirely by reversing the direction.

### On-screen behaviour

Both paths report progress on the 72x40 OLED instead of the chamber view:

| Screen | Meaning |
| --- | --- |
| `OTA / starting`, `HOTA / starting` | Authenticated, image transfer beginning |
| `HOTA / nn%` or `nnk` | Transfer progress |
| `... / done reboot` | Image written; the board reboots automatically |
| `OTA / auth` | `OTA_AUTH_ERROR` — the `--auth` password did not match |
| `OTA / begin` | `OTA_BEGIN_ERROR` — could not start the update |
| `OTA / connect` | `OTA_CONNECT_ERROR` — host firewall blocked the callback |
| `OTA / recv` | `OTA_RECEIVE_ERROR` — the image transfer was interrupted |
| `OTA / end` | `OTA_END_ERROR` — the image failed validation after writing |

A failed update holds the error for 10 seconds and then returns to the normal
chamber display, so a bad password does not leave the screen dead until a manual
power cycle. Failures are always printed on serial as `[OTA] failed: ...` or
`[HOTA] FAILED: ...`.

### During an update

Regulation is suspended for the few seconds the OTA slot is erased and written,
and the reboot runs the normal `louver.selfTest()`. The servo and fan hold their
last hardware PWM state throughout, so nothing snaps shut — but an update
briefly removes enclosure temperature control, so avoid starting one close to a
print's material-change step. Both paths use the `default.csv` partition table
(two 1.25 MB app slots); if the firmware ever exceeds 1.25 MB a custom
`board_build.partitions` is required, and the build output's flash percentage
shows the remaining headroom.

