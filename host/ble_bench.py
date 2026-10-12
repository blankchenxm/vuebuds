"""Measure BLE streaming throughput of the "mustard" firmware, for comparing builds / sessions.

Resets the board (so the RTT log starts at boot), streams for --duration seconds and prints:
  - the connection parameters the firmware reports (interval, PHY, data length, MTU)
  - frames / fps, time per frame (from the 2nd frame on), packets per second
  - packets per connection event, estimated from notification arrival gaps
  - firmware frame lines that were dropped as bad (PR #20)

Usage:
  .venv/Scripts/python ble_bench.py --board hm01b0 --duration 60 [--label main] [--csv logs/bench.csv]
  .venv/Scripts/python ble_bench.py --bridge --board hm0360-5340 hm0360b-5340 --interval-ms 15
      through the BLE-USB bridge (fw_bridge/, bridge.py): one or two boards stream at once,
      with the connection interval the bridge sets; one summary line / CSV row per board

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


def start_rtt(board: dict, out_file: pathlib.Path) -> subprocess.Popen:
    # Reset first: nrfjprog and the RTT logger cannot share the probe.
    snr = board["snr"]
    family = board.get("family", protocol.DEFAULT_FAMILY)
    subprocess.run(["nrfjprog", "-f", family, "--snr", snr, "--reset"], capture_output=True)
    if out_file.exists():
        out_file.unlink()
    return subprocess.Popen(
        [str(JLINK_DIR / "JLinkRTTLogger.exe"), "-USB", snr, "-Device", board.get("device", protocol.DEFAULT_DEVICE), "-If", "SWD",
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


async def stream_bridge(addresses: list[str], duration: float, interval_ms: float, port, reset):
    """Stream from all boards at once through the bridge. reset() resets the boards (RTT start)."""
    import bridge
    n = len(addresses)
    arrivals = [[] for _ in range(n)]
    frames = [[] for _ in range(n)]
    asms = [protocol.FrameAssembler(frames[i].append, lambda m: None) for i in range(n)]

    def on_notify(slot, data):
        if len(data) >= 2:
            arrivals[slot].append((time.perf_counter(), data[0], bool(data[1] & protocol.FLAG_START_OF_FRAME)))
        asms[slot].feed(data)

    with bridge.Bridge(port, on_notify=on_notify, on_text=lambda s, t: None) as br:
        br.configure(addresses, interval_ms)
        reset()
        t_reset = time.monotonic()
        if not br.wait_ready(range(n), timeout=30.0, after=t_reset):
            raise RuntimeError(f"boards not ready through the bridge: {br.links}")
        await asyncio.sleep(0.5)  # MTU / PHY / data length settle
        t0 = time.perf_counter()
        br.write((1 << n) - 1, bytes([protocol.CMD_STREAM_START]))
        await asyncio.sleep(duration)
        links = [bridge.Link(**vars(l)) for l in br.links[:n]]
        br.disconnect((1 << n) - 1)  # the boards reboot: no stream left running for the next tool
        time.sleep(0.2)
    return t0, links, arrivals, frames, asms


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
    ap.add_argument("--board", nargs="+", choices=sorted(protocol.BOARDS), required=True,
                    help="one board; with --bridge one or two")
    ap.add_argument("--bridge", nargs="?", const="", default=None, metavar="PORT",
                    help="stream through the BLE-USB bridge (COM port, default: found by USB VID)")
    ap.add_argument("--interval-ms", type=float, default=15.0, help="with --bridge: connection interval")
    ap.add_argument("--duration", type=float, default=60.0, help="streaming time in seconds")
    ap.add_argument("--label", default="", help="name of the build under test, written to the CSV")
    ap.add_argument("--csv", help="append one summary row to this CSV file")
    ap.add_argument("--gap-ms", type=float, default=2.0, help="arrival gap that separates connection events")
    args = ap.parse_args()

    names = args.board
    if args.bridge is None and len(names) != 1:
        ap.error("several boards need --bridge")
    stamp = f"{datetime.datetime.now():%Y%m%d-%H%M%S}"
    (HERE / "logs").mkdir(exist_ok=True)
    rtt_files = [HERE / "logs" / f"bench-rtt-{stamp}-{n}.log" for n in names]
    rtts = []

    def reset():
        for n, f in zip(names, rtt_files):
            rtts.append(start_rtt(protocol.BOARDS[n], f))

    try:
        if args.bridge is None:
            import bridge
            if bridge.release():  # a plugged-in bridge would hold the board's only connection
                print("bridge released the boards")
            reset()
            time.sleep(2.0)  # firmware boot + advertising
            t0, mtu, arrivals, frames, asm = asyncio.run(stream(protocol.BOARDS[names[0]]["address"], args.duration))
            results = [(names[0], f"direct, host MTU {mtu}", arrivals, frames, asm)]
        else:
            t0, links, arrivals, frames, asms = asyncio.run(stream_bridge(
                [protocol.BOARDS[n]["address"] for n in names], args.duration, args.interval_ms,
                args.bridge or None, reset))
            results = [(n, f"bridge, {len(names)} board(s), interval {l.interval_ms:g} ms, PHY {l.phy}, "
                           f"data length {l.data_length}, MTU {l.mtu}", arrivals[i], frames[i], asms[i])
                       for i, (n, l) in enumerate(zip(names, links))]
    finally:
        time.sleep(1.0)
        for r in rtts:
            r.kill()

    for (name, how, arrivals, frames, asm), rtt_file in zip(results, rtt_files):
        report(args, name, how, t0, arrivals, frames, asm, rtt_file, stamp)


def report(args, name, how, t0, arrivals, frames, asm, rtt_file, stamp):
    board = protocol.BOARDS[name]
    fw = parse_rtt(rtt_file.read_text(errors="replace") if rtt_file.exists() else "")

    # The first frame includes stream start-up and the pre-update connection interval.
    steady = [f.duration_s for f in frames[1:]]
    span = (frames[-1].completed_at - frames[0].completed_at) if len(frames) > 1 else 0.0
    fps = (len(frames) - 1) / span if span > 0 else 0.0
    t_first = (arrivals[0][0] - t0) if arrivals else 0.0
    pkt_rate = len(arrivals) / (arrivals[-1][0] - arrivals[0][0]) if len(arrivals) > 1 else 0.0
    ev = bursts([a for a in arrivals if frames and a[0] >= frames[0].completed_at], args.gap_ms / 1000)
    kbs = sum(len(f.pixels) for f in frames[1:]) / span / 1024 if span else 0.0

    print(f"board {name} ({board['address']}), {args.duration:.0f} s, label '{args.label}', {how}")
    print(f"  firmware link: intervals(ms) {fw['intervals_ms']}, PHY {fw['phy']}, data length {fw['data_length']}, "
          f"MTU {fw['mtu']}")
    print(f"  frames {len(frames)}, steady fps {fps:.2f} (from frame 2), {kbs:.0f} KB/s, lost packets {asm.total_lost}, "
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
                w.writerow(["time", "board", "label", "link", "duration_s", "frames", "fps", "kb_per_s",
                            "median_frame_s", "packets_per_s", "pkts_per_event_mean", "lost", "fw_bad",
                            "intervals_ms", "phy"])
            w.writerow([stamp, name, args.label, how, args.duration, len(frames), f"{fps:.3f}", f"{kbs:.1f}",
                        f"{statistics.median(steady):.3f}" if steady else "", f"{pkt_rate:.1f}",
                        f"{statistics.mean(ev):.2f}" if ev else "", asm.total_lost, fw["fw_bad"],
                        " ".join("/".join(i) for i in fw["intervals_ms"]), " ".join(fw["phy"])])


if __name__ == "__main__":
    main()
