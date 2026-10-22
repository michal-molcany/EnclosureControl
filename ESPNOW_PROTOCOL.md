# CYD ↔ C3 ESP-NOW Protocol

## Purpose

The PrinterDisplay CYD and EnclosureFanControl C3 use ESP-NOW for low-latency enclosure control. Two packet types share one unidirectional flow each:

- **CYD → C3:** `EnclosureCommand` — desired enclosure mode, values, material, and job state.
- **C3 → CYD:** `EnclosureTelemetry` — live enclosure state.

The shared C/C++ definitions are in [`lib/EnclosureProto/src/enclosure_proto.h`](lib/EnclosureProto/src/enclosure_proto.h).

Both boards still poll OctoPrint independently for their own displays, but telemetry carries the C3's current chamber and bed readings so the CYD can show the same numbers during PID calibration. CYD forwards its control mode, setpoint, material classification, active-job state, and a one-shot calibration cancel action.

## Transport and pairing

| Property | Current behavior |
|---|---|
| Transport | ESP-NOW peer-to-peer traffic on the Wi-Fi radio |
| Direction | Unicast to the configured peer MAC |
| Encryption | **Enabled** (`peer.encrypt = true`, 16-byte LTK from `include/espnow_key.h`) — unicast only; the discovery broadcast peer is deliberately unencrypted |
| Application acknowledgment | None |
| Peer identity validation | Both receive callbacks check the sender MAC against the active peer |
| Peer address | Discovered by broadcast hello; `ENC_PEER_MAC` is the initial value and fallback |
| Shared protocol magic | `0x454E` (`"EN"`) |
| CYD peer address | `ENC_PEER_MAC` in `PrinterDisplay/include/const.h` |
| C3 peer address | `ENC_PEER_MAC` in `EnclosureFanControl/include/const.h` |
| Channel | Both boards must settle on the same Wi-Fi channel **and the same AP radio (BSSID)**; each logs both at boot |
| Channel resynchronization | Every `ENC_CHANNEL_RESYNC_MS` (5 seconds) |
| Channel pin | `ENC_WIFI_CHANNEL` in each project's `const.h`; `0` follows the AP, and a pin cannot bridge two different BSSIDs |
| Command link timeout | `ENC_LINK_LOSS_TIMEOUT_MS` (10 seconds) |
| CYD telemetry stale threshold | `ENC_LINK_STALE_MS` (5 seconds) |
| Status line | Both boards print a link summary every `ENC_DIAG_INTERVAL_MS` (5 seconds) |

Each board must store the other board's Wi-Fi MAC address. An all-zero peer MAC disables transmission, but ESP-NOW reception remains initialized. While unpaired, the sender-MAC check is skipped, since the whole point of that mode is to accept a first contact from an unknown board.

## Peer discovery

`ENC_PEER_MAC` is only the *initial* value. Both boards also register the
broadcast peer (`FF:FF:FF:FF:FF:FF` on channel 0) and every
`ENC_DISCOVERY_INTERVAL_MS` (2 s) broadcast a 4-byte hello:

| Offset | Size | Field | Meaning |
|---:|---:|---|---|
| 0 | 2 | `magic` | `0x4E44` (`"DN"`) — distinct from the data packets |
| 2 | 1 | `role` | `ENC_ROLE_CYD = 0` or `ENC_ROLE_C3 = 1` |
| 3 | 1 | `channel` | Sender's current Wi-Fi channel |

A board that hears a well-formed hello from the *other* role adopts the sender
as its ESP-NOW peer, replacing `ENC_PEER_MAC`, and logs it. This is why a
hand-copied wrong MAC now self-corrects within a couple of seconds instead of
producing a permanently dead link. Definitions are in
[`lib/EnclosureProto/src/enclosure_discovery.h`](lib/EnclosureProto/src/enclosure_discovery.h).

**Discovery cannot fix a channel or AP mismatch.** ESP-NOW frames never leave
the radio, so a hello is only audible on the channel its sender is associated
to. "No peer heard" is therefore a *diagnosis*, not a fault in the code.

This is the failure that was actually observed on this hardware: the C3
associated to channel 11 and the CYD to channel 1 on the same SSID. The
signature is unmistakable —

```text
C3   [LINK] ch 11 peer registered on 11
C3   [LINK] ch=11 rx=0 tx=0/1120 drop=0 last=- age=0ms
CYD  [ENC-LINK] joined on channel 1
CYD  [ENC-LINK] ch=1 rx=0 tx=5/5 drop=0 last=- age=0ms
CYD  [ENC-LINK] discovery hellosRx=0 we=ch1 NO PEER HEARD
```

