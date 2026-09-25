#!/usr/bin/env python3
"""End-to-end protocol test against a real device, checked against WLED's HTTP API.

The PC must already be paired (tools/ble_client.py pair PASSKEY). The test
changes the device's state and, with --paging, temporarily adds presets
200..(200+N) plus a playlist; it deletes them and restores power, brightness
and preset at the end.

  python tools/protocol_test.py --address D4:E9:F4:7C:4F:76 --host 192.168.1.91 [--paging 40]
"""

import argparse
import asyncio
import json
import struct
import sys
import time
import urllib.request

from ble_client import COMMAND, INFO, PRESETS, STATE, client, decode_info, decode_state, read_presets

PASS, FAIL = "PASS", "FAIL"
results = []


def check(name, ok, detail=""):
    results.append((name, ok))
    print(f"[{PASS if ok else FAIL}] {name}" + (f"  ({detail})" if detail else ""))


class Wled:
    def __init__(self, host):
        self.base = f"http://{host}"

    def get(self, path):
        return json.load(urllib.request.urlopen(self.base + path, timeout=5))

    def post(self, obj):
        req = urllib.request.Request(self.base + "/json/state", data=json.dumps(obj).encode(),
                                     headers={"Content-Type": "application/json"})
        return json.load(urllib.request.urlopen(req, timeout=5))

    def state(self):
        return self.get("/json/state")


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--address", required=True)
    ap.add_argument("--host", required=True)
    ap.add_argument("--paging", type=int, default=0, help="add N temporary presets to test paging")
    args = ap.parse_args()
    wled = Wled(args.host)
    orig = wled.state()
    print(f"original: on={orig['on']} bri={orig['bri']} ps={orig['ps']}")

    temp_ids = []
    if args.paging:
        for i in range(args.paging):
            pid = 200 + i
            name = f"Paging test preset number {pid} with a long name ✨ überlänge"
            body = {"psave": pid, "n": name, "ib": True, "sb": True}
            if i % 5 == 0:
                body["ql"] = "🔥" if i % 10 == 0 else "Q"
            wled.post(body)
            temp_ids.append(pid)
            time.sleep(0.15)
        pl = 200 + args.paging
        wled.post({"psave": pl, "n": "Test playlist", "playlist": {"ps": [1, 2], "dur": [100, 100], "transition": [0, 0], "repeat": 0}})
        temp_ids.append(pl)
        # Saving a playlist through the JSON API also starts it; stop it again.
        wled.post({"ps": orig["ps"]} if orig["ps"] > 0 else {"on": orig["on"]})
        time.sleep(1.5)  # let the usermod rebuild its cache

    try:
        await run(args, wled, orig, temp_ids)
    finally:
        for attempt in range(5):  # a busy device can drop a pdel; retry until they're all gone
            left = [pid for pid in temp_ids if str(pid) in wled.get("/presets.json")]
            if not left:
                break
            for pid in left:
                wled.post({"pdel": pid})
                time.sleep(0.3)
        else:
            print(f"WARNING: could not delete temporary presets {left}")
        wled.post({"on": orig["on"], "bri": orig["bri"]})
        if orig["ps"] > 0:
            wled.post({"ps": orig["ps"]})
        print("restored original state" + (f", deleted {len(temp_ids)} temporary presets" if temp_ids else ""))

    failed = [n for n, ok in results if not ok]
    print(f"\n{len(results) - len(failed)}/{len(results)} passed" + (f"; failed: {', '.join(failed)}" if failed else ""))
    sys.exit(1 if failed else 0)


