#!/usr/bin/env python3
"""Reference client and test harness for the WLED BLE Control protocol.

Implements docs/PROTOCOL.md from the client side, as the WearOS app must.
Needs Python 3.10+ and `pip install bleak`. Pairing uses the WinRT API, so
`pair` / `unpair` only work on Windows.

  python tools/ble_client.py scan
  python tools/ble_client.py info
  python tools/ble_client.py pair 123456        # open the pairing window in WLED first
  python tools/ble_client.py state
  python tools/ble_client.py watch              # print State notifications
  python tools/ble_client.py presets
  python tools/ble_client.py cmd power toggle   # also: bri N, step +/-N, preset N,
                                                #  next [fav], prev [fav], nl MIN, off
  python tools/ble_client.py unpair

--address AA:BB:... skips scanning; --name picks a device by advertised name.
"""

import argparse
import asyncio
import struct
import sys
import time

from bleak import BleakClient, BleakScanner

BASE = "8e59{:04x}-14dc-405e-9f47-e363750a861a"
SERVICE, INFO, STATE, COMMAND, PRESETS = (BASE.format(n) for n in range(1, 6))

OPS = {"power": 0x01, "bri": 0x02, "step": 0x03, "preset": 0x04,
       "next": 0x05, "prev": 0x06, "nl": 0x07, "off": 0x08}
STATUS = {0x00: "OK", 0x80: "unknown opcode", 0x81: "bad length", 0x82: "invalid value", 0x83: "busy"}


# ---- Decoding -----------------------------------------------------------------

def _string(buf, pos):
    n = buf[pos]
    return buf[pos + 1:pos + 1 + n].decode("utf-8", "replace"), pos + 1 + n


def decode_info(buf):
    version, features, status = struct.unpack_from("<BHB", buf, 0)
    name, pos = _string(buf, 4)
    wled, _ = _string(buf, pos)
    return dict(protocol=version, features=f"0x{features:04x}", pairing_open=bool(status & 1), name=name, wled=wled)


def decode_state(buf):
    flags, bri, preset, playlist, rev, nl = struct.unpack_from("<BBBBHH", buf, 0)
    return dict(power=bool(flags & 1), nightlight=bool(flags & 2), bri=bri, preset=preset,
                playlist=playlist, rev=f"{rev:04x}", nl_remaining=nl)


def decode_page(buf):
    rev, next_id, count = struct.unpack_from("<HBB", buf, 0)
    pos, records = 4, []
    for _ in range(count):
        pid, flags = buf[pos], buf[pos + 1]
        name, pos = _string(buf, pos + 2)
        ql, pos = _string(buf, pos)
        records.append(dict(id=pid, name=name, ql=ql, playlist=bool(flags & 1), favourite=bool(flags & 2)))
    return rev, next_id, records


def encode_command(words):
    op = words[0]
    if op not in OPS:
        raise SystemExit(f"unknown command {op!r}; one of {', '.join(OPS)}")
    arg = words[1] if len(words) > 1 else None
    if op == "power":
        return bytes([0x01, {"off": 0, "on": 1, "toggle": 2}[arg or "toggle"]])
    if op == "step":
        return bytes([0x03]) + struct.pack("<b", int(arg))
    if op in ("next", "prev"):
        return bytes([OPS[op], 1 if arg == "fav" else 0])
    if op == "off":
        return bytes([0x08])
    return bytes([OPS[op], int(arg)])


# ---- Connection ---------------------------------------------------------------

async def find(args):
    if args.address:
        return args.address

    def match(dev, adv):
        if SERVICE not in [u.lower() for u in adv.service_uuids]:
            return False
        return not args.name or (adv.local_name or dev.name) == args.name

    dev = await BleakScanner.find_device_by_filter(match, timeout=10)
    if not dev:
        raise SystemExit("no WLED BLE device found")
    return dev


def client(target):
    # Don't trust Windows' GATT cache: the service layout changes between builds.
    return BleakClient(target, timeout=15, winrt={"use_cached_services": False})


async def read_presets(c):
    records, start, rev0 = [], 1, None
    while True:
        await c.write_gatt_char(PRESETS, bytes([start]), response=True)
        rev, next_id, page = decode_page(await c.read_gatt_char(PRESETS))
        if rev0 is None:
            rev0 = rev
        elif rev != rev0:  # list changed mid-fetch: start again
            records, start, rev0 = [], 1, None
            continue
        records += page
        if next_id == 0:
            return rev0, records
        start = next_id


