"""Left board auto exposure check (hub #68 item 3, F6): both boards run the ring buffer at one
period; afterwards every stored picture is fetched and its exposure (frame header: integration,
analog / digital gain), mean brightness, a horizontal brightness step (row means) and wake -> FVLD
(capture time - kP) are written per picture. Point both cameras at a still scene.

  python ae_check.py --seconds 240 --period-ms 1000 --csv logs/f6/ae.csv --rtt logs/f6/ae

With --rtt and firmware built with CAMERA_REG_DUMP, the sensor registers (ID, revision, AE
settings) printed at camera start are compared between the boards.
"""
import argparse
import csv
import os
import re
import statistics
import time

import bridge
import protocol as P
import devtools  # noqa: E402
from m2_test import Session, collect_ring

REG = re.compile(r"\[regs\] ([0-9a-f]{4}):((?: [0-9a-f]{2})+)")


def regs(path):
    try:
        text = open(path, errors="replace").read().rsplit("[mustard] booting", 1)[-1]
    except OSError:
        return {}
    out = {}
    for base, vals in REG.findall(text):
        for k, v in enumerate(vals.split()):
            out[int(base, 16) + k] = int(v, 16)
    return out


def row_step(pixels, w, h):
    """Largest jump between the means of two neighbouring 8-row bands (a horizontal step)."""
    bands = []
    for y0 in range(0, h - 7, 8):
        seg = pixels[y0 * w:(y0 + 8) * w]
        bands.append(sum(seg) / len(seg))
    jumps = [abs(b - a) for a, b in zip(bands, bands[1:])]
    i = max(range(len(jumps)), key=jumps.__getitem__) if jumps else 0
    return (round(jumps[i], 1), (i + 1) * 8) if jumps else (0, 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--boards", nargs="+", default=["hm0360-5340", "hm0360b-5340"], choices=sorted(P.BOARDS))
    ap.add_argument("--seconds", type=float, default=240)
    ap.add_argument("--period-ms", type=int, default=1000)
    ap.add_argument("--quality", type=int, default=90, help="JPEG quality, 0 raw (raw: only ~100 QVGA pictures fit, 1.2 s each to fetch)")
    ap.add_argument("--csv", default="logs/f6/ae.csv")
    ap.add_argument("--rtt")
    args = ap.parse_args()
    loggers = [devtools.RttLogger(P.BOARDS[b]["snr"], P.BOARDS[b].get("device", P.DEFAULT_DEVICE), f"{args.rtt}_{'LR'[i]}.log")
               for i, b in enumerate(args.boards)] if args.rtt else []
    rows = []
    with bridge.Bridge(on_text=lambda s, t: None, on_event=lambda s, l: None) as br:
        ss = Session(br, args.boards)
        br.configure([P.BOARDS[b]["address"] for b in args.boards])
        t_x = time.monotonic()
        br.disconnect(ss.mask)
        time.sleep(0.3)
        if not br.wait_ready(range(len(args.boards)), timeout=30, after=t_x):
            raise SystemExit(f"boards not ready: {br.links}")
        time.sleep(1.0)
        ss.sync()
        ss.send(P.set_format_command(P.FORMAT_JPEG, args.quality) if args.quality else P.set_format_command(P.FORMAT_RAW))
        time.sleep(0.2)
        ss.ask(P.ring_command(args.period_ms), (P.R_STATUS,))
        time.sleep(args.seconds)
        ss.ask(P.ring_command(0), (P.R_STATUS,))
        for b in ss.boards:
            frames, _ = collect_ring(ss, b)
            for rf, f in frames:
                step, at = row_step(f.pixels, f.width, f.height)
                rows.append({"board": b.name, "seq": rf["seq"], "t_s": round(rf["capture_us"] / 1e6, 3),
                             "wake_to_fvld_ms": round(rf["capture_us"] % (args.period_ms * 1000) / 1000, 2),
                             "integration": f.integration, "analog_gain": f.analog_gain, "digital_gain": f.digital_gain,
                             "mean": round(sum(f.pixels) / len(f.pixels), 1), "row_step": step, "row_step_at": at})
    for lg in loggers:
        lg.stop()
    os.makedirs(os.path.dirname(args.csv) or ".", exist_ok=True)
    with open(args.csv, "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)
    for b in args.boards:
        r = [x for x in rows if x["board"] == b]
        if not r:
            continue
        integ = [x["integration"] for x in r]
        mean = [x["mean"] for x in r]
        lat = [x["wake_to_fvld_ms"] for x in r]
        changes = sum(1 for a, c in zip(r, r[1:]) if (a["integration"], a["analog_gain"], a["digital_gain"]) !=
                      (c["integration"], c["analog_gain"], c["digital_gain"]))
        print(f"{b}: {len(r)} pictures; integration {min(integ)}-{max(integ)} (median {statistics.median(integ)}), "
              f"exposure changed in {changes}; gains {sorted({(x['analog_gain'], x['digital_gain']) for x in r})[:6]}; "
              f"mean {min(mean)}-{max(mean)} (sd {statistics.pstdev(mean):.1f}); wake -> FVLD {min(lat)}-{max(lat)} ms "
              f"(sd {statistics.pstdev(lat):.1f}); row steps > 10: {sum(1 for x in r if x['row_step'] > 10)}")
    if args.rtt and len(args.boards) == 2:
        a, b = regs(f"{args.rtt}_L.log"), regs(f"{args.rtt}_R.log")
        if a and b:
            diff = {k: (a.get(k), b.get(k)) for k in sorted(set(a) | set(b)) if a.get(k) != b.get(k)}
            print(f"registers: {len(a)} read; different (L, R): " +
                  (", ".join(f"0x{k:04x} {v[0]:#04x}/{v[1]:#04x}" for k, v in diff.items() if None not in v) or "none"))
        else:
            print("registers: no [regs] lines (build with --cflags CAMERA_REG_DUMP)")


if __name__ == "__main__":
    main()
