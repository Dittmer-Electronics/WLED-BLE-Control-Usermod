# WLED BLE Control protocol, v1

The contract between the `wled-ble-control` usermod (GATT server, this repo) and the WledBleWear WearOS app (GATT client, `WLED-BLE-Control-WearOS-App`). Both sides implement this document. Change it first, and bump the protocol version for anything that is not backwards compatible.

All multi-byte integers are **little endian**. All strings are **UTF-8**, length-prefixed, and not NUL-terminated.

## Advertising

| Field | Where | Value |
|---|---|---|
| Flags | advert | LE General Discoverable, BR/EDR not supported |
| Complete list of 128-bit service UUIDs | advert | `8e590001-14dc-405e-9f47-e363750a861a` |
| Complete local name | scan response | WLED's server description ("Server description" in WLED's UI settings), max 32 bytes |

- Scan with a filter on the service UUID. Use active scanning if you want the name before connecting. Info also carries the name.
- The device advertises whenever a connection slot is free. Current builds allow **one connection at a time**, a limit of the framework's prebuilt BT configuration.
- The device address is its public address, which is stable. The app can save it and connect directly, without scanning.

## Service and characteristics

UUID base: `8e59xxxx-14dc-405e-9f47-e363750a861a`

| Name | UUID | Properties | Security |
|---|---|---|---|
| Service | `8e590001-…` | primary | - |
| Info | `8e590002-…` | read | none |
| State | `8e590003-…` | read, notify | encrypted + authenticated (bonded, MITM) |
| Command | `8e590004-…` | write (with response), notify | encrypted + authenticated |
| Preset list | `8e590005-…` | write (with response), read | encrypted + authenticated |

