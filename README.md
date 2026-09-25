# WLED BLE Control usermod

Control WLED over Bluetooth Low Energy from a Wear OS watch, with no Wi-Fi needed on the watch. It pairs with the **WledBleWear** app, which provides a control screen plus tiles and complications for one-tap power, presets and more.

- **Power:** on, off or toggle. The watch sees changes live, whatever caused them (web UI, buttons, IR, other apps).
- **Brightness:** set a value, or step up and down (e.g. with the watch's bezel).
- **Presets and playlists:** the full list by name, the active one highlighted, next and previous. Presets with a quick-load label count as favourites.
- **Nightlight / sleep timer** with a countdown.
- **Off and disconnect** in one tap.
- **Secure pairing:** a 6-digit passkey, accepted only while you open a pairing window in WLED. Paired watches reconnect instantly.

The BLE protocol is documented in [docs/PROTOCOL.md](docs/PROTOCOL.md), in case you want to write your own client.

## Requirements

- WLED **16.0** or newer.
- An **ESP32 or ESP32-C3** with 4 MB flash or more. The ESP8266 and ESP32-S2 have no Bluetooth.
- Tested on a Gledopto GL-C-015WL-D (ESP32, 4 MB) and a generic ESP32-C3 (4 MB).

## Installation

Build WLED yourself with PlatformIO. In WLED's `platformio_override.ini`, add the usermod to your board's env:

```ini
[env:esp32dev_ble]
extends = env:esp32dev
custom_usermods =
  ${env:esp32dev.custom_usermods}
  https://github.com/JPChurchouse/WLED-BLE-Control-Usermod.git
```

For a local checkout, use `symlink:///path/to/WLED-BLE-Control-Usermod` instead of the URL. Then build and flash as usual: `pio run -e esp32dev_ble`.

The usermod adjusts the build for you:
- **LED driver.** On the classic ESP32 it switches WLED from its "RMT-HI" LED driver to the standard RMT driver. RMT-HI shares an interrupt with the Bluetooth controller and crashes the device once Bluetooth starts. On very long strips under heavy Wi-Fi traffic, the standard driver may occasionally glitch.
- **NimBLE build fix.** It resolves a duplicate-symbol clash between NimBLE-Arduino and the Bluetooth library that ships with WLED's framework.

**Flash size.** The firmware, including audioreactive, fits WLED's default 4 MB partition layout, so OTA updates keep working. If you add many other usermods and it no longer fits, use `board_build.partitions = ${esp32.big_partitions}`. Changing the partition table needs a USB flash and erases the filesystem, so back up your config and presets first.

**Wi-Fi sleep.** Bluetooth needs Wi-Fi modem sleep, so the usermod overrides WLED's "Disable WiFi sleep" option while it's running.

**First OTA install on an ESP32-C3.** On our test board, the stock WLED 16.0.1 C3 firmware rebooted partway through full-speed uploads. A throttled upload worked, e.g. `curl --limit-rate 40K -F "update=@firmware.bin" http://<device-ip>/update`. Later updates onto this usermod's firmware work at full speed.

## Pairing a watch

1. In WLED, open **Config → Usermods → BLEControl** and note the **passkey**. It's generated on first boot; you can change it there.
2. Open WLED's **Info** page and press **BLE pairing: Enable**. The window stays open for 120 s by default.
3. In the WledBleWear app, scan, pick your device, and enter the passkey when the watch asks.

Once paired, the watch reconnects whenever it needs to; no pairing window is needed again. Up to 3 watches can be paired, and one can be connected at a time.

To unpair everything, tick **Forget all devices** in the usermod settings and save.

## Settings (Config → Usermods → BLEControl)

| Setting | Default | Notes |
|---|---|---|
| enabled | on | Reboot to apply |
| passkey | random | 6 digits, entered on the watch when pairing |
| pairingWindowSec | 120 | How long the window stays open after pressing Enable |
| forgetAllDevices | off | One-shot: unpairs every watch when you save |

The same actions are available through the JSON API: `POST /json/state` with `{"ble":{"pair":true}}` or `{"ble":{"forget":true}}`.

## Memory

Measured with audioreactive running:

| Board | Free heap before | With BLE | Notes |
|---|---|---|---|
| ESP32 (Gledopto, 144 LEDs) | 118 KB | about 52 KB | The Bluetooth controller reserves about 46 KB at boot, and the usermod uses about 15 KB more. With the web UI open, it drops to about 25 KB |
| ESP32-C3 (288 LEDs) | 150 KB | about 81 KB | About 47 KB is used when BLE starts |

That works in practice, but be careful about stacking other memory-hungry usermods, especially on the classic ESP32.

## Development

This repo builds only as part of a WLED checkout (see Installation). `tools/` contains:
- `ble_client.py`: a Python reference client (`pip install bleak`) to scan, pair, read state, list presets and send commands.
- `protocol_test.py`: an end-to-end test against a real device, checked against WLED's HTTP API.
- `build_flags.py`: the PlatformIO build adjustments described above.

## License

[EUPL-1.2](LICENSE) or later, the same as WLED.
