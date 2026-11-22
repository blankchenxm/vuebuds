"""Shortest ring period that keeps up (hub #68 item 2, F9): for each period, both boards run the
ring buffer (0xB8 2) for a while; reports stored / expected pictures, skipped capture times, where
each picture's time goes ("[ring] timing" RTT lines: first FVLD, capture done, stored, ms after the
capture time T) and the brightness of a few fetched pictures.

  python ring_period_test.py --period-ms 1000 1200 1500 --seconds 60 --label vga_64mhz --rtt logs/f9/vga64

The boards are rebooted ('X') before each period. RTT goes to <prefix>_<period>_<L|R>.log.
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
from m2_test import Session

TIMING = re.compile(r"\[ring\] timing: fvld \+(-?\d+) done \+(-?\d+)(?: asleep (\w+) (\d+) us,|,) (stored|NOT stored) \+(-?\d+) ms")
CAMFRAME = re.compile(r"\[cam\] frame \d+: (\d+) us")
ENCODE = re.compile(r"\[ring\] stored \d+ .* (\d+) B crc \w+, (?:encode \+ write|write) (\d+) us")
TIMEOUT = "no frame boundary"


def parse(path):
    try:
        text = open(path, errors="replace").read().rsplit("[mustard] booting", 1)[-1]
    except OSError:
        return {}
    t = [tuple(m.groups()) for m in TIMING.finditer(text)]
    enc = [(int(a), int(b)) for a, b in ENCODE.findall(text)]
    read = [int(x) for x in CAMFRAME.findall(text)]
    out = {"pictures": len(t), "timeouts": text.count(TIMEOUT)}
    if t:
        for k, i in (("fvld_ms", 0), ("done_ms", 1), ("stored_ms", 5)):
            v = [int(x[i]) for x in t[1:]]  # the first one after the start can include the sensor start
            if v:
                out[k] = round(statistics.median(v))
                out[k + "_max"] = max(v)
        out["boundary_waits"] = sum(1 for x in t if x[2] == "boundary")
    if enc:
        out["encode_ms"] = round(statistics.median(us for _, us in enc) / 1000)
        out["bytes"] = round(statistics.median(n for n, _ in enc))
    if read:
        out["readout_ms"] = round(statistics.median(read) / 1000)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--boards", nargs="+", default=["hm0360-5340", "hm0360b-5340"], choices=sorted(P.BOARDS))
    ap.add_argument("--period-ms", nargs="+", type=int, default=[1000, 1200, 1500])
    ap.add_argument("--seconds", type=float, default=60)
    ap.add_argument("--quality", type=int, default=85)
    ap.add_argument("--label", default="")
    ap.add_argument("--rtt", required=True, help="prefix for the RTT logs")
    ap.add_argument("--csv", default="logs/f9/ring_period.csv")
    args = ap.parse_args()
    rows = []
    for period in args.period_ms:
        loggers = [devtools.RttLogger(P.BOARDS[b]["snr"], P.BOARDS[b].get("device", P.DEFAULT_DEVICE),
                                      f"{args.rtt}_{period}_{'LR'[i]}.log") for i, b in enumerate(args.boards)]
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
            ss.ask(P.ring_command(period), (P.R_STATUS,))
            time.sleep(3.0)  # sensor start + first pictures
            s0 = ss.status()
            time.sleep(args.seconds)
            s1 = ss.status()
            means = []
            for b, s in zip(ss.boards, s1):
                frames, _, _ = ss.fetch(b, P.fetch_range_command(s["newest_us"] - 3 * period * 1000 + 1, s["newest_us"], 3)) \
                    if s else ([], None, 0)
                means.append(round(statistics.mean(sum(f.pixels) / len(f.pixels) for _, f in frames), 1) if frames else None)
            ss.ask(P.ring_command(0), (P.R_STATUS,))
        for lg in loggers:
            lg.stop()
        for i, b in enumerate(args.boards):
            a, z = s0[i], s1[i]
            stored = z["newest_seq"] - a["newest_seq"] if a and z else None
            expect = args.seconds * 1000 / period
            r = {"label": args.label, "period_ms": period, "board": b, "seconds": args.seconds,
                 "stored": stored, "expected": round(expect), "skipped": (z["skipped"] - a["skipped"]) if a and z else None,
                 "on_time": stored is not None and stored >= expect - 1 and (z["skipped"] - a["skipped"]) == 0,
                 "mean_brightness": means[i], **parse(f"{args.rtt}_{period}_{'LR'[i]}.log")}
            rows.append(r)
            print(f"P {period} ms {b}: stored {stored}/{expect:.0f}, skipped {r['skipped']}, "
                  f"fvld +{r.get('fvld_ms')} (max {r.get('fvld_ms_max')}), readout {r.get('readout_ms')}, "
                  f"done +{r.get('done_ms')}, encode {r.get('encode_ms')} ms ({r.get('bytes')} B), "
                  f"stored +{r.get('stored_ms')} (max {r.get('stored_ms_max')}), sleep timeouts {r.get('timeouts')}/"
                  f"{r.get('pictures')}, brightness {means[i]} -> {'ON TIME' if r['on_time'] else 'late'}", flush=True)
    if rows:
        os.makedirs(os.path.dirname(args.csv) or ".", exist_ok=True)
        keys = sorted({k for r in rows for k in r}, key=lambda k: list(rows[0]).index(k) if k in rows[0] else 99)
        new = not os.path.exists(args.csv)
        with open(args.csv, "a", newline="") as fh:
            w = csv.DictWriter(fh, fieldnames=keys, extrasaction="ignore")
            if new:
                w.writeheader()
            w.writerows(rows)


if __name__ == "__main__":
    main()