async def run(args, wled, orig, temp_ids):
    t0 = time.perf_counter()
    async with client(args.address) as c:
        check("connect", True, f"{time.perf_counter() - t0:.2f}s")
        info = decode_info(await c.read_gatt_char(INFO))
        check("info protocol v1", info["protocol"] == 1, str(info))

        state = decode_state(await c.read_gatt_char(STATE))
        check("state matches HTTP", state["power"] == wled.state()["on"] and state["bri"] == wled.state()["bri"], str(state))

        # Preset list vs presets.json
        rev, records = await read_presets(c)
        pj = wled.get("/presets.json")
        expected = sorted(int(k) for k in pj if k.isdigit() and 1 <= int(k) <= 250)
        check("preset ids match presets.json", [r["id"] for r in records] == expected, f"{len(records)} presets, rev {rev:04x}")
        for r in records:
            p = pj[str(r["id"])]
            want = (p.get("n") or f"Preset {r['id']}").encode()[:32].decode("utf-8", "ignore")
            if r["name"] != want or r["playlist"] != ("playlist" in p) or r["favourite"] != bool(p.get("ql")):
                check(f"preset {r['id']} record", False, f"got {r}, file {p.get('n')!r} ql={p.get('ql')!r} playlist={'playlist' in p}")
                break
        else:
            check("preset names/flags match", True)
        check("state revision == list revision", decode_state(await c.read_gatt_char(STATE))["rev"] == f"{rev:04x}")

        notes, replies = [], []
        await c.start_notify(STATE, lambda _, d: notes.append((time.perf_counter(), decode_state(d))))
        await c.start_notify(COMMAND, lambda _, d: replies.append((d[0], d[1])))

        async def cmd(payload, expect_status=0x00):
            replies.clear()
            n0 = len(notes)
            ts = time.perf_counter()
            await c.write_gatt_char(COMMAND, payload, response=True)
            for _ in range(200):
                if replies:
                    break
                await asyncio.sleep(0.01)
            await asyncio.sleep(0.4)  # let WLED apply it and the throttled notification arrive
            status = replies[0][1] if replies else None
            ok = status == expect_status
            latency = f"{(notes[n0][0] - ts) * 1000:.0f} ms" if len(notes) > n0 else "no notify"
            return ok, status, latency

        async def expect(name, payload, http_check, expect_status=0x00):
            ok, status, latency = await cmd(payload, expect_status)
            s = wled.state()
            good = ok and http_check(s)
            check(name, good, f"status {status}, {latency}, http on={s['on']} bri={s['bri']} ps={s['ps']} nl={s['nl']['on']}")

        await expect("POWER on", bytes([1, 1]), lambda s: s["on"])
        await expect("POWER off", bytes([1, 0]), lambda s: not s["on"])
        await expect("POWER toggle", bytes([1, 2]), lambda s: s["on"])
        await expect("BRIGHTNESS_SET 80", bytes([2, 80]), lambda s: s["bri"] == 80)
        await expect("BRIGHTNESS_STEP +20", bytes([3]) + struct.pack("<b", 20), lambda s: s["bri"] == 100)
        await expect("BRIGHTNESS_STEP -128 clamps to 1", bytes([3]) + struct.pack("<b", -128), lambda s: s["bri"] == 1)
        await expect("BRIGHTNESS_SET 0 invalid", bytes([2, 0]), lambda s: True, 0x82)
        await expect("BRIGHTNESS_SET 60", bytes([2, 60]), lambda s: s["bri"] == 60)
        wled.post({"on": False})
        await asyncio.sleep(0.3)
        await expect("STEP -10 while off does nothing", bytes([3]) + struct.pack("<b", -10), lambda s: not s["on"])
        await expect("STEP +10 while off turns on", bytes([3, 10]), lambda s: s["on"])

        ids = [r["id"] for r in records]
        favs = [r["id"] for r in records if r["favourite"]]
        if ids:
            await expect(f"PRESET_APPLY {ids[0]}", bytes([4, ids[0]]), lambda s: s["ps"] == ids[0])
            if len(ids) > 1:
                await expect("PRESET_NEXT", bytes([5, 0]), lambda s: s["ps"] == ids[1])
                await expect("PRESET_PREV", bytes([6, 0]), lambda s: s["ps"] == ids[0])
                await expect("PRESET_PREV wraps", bytes([6, 0]), lambda s: s["ps"] == ids[-1])
            missing = next(i for i in range(1, 251) if i not in ids)
            await expect(f"PRESET_APPLY missing {missing} invalid", bytes([4, missing]), lambda s: True, 0x82)
        if favs:
            await expect("PRESET_NEXT favourites", bytes([5, 1]), lambda s: s["ps"] in favs)
        else:
            await expect("PRESET_NEXT favourites, none", bytes([5, 1]), lambda s: True, 0x82)

        wled.post({"on": True, "bri": 128})
        await asyncio.sleep(0.3)
        await expect("NIGHTLIGHT 5 min", bytes([7, 5]), lambda s: s["nl"]["on"] and s["nl"]["dur"] == 5 and s["on"])
        st = decode_state(await c.read_gatt_char(STATE))
        check("State nightlight flag + remaining", st["nightlight"] and 290 <= st["nl_remaining"] <= 300, str(st))
        await expect("NIGHTLIGHT cancel", bytes([7, 0]), lambda s: not s["nl"]["on"])

        await expect("unknown opcode", bytes([0x42, 0]), lambda s: True, 0x80)
        await expect("bad length", bytes([2]), lambda s: True, 0x81)

        # External change -> notification
        n0 = len(notes)
        ts = time.perf_counter()
        wled.post({"bri": 42})
        for _ in range(300):
            if len(notes) > n0:
                break
            await asyncio.sleep(0.01)
        check("HTTP change notifies", len(notes) > n0 and notes[-1][1]["bri"] == 42,
              f"{(notes[-1][0] - ts) * 1000:.0f} ms" if len(notes) > n0 else "none")

        # Burst: throttled to <= 1 per 250 ms, last state delivered
        n0 = len(notes)
        for b in range(10, 60, 5):
            wled.post({"bri": b})
        await asyncio.sleep(1.0)
        burst = notes[n0:]
        check("burst throttled, final state delivered", burst and burst[-1][1]["bri"] == 55 and len(burst) <= 6,
              f"{len(burst)} notifications, last bri {burst[-1][1]['bri'] if burst else None}")

        await c.write_gatt_char(COMMAND, bytes([8]), response=True)
        for _ in range(200):
            if not c.is_connected:
                break
            await asyncio.sleep(0.02)
        check("OFF_AND_DISCONNECT drops the link", not c.is_connected)
        check("OFF_AND_DISCONNECT turned off", not wled.state()["on"])

    # One-shot action, as a tile would do it
    t = time.perf_counter()
    async with client(args.address) as c:
        await c.write_gatt_char(COMMAND, bytes([1, 1]), response=True)
    dt = time.perf_counter() - t
    await asyncio.sleep(0.3)
    check("one-shot connect+command+disconnect", wled.state()["on"], f"{dt:.2f}s")


if __name__ == "__main__":
    asyncio.run(main())
