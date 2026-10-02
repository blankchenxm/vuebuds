"""Receive camera frames from the "mustard" firmware over BLE and save them as PNG.

Usage:
  tools/.venv/Scripts/python tools/ble_receive.py [--frames 3] [--timeout 60] [--out _build_win/frames]

The protocol (frame header with width/height) is parsed by host/protocol.py.
"""
import argparse
import asyncio
import pathlib
import sys
import time

from bleak import BleakClient, BleakScanner
from PIL import Image

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "host"))
import protocol  # noqa: E402


class FrameSaver:
    def __init__(self, out_dir: pathlib.Path):
        self.out_dir = out_dir
        self.assembler = protocol.FrameAssembler(self._save, lambda m: print(f"  {m}"))

    def _save(self, frame: protocol.Frame):
        path = self.out_dir / f"frame_{frame.index:03d}.png"
        Image.frombytes("L", (frame.width, frame.height), frame.pixels).save(path)
        (self.out_dir / f"frame_{frame.index:03d}.raw").write_bytes(frame.pixels)
        print(f"  frame {frame.index}: {frame.width}x{frame.height} in {frame.duration_s:.2f}s, "
              f"lost packets so far: {self.assembler.total_lost} -> {path}")


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", default="mustard")
    ap.add_argument("--address", help="BLE address, skips name scan")
    ap.add_argument("--board", choices=sorted(protocol.BOARDS), help="known DK: sets --address")
    ap.add_argument("--frames", type=int, default=3)
    ap.add_argument("--timeout", type=float, default=60.0)
    ap.add_argument("--out", default="_build_win/frames")
    args = ap.parse_args()

    out_dir = pathlib.Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    if args.board and not args.address:
        args.address = protocol.BOARDS[args.board]["address"]
    if args.address:
        device = args.address
    else:
        print(f"scanning for '{args.name}'...")
        device = await BleakScanner.find_device_by_name(args.name, timeout=15.0)
        if device is None:
            raise SystemExit(f"device '{args.name}' not found")
        print(f"found {device.address}")

    saver = FrameSaver(out_dir)
    asm = saver.assembler
    async with BleakClient(device) as client:
        print(f"connected, mtu={client.mtu_size}")
        await client.start_notify(protocol.DATA_UUID, lambda _, d: asm.feed(bytes(d)))
        await client.write_gatt_char(protocol.CONTROL_UUID, bytes([protocol.CMD_STREAM_START]), response=False)
        print("stream start (0xB1) sent, waiting for frames...")

        deadline = time.monotonic() + args.timeout
        last_report = time.monotonic()
        while asm.frames < args.frames and time.monotonic() < deadline:
            await asyncio.sleep(0.2)
            if time.monotonic() - last_report > 5:
                print(f"  ...{asm.total_packets} packets, {asm.partial_bytes()} bytes in current frame")
                last_report = time.monotonic()

    print(f"done: {asm.frames} frame(s), {asm.total_packets} packets, {asm.total_lost} lost")
    if asm.frames == 0:
        raise SystemExit(1)


if __name__ == "__main__":
    asyncio.run(main())
