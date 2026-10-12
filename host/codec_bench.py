"""On-board compression bench (fw_nrf5340 command 0xB4) through the BLE-USB bridge.

Each round the board takes one picture and sends it 7 times with the same frame id: raw, PNG,
JPEG q95 / 90 / 85 / 75 / 60. For every format this prints the size, PSNR to the raw picture,
the BLE time (first to last packet) and the board's encode time (read from its RTT log; the
logger attaches without a reset). Files go to captures/codec/<time>/<board>-<round>/.

    python codec_bench.py --board hm0360-5340 --rounds 3
    python codec_bench.py --board hm0360-5340 hm0360b-5340 --csv logs/codec.csv
"""
import argparse
import csv
import datetime
import pathlib
import re
import subprocess
import time

import cv2
import numpy as np

import bridge
import protocol

HERE = pathlib.Path(__file__).resolve().parent
JLINK_DIR = pathlib.Path(r"C:\Program Files\SEGGER\JLink")
FORMATS = ["raw", "png", "q95", "q90", "q85", "q75", "q60"]
ENCODE_RE = re.compile(r"\[img\] frame (\d+): (JPEG|PNG) q(\d+) (\d+) B, encode (\d+) us")


def name_of(frame: protocol.Frame) -> str:
    if frame.format == protocol.FORMAT_RAW:
        return "raw"
    if frame.format == protocol.FORMAT_PNG:
        return "png"
    return f"q{frame.quality}"


def psnr(a: np.ndarray, b: np.ndarray) -> float:
    mse = np.mean((a.astype(float) - b.astype(float)) ** 2)
    return float("inf") if mse == 0 else 10 * np.log10(255 ** 2 / mse)


def attach_rtt(board: dict, out: pathlib.Path) -> subprocess.Popen:
    """RTT logger without a reset (the bridge keeps its link to the board)."""
    return subprocess.Popen(
        [str(JLINK_DIR / "JLinkRTTLogger.exe"), "-USB", board["snr"], "-Device", board.get("device", protocol.DEFAULT_DEVICE),
         "-If", "SWD", "-Speed", "4000", "-RTTChannel", "0", str(out)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--board", nargs="+", default=["hm0360-5340"], choices=sorted(protocol.BOARDS))
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--bridge", default=None, metavar="PORT")
    ap.add_argument("--interval-ms", type=float, default=15.0)
    ap.add_argument("--csv", default=str(HERE / "logs" / "codec_bench.csv"))
    args = ap.parse_args()

    stamp = f"{datetime.datetime.now():%Y%m%d-%H%M%S}"
    out_dir = HERE / "captures" / "codec" / stamp
    (HERE / "logs").mkdir(exist_ok=True)
    n = len(args.board)
    frames: list[list[protocol.Frame]] = [[] for _ in range(n)]
    asms = [protocol.FrameAssembler(frames[i].append, lambda m: None) for i in range(n)]
    rtt_files = [HERE / "logs" / f"codec-rtt-{stamp}-{b}.log" for b in args.board]
    rows = []

    with bridge.Bridge(args.bridge, on_notify=lambda s, d: asms[s].feed(d), on_text=lambda s, t: None) as br:
        br.configure([protocol.BOARDS[b]["address"] for b in args.board], args.interval_ms)
        if not br.wait_ready(range(n), 30):
            raise SystemExit(f"boards not ready: {br.links}")
        time.sleep(0.5)
        loggers = [attach_rtt(protocol.BOARDS[b], f) for b, f in zip(args.board, rtt_files)]
        time.sleep(1.5)
        try:
            for r in range(args.rounds + 1):  # round 0: warm-up (camera init)
                start = [len(f) for f in frames]
                br.write((1 << n) - 1, bytes([protocol.CMD_CODEC_BENCH]))
                t_end = time.monotonic() + 60
                while any(len(frames[i]) < start[i] + len(FORMATS) for i in range(n)) and time.monotonic() < t_end:
                    time.sleep(0.05)
                if r == 0:
                    continue
                for i, b in enumerate(args.board):
                    got = frames[i][start[i]:]
                    rows += [dict(board=b, round=r, frame_id=f.frame_id, format=name_of(f), frame=f) for f in got]
        finally:
            time.sleep(1.0)
            for lg in loggers:
                lg.kill()
            br.disconnect((1 << n) - 1)
            time.sleep(0.2)

    encode_us = {}  # (board, frame_id, format) -> us
    for b, f in zip(args.board, rtt_files):
        text = f.read_text(errors="replace") if f.exists() else ""
        for m in ENCODE_RE.finditer(text):
            fmt = "png" if m.group(2) == "PNG" else f"q{m.group(3)}"
            encode_us[(b, int(m.group(1)), fmt)] = int(m.group(5))

    summary = {}
    new_csv = not pathlib.Path(args.csv).exists()
    with open(args.csv, "a", newline="") as fcsv:
        w = csv.writer(fcsv)
        if new_csv:
            w.writerow(["time", "board", "round", "size", "format", "bytes", "psnr_db", "ble_s", "encode_ms"])
        by_round = {}
        for row in rows:
            by_round.setdefault((row["board"], row["round"]), {})[row["format"]] = row["frame"]
        for (b, r), fs in sorted(by_round.items()):
            raw = fs.get("raw")
            if raw is None:
                print(f"{b} round {r}: no raw frame")
                continue
            ref = np.frombuffer(raw.pixels, np.uint8).reshape(raw.height, raw.width)
            d = out_dir / f"{b}-{r}"
            d.mkdir(parents=True, exist_ok=True)
            cv2.imwrite(str(d / "raw.png"), ref)
            for fmt in FORMATS:
                f = fs.get(fmt)
                if f is None:
                    continue
                img = np.frombuffer(f.pixels, np.uint8).reshape(f.height, f.width)
                p = psnr(ref, img)
                if fmt != "raw":
                    (d / (f"{fmt}.jpg" if fmt.startswith("q") else "sent.png")).write_bytes(f.data)
                enc = encode_us.get((b, f.frame_id, fmt))
                w.writerow([stamp, b, r, f"{f.width}x{f.height}", fmt, len(f.data), f"{p:.2f}", f"{f.duration_s:.3f}",
                            f"{enc / 1000:.1f}" if enc is not None else ""])
                summary.setdefault((b, f"{f.width}x{f.height}", fmt), []).append((len(f.data), p, f.duration_s, enc))

    print(f"files: {out_dir}")
    print(f"{'board':14s} {'size':8s} {'format':6s} {'KB':>7s} {'PSNR':>7s} {'BLE s':>7s} {'encode ms':>10s}")
    for (b, size, fmt), vals in summary.items():
        kb = np.mean([v[0] for v in vals]) / 1024
        ps = np.mean([v[1] for v in vals])
        ble = np.mean([v[2] for v in vals])
        encs = [v[3] for v in vals if v[3] is not None]
        enc = f"{np.mean(encs) / 1000:.0f}" if encs else "-"
        pstr = "lossless" if np.isinf(ps) else f"{ps:.1f}"
        print(f"{b:14s} {size:8s} {fmt:6s} {kb:7.1f} {pstr:>7s} {ble:7.2f} {enc:>10s}")


if __name__ == "__main__":
    main()
