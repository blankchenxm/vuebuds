"""Find which BLE peripheral is which board on this Mac and write boards.local.json.

All boards advertise as "mustard", and macOS (CoreBluetooth) hides BLE addresses: bleak sees
a UUID per peripheral that this Mac makes up. To tell the boards apart without changing the
firmware, each board's network core (the radio) is halted over J-Link for a few seconds; the
"mustard" that stops advertising is that board. The board is reset afterwards.

  python ble_identify.py                         # the two nRF5340 boards
  python ble_identify.py --boards hm0360-5340    # one board
  python ble_identify.py --list                  # only list the "mustard" peripherals in range

Windows / Linux do not need this (bleak connects by address).
"""
import argparse
import asyncio
import json
import threading
import time

from bleak import BleakScanner

import protocol
import devtools  # ../tools (path set by protocol)

NET_DEVICE = "nRF5340_xxAA_NET"


async def scan(seconds: float) -> dict:
    found = await BleakScanner.discover(timeout=seconds, return_adv=True)
    return {addr: adv.rssi for addr, (dev, adv) in found.items()
            if (adv.local_name or dev.name) == protocol.DEVICE_NAME}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    five340 = [b for b, v in protocol.BOARDS.items() if v.get("family") == "nrf53"]
    ap.add_argument("--boards", nargs="+", default=five340, choices=five340)
    ap.add_argument("--scan", type=float, default=4.0, help="seconds per scan")
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args()

    try:
        import bridge
        if bridge.release():  # a connected board does not advertise
            print("bridge released the boards")
            time.sleep(2.0)
    except Exception as e:  # no bridge plugged in (or no pyserial): nothing to release
        print(f"(bridge not released: {type(e).__name__}: {e})")

    everyone = asyncio.run(scan(args.scan))
    print(f'"{protocol.DEVICE_NAME}" in range: ' + (", ".join(f"{a} ({r} dBm)" for a, r in everyone.items()) or "none"))
    if args.list:
        return
    if not everyone:
        raise SystemExit("no board advertising (bridge holding them? boards powered and flashed?)")

    uuids = json.loads(protocol.LOCAL_BOARDS.read_text("utf-8")) if protocol.LOCAL_BOARDS.exists() else {}
    for board in args.boards:
        snr = protocol.BOARDS[board]["snr"]
        # Commander resumes the core when it exits, so the halt lasts for one session: halt,
        # sleep through the scan, resume. The scan starts once the session has connected.
        halt_ms = int((args.scan + 4.0) * 1000)
        session = threading.Thread(target=devtools.jlink_commands,
                                   args=(snr, NET_DEVICE, ["h", f"sleep {halt_ms}", "g"]), daemon=True)
        session.start()
        try:
            time.sleep(2.5)
            seen = asyncio.run(scan(args.scan))
        finally:
            session.join(timeout=30)
            devtools.reset_board(snr, "nrf53")
        gone = sorted(set(everyone) - set(seen))
        if len(gone) != 1:
            print(f"{board} (J-Link {snr}): {len(gone)} peripheral(s) went silent ({gone}); not saved, run again")
            continue
        uuids[board] = gone[0]
        print(f"{board} (J-Link {snr}) = {gone[0]}")
        time.sleep(2.0)  # the board advertises again after its reset before the next one is checked

    protocol.LOCAL_BOARDS.write_text(json.dumps(uuids, indent=2) + "\n", encoding="utf-8")
    print(f"-> {protocol.LOCAL_BOARDS}")


if __name__ == "__main__":
    main()
