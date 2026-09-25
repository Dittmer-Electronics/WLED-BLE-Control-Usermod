# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

An out-of-tree WLED 16.x usermod that runs a BLE GATT server (NimBLE-Arduino 2.x) so the WledBleWear WearOS app can control power, brightness, presets and the nightlight. It targets ESP32 and ESP32-C3; ESP8266 and ESP32-S2 have no BLE. The project brief is `initial-prompt.md`.

**`docs/PROTOCOL.md` is the contract with the app.** Change it first, and keep `wled_ble_control.cpp`, `tools/ble_client.py` and `tools/protocol_test.py` in step with it.

The WearOS app is in a **separate repo**, `C:\Users\Jamie\Git\WLED-BLE-Control-WearOS-App`: the v1 WledBleWear repo, renamed. v2 keeps `com.jpchurchouse.wledblewear` and the same Play listing. v1's protocol (UUIDs `4fafc20x-…`, no security) is intentionally **not** compatible with this one.

## Building

Nothing builds from this repo on its own. It compiles as part of the WLED checkout at `C:\Users\Jamie\Git\WLED`, whose `platformio_override.ini` (gitignored in WLED) symlinks this repo:

```ini
[env:esp32dev_ble]
extends = env:esp32dev
custom_usermods =
  ${env:esp32dev.custom_usermods}
  symlink://C:/Users/Jamie/Git/WLED-BLE-Control-Usermod
```

- The same file also defines `esp32c3dev_ble` and `esp32dev_ble_diag` (see Diagnostics).
- Build from the WLED checkout with `~/.platformio/penv/Scripts/pio.exe run -e esp32dev_ble`. `pio` is not on PATH.
- Current sizes (with audioreactive): ESP32 about 1.48 MB and C3 about 1.46 MB. Both fit WLED's default 1.5 MB app slot, so OTA updates work.
- `big_partitions` (1.8 MB) is the fallback. Changing the partition table needs a USB flash and wipes the filesystem.

`tools/build_flags.py` (run via `library.json` `extraScript`) fixes two build-level conflicts. Don't remove it:
- **NimBLE duplicate symbols.** WLED's framework (Tasmota Arduino 2.0.18 / IDF 4.4.8) ships a prebuilt `libbt.a` that already contains a NimBLE host. NimBLE-Arduino's tentative definition of `ble_hs_pvcy_default_irk` plus GCC 8's `-fcommon` makes the linker pull in the IDF's `ble_hs_pvcy.c.obj` as well. The script compiles NimBLE-Arduino with `-fno-common`.
- **RMT-HI LED driver vs the BT controller (classic ESP32).** WLED 16's default RMT-HI driver shares the level-5 interrupt with the BT controller. About 100 ms after the controller starts, the device resets with `ESP_RST_INT_WDT` (confirmed on hardware). The script adds `WLED_USE_SHARED_RMT` to `projenv` (WLED's sources) and to the libraries. `wled_ble_control.cpp` `#error`s if the define is missing. To check the result, `firmware.map` should have about 48 `NeoEsp32RmtHI` references rather than about 191.

## Testing on hardware

The test device is flashed over OTA: `curl -F "update=@.pio/build/<env>/firmware.bin" http://<ip>/update`. There is no serial port on the dev PC. Python `bleak` is used from the dev PC:
- `tools/ble_client.py`: reference client (`scan`, `info`, `pair PASSKEY`, `state`, `watch`, `presets`, `cmd …`, `unpair`). Pairing uses a WinRT custom pairing handler, so no Windows dialog appears. Open the pairing window first with `POST /json/state {"ble":{"pair":true}}`. The passkey is in `/json/cfg` under `um.BLEControl.passkey`.
- `tools/protocol_test.py --address … --host … [--paging N]`: end-to-end check of every opcode, notifications, throttling and preset paging against WLED's HTTP API. It restores the device's state and deletes any temporary presets. The PC must already be paired. All 32 checks should pass.
- Windows keeps a GATT link open for a few seconds after `BleakClient` exits.

