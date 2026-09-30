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

## Configuration

Copy `include/secrets.example.h` to `include/secrets.h` and fill in the real
values:

```text
copy include\secrets.example.h include\secrets.h   (Windows)
cp include/secrets.example.h include/secrets.h     (Unix)
```

Set the OctoPrint API key, server IP/port, and the OTA password:

```cpp
inline String secrets::getOctoprintApiKey() { return "OCTOPRINT API"; }
inline String secrets::getOTAPassword()     { return "OTA PASSWORD"; }
#define OCTOPRINT_IP   "192.168.0.100"
#define OCTOPRINT_PORT 5000
```

`include/secrets.h` is git-ignored; only `include/secrets.example.h` is tracked.
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
[LINK] ap ERNI Device Network ch 6 peer AC:EB:E6:4B:46:1C
[LINK] peer added on channel 6
[LINK] discovery hellosRx=0 we=ch6 NO PEER HEARD - check both boards are on the same AP and channel
[LINK] discovery: peer learned AC:EB:E6:4B:46:1C
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

Both OTA paths need the board on your network. If the C3 has no stored
credentials, the WiFiManager captive portal opens (AP `EnclosureFanControl`,
password protected) and you must enter your WiFi details once through it. The
serial log distinguishes the two states: `Connected to WiFi` followed by an IP
address means you are on the network, while `Connecting to WiFi...` with no IP
means the portal is waiting for input.

## Building

```text
pio run -e esp32-c3-devkitm-1
```

The firmware is roughly 970 KB and the default partition table provides two
1.25 MB app slots, so images up to ~1.25 MB can be sent OTA.

## Uploading from VS Code

`Ctrl+Shift+B` runs the default task, **C3: Build + OTA**, which builds and
then pushes the image to the board over HTTP. Pick the others via
**Terminal → Run Task…**:

| Task | What it does |
| --- | --- |
| `C3: Build + OTA` | build, then upload over the air (default) |
| `C3: OTA (skip build)` | upload whatever is already in `.pio/build` |
| `C3: Build only` | compile, no upload; errors land in the Problems panel |
| `C3: Flash over USB` | USB recovery flash on `COM11` |
| `C3: Monitor serial` | serial output until `Ctrl+C` |
| `C3: OTA web page` | open the board's upload form in a browser |

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

## Flashing over USB

The first upload must go over USB so the board can install OTA-enabled
firmware. The C3 uses native USB, and `ARDUINO_USB_CDC_ON_BOOT=1` routes
`Serial` to the USB port; the board enumerates as `COM11` here.

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
inbound connections.

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