`tx=0/1120` is the clearest tell: 1120 transmissions, **zero** acknowledged, and
`rx=0 drop=0 last=-` with `hellosRx=0` on the other board means not a single
frame of any kind — valid, malformed, or broadcast — ever arrived. Nothing is
wrong with the firmware at that point; the two radios are simply on different
frequencies. Every send fails because the peer never hears its preamble.

### Fixing a channel split

In order of preference:

1. **Put both boards on one 2.4 GHz AP with a single fixed channel.** Disable
   band steering and any split 2.4/5 GHz SSIDs. This is the only fix that
   survives, because a station re-synchronises to whatever channel its AP beacons
   on.
2. **Move the two boards physically closer together** so they associate to the
   same access point. On a roaming or mesh network this is often enough, since
   the split here is per-client channel assignment or a different AP node.
3. **Re-provision one board onto a different SSID** that is on the same channel
   as the other. The C3 is easy — it joins through the WiFiManager captive portal
   and persists the result in NVS. The CYD's SSID is hard-coded in
   `include/secrets.h`.
4. **Pin the channel in firmware** by setting `ENC_WIFI_CHANNEL` to the same
   value in both `const.h` files. This is best effort: the AP re-synchronises the
   station on every beacon, so a pin the AP will not honour either does nothing
   or drops the board off the network. It is off by default for that reason.
   Each board logs the channel that *actually* took effect, not the one
   requested, and re-asserts the pin every `ENC_CHANNEL_RESYNC_MS`, so a pin the
   AP is actively fighting is visible on the log rather than silent.

Whichever route is taken, both boards print their negotiated channel at boot:

```text
[ENC-LINK] joined on channel 1
[LINK] joined on channel 11
```

Those two numbers must match. Note the C3's SSID is chosen at runtime through
WiFiManager and persisted in NVS, so it can easily have drifted away from
whatever the CYD is hard-coded to join — the C3 prints its actual SSID and
channel at pair time for exactly this reason.

Once the channels agree, the broadcast discovery handshake pairs the two boards
on its own, and a wrong `ENC_PEER_MAC` on either side becomes irrelevant.

## HTTP fallback transport

ESP-NOW is and stays the primary transport. It is fast, needs no network, and
works even when the router is unreachable. But none of the fixes above help when
the two boards are on **different AP radios** — the measured fault on this
hardware, where the BSSIDs differed even though the SSID matched. In that case
the link is simply gone, and no firmware setting recovers it, because a station
re-associates to whatever radio it is on.

For exactly that case, `lib/EnclosureApi` adds a second transport that rides the
IP network, which both boards share regardless of channel or BSSID.

### When it engages

The CYD engages the fallback only while ESP-NOW telemetry has been stale for
`ENC_LINK_STALE_MS` (5 s). Freshness is tracked from the **ESP-NOW** receive
timestamp specifically, not from the shared snapshot's timestamp — otherwise a
successful API poll would look like ESP-NOW recovering and would switch the
arbiter back off on its own data.

When the fallback is live, ESP-NOW command sends stop: firing packets into a
channel split accomplishes nothing, and the C3 would then be driven by whichever
transport answered first.

The CYD header shows the API icon alongside the C3 chip icon while this is
happening, so the transport in use is visible without a serial cable.

### Endpoints

The C3 serves these on `ENC_API_PORT` (8081), deliberately separate from
`OTA_HTTP_PORT` (8080) so a firmware upload and the API never contend for one
socket. Both require HTTP Basic auth with `ENC_API_TOKEN` as the password.

```text
GET  /api/enclosure  ->  {"louvre":0-100,"fan":0-100,"chamber":int16,"bed":int16,
                          "state":u8,"mode":u8,"seq":u8,
                          "calPhase":u8,"calElapsed":seconds,"calCycles":u16,
                          "calBedTarget":C,"calSetpoint":C,"calError":u8}
POST /api/enclosure  <-  {"mode":u8,"setpoint":0-60,"louvre":0-100,"fan":0-100,
                          "material":u8,"job":0|1,"calAction":0|nonzero}
```

`chamber` and `bed` are in 0.1 °C units and use `ENC_CHAMBER_INVALID`
(`INT16_MIN`) when unknown, exactly as the struct does. Field names and meanings
mirror `EnclosureTelemetry` / `EnclosureCommand`; `seq` is a per-request counter
on this transport, independent of the ESP-NOW one, since the two packet streams
are not comparable.