Accessing a secured characteristic on an unauthenticated link fails with ATT `Insufficient Authentication` (0x05) or `Insufficient Encryption` (0x0F). Android starts pairing automatically when this happens. See [Security](#security).

### Info (`…0002`), read, open

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | protocol version, `1` |
| 1 | u16 | feature bits: bit0 brightness, bit1 presets, bit2 nightlight, bit3 playlists, bit4 off-and-disconnect command. Others are reserved and set to 0 |
| 3 | u8 | status bits: bit0 **pairing window open**. Others are reserved |
| 4 | u8 + bytes | device name (server description) |
| … | u8 + bytes | WLED version string, e.g. `16.0.1` |

The app should refuse to operate if the protocol version is higher than it understands, and show "update the app".

### State (`…0003`), read + notify

8 bytes:

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | flags: bit0 **power on**, bit1 **nightlight active** |
| 1 | u8 | brightness 1–255. While off, this is the brightness it will return to |
| 2 | u8 | active preset ID, 0 = none |
| 3 | u8 | active playlist ID, 0 = none |
| 4 | u16 | preset list revision (see [Preset list](#preset-list-0005-write--read)) |
| 6 | u16 | nightlight seconds remaining, 0 when inactive |

- A notification is sent whenever power, nightlight active, brightness, preset, playlist or revision changes, **whatever the source**: BLE, web UI, buttons, IR, other APIs, or a preset or playlist advancing. Notifications are rate-limited to one per 250 ms, and the latest state is always sent after a burst.
- The nightlight countdown alone does not trigger notifications. Clients should compute remaining time from the value last received.
- **Active preset semantics are WLED's own:** any manual change after a preset is applied (brightness, power, colour…) resets it to 0. "Active" means "the last preset applied and not yet modified".
- Clients must ignore any bytes after offset 7. Future versions may append fields.

### Command (`…0004`), write with response + notify

`[opcode u8][arguments…]`, at most 16 bytes. A successful write response means the command was received. The command takes effect within one WLED loop, usually a few ms, and the resulting state arrives as a State notification.

**Results:** after each command, the device sends `[opcode u8][status u8]` as a notification on Command to the connection that wrote it, if that connection subscribed. Tiles and complications can skip subscribing and rely on State.

| Opcode | Name | Arguments | Effect |
|---|---|---|---|
| `0x01` | POWER | u8: 0 off, 1 on, 2 toggle | Like the web UI's power button |
| `0x02` | BRIGHTNESS_SET | u8: 1–255 | Sets brightness and turns on |
| `0x03` | BRIGHTNESS_STEP | i8: delta | Adds delta, clamped to 1–255. While off, a positive delta turns on at the previous brightness, and a negative delta does nothing |
| `0x04` | PRESET_APPLY | u8: preset ID 1–250 | Applies a preset or playlist |
| `0x05` | PRESET_NEXT | u8: scope, 0 all presets, 1 favourites only | Next preset by ID after the active one, wrapping round. Without an active preset, the first |
| `0x06` | PRESET_PREV | u8: scope | As NEXT, backwards |
| `0x07` | NIGHTLIGHT | u8: minutes 1–255, 0 = cancel | Starts WLED's nightlight with the device's configured mode and target brightness (default: fade to off) |
| `0x08` | OFF_AND_DISCONNECT | none | Power off, then the device drops the connection (about 250 ms later). For "turn off and disconnect" tile and complication actions |

Result status codes:

| Code | Meaning |
|---|---|
| `0x00` | OK |
| `0x80` | unknown opcode |
| `0x81` | wrong argument length |
| `0x82` | invalid value (e.g. preset ID doesn't exist, brightness 0) |
| `0x83` | busy: command queue full, retry |

### Preset list (`…0005`), write + read

The list is paged, because an attribute is at most 512 bytes and WLED allows 250 presets.

1. **Write** `[start ID u8]`: the lowest preset ID to include (use 1 for the first page).
2. **Read**: returns one page. Use a full (long) read; Android reassembles it. The page is prepared for that connection when the write is received.

Page format:

| Offset | Type | Field |
|---|---|---|
| 0 | u16 | preset list revision (same as State) |
| 2 | u8 | next start ID. Write this for the next page. 0 = this was the last page |
| 3 | u8 | number of records in this page |
| 4 | records | repeated |

Record:

| Type | Field |
|---|---|
| u8 | preset ID |
| u8 | flags: bit0 **playlist**, bit1 **favourite** (has a WLED quick-load label) |
| u8 + bytes | name, truncated to 32 bytes on a UTF-8 boundary |
| u8 + bytes | quick-load label (WLED's "ql", usually 1–2 characters or an emoji), length 0 if none |

- Pages are at most 500 bytes. Records are sorted by preset ID.
- Presets without a name are listed as "Preset N".
- **Caching:** the revision is a CRC-16 of the list contents, so it stays the same across reboots and only changes when presets are added, renamed, deleted or re-flagged. The app caches the list keyed by device address and revision, and refetches only when State reports a different revision. If the revision in a page differs from the first page's (the list changed mid-fetch), restart from ID 1.

## Security

**Model:** LE Secure Connections bonding with passkey entry (MITM-protected). Pairing is only possible while the pairing window is open.

- **Passkey:** a fixed 6-digit number shown in the usermod's settings in WLED (Config → Usermods → BLE Control). It's generated randomly on first boot and can be changed there. The device has the "DisplayOnly" IO capability. The watch user types the passkey into the system pairing dialog.
- **Pairing window:** opened from WLED's Info page (the "Enable BLE pairing" button) or with the JSON API `{"ble":{"pair":true}}`. It stays open for 120 s by default (configurable), and Info status bit0 shows it. While it's closed, the device offers no MITM or bonding capability. A new device can then only get an unauthenticated link, and every secured characteristic returns Insufficient Authentication. Devices that are already bonded reconnect using their stored keys at any time, so tiles and complications never need the window.
- **Bonds:** up to 3 are stored (a framework limit). "Forget all BLE devices" in the usermod settings, or `{"ble":{"forget":true}}`, deletes them all. Info shows how many bonds there are.
- **App flow:**
  1. Connect.
  2. Read Info. If it's not bonded and status bit0 is clear, show "Open WLED → Info → Enable BLE pairing".
  3. Read State, which triggers pairing. Android shows the PIN dialog.
  4. Enable notifications on State.
  5. On a later connection, go straight to reading State.

## Typical sequences

**Full control session (app open)**
1. Connect by saved address, or scan by service UUID.
2. Request MTU 517.
3. Read Info.
4. Read State; this pairs if needed.
5. Write `0x01` to State's CCCD to enable notifications.
6. If State's revision ≠ the cached revision: write `[1]` to Preset list, read, then repeat with the next start ID until it's 0.

**Tile or complication action (one shot)**
1. Connect by saved address.
2. Write the command. Encryption resumes from the bond; the first write may trigger it.
3. Optionally read State for fresh display data.
4. Disconnect. For OFF_AND_DISCONNECT, the device disconnects.

About 1–2 s end to end.

## Versioning

- Adding features (new opcodes, trailing fields, new flag bits) keeps protocol v1. Clients must ignore unknown flag bits and trailing bytes, and treat result status `0x80` as "not supported by this device".
- Incompatible changes bump the protocol version in Info.