**Diagnostics** (in the `esp32dev_ble_diag` env):
- `-D BLE_DEFER_START`: BLE starts only on `{"ble":{"start":true}}`, so a crashing init can't cause an OTA rollback. WLED only marks new firmware valid at the end of `setup()`.
- `-D BLE_DIAG`: `BLE_LOG` lines go to RTC memory, which survives a panic. They appear in `/json/info` as "BLE log" (current) and "BLE last boot" (previous boot), along with the reset reason.

## Architecture

All the code is in `wled_ble_control.cpp`.

**Two execution contexts.** NimBLE callbacks run on the BLE host task, and WLED state is not thread-safe:
- Callbacks only validate input and queue a `Command` (FreeRTOS queue), fill a preset page from the cache, or update the peer table.
- `loop()` drains the queue and applies each command by building a small JSON object for `deserializeState(…, CALL_MODE_DIRECT_CHANGE)`, so the behaviour is identical to the web UI and HTTP API.

**State.** Every loop, `loop()` snapshots WLED's globals (`bri`, `briLast`, `currentPreset`, `currentPlaylist`, `nightlight*`) into a `StatePacket`. It refreshes the characteristic value and sends a notification when the first 6 bytes change, at most once per 250 ms. The nightlight countdown is left out of change detection.

**Notifications are sent per connection.** NimBLE's `notify(value, len)` goes to every connected peer, whether or not it has subscribed. So the usermod keeps its own `peers[]` table (subscriptions per handle) and only notifies subscribed, *authenticated* links. For bonded peers, NimBLE restores subscriptions (a subscribe event with reason RESTORE) **before** `onConnect` runs. That's why `peerSlot()` creates the slot on whichever callback comes first.

**Preset cache (`PresetCache`).** It's built in `loop()` from `presets.json`, using a filtered read under WLED's JSON buffer lock, and records are stored in wire format. The revision is a CRC-16 of the records, so it's stable across reboots. `presetsModifiedTime` has one-second resolution, so a rebuild waits until changes have settled for 1.1 s. The file size is also checked every 10 s, because uploads don't touch that timestamp. The host task reads pages under a mutex.

**Security.** LE Secure Connections with a static 6-digit passkey (config `BLEControl.passkey`, generated on first boot):
- While the pairing window is open, the device offers bonding + MITM with the DisplayOnly IO capability.
- While it's closed, it offers no bonding, no MITM and NoInputNoOutput, so new peers only get unauthenticated links. Everything except Info needs READ/WRITE_AUTHEN, so those links are refused.
- Bonds persist in NVS across reboots and OTA updates. Deleting a connected peer's bond (`{"ble":{"forget":true}}`) drops its link.

**Advertising.**
- NimBLE-Arduino 2.x defaults `advertiseOnDisconnect` to false, so the usermod sets it.
- Advertising can still stop unexpectedly (seen when Windows retries a connection with stale keys), so `keepAdvertising()` restarts it every second while a connection slot is free.
- The framework's `sdkconfig.h` fixes `CONFIG_BT_NIMBLE_MAX_CONNECTIONS` at 1 and max bonds at 3.
- The advert (flags + 128-bit UUID) fills the 31 bytes, so the name goes in the scan response.

**WiFi.** WLED's "disable WiFi sleep" setting (the default) is overridden (`noWifiSleep = false`) in `setup()` and every loop, because the WiFi driver aborts if power save is off while BT runs.

**ESP32-C3** (tested at 192.168.1.81, 288 LEDs):
- It already uses the standard RMT driver (the RMT-HI guard is skipped on RISC-V).
- The BT controller memory isn't reserved at boot; starting BLE uses about 47 KB (free heap goes from 150 KB to 81 KB).
- The stock WLED 16.0.1 C3 firmware rebooted mid-upload on full-speed OTA, but `curl --limit-rate 40K` worked. OTA onto this usermod's firmware works at full speed.

**Heap** (Gledopto ESP32, 144 LEDs, audioreactive):
- Linking BT reserves about 46 KB for the controller before init (WLED free heap goes from 118 KB to 72 KB).
- BLE init plus the service use about 14–17 KB more.
- An open web UI session costs WLED about 25 KB, so free heap can fall to about 26 KB.
