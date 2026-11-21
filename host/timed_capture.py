"""Timed capture (0xB8, F5) through the BLE-USB bridge: the boards take a picture every period
by themselves; this tool collects them and reports the period actually reached.

  python timed_capture.py --period-ms 1000 --format raw --duration 20
  python timed_capture.py --period-ms 500 1000 2000 --format raw jpeg:85 --duration 20 --csv logs/timed.csv

Per board and setting: pictures, the capture interval (frame header capture_us, board clock),
how many periods were skipped (header flag 0x04 / gaps), lost packets. The boards are rebooted
('X') before and after each setting, so 0xB1 / 0xB2 state from earlier tools does not matter.
"""
import argparse
import csv
import statistics
import time

import bridge
import protocol

FORMATS = {"raw": (protocol.FORMAT_RAW, 0), "png": (protocol.FORMAT_PNG, 0),
           **{f"jpeg:{q}": (protocol.FORMAT_JPEG, q) for q in range(1, 101)}}


def run(br, boards, period_ms, fmt, duration):
    frames = [[] for _ in boards]
    asms = [protocol.FrameAssembler(lambda f, i=i: frames[i].append(f), lambda m: None) for i in range(len(boards))]
    br.on_notify = lambda s, d: asms[s].feed(d) if s < len(asms) else None
    mask = (1 << len(boards)) - 1
    t_x = time.monotonic()
    br.disconnect(mask)  # fresh boot: neither 0xB1 nor 0xB2 used yet
    time.sleep(0.3)
    if not br.wait_ready(range(len(boards)), timeout=30, after=t_x):
        raise SystemExit(f"boards not ready: {br.links}")
    time.sleep(0.5)
    br.write(mask, protocol.set_format_command(*FORMATS[fmt]))
    br.write(mask, protocol.timed_command(period_ms))
    time.sleep(duration)
    br.write(mask, protocol.timed_command(0))
    time.sleep(0.5 + max(1.0, period_ms / 1000))  # the picture in flight
    rows = []
    for i, b in enumerate(boards):
        fs = [f for f in frames[i] if f.header_flags & protocol.HEADER_TIMED]
        # Skip the first picture: it includes the camera start (sensor init), like stereo.py's warm-up.
        steady = fs[1:]
        caps = [f.capture_us for f in steady]
        gaps = [((b2 - a2) & 0xFFFFFFFF) / 1000 for a2, b2 in zip(caps, caps[1:])]
        skips = sum(1 for f in steady if f.header_flags & protocol.HEADER_SKIPPED)
        sizes = [len(f.data) for f in steady]
        row = {"board": b, "period_ms": period_ms, "format": fmt, "pictures": len(fs),
               "interval_ms_mean": round(statistics.mean(gaps), 1) if gaps else None,
               "interval_ms_min": round(min(gaps), 1) if gaps else None,
               "interval_ms_max": round(max(gaps), 1) if gaps else None,
               "skipped_flags": skips, "lost_packets": sum(f.lost_packets for f in fs),
               "bytes_mean": round(statistics.mean(sizes)) if sizes else None,
               "rx_s_mean": round(statistics.mean(f.duration_s for f in steady), 2) if steady else None,
               "size": f"{fs[0].width}x{fs[0].height}" if fs else ""}
        rows.append(row)
        print(f"  {b}: {row['pictures']} pictures {row['size']}, interval {row['interval_ms_mean']} ms "
              f"({row['interval_ms_min']}-{row['interval_ms_max']}), skipped flags {skips}, "
              f"lost packets {row['lost_packets']}, {row['bytes_mean']} B, rx {row['rx_s_mean']} s", flush=True)
    br.disconnect(mask)
    time.sleep(0.3)
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--boards", nargs="+", default=["hm0360-5340", "hm0360b-5340"], choices=sorted(protocol.BOARDS))
    ap.add_argument("--period-ms", nargs="+", type=int, default=[1000])
    ap.add_argument("--format", nargs="+", default=["raw"], choices=list(FORMATS), metavar="FMT",
                    help="raw, png or jpeg:<quality 1-100>")
    ap.add_argument("--duration", type=float, default=20.0, help="seconds per setting")
    ap.add_argument("--port")
    ap.add_argument("--csv", help="append one row per board and setting")
    ap.add_argument("--label", default="")
    args = ap.parse_args()
    rows = []
    with bridge.Bridge(args.port, on_text=lambda s, t: None, on_event=lambda s, l: None) as br:
        br.configure([protocol.BOARDS[b]["address"] for b in args.boards])
        for fmt in args.format:
            for p in args.period_ms:
                print(f"period {p} ms, {fmt}, {args.duration:.0f} s:", flush=True)
                rows += [dict(r, label=args.label) for r in run(br, args.boards, p, fmt, args.duration)]
    if args.csv and rows:
        new = not __import__("os").path.exists(args.csv)
        with open(args.csv, "a", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0]))
            if new:
                w.writeheader()
            w.writerows(rows)


if __name__ == "__main__":
    main()