# ---- Pairing (Windows) -----------------------------------------------------------

async def winrt_pairing(address):
    from winrt.windows.devices.bluetooth import BluetoothLEDevice
    dev = await BluetoothLEDevice.from_bluetooth_address_async(int(str(address).replace(":", ""), 16))
    if dev is None:
        raise SystemExit("device not found by Windows")
    return dev.device_information.pairing


async def pair(address, pin):
    from winrt.windows.devices.enumeration import (DevicePairingKinds, DevicePairingProtectionLevel,
                                                   DevicePairingResultStatus)
    pairing = await winrt_pairing(address)
    if pairing.is_paired:
        print("already paired")
        return True

    def on_request(_, req):
        if req.pairing_kind == DevicePairingKinds.PROVIDE_PIN:
            req.accept_with_pin(f"{pin:06d}")
        else:
            req.accept()

    token = pairing.custom.add_pairing_requested(on_request)
    try:
        kinds = DevicePairingKinds.PROVIDE_PIN | DevicePairingKinds.CONFIRM_ONLY | DevicePairingKinds.CONFIRM_PIN_MATCH
        result = await pairing.custom.pair_with_protection_level_async(
            kinds, DevicePairingProtectionLevel.ENCRYPTION_AND_AUTHENTICATION)
    finally:
        pairing.custom.remove_pairing_requested(token)
    status = DevicePairingResultStatus(result.status).name
    print(f"pairing: {status}, protection level {result.protection_level_used}")
    return result.status in (DevicePairingResultStatus.PAIRED, DevicePairingResultStatus.ALREADY_PAIRED)


async def unpair(address):
    pairing = await winrt_pairing(address)
    if not pairing.is_paired:
        print("not paired")
        return
    result = await pairing.unpair_async()
    print(f"unpair: status {result.status}")


# ---- Commands -----------------------------------------------------------------

async def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--address")
    ap.add_argument("--name")
    ap.add_argument("action", choices=["scan", "info", "state", "watch", "presets", "cmd", "pair", "unpair"])
    ap.add_argument("words", nargs="*")
    args = ap.parse_args()

    if args.action == "scan":
        found = await BleakScanner.discover(timeout=6, return_adv=True)
        for addr, (dev, adv) in found.items():
            if SERVICE in [u.lower() for u in adv.service_uuids]:
                print(f"{addr}  {adv.local_name or dev.name or '?'}  rssi {adv.rssi}")
        return

    target = await find(args)
    address = getattr(target, "address", target)

    if args.action == "pair":
        if not args.words:
            raise SystemExit("usage: pair PASSKEY")
        ok = await pair(address, int(args.words[0]))
        sys.exit(0 if ok else 1)
    if args.action == "unpair":
        await unpair(address)
        return

    t0 = time.perf_counter()
    async with client(target) as c:
        if args.action == "info":
            print(decode_info(await c.read_gatt_char(INFO)))
        elif args.action == "state":
            print(decode_state(await c.read_gatt_char(STATE)))
        elif args.action == "presets":
            rev, records = await read_presets(c)
            print(f"revision {rev:04x}, {len(records)} presets")
            for r in records:
                tags = " ".join(t for t in ("playlist", "favourite") if r[t])
                print(f"  {r['id']:3d}  {r['name']:<32}  {r['ql']:<4} {tags}")
        elif args.action == "watch":
            await c.start_notify(STATE, lambda _, d: print(time.strftime("%H:%M:%S"), decode_state(d)))
            print("watching State; Ctrl+C to stop")
            while True:
                await asyncio.sleep(1)
        elif args.action == "cmd":
            done = asyncio.Event()
            await c.start_notify(COMMAND, lambda _, d: (print(f"result: op 0x{d[0]:02x} {STATUS.get(d[1], hex(d[1]))}"), done.set()))
            await c.write_gatt_char(COMMAND, encode_command(args.words), response=True)
            try:
                await asyncio.wait_for(done.wait(), 3)
            except asyncio.TimeoutError:
                print("no result notification (device disconnected, or not subscribed)")
    print(f"({time.perf_counter() - t0:.2f}s)")


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
