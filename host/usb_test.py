"""USB link test for the BLE-USB bridge (fw_bridge, USB test build).

Finds the bridge's CDC ACM port (Zephyr VID 0x2FE3), then measures
  bridge -> PC: the bridge sends N bytes of a u32 counter pattern, the PC checks it;
  PC -> bridge: the PC sends the pattern, the bridge checks it and reports bad words.

    python usb_test.py                 # 3 rounds of 4 MB each way
    python usb_test.py --mb 8 --rounds 5 --port COM25
"""
import argparse
import struct
import sys
import time

import numpy as np
import serial
from serial.tools import list_ports

ZEPHYR_VID = 0x2FE3


def find_port():
    for p in list_ports.comports():
        if p.vid == ZEPHYR_VID:
            return p.device
    sys.exit("bridge USB port not found (is the cable in the 'nRF USB' connector?)")


def read_exact(ser, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = ser.read(n - len(buf))
        if not chunk:
            raise TimeoutError(f"got {len(buf)} of {n} bytes")
        buf += chunk
    return bytes(buf)


def pattern(nbytes):
    return np.arange(nbytes // 4, dtype="<u4").tobytes()


def bridge_to_pc(ser, nbytes):
    ser.write(b"T" + struct.pack("<I", nbytes))
    t0 = time.perf_counter()
    data = read_exact(ser, nbytes)
    secs = time.perf_counter() - t0
    tag, dev_ms = struct.unpack("<cI", read_exact(ser, 5))
    assert tag == b"D", tag
    got = np.frombuffer(data, dtype="<u4")
    bad = int(np.count_nonzero(got != np.arange(len(got), dtype="<u4")))
    return secs, bad, dev_ms


def pc_to_bridge(ser, nbytes):
    data = pattern(nbytes)
    t0 = time.perf_counter()
    ser.write(b"R" + struct.pack("<I", nbytes))
    ser.write(data)
    tag, bad, dev_ms = struct.unpack("<cII", read_exact(ser, 9))
    secs = time.perf_counter() - t0
    assert tag == b"r", tag
    return secs, bad, dev_ms


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port")
    ap.add_argument("--mb", type=float, default=4)
    ap.add_argument("--rounds", type=int, default=3)
    args = ap.parse_args()

    port = args.port or find_port()
    nbytes = int(args.mb * 1024 * 1024) & ~3
    with serial.Serial(port, timeout=5) as ser:
        ser.dtr = True
        time.sleep(0.3)
        ser.reset_input_buffer()
        ser.write(b"I")
        ident = ser.readline().decode(errors="replace").strip()
        print(f"{port}: {ident!r}")
        ok = True
        for r in range(args.rounds):
            for name, fn in (("bridge -> PC", bridge_to_pc), ("PC -> bridge", pc_to_bridge)):
                secs, bad, dev_ms = fn(ser, nbytes)
                ok &= bad == 0
                print(f"round {r + 1} {name}: {nbytes / 1024:.0f} KB in {secs:.2f} s = "
                      f"{nbytes / secs / 1024:.0f} KB/s ({nbytes * 8 / secs / 1e6:.2f} Mbit/s), "
                      f"bad words {bad}, bridge {dev_ms} ms")
        print("PASS" if ok else "FAIL")


if __name__ == "__main__":
    main()
