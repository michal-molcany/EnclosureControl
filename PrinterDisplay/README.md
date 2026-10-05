# EnclosureDisplay

## Overview

This project involves using an ESP32 microcontroller to monitor and display the status of a 3D printer connected to an OctoPrint server. The ESP32 fetches data such as temperature, print progress, and print time from the OctoPrint server over a local network and displays it on a CYD display.

## Features

- Real-time monitoring of 3D printer status
- Displays printer temperature (hotend and bed)
- Shows print progress percentage
- Displays estimated print time remaining
- Connects to OctoPrint server via WiFi
- Controls the enclosure C3 over ESP-NOW

For the exact CYD ↔ C3 packet layout, timing, fallback behavior, and security limitations, see [`ESPNOW_PROTOCOL.md`](../ESPNOW_PROTOCOL.md).

## Hardware Requirements

- ESP32 development board
- CYD (Cheap Yellow Display) compatible display
- USB cable for power and programming
- 3D printer connected to OctoPrint

## Software Requirements

- Arduino IDE
- Required libraries (see Installation)
- OctoPrint server running on your network

## Installation

1. Clone this repository
2. Open the project in Arduino IDE
3. Install the required libraries
4. Configure your WiFi and OctoPrint API settings in the config file
5. Upload to your ESP32

## Configuration

After cloning the repository, you'll need to configure both your WiFi credentials and OctoPrint connection settings.

### WiFi and API Settings

Copy the template and fill in your own values (never commit the real file):

```text
copy include\secrets.h.example include\secrets.h
```

Edit the `secrets.h` file to set:

```cpp
String secrets::getWiFiSSID()
{
    return "WIFI SSID";
}

String secrets::getWiFiPassword()
{
    return "WIFI PASSWORD";
}

String secrets::getOctoprintApiKey()
{
    return "OCTOPRINT API";
}

String secrets::getOTAPassword()
{
    return "OTA PASSWORD";
}
```

`include/secrets.h` is git-ignored. Only `include/secrets.h.example` is tracked.

### OctoPrint Server Settings

Configure the OctoPrint server IP address and port in `const.h`:

```cpp
#define OCTOTPRINT_PORT 80            // Port of the OctoPrint server
#define OCTOTPRINT_IP "192.168.1.xxx" // IP address of the OctoPrint server (change this to your OctoPrint server IP address)

```

> WARNING: Rotate your WiFi password and OctoPrint API key if they were ever committed.

## Usage

Once configured and powered on, the ESP32 will automatically connect to your WiFi network and begin polling the OctoPrint server for status updates, displaying the information on the CYD screen.

### Enclosure link (C3)

The chamber view receives louvre/fan telemetry from the EnclosureFanControl C3
over ESP-NOW.

**You do not need to configure the C3's MAC to use this.** Both boards register
the ESP-NOW broadcast address and exchange a short hello every 2 s; whichever
board hears a hello from the other adopts it as its peer. `ENC_PEER_MAC` in
`include/const.h` is only the starting value, so a wrong or stale one repairs
itself within a couple of seconds instead of producing a permanently dead link.

ESP-NOW frames never leave the radio, so this works only between boards on the
**same AP and the same Wi-Fi channel**. "Same SSID" does not guarantee that — an
enterprise AP with band steering, split 2.4/5 GHz SSIDs, or several mesh nodes on
different channels will happily put the two boards on different ones. That is
the failure this setup actually hit: the C3 came up on channel 11 and the CYD on
channel 1, so 1120 transmissions were sent and not one was acknowledged.

The CYD reports its channel at boot and continuously thereafter:

```text
[ENC-LINK] own MAC: 5C:01:3B:8A:63:54
[ENC-LINK] joined on channel 1
[ENC-LINK] ch=1 rx=0 tx=5/5 drop=0 last=- age=0ms
[ENC-LINK] discovery hellosRx=0 we=ch1 NO PEER HEARD
```

- **`we=ch` is the number to check.** If the C3 prints a different `we=ch`, the
  boards are on different channels and nothing will ever cross. That is a
  network problem — put both on a 2.4 GHz AP with one fixed channel, move them
  closer together so they hit the same AP, or re-provision one onto an SSID on
  the other's channel. See [`../ESPNOW_PROTOCOL.md`](../ESPNOW_PROTOCOL.md) for
  the full decision tree.
- **`tx=0/N` with a large N is the signature of a channel split** — every send
  fails because the peer never hears it.
- `discovery: peer learned …` → pairing succeeded; the address shown is what the
  C3 is actually using.
- `ch=`, `rx=`, `drop=`, `last=` describe the unicast link. A `last=sender` with
  a climbing `drop=` means packets are arriving from a board that is not the
  current peer.

`ENC_WIFI_CHANNEL` in `include/const.h` can pin the channel, but it is best
effort and off by default: the AP re-synchronises the station on every beacon, so
a pin it will not honour either does nothing or drops Wi-Fi entirely.

The small C3 icon in the top-right of the main view goes green while telemetry is
fresh and grey once it is older than 5 s. It reports the C3→CYD direction only;
the C3's own `+CYD OK` footer reports the other way.

### Nozzle and bed preheat

Tapping the nozzle panel or the bed panel on the main view opens a preheat screen
for that target. The two screens are the same UI generated from one template.

The bed and tool endpoints take **different** JSON bodies, which is easy to get
wrong because the OctoPrint docs describe them in the same breath:

```text
/api/printer/tool   {"command": "target", "targets": {"tool0": 215}}   # a map
/api/printer/bed    {"command": "target", "target": 40}                # a scalar
```

Sending the tool's `targets` map to the bed endpoint is rejected with
`400 Mandatory parameters missing`, and the temperature silently does not change.

