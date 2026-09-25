# CYD ↔ C3 ESP-NOW Protocol

## Purpose

The PrinterDisplay CYD and EnclosureFanControl C3 use ESP-NOW for low-latency enclosure control. Two packet types share one unidirectional flow each:

- **CYD → C3:** `EnclosureCommand` — desired enclosure mode and values.
- **C3 → CYD:** `EnclosureTelemetry` — live enclosure state.

The shared C/C++ definitions are in [`lib/EnclosureProto/src/enclosure_proto.h`](lib/EnclosureProto/src/enclosure_proto.h).

OctoPrint data is **not** transferred over ESP-NOW. Both boards poll OctoPrint independently over Wi-Fi.

## Transport and pairing

| Property | Current behavior |
|---|---|
| Transport | ESP-NOW peer-to-peer traffic on the Wi-Fi radio |
| Direction | Unicast to the configured peer MAC |
| Encryption | Disabled (`peer.encrypt = false`) |
| Application acknowledgment | None |
| Peer identity validation | Sender MAC is not checked by either receive callback |
| Shared protocol magic | `0x454E` (`"EN"`) |
| CYD peer address | `ENC_PEER_MAC` in `PrinterDisplay/include/const.h` |
| C3 peer address | `ENC_PEER_MAC` in `EnclosureFanControl/include/const.h` |
| Channel | Both boards use the same Wi-Fi channel |
| Channel resynchronization | Every `ENC_CHANNEL_RESYNC_MS` (5 seconds) |
| Command link timeout | `ENC_LINK_LOSS_TIMEOUT_MS` (10 seconds) |
| CYD telemetry stale threshold | `ENC_LINK_STALE_MS` (5 seconds) |

Each board must store the other board's Wi-Fi MAC address. An all-zero peer MAC disables transmission, but ESP-NOW reception remains initialized.

Peer MAC values are hardware-specific and should not be copied between installations.

## Packet format details

Packets are sent as the in-memory shared structs using `sizeof(struct)`. Multi-byte values therefore use the boards' native little-endian layout.

The structs are not packed. Alignment padding is included in the transmitted packet and must remain compatible with the receiving compiler and architecture.

`ENC_PROTO_MAGIC = 0x454E` is therefore sent on the wire as bytes `4E 45` on both boards.

## CYD → C3: `EnclosureCommand`

**Direction:** PrinterDisplay CYD to EnclosureFanControl C3  
**Size:** 8 bytes  
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
| 7 | 1 | padding | Struct tail padding; normally zero |

The CYD sends a complete command every time. Fields that do not apply to the current mode are still present:

- **Material tab:** `mode = AUTO`, `setpoint = selected material profile temperature`.
- **Temp tab:** `mode = AUTO`, `setpoint = user-selected chamber temperature`.
- **Manual tab:** `mode = MANUAL`, `louvrePct` and `fanPct` contain the selected outputs.

The C3 currently accepts a valid AUTO setpoint from **10–80°C**. The current CYD UI constrains its setpoint to **20–60°C**.

In MANUAL mode, C3 clamps louvre and fan values above 100% to 100%.

### Example: AUTO, 30°C, sequence 7

```text
4E 45 07 00 1E 00 00 00
```

| Byte | Value |
|---:|---|
| `magic` | `4E 45` = `0x454E` |
| `seq` | `07` |
| `mode` | `00` = AUTO |
| `setpoint` | `1E` = 30°C |
| `louvrePct` | `00` |
| `fanPct` | `00` |
| padding | `00` |

### Example: MANUAL, 40% louvre, 50% fan, sequence 8

```text
4E 45 08 01 1E 28 32 00
```

The setpoint remains in the packet but is ignored while the mode is MANUAL.

## C3 → CYD: `EnclosureTelemetry`

**Direction:** EnclosureFanControl C3 to PrinterDisplay CYD  
**Size:** 10 bytes  
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

The state comes from C3's own OctoPrint material detection. It is not the material profile selected on CYD.

### Mode values

| Value | Shared constant | Meaning |
|---:|---|---|
| 0 | `ENC_MODE_AUTO` | C3 uses automatic enclosure regulation |
| 1 | `ENC_MODE_MANUAL` | C3 uses the louvre and fan values from CYD |

CYD cannot distinguish Material Automatic from Temperature Automatic over ESP-NOW because both transmit `ENC_MODE_AUTO`. The only difference visible to C3 is the selected setpoint value.

### Example: 40% louvre, 50% fan, 42.3°C, PLA, AUTO

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
2. CYD accepts only packets with the exact telemetry length and valid magic.
3. The CYD receive callback copies telemetry into a mutex-protected snapshot.
4. If the snapshot mutex is busy, that telemetry packet is dropped; the next 1-second packet supplies a newer value.
5. The UI marks telemetry stale after 5 seconds.

ESP-NOW's send callback on C3 tracks local send success/failure counters, but this is not an application-level acknowledgment from CYD.

## Desired-temperature ownership and fallback

While CYD is available, CYD is the primary source of the desired chamber temperature.

C3 considers CYD unavailable after **10 seconds without receiving any valid CYD command**. On timeout:

- C3 falls back to its material target.
- PLA target: **30°C**.
- PETG target: **30°C**.
- ASA/ABS target: **50°C**.
- The PID controller is reset when the active setpoint source or target changes.

If CYD is in MANUAL mode when the link is lost, C3 returns to automatic control and then follows the fallback policy above.

When CYD commands resume, its latest AUTO setpoint becomes primary again and overrides the material fallback target.

C3 still uses its own OctoPrint material detection to select the cooling strategy, louvre behavior, cooldown logic, and fallback target. CYD separately uses OctoPrint material data to select and send its material profile setpoint.

## Validation and failure handling

### CYD telemetry receive

CYD rejects a packet when:

- Its length is not exactly `sizeof(EnclosureTelemetry)`.
- Its `magic` is not `ENC_PROTO_MAGIC`.
- The snapshot mutex cannot be acquired immediately.

### C3 command receive

C3 first rejects a packet when its length is not exactly `sizeof(EnclosureCommand)`. When the packet is processed by the regulator, its `magic` is validated against `ENC_PROTO_MAGIC` before it updates link or control state.

## Security limitations

The current implementation is suitable only for a trusted local environment:

- ESP-NOW encryption is disabled.
- Neither receive callback checks the sender MAC.
- The receiver validates packet length and magic, not sender identity.
- A device able to transmit on the current Wi-Fi channel may be able to inject packets with a valid header.
- Commands can change automatic temperature, manual louvre position, and manual fan output.

For a stronger deployment, add sender-MAC validation at minimum. Cryptographic authentication would require a protocol change and authenticated/shared key handling on both boards.

## Compatibility requirements

The wire format depends on the shared struct definitions, native little-endian byte order, and struct alignment padding. Any protocol change must update both firmwares together and keep `magic`, packet length, field offsets, and alignment compatible.

Recommended safeguards for future protocol revisions:

- Add explicit static assertions for struct sizes and field offsets.
- Consider packed wire structs with fixed-width serialization.
- Use an explicit protocol version field.
- Define sequence handling and duplicate rejection behavior.
- Add the desired setpoint/source to telemetry if CYD needs to display what C3 is actually controlling.
