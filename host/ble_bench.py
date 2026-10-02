"""Measure BLE streaming throughput of the "mustard" firmware, for comparing builds / sessions.

Resets the board (so the RTT log starts at boot), streams for --duration seconds and prints:
  - the connection parameters the firmware reports (interval, PHY, data length, MTU)
  - frames / fps, time per frame (from the 2nd frame on), packets per second
  - packets per connection event, estimated from notification arrival gaps
  - firmware frame lines that were dropped as bad (PR #20)

Usage:
  .venv/Scripts/python ble_bench.py --board hm01b0 --duration 60 [--label main] [--csv logs/bench.csv]

BLE throughput changes with the radio environment, so compare builds by alternating them
in the same session (A B A B ...), not against numbers from another day.
"""
import argparse
import asyncio
import csv
import datetime
import pathlib
import re
import statistics
import subprocess
import time

from bleak import BleakClient

import protocol

HERE = pathlib.Path(__file__).resolve().parent
JLINK_DIR = pathlib.Path(r"C:\Program Files\SEGGER\JLink")


def start_rtt(snr: str, out_file: pathlib.Path) -> subprocess.Popen:
    # Reset first: nrfjprog and the RTT logger cannot share the probe.
    subprocess.run(["nrfjprog", "-f", "nrf52", "--snr", snr, "--reset"], capture_output=True)
    if out_file.exists():
        out_file.unlink()
    return subprocess.Popen(
        [str(JLINK_DIR / "JLinkRTTLogger.exe"), "-USB", snr, "-Device", "NRF52840_XXAA", "-If", "SWD",
         "-Speed", "4000", "-RTTChannel", "0", str(out_file)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL)


def parse_rtt(text: str) -> dict:
    info = {"intervals_ms": re.findall(r"Connection interval updated: (\d+), (\d+)", text),
            "phy": re.findall(r"PHY update \w+\. PHY set to ([^.\n]+)", text),
            "data_length": re.findall(r"Data length updated to (\d+)", text),
            "mtu": re.findall(r"MTU set to (\d+)", text),
            "fw_frames": len(re.findall(r"\[cam\] frame \d+:", text)),
            "fw_bad": len(re.findall(r"BAD, dropped", text))}
    return info


async def stream(address: str, duration: float):
    arrivals = []  # (time, seq, start_of_frame)
    frames = []

    def on_frame(f: protocol.Frame):
        frames.append(f)

    asm = protocol.FrameAssembler(on_frame, lambda m: None)

    def on_notify(_, data: bytearray):
        if len(data) >= 2:
            arrivals.append((time.perf_counter(), data[0], bool(data[1] & protocol.FLAG_START_OF_FRAME)))
        asm.feed(bytes(data))

    async with BleakClient(address, timeout=20.0) as client:
        mtu = client.mtu_size
        await client.start_notify(protocol.DATA_UUID, on_notify)
        t0 = time.perf_counter()
        await client.write_gatt_char(protocol.CONTROL_UUID, bytes([protocol.CMD_STREAM_START]), response=False)
        await asyncio.sleep(duration)
    return t0, mtu, arrivals, frames, asm


def bursts(arrivals, gap_s: float):
    """Group notifications whose arrival gaps are below gap_s: one group ~ one connection event."""
    sizes, n, last = [], 0, None
    for t, _, _ in arrivals:
        if last is not None and t - last > gap_s:
            sizes.append(n)
            n = 0
        n += 1
        last = t
    if n:
        sizes.append(n)
    return sizes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--board", choices=sorted(protocol.BOARDS), required=True)
    ap.add_argument("--duration", type=float, default=60.0, help="streaming time in seconds")
    ap.add_argument("--label", default="", help="name of the build under test, written to the CSV")
    ap.add_argument("--csv", help="append one summary row to this CSV file")
    ap.add_argument("--gap-ms", type=float, default=2.0, help="arrival gap that separates connection events")
    args = ap.parse_args()

    board = protocol.BOARDS[args.board]
    stamp = f"{datetime.datetime.now():%Y%m%d-%H%M%S}"
    rtt_file = HERE / "logs" / f"bench-rtt-{stamp}.log"
    rtt_file.parent.mkdir(exist_ok=True)
    rtt = start_rtt(board["snr"], rtt_file)
    time.sleep(2.0)  # firmware boot + advertising
    try:
        t0, mtu, arrivals, frames, asm = asyncio.run(stream(board["address"], args.duration))
    finally:
        time.sleep(1.0)
        rtt.kill()
    fw = parse_rtt(rtt_file.read_text(errors="replace") if rtt_file.exists() else "")

    # The first frame includes stream start-up and the pre-update connection interval.
    steady = [f.duration_s for f in frames[1:]]
    span = (frames[-1].completed_at - frames[0].completed_at) if len(frames) > 1 else 0.0
    fps = (len(frames) - 1) / span if span > 0 else 0.0
    t_first = (arrivals[0][0] - t0) if arrivals else 0.0
    pkt_rate = len(arrivals) / (arrivals[-1][0] - arrivals[0][0]) if len(arrivals) > 1 else 0.0
    ev = bursts([a for a in arrivals if frames and a[0] >= frames[0].completed_at], args.gap_ms / 1000)

    print(f"board {args.board} ({board['address']}), {args.duration:.0f} s, label '{args.label}'")
    print(f"  link: intervals(ms) {fw['intervals_ms']}, PHY {fw['phy']}, data length {fw['data_length']}, "
          f"MTU fw {fw['mtu']} / host {mtu}")
    print(f"  frames {len(frames)}, steady fps {fps:.2f} (from frame 2), lost packets {asm.total_lost}, "
          f"discarded partial {asm.discarded}")
    if steady:
        print(f"  time per frame: median {statistics.median(steady):.2f} s, min {min(steady):.2f}, "
              f"max {max(steady):.2f}; first frame {frames[0].duration_s:.2f} s, first packet {t_first:.2f} s")
    print(f"  packets/s {pkt_rate:.0f}; per connection event (gap > {args.gap_ms} ms): "
          f"median {statistics.median(ev) if ev else 0}, mean {statistics.mean(ev) if ev else 0:.1f}, "
          f"max {max(ev) if ev else 0}, events/s {len(ev) / span if span else 0:.0f}")
    print(f"  firmware: {fw['fw_frames']} frames captured, {fw['fw_bad']} dropped as bad; RTT log {rtt_file}")

    if args.csv:
        path = pathlib.Path(args.csv)
        new = not path.exists()
        with open(path, "a", newline="") as f:
            w = csv.writer(f)
            if new:
                w.writerow(["time", "board", "label", "duration_s", "frames", "fps", "median_frame_s",
                            "packets_per_s", "pkts_per_event_mean", "lost", "fw_bad", "intervals_ms", "phy"])
            w.writerow([stamp, args.board, args.label, args.duration, len(frames), f"{fps:.3f}",
                        f"{statistics.median(steady):.3f}" if steady else "", f"{pkt_rate:.1f}",
                        f"{statistics.mean(ev):.2f}" if ev else "", asm.total_lost, fw["fw_bad"],
                        " ".join("/".join(i) for i in fw["intervals_ms"]), " ".join(fw["phy"])])


if __name__ == "__main__":
    main()
