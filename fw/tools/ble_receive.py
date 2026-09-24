"""Receive HM01B0 frames from the "mustard" firmware over BLE and save them as PNG.

Usage:
  tools/.venv/Scripts/python tools/ble_receive.py [--frames 3] [--timeout 60] [--out _build_win/frames]

Protocol (see ble_cus.c / ble_manager.c):
  control char 0x1403: write 0xB1 to start the camera stream
  data    char 0x1402: notifications of [seq, flags, pixels...]
                       flags bit0 = start of frame, bit1 = 324x239 (else 162x119)
"""
import argparse
import asyncio
import pathlib
import time

from bleak import BleakClient, BleakScanner
from PIL import Image

# As advertised by the device; the comment in ble_cus.h lists these bytes in a different order.
BASE = "47ea{:04x}-a0e4-554e-5282-0afcd3246970"
DATA_UUID = BASE.format(0x1402)
CONTROL_UUID = BASE.format(0x1403)
CMD_STREAM_START = 0xB1


class FrameAssembler:
    def __init__(self, out_dir: pathlib.Path):
        self.out_dir = out_dir
        self.buf = bytearray()
        self.size = None
        self.last_seq = None
        self.lost = 0
        self.frames = 0
        self.packets = 0
        self.t_start = None

    def feed(self, data: bytes):
        if len(data) < 2:
            return
        self.packets += 1
        seq, flags, pixels = data[0], data[1], data[2:]
        if self.last_seq is not None and seq != (self.last_seq + 1) & 0xFF:
            self.lost += (seq - self.last_seq - 1) & 0xFF
        self.last_seq = seq

        if flags & 0x01:
            if self.buf:
                print(f"  discarding partial frame ({len(self.buf)} bytes)")
            self.buf = bytearray()
            self.size = (324, 239) if flags & 0x02 else (162, 119)
            self.t_start = time.monotonic()
        if self.size is None:
            return

        self.buf += pixels
        w, h = self.size
        if len(self.buf) >= w * h:
            self._save(bytes(self.buf[: w * h]), w, h)
            self.buf = bytearray()
            self.size = None

    def _save(self, pixels: bytes, w: int, h: int):
        self.frames += 1
        dt = time.monotonic() - self.t_start
        path = self.out_dir / f"frame_{self.frames:03d}.png"
        Image.frombytes("L", (w, h), pixels).save(path)
        (self.out_dir / f"frame_{self.frames:03d}.raw").write_bytes(pixels)
        print(f"  frame {self.frames}: {w}x{h} in {dt:.2f}s, lost packets so far: {self.lost} -> {path}")


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", default="mustard")
    ap.add_argument("--address", help="BLE address, skips name scan")
    ap.add_argument("--frames", type=int, default=3)
    ap.add_argument("--timeout", type=float, default=60.0)
    ap.add_argument("--out", default="_build_win/frames")
    args = ap.parse_args()

    out_dir = pathlib.Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    if args.address:
        device = args.address
    else:
        print(f"scanning for '{args.name}'...")
        device = await BleakScanner.find_device_by_name(args.name, timeout=15.0)
        if device is None:
            raise SystemExit(f"device '{args.name}' not found")
        print(f"found {device.address}")

    asm = FrameAssembler(out_dir)
    async with BleakClient(device) as client:
        print(f"connected, mtu={client.mtu_size}")
        await client.start_notify(DATA_UUID, lambda _, d: asm.feed(bytes(d)))
        await client.write_gatt_char(CONTROL_UUID, bytes([CMD_STREAM_START]), response=False)
        print("stream start (0xB1) sent, waiting for frames...")

        deadline = time.monotonic() + args.timeout
        last_report = time.monotonic()
        while asm.frames < args.frames and time.monotonic() < deadline:
            await asyncio.sleep(0.2)
            if time.monotonic() - last_report > 5:
                print(f"  ...{asm.packets} packets, {len(asm.buf)} bytes in current frame")
                last_report = time.monotonic()

    print(f"done: {asm.frames} frame(s), {asm.packets} packets, {asm.lost} lost")
    if asm.frames == 0:
        raise SystemExit(1)


if __name__ == "__main__":
    asyncio.run(main())