`GET /api/enclosure` also carries `octoIp`, `octoPort`, `octoKeyFlags`, and —
only when the flags are set — `octoKey`, with exactly the meanings given in
[API key propagation](#octoprint-api-key-propagation). The CYD hands both the
key and the address to the same `pollerSetApiKey()` / `pollerSetEndpoint()` entry
points on both transports, so there is one implementation of "adopt what the C3
says" rather than two that can drift apart — and a drift would only ever show up
while one transport was down, i.e. never in testing.

The port and token are shared between both firmwares through the git-ignored
`include/enclosure_api.h` (copy from `enclosure_api.example.h`), so the two
cannot drift apart the way a duplicated `#define` can.

### Discovery

The CYD finds the C3 by mDNS A-record query on `enclosure-fan-control`, which the
C3 registers via `ArduinoOTA.begin()`. `ENC_API_STATIC_IP` in the CYD's
`const.h` is a dotted-quad fallback for networks where mDNS is blocked; it is
`0.0.0.0` (disabled) by default.

Re-resolution happens on a 30 s cadence (`ENC_API_RESOLVE_MS`), and a cached
address that starts refusing also backs off to 30 s
(`ENC_API_UNREACHABLE_RETRY_MS`) rather than retrying per tick. That backoff is
load-bearing: `resolve()` costs up to 1.5 s of blocking mDNS query, so retrying
without it turns an unreachable C3 into an mDNS busy-loop for as long as
ESP-NOW stays down.

### Security posture

The token is a shared secret over plain HTTP. It stops a casual device on the
LAN from driving the fan and louvre — it is not a defence against anything that
can already observe the Wi-Fi link. Use a long random value; the shipped default
is a placeholder. The same token also guards the browser page on 8082, so one
leaked value covers both transports. This matches the existing posture of the OTA
path (see [Security limitations](#security-limitations)); it is a consistency
improvement, not a change of threat model.

## Packet format details

Packets are sent as the in-memory shared structs using `sizeof(struct)`. Multi-byte values therefore use the boards' native little-endian layout.

The structs are not packed. Alignment padding is included in the transmitted packet and must remain compatible with the receiving compiler and architecture. `static_assert`s in `enclosure_proto.h` pin every size and field offset, so a toolchain or packing change that altered the layout fails the build on both boards instead of producing a link that is silently dead.

`ENC_PROTO_MAGIC = 0x454E` is therefore sent on the wire as bytes `4E 45` on both boards.

## CYD → C3: `EnclosureCommand`

**Direction:** PrinterDisplay CYD to EnclosureFanControl C3  
**Size:** 10 bytes
**Sent:** immediately after a UI change, then repeated every `ENC_CMD_HEARTBEAT_INTERVAL_MS` (2 seconds)

### Byte layout

| Offset | Size | Field | Meaning |
|---:|---:|---|---|
| 0 | 2 | `magic` | Always `0x454E`; stamped by the CYD link task |
| 2 | 1 | `seq` | Command send counter, wraps from 255 to 0 |
| 3 | 1 | `mode` | `ENC_MODE_AUTO = 0` or `ENC_MODE_MANUAL = 1` |
| 4 | 1 | `setpoint` | Desired chamber temperature in whole °C, used in AUTO |
| 5 | 1 | `louvrePct` | Desired louvre position 0–100%, used in MANUAL |
| 6 | 1 | `fanPct` | Desired fan output 0–100%, used in MANUAL |
| 7 | 1 | `material` | `EnclosureMaterial` classification used for automatic strategy and cooldown |
| 8 | 1 | `jobActive` | Printing or paused job is active |
| 9 | 1 | `calAction` | One-shot PID-calibration cancel: `0` = none, nonzero = cancel; C3 acts only when the value changes |

The CYD sends a complete command every time. It keeps the cancel value in the repeated heartbeat until the run ends, then returns the field to `0` and sends that reset immediately; the C3 uses the zero to re-arm its change detector for the next run. A reboot also starts at `0`.

Fields that do not apply to the current mode are still present:

- **Material tab:** `mode = AUTO`, `setpoint = selected material profile temperature`.
- **Temp tab:** `mode = AUTO`, `setpoint = user-selected chamber temperature`.
- **Manual tab:** `mode = MANUAL`, `louvrePct` and `fanPct` contain the selected outputs.
- `material` and `jobActive` describe the CYD's current OctoPrint-derived material and whether a printing or paused job is active.

The C3 accepts a valid AUTO setpoint from **20–60°C**.

In MANUAL mode, C3 clamps louvre and fan values above 100% to 100%.

### Example: AUTO, 30°C, sequence 7

```text
4E 45 07 00 1E 00 00 00 01 00
```

| Byte | Value |
|---:|---|
| `magic` | `4E 45` = `0x454E` |
| `seq` | `07` |
| `mode` | `00` = AUTO |
| `setpoint` | `1E` = 30°C |
| `louvrePct` | `00` |
| `fanPct` | `00` |
| `material` | `00` = PLA |
| `jobActive` | `01` |
| `calAction` | `00` = no calibration action |

### Example: MANUAL, 40% louvre, 50% fan, sequence 8

```text
4E 45 08 01 1E 28 32 00 00 00
```

The setpoint remains in the packet but is ignored while the mode is MANUAL.

## C3 → CYD: `EnclosureTelemetry`

**Direction:** EnclosureFanControl C3 to PrinterDisplay CYD  
**Size:** 92 bytes (was 82; see *Protocol revision 4* below)<br>
**Sent:** approximately every `ENC_TELEMETRY_INTERVAL_MS` (1 second)

### Byte layout

| Offset | Size | Field | Meaning |
|---:|---:|---|---|
| 0 | 2 | `magic` | Always `0x454E`; stamped by the C3 |
| 2 | 1 | `seq` | Telemetry send counter, wraps from 255 to 0 |
| 3 | 1 | `louvrePct` | Current louvre position, 0–100% |
| 4 | 1 | `fanPct` | Current fan output, 0–100% |
| 5 | 1 | padding | Alignment padding before `chamberCx10`; normally zero |
| 6 | 2 | `chamberCx10` | Chamber temperature in 0.1°C units, little-endian |
| 8 | 1 | `state` | C3 material-regulation state |
| 9 | 1 | `mode` | Current CYD-selected control mode received by C3 |
| 10 | 4 | `octoIp` | OctoPrint address the C3 is polling, 4 octets |
| 14 | 2 | `octoPort` | OctoPrint port the C3 is polling |
| 16 | 65 | `octoKey` | OctoPrint API key, NUL-terminated; meaningful only when `octoKeyFlags` says so |
| 81 | 1 | `octoKeyFlags` | `ENC_KEYFLAG_*` state of `octoKey` |
| 82 | 2 | `bedCx10` | Bed temperature in 0.1°C units; same invalid sentinel as `chamberCx10` |
| 84 | 2 | `calElapsedSec` | Elapsed calibration time in seconds, saturating at 65535 |
| 86 | 2 | `calCycles` | Completed relay cycles |
| 88 | 1 | `calPhase` | `EnclosureCalPhase` (`0` none, `1` preheat, `2` settling, `3` relay, `4` done, `5` aborted) |
| 89 | 1 | `calBedTargetC` | Bed preheat target in whole °C (currently 90) |
| 90 | 1 | `calSetpointC` | Chamber setpoint in whole °C (currently 35) |
| 91 | 1 | `calErrorCode` | `EnclosureCalError`, meaningful when `calPhase == 5` |

### OctoPrint address propagation

`octoIp` / `octoPort` carry the address **the C3 itself is polling**, so the CYD
uses the same server instead of holding a second, independently-editable copy.
The C3 owns the value (its web UI is where it is set); the CYD follows it and
stores it in NVS.

Design points worth knowing before changing this:

- **It rides every frame, not just on change.** There is no ack and no C3→CYD
  back channel, so repeating until the far end has it is the entire reliability
  story. A CYD that boots late, or was powered off while the address changed,
  converges within one frame — about a second — with no state kept on either
  board.
- **All-zero `octoIp` means "the C3 has no runtime override."** The CYD must
  then fall back to its own compiled-in `OCTOPRINT_IP` and clear its stored
  value. That sentinel is what makes "Reset to firmware default" on the C3 reset
  *both* boards instead of leaving them disagreeing.
- **The CYD stores it, the C3 does not ask for it back.** Telemetry is one-way,
  so the C3 can report "the CYD is being told" but never "the CYD has it".
- **Both directions of transport carry it.** The HTTP fallback's `/api/enclosure`
  includes `octoIp`/`octoPort` for exactly this reason: otherwise a change made
  on the web UI would not reach the CYD during an ESP-NOW outage — which is
  precisely when the CYD most needs to know.

### OctoPrint API key propagation

`octoKey` carries the OctoPrint **credential**, so it is handled differently from
the address in three ways: it is **not** in every frame, it needs an explicit
flag byte, and it is a security decision rather than a convenience.

#### `octoKeyFlags` — three states, not two

| Value | Meaning | Receiver action |
|---|---|---|
| `0` | Nothing to say | Keep whatever it already has |
| `ENC_KEYFLAG_PRESENT` (1) | `octoKey` holds a real key | Use it |
| `ENC_KEYFLAG_CLEAR` (2) | The C3 has no override | Drop yours, return to your compiled-in default |

The obvious design — "empty string means no override" — **cannot work here**, and
the reason is worth stating because it is not obvious. The key is deliberately
*not* sent in every frame, so the frames carrying no key are the overwhelming
majority. Under an empty-string sentinel, every one of those frames would read as
"drop your override", and the CYD would flip back to its own compiled-in key once
per second.

`0` is the third state that makes a sparse cadence possible. It is also what keeps
a mixed-version pair working: an older C3 sends no `octoKeyFlags` at all, which
reads as `0`, i.e. "leave the CYD's key alone" rather than "clear it".

#### Cadence

- Repeatedly for `OCTO_KEY_ANNOUNCE_MS` (5 s) after any change, so a CYD that is
  mid-reboot or that missed the first frame still converges quickly.
- Then once every `OCTO_KEY_REFRESH_MS` (30 s), so a CYD powered off across the
  change still picks it up without being told again.
- `0` otherwise.

The 5 s window is re-armed on every frame that claims it, so it repeats rather
than firing once, and the next refresh is rescheduled on each claimed frame — a
frame that is sent but lost is followed by another.

#### Security consequences — read before changing this

This puts a live printer credential on the radio every 30 s and into the
CYD's flash. Link encryption ([above](#link-encryption)) protects it in flight,
but it remains stored in plaintext in the CYD's NVS, and it still crosses the
HTTP fallback as bearer-authenticated cleartext. The 30 s cadence limits how
often it is transmitted; it does not change what a flash dump yields.

This is a deliberate trade: one place to configure the printer, instead of a key
frozen into two firmwares that can disagree. The alternative — carrying the key
only over the token-protected HTTP link — reintroduces exactly the two-sources-of-
truth problem the shared header this field replaces was created to prevent.

If this exposure is unacceptable, removing `octoKey` from telemetry and letting
the CYD keep its own `secrets.h` copy is the cleanest retreat; it costs the
single-source-of-truth property and nothing else.

#### Receiver validation

The CYD treats the key as untrusted, for the same reason it treats the address as
untrusted: it accepts frames from any sender in receive-only bootstrap mode. It
rejects (rather than sanitises) any value that is empty, over-length, or contains
a non-printable-ASCII byte, **before** the value is stored or handed to the
client. A CR or LF would end the `X-Api-Key:` header line early and turn the rest
of the value into headers sent to the OctoPrint server. A key is machine-
generated ASCII, so nothing legitimate is refused by this.

### Protocol revision 2

`ENC_PROTO_VERSION` is now 2. Revision 2 grew `EnclosureTelemetry` from 10 to 16
bytes by adding `octoIp` + `octoPort`.

### Protocol revision 3

Revision 3 grew `EnclosureTelemetry` from 16 to **82** bytes by adding `octoKey`
(65 bytes) + `octoKeyFlags` (1 byte) — the API key propagation described above.
Still comfortably inside the 127-byte ESP-NOW payload limit.

### Protocol revision 4

Revision 4 grew `EnclosureTelemetry` from 82 to **92** bytes by adding the bed
reading and calibration progress (`bedCx10`, `calElapsedSec`, `calCycles`,
`calPhase`, `calBedTargetC`, `calSetpointC`, `calErrorCode`). The formerly
reserved tail-padding byte of `EnclosureCommand` became `calAction`, so that
packet stays 10 bytes. `/api/enclosure` gained the matching JSON keys.

The CYD uses these fields for a modal calibration screen that shows live
chamber/bed readings and phase, blocks navigation until the run ends, and can
request a confirmed cancel. All other CYD command fields remain ignored by the
C3 while the relay owns the outputs.

Both boards validate the packet length against their own `sizeof()`, so a C3 and
a CYD on mismatched builds **drop every frame** — the link goes quiet until both
are flashed, with no error beyond the drop counters. Reflash both when updating.
This is the only reason a change like this is safe: it fails loudly (a dead link)
rather than silently (a field read from the wrong offset).

**A mismatched pair does not break the pair.** A dead ESP-NOW link is exactly the
condition the HTTP fallback exists for, so a CYD on an older build keeps working
against a newer C3 over port 8081 — it just loses the key hand-off until both are
flashed. The drop counters (`last=len`) are how you tell that is what is
happening.

`chamberCx10` encodes actual temperature as:

```text
encoded_temperature = temperature_C × 10
```

For example, 42.3°C is stored as 423 (`0x01A7`) and sent as bytes `A7 01`.

When OctoPrint chamber data is invalid, C3 sends `ENC_CHAMBER_INVALID`, which is `INT16_MIN` (`-32768`). CYD must display the chamber value as unavailable rather than converting it to a temperature.

### State values

| Value | Shared constant | C3 meaning |
|---:|---|---|
| 0 | `ENC_STATE_IDLE` | No active material regulation |
| 1 | `ENC_STATE_PLA` | PLA cooling state |
| 2 | `ENC_STATE_PETG` | PETG cooling state |
| 3 | `ENC_STATE_ASA` | ASA/ABS and other high-temperature material state |

The state reflects the material and cooldown phase currently applied by C3, using CYD material data while the link is alive and C3 OctoPrint material as fallback.

### Mode values

| Value | Shared constant | Meaning |
|---:|---|---|
| 0 | `ENC_MODE_AUTO` | C3 uses automatic enclosure regulation |
| 1 | `ENC_MODE_MANUAL` | C3 uses the louvre and fan values from CYD |

CYD cannot distinguish Material Automatic from Temperature Automatic over ESP-NOW because both transmit `ENC_MODE_AUTO`. The only difference visible to C3 is the selected setpoint value.

### Example: 40% louvre, 50% fan, 42.3°C, PLA, AUTO

Only the first ten bytes are shown; the OctoPrint and calibration fields follow.

```text
4E 45 09 28 32 00 A7 01 01 00
```

| Byte(s) | Value |
|---:|---|
| `magic` | `4E 45` |
| `seq` | `09` |
| `louvrePct` | `28` = 40% |
| `fanPct` | `32` = 50% |
| alignment padding | `00` |
| `chamberCx10` | `A7 01` = 423 = 42.3°C |
| `state` | `01` = PLA |
| `mode` | `00` = AUTO |

## Sequence numbers

Both packet types contain an 8-bit `seq` counter that increments for every transmitted packet and wraps naturally.

Current behavior:

- CYD stores the latest telemetry sequence in `EnclosureSnapshot`.
- C3 does not track the command sequence.
- Neither board uses sequence numbers to reject duplicates, reorder packets, or detect a gap.
- Loss detection currently relies on periodic retransmission and receive timestamps.

## Timing and reliability

### CYD command behavior

1. A chamber tab change, setpoint step, material selection, louvre step, or fan step creates an `EnclosureCommand`.
2. The UI enqueues the command without blocking.
3. The CYD ESP-NOW task sends the latest queued command within approximately 50 ms.
4. That command is repeated every 2 seconds until replaced by a newer command.
5. The C3 receive callback stores only the latest command slot.
6. C3's main loop calls the link poll at approximately 1 Hz and applies the stored command during that poll.

The 2-second heartbeat repairs a lost command without requiring a user interaction. If another command arrives before C3 drains its receive slot, the newest command replaces the older one.

### C3 telemetry behavior

1. C3 sends telemetry approximately once per second to its configured CYD peer.
2. CYD accepts only packets with the exact telemetry length, valid magic, and — once paired — the configured sender MAC.
3. The CYD receive callback copies telemetry into a mutex-protected snapshot.
4. If the snapshot mutex cannot be acquired within 5 ms, that telemetry packet is dropped and counted; the next 1-second packet supplies a newer value. (This was a zero-timeout take, which discarded the packet outright whenever the UI task happened to hold the mutex — a collision rate high enough to look like a dead link.)
5. The UI marks telemetry stale after 5 seconds.

ESP-NOW's send callback on both boards tracks local send success/failure counters, but this is not an application-level acknowledgment from the peer.

## Desired-temperature ownership and fallback

While CYD is available, CYD is the primary source of mode, setpoint, material, and job state. C3 continues to use its own chamber sensor and OctoPrint connection for temperature, emergency cancellation, and fallback data.

C3 considers CYD unavailable after **10 seconds without receiving any valid CYD command**. On timeout:

- C3 falls back to its own OctoPrint material and active-job state.
- PLA/PETG target: **30°C**.
- TPU target: **35°C**.
- ASA/ABS/Nylon/other high-temperature target: **50°C**.
- Unknown material uses the conservative 50°C, louvre-only automatic strategy.
- The PID controller is reset when the active setpoint source or target changes.

If CYD is in MANUAL mode when the link is lost, C3 returns to automatic control and then follows the fallback policy above.

When CYD commands resume, its latest mode, setpoint, material, and job state become primary again.

## Validation and failure handling

Both boards apply the same checks in the same order, and count each rejection
separately so a capture says *which* one is firing rather than just "nothing
arrived":

1. Length must equal `sizeof()` of the expected struct (`ENC_DROP_LEN`).
2. Magic must be `ENC_PROTO_MAGIC` (`ENC_DROP_MAGIC`).
3. Once paired, the sender MAC must be the active peer (`ENC_DROP_SENDER`).
   Skipped while unpaired. The active peer is the discovered one if a hello has
   been heard, otherwise the compiled-in `ENC_PEER_MAC`.
4. The inbound slot must be free (`ENC_DROP_BUSY`).

A 4-byte discovery hello is dispatched before these checks, so hellos are never
counted as malformed data.

C3 additionally revalidates the magic in `TemperatureRegulation::applyRemoteCommand()` before it updates link or control state.

## Diagnosing a dead link

Both boards print a status line every 5 seconds, so a 20-second capture from each
is enough to localize the failure:

```text
[ENC-LINK] ch=6 rx=142 tx=7/2 drop=3 last=busy age=980ms      # CYD
[LINK] ch=6 rx=14 tx=61/0 drop=0 last=- age=1002ms            # C3
```

- `ch=` is the channel the peer is registered on, not the current Wi-Fi channel.
  If the two boards print different `ch=` values, that is the whole problem.
- `rx=` accepted inbound packets, `tx=` is send-callback successes/failures.
- `drop=` counts rejected packets and `last=` names the most recent reason.

| Symptom | Meaning | Usual cause |
|---|---|---|
| `rx=0 drop=0 last=-` on both | Nothing crossing the air | Different AP or channel, or a wrong `ENC_PEER_MAC` on at least one side |
| `drop=` climbing, `last=sender` | Frames arrive, identity rejected | Stale `ENC_PEER_MAC`; the sender is real but not the configured peer |
| `drop=` climbing, `last=len` | Frame length wrong | A peer running different firmware — structs changed on one side only |
| `drop=` climbing, `last=magic` | Header rejected | Same cause as `len`; a peer whose protocol revision differs |
| `drop=` climbing, `last=busy` | Accepted then dropped on contention | Mutex starvation; should be rare after the bounded-take fix |
| `tx=0/N` | Local sends failing outright | Peer not registered, or the radio cannot reach it |
| `tx=0/0` | No sends attempted yet | CYD UI has not queued a command since boot |

The two on-screen indicators isolate the two directions independently: the C3
OLED footer (`+CYD OK` / `NO CYD`) shows whether commands are reaching the C3,
and the CYD's C3 icon (green/grey, `PrinterDisplay/src/display.cpp`) shows
whether telemetry is reaching the CYD.

Pairing checklist, in order:

1. Compare the C3's `[LINK] own MAC` with `ENC_PEER_MAC` in `PrinterDisplay/include/const.h`, and the CYD's `[ENC-LINK] own MAC` with `ENC_PEER_MAC` in `EnclosureFanControl/include/const.h`.
2. Compare the `we=ch` values in both boot lines. **Different values means different channels — that alone explains a dead link, and no amount of rebuilding fixes it.**
3. Compare the `bssid` values in both boot lines. This is the step that tells you *which* of the two very different problems you have:

   | | Same BSSID, different `we=ch` | Different BSSID |
   |---|---|---|
   | Cause | One AP that moved channel | Two AP radios broadcasting the same SSID (mesh/extender) |
   | `ENC_WIFI_CHANNEL` | **Fixes it** — set it to that channel in both projects | **Cannot fix it** |
   | Action | Firmware | Network: align the APs on one 2.4 GHz channel, disable the extender, or move the boards onto one radio |

   A BSSID is the hardware MAC of one specific AP radio. The SSID is only a name and is not unique, so a station that joined "ExampleNet" may be on any of several radios. A channel pin cannot bridge two different BSSIDs: forcing a station off the AP it is associated with just gets it deauthenticated, after which it re-associates to the same radio it was trying to escape. `ENC_CHANNEL_PIN_GIVEUP_MS` makes that failure mode safe rather than silent — the board logs `giving up channel pin` and follows the AP again.

   This pair has been on two BSSIDs on one SSID: `02:00:5E:10:00:11` on channel 11 and `02:00:5E:10:00:12` on channel 1. Same vendor OUI with a byte apart, which is what a mesh or extender looks like. The link recovered on its own once a reboot moved the CYD onto the C3's BSSID — so this failure can be transient, and a reboot is worth trying before changing any configuration.
4. Check `espnow encryption ON` on both boards. If one board is missing that line, or the two were built with different `include/espnow_key.h` values, the link is dead with no other symptom — see [Link encryption](#link-encryption).
5. `hellosRx` climbing on both boards means they are on the same channel and the unicast path is about to work; look for `discovery: peer learned`.
6. If `hellosRx` stays 0 on one board but not the other, that board is on a different channel or AP entirely.

## Link encryption

ESP-NOW frames never leave the radio, so the access point's WPA2 does nothing for
them. Both boards therefore set `peer.encrypt = true` with a shared 16-byte LTK
(AES-128-CCM) from the git-ignored `include/espnow_key.h`:

```cpp
peer.encrypt = true;
memcpy(peer.lmk, ltk, sizeof(ltk));   // ESP_NOW_KEY_LEN bytes
```

Both boards print `espnow encryption ON` at boot, **outside** `SERIAL_DEBUG`, so
encryption state can be confirmed without a debug build.

**If the two boards hold different keys, the link is simply dead.** Each encrypts
to the key it has and the other cannot read it — no negotiation, no error. It is
indistinguishable from a channel split, a wrong `ENC_PEER_MAC`, or an absent
peer. Compare the two boot lines *before* working through the channel/AP
checklist below; changing the key requires reflashing both boards.

### The broadcast peer stays unencrypted

ESP-NOW encryption is per-unicast-peer and does not work against a broadcast
destination, so an encrypted broadcast peer would simply stop discovery hellos
going out. The consequence is that **peer discovery remains unauthenticated**:
anyone can answer a hello and take the peer slot.

What that buys an attacker changes, though. A hijacked peer is added as an
*encrypted* peer, so everything sent to it is ciphertext they hold no key for.
Hijacking now costs you the link, not the OctoPrint credential.

## Security limitations

The current implementation remains suitable only for a trusted local environment:

- **Outbound frames are encrypted, inbound frames are not authenticated.**
  Encryption is a send-side setting; there is no receiver-side option to reject
  an unencrypted frame. Anyone who forges the other board's MAC — and it is
  broadcast in every frame — can still inject plaintext `EnclosureCommand`s and
  be believed. Closing this needs an application-level MAC over the command
  struct, which is not implemented.
- **A live OctoPrint API key travels in telemetry** (`octoKey`), every 30 s and for
  5 s after any change. It is now encrypted on the radio, but it is still stored
  in plaintext in the CYD's flash, and it is still sent over the HTTP fallback
  as bearer-authenticated cleartext. A leak of either board's flash still yields a
  printer credential. See [API key propagation](#octoprint-api-key-propagation).
- **Peer adoption is unauthenticated** — see above; the impact is denial of the
  link rather than disclosure.
- Receivers validate packet length, magic, and sender MAC, but the MAC check is
  spoofable by a device that can transmit on the current channel.
- Commands can change automatic temperature, manual louvre position, and manual fan output.
- The HTTP fallback requires a shared token (`ENC_API_TOKEN`) in HTTP Basic auth, sent in the clear. It prevents an unauthenticated device from driving the enclosure, but not one that can observe the network. Use a long random value — the shipped default is a placeholder.

Sender-MAC validation closes the "any device on the channel can inject commands" gap and is the minimum recommended by the protocol design. Link encryption (above) closes the confidentiality gap. Neither closes inbound authenticity; that needs an application-level MAC over the command struct, which remains open.

## Compatibility requirements

The wire format depends on the shared struct definitions, native little-endian byte order, and struct alignment padding. Any protocol change must update both firmwares together and keep `magic`, packet length, field offsets, and alignment compatible.

Recommended safeguards for future protocol revisions:

- ~~Add explicit static assertions for struct sizes and field offsets.~~ Done — `enclosure_proto.h` pins `EnclosureTelemetry` to 92 bytes and `EnclosureCommand` to 10, with every offset asserted, including the byte that is now `calAction`.
- ~~Add sender-MAC validation.~~ Done — both receive callbacks enforce it once paired.
- Consider packed wire structs with fixed-width serialization.
- Use an explicit protocol version field.
- Define sequence handling and duplicate rejection behavior.
- Add the desired setpoint/source to telemetry if CYD needs to display what C3 is actually controlling.
