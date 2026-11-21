"""Time sync plan E on the boards (hub #39 step 3): both boards take a picture at one bridge time T
(0xB7 through the bridge's 'T' command); the frames carry the FVLD rise on the bridge's clock
(header flag 0x01). Also measures the PC <-> bridge clock over USB ('Q').

  python timesync_test.py --duration 600 --every 3 --csv logs/timesync.csv

Per picture: T, each board's capture time - T (wake -> FVLD rise: the sensor's own metering after
the S2 wake) and left - right. Per USB sync: bridge - PC offset and round trip. The boards are
rebooted ('X') first; the first picture after boot is taken at once (camera start), so it is dropped.
"""
import argparse
import csv
import re
import statistics
import time

import bridge
import protocol
import devtools  # noqa: E402 (protocol puts ../tools on sys.path)

CMD_SNAPSHOT_AT = 0xB7
FORMATS = {"raw": (protocol.FORMAT_RAW, 0), **{f"jpeg:{q}": (protocol.FORMAT_JPEG, q) for q in range(1, 101)}}


def signed32(x):
    x &= 0xFFFFFFFF
    return x - (1 << 32) if x >= 1 << 31 else x


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--boards", nargs="+", default=["hm0360-5340", "hm0360b-5340"], choices=sorted(protocol.BOARDS))
    ap.add_argument("--duration", type=float, default=60.0)
    ap.add_argument("--every", type=float, default=3.0, help="seconds between pictures")
    ap.add_argument("--delay-ms", type=int, default=300, help="T = newest anchor + this")
    ap.add_argument("--format", default="jpeg:85", choices=list(FORMATS), metavar="FMT")
    ap.add_argument("--exposure", choices=["auto", "fixed"], default="auto")
    ap.add_argument("--integration", type=int, default=200, help="--exposure fixed: lines")
    ap.add_argument("--port")
    ap.add_argument("--csv", help="one row per picture")
    ap.add_argument("--usb-csv", help="one row per PC <-> bridge sync")
    ap.add_argument("--label", default="")
    ap.add_argument("--no-reset", action="store_true", help="keep the boards running (continue a run)")
    ap.add_argument("--rtt", help="RTT of each board into <this>_<L|R>.log (attached, no reset)")
    args = ap.parse_args()

    n = len(args.boards)
    mask = (1 << n) - 1
    frames = [[] for _ in range(n)]
    shots = []  # (pc time, T us)
    usb = []
    timed_re = re.compile(r"timed 0x([0-9a-f]{2}) at (\d+)\.(\d{3}) ms")

    def on_text(slot, text):
        m = timed_re.search(text)
        if m:
            shots.append((time.monotonic(), int(m.group(2)) * 1000 + int(m.group(3))))

    loggers = []
    if args.rtt:
        for i, b in enumerate(args.boards):
            brd = protocol.BOARDS[b]
            loggers.append(devtools.RttLogger(brd["snr"], brd.get("device", protocol.DEFAULT_DEVICE),
                                              f"{args.rtt}_{'LR'[i]}.log"))
    asms = [protocol.FrameAssembler(lambda f, i=i: frames[i].append(f), lambda m: None) for i in range(n)]
    with bridge.Bridge(args.port, on_text=on_text, on_event=lambda s, l: None) as br:
        br.on_notify = lambda s, d: asms[s].feed(d) if s < n else None
        br.configure([protocol.BOARDS[b]["address"] for b in args.boards])
        t_x = 0.0 if args.no_reset else time.monotonic()
        if not args.no_reset:
            br.disconnect(mask)
            time.sleep(0.3)
        if not br.wait_ready(range(n), timeout=30, after=t_x):
            raise SystemExit(f"boards not ready: {br.links}")
        if not args.no_reset:
            time.sleep(1.0)  # first references (0xB5, 200 ms after ready)
            br.write(mask, protocol.set_format_command(*FORMATS[args.format]))
            if args.exposure == "fixed":
                br.write(mask, protocol.set_exposure_command(args.integration))
            br.write(mask, bytes([protocol.CMD_SNAPSHOT]))  # starts the camera (taken at once, dropped)
            time.sleep(2.5)
        else:
            time.sleep(1.0)
        t0 = time.monotonic()
        next_shot = t0
        while time.monotonic() - t0 < args.duration:
            c = br.bridge_clock(samples=8)
            if c:
                usb.append((time.monotonic() - t0, c[0], c[1], c[2]))
            if time.monotonic() >= next_shot:
                br.timed_write(mask, CMD_SNAPSHOT_AT, args.delay_ms)
                next_shot += args.every
            time.sleep(max(0.0, min(next_shot - time.monotonic(), 1.0)))
        time.sleep(3.0)
    for lg in loggers:
        lg.stop()

    # Match frames to shots by the bridge time: the frame's capture time is T + wake -> FVLD.
    rows = []
    Ts = [t for _, t in shots]
    per_board = [{} for _ in range(n)]
    for i in range(n):
        for f in frames[i]:
            if not (f.header_flags & protocol.HEADER_COMMON_CLOCK):
                continue
            best = min(Ts, key=lambda T: abs(signed32(f.capture_us - T)), default=None)
            if best is None:
                continue
            lat = signed32(f.capture_us - best)
            if -50_000 < lat < 2_000_000:
                per_board[i][best] = (lat, f)
    for k, T in enumerate(Ts):
        row = {"label": args.label, "shot": k, "t_s": round(shots[k][0] - shots[0][0], 2), "T_ms": T / 1000}
        for i, b in enumerate(args.boards):
            lat, f = per_board[i].get(T, (None, None))
            side = "L" if i == 0 else "R"
            row[f"{side}_wake_to_fvld_ms"] = round(lat / 1000, 3) if lat is not None else None
            row[f"{side}_integration"] = f.integration if f else None
            row[f"{side}_mean"] = round(sum(f.pixels) / len(f.pixels), 1) if f else None
        if n == 2 and row["L_wake_to_fvld_ms"] is not None and row["R_wake_to_fvld_ms"] is not None:
            row["L_minus_R_ms"] = round(row["L_wake_to_fvld_ms"] - row["R_wake_to_fvld_ms"], 3)
        else:
            row["L_minus_R_ms"] = None
        rows.append(row)

    unflagged = [sum(1 for f in frames[i] if not f.header_flags & protocol.HEADER_COMMON_CLOCK) for i in range(n)]
    print(f"{len(shots)} shots, frames {[len(x) for x in frames]} (without flag 0x01: {unflagged})")
    for i, b in enumerate(args.boards):
        side = "L" if i == 0 else "R"
        v = [r[f"{side}_wake_to_fvld_ms"] for r in rows if r[f"{side}_wake_to_fvld_ms"] is not None]
        if v:
            print(f"  {side} {b}: {len(v)} matched, wake -> FVLD mean {statistics.mean(v):.2f} ms, "
                  f"min {min(v):.2f}, max {max(v):.2f}, sd {statistics.pstdev(v):.2f}")
    d = [r["L_minus_R_ms"] for r in rows if r["L_minus_R_ms"] is not None]
    if d:
        print(f"  L - R: {len(d)} pairs, mean {statistics.mean(d):.3f} ms, median {statistics.median(d):.3f}, "
              f"min {min(d):.3f}, max {max(d):.3f}, |d| <= 1 ms: {sum(abs(x) <= 1 for x in d)}")
    if usb:
        offs = [u[1] for u in usb]
        rtts = [u[2] for u in usb]
        span = usb[-1][0] - usb[0][0]
        slope = (offs[-1] - offs[0]) / span if span > 0 else 0
        resid = [o - (offs[0] + slope * (u[0] - usb[0][0])) for o, u in zip(offs, usb)]
        print(f"  USB: {len(usb)} syncs over {span:.0f} s, best round trip median {statistics.median(rtts):.0f} us "
              f"(min {min(rtts):.0f}, max {max(rtts):.0f}), drift {slope:.1f} us/s, "
              f"offset residual sd {statistics.pstdev(resid):.0f} us, max |{max(abs(r) for r in resid):.0f}| us")
    if args.csv and rows:
        new = not __import__("os").path.exists(args.csv)
        with open(args.csv, "a", newline="") as fh:
            w = csv.DictWriter(fh, fieldnames=list(rows[0]))
            if new:
                w.writeheader()
            w.writerows(rows)
    if args.usb_csv and usb:
        with open(args.usb_csv, "a", newline="") as fh:
            w = csv.writer(fh)
            w.writerows([(args.label, round(t, 2), round(o), round(r), k) for t, o, r, k in usb])


if __name__ == "__main__":
    main()