Each screen offers one preset per material (parallel to the chamber's Material
tab), a `+/-` stepper for an arbitrary setpoint, and `Off`:

| Material | Nozzle | Bed |
| --- | --- | --- |
| PLA | 205C | 60C |
| PETG | 235C | 70C |
| ASA | 260C | 100C |
| ABS | 250C | 100C |
| TPU | 230C | 30C |
| Nylon | 270C | 70C |

The screen's large number shows the target, the small one above it the live
actual. The target is adopted from OctoPrint's polled value rather than echoing
what was last sent, so a change made from a gcode macro shows up too — but the
value just sent is held for `PREHEAT_TARGET_SETTLE_MS`, because a poll that
predates the press would otherwise drag the label backwards.

**All preheat controls go inert while a print is running.** Changing a target
mid-print is almost always a slip, so they grey out and stop accepting taps. The
screens stay readable, so you can still watch temperatures during a print.

The presets live in `TOOL_PLA_TEMP` / `BED_PLA_TEMP` and friends in
`include/const.h`, indexed the same order as `MATERIAL_PRESETS`. `TOOL_TARGET_MAX`
(285C) and `BED_TARGET_MAX` (120C) are per-printer caps — **lower them if your
hardware cannot reach them**, since the stepper would otherwise set a target that
is never reached.

The main view's four preheat buttons are **PLA / PETG / ASA / Off**, and one press
sets the **nozzle and the bed together** — a nozzle-only preheat just runs the bed
cold. They are hidden while a print runs, same as the preheat screens.

The material buttons reach the same presets as the preheat screens, from the same
`TOOL_PRESETS` / `BED_PRESETS` tables, so the two can never disagree. Off sends
target 0 to both.



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
`enclosure-display.local:8080` works too if mDNS resolves. The board writes the
image to the inactive OTA slot, validates it, and reboots.

The full-screen splash reports progress, and the main view comes back
automatically if an upload fails:

```text
OTA starting  ->  OTA 42%  ->  OTA done reboot
```

The response body is `OK` (HTTP 200) or the short failure name — `end`,
`write`, `begin`, `stalled`, `busy` (HTTP 500) — so the uploader can tell a
real flash from a failed one. On success the board **reboots itself** once the
response has been sent, so the new image is the one actually running; it is
back on the network roughly 15 seconds later.

`GET /status` reports the same state without uploading, which matters here
because `SERIAL_DEBUG` is off:

```text
curl -u enclosure:YOUR_OTA_PASSWORD http://<board>:8080/status
busy=0 ok=0 starts=0 written=0 total=0 age=54905ms err=
```

`starts` counts upload attempts since boot and `age` is time since the last
chunk, so a rising `age` means the board is up and idle rather than wedged.
A transfer that stops mid-stream is abandoned after 30s (`err=stalled`) so a
half-open connection cannot leave the splash stuck on screen.

Per-chunk progress is also written to the serial log about once a second when
`SERIAL_DEBUG` is enabled in `const.h`.

The endpoint lives in the shared [`../lib/HttpOta`](../lib/HttpOta) library,
which the C3 uses too, so the auth and flash logic exist once.

### ArduinoOTA (may be blocked by the host firewall)

The board also serves ArduinoOTA at **`enclosure-display.local`** on port 3232:

```text
platformio run --target upload --environment esp32dev --upload-protocol espota --upload-port enclosure-display.local --upload-flags --auth=YOUR_OTA_PASSWORD
```

**This path often fails on Windows.** ArduinoOTA always makes the *board* dial
out to the uploader, so the uploader's PC must accept an inbound connection. A
default Windows Firewall blocks that, and the board reports:

```text
[OTA] failed: connect (2)      <- OTA_CONNECT_ERROR
[OTA] failed: end (4)         <- OTA_END_ERROR
[E][ArduinoOTA.cpp:360] _runUpdate(): Update ERROR: Aborted
```

`Authenticating...OK` still appears, because the UDP handshake is allowed as a
reply to the uploader's own outbound flow — only the new inbound TCP connection
is dropped, which makes the failure look like a mystery. Changing the port does
**not** help: the default policy is block-all-inbound, not block-by-port, and
port 80 fails identically. The HTTP path above avoids it entirely by reversing
the direction.

### Uploading from VS Code

`Ctrl+Shift+B` runs **CYD: Build + OTA**; pick the others via
**Terminal → Run Task…**:

| Task | What it does |
| --- | --- |
| `CYD: Build + OTA` | build, then upload over the air (default) |
| `CYD: OTA (skip build)` | upload whatever is already in `.pio/build` |
| `CYD: Build only` | compile, no upload; errors land in the Problems panel |
| `CYD: Flash over USB` | USB flash on `COM7` (CH340 USB-UART bridge) |
| `CYD: Monitor serial` | serial output until `Ctrl+C` |
| `CYD: OTA web page` | open the board's upload form in a browser |

These drive the shared [`../tools/ota-upload.ps1`](../tools/ota-upload.ps1), the
same script the C3 uses. It reads the password from the git-ignored
`include/secrets.h`, resolves the board by mDNS, and refuses to upload if the
build failed.

> **The PlatformIO toolbar's "Upload" button only ever flashes over USB.** It
> cannot do OTA.

The first OTA-enabled firmware must still be flashed over USB, and the **CYD has
much less flash headroom than the C3** — the image is ~1.26 MB against a 1.31 MB
app slot, so the default partition table leaves only ~50 KB. PlatformIO's size
check fails the build if an image outgrows the slot, but additions to this
firmware should be kept small or the partition table should be revisited (the
project uses no filesystem at all, so its 1.31 MB SPIFFS partition is currently
unused and could be traded for larger app slots).

