"""Real-time viewer for the vuebuds HM01B0 BLE stream.

  python viewer.py                 # connect to "mustard" and show frames
  python viewer.py --rtt --reset   # also capture firmware RTT logs (from boot) into the same session log
  python viewer.py --duration 30 --snapshot shot.png   # auto-quit and save the last rendered view

Keys: Space saves the current frame to captures/, q / Esc quits.
"""
import argparse
import asyncio
import collections
import datetime
import os
import pathlib
import subprocess
import sys
import threading
import time

import cv2
import numpy as np
from bleak import BleakClient, BleakScanner

import protocol

HERE = pathlib.Path(__file__).resolve().parent
JLINK_DIR = pathlib.Path(os.environ.get("JLINK_DIR", r"C:\Program Files\SEGGER\JLink"))
WINDOW = "vuebuds viewer"


class SessionLog:
    def __init__(self, path: pathlib.Path):
        path.parent.mkdir(parents=True, exist_ok=True)
        self.path = path
        self._f = open(path, "a", encoding="utf-8")
        self._lock = threading.Lock()

    def __call__(self, source: str, msg: str):
        line = f"{datetime.datetime.now():%H:%M:%S.%f}"[:-3] + f" [{source:<4}] {msg}"
        with self._lock:
            print(line, flush=True)
            self._f.write(line + "\n")
            self._f.flush()

    def close(self):
        self._f.close()


class RttTap:
    """Runs JLinkRTTLogger into a file and forwards each new line to the session log."""

    def __init__(self, log: SessionLog, out_file: pathlib.Path, reset: bool):
        self.log = log
        self.out_file = out_file
        self._stop = threading.Event()
        if reset:
            # Reset before attaching so the log starts at boot; nrfjprog and the logger can't share the probe.
            rc = subprocess.run(["nrfjprog", "-f", "nrf52", "--reset"], capture_output=True).returncode
            log("rtt", f"target reset (nrfjprog rc={rc})")
        if out_file.exists():
            out_file.unlink()
        self._proc = subprocess.Popen(
            [str(JLINK_DIR / "JLinkRTTLogger.exe"), "-Device", "NRF52840_XXAA", "-If", "SWD",
             "-Speed", "4000", "-RTTChannel", "0", str(out_file)],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL)
        self._thread = threading.Thread(target=self._tail, daemon=True)
        self._thread.start()

    def _tail(self):
        pos, pending = 0, b""
        while not self._stop.is_set():
            if self.out_file.exists():
                with open(self.out_file, "rb") as f:
                    f.seek(pos)
                    chunk = f.read()
                    pos = f.tell()
                pending += chunk
                *lines, pending = pending.split(b"\n")
                for raw in lines:
                    text = raw.decode("utf-8", "replace").strip()
                    if text:
                        self.log("fw", text)
            time.sleep(0.1)

    def close(self):
        self._stop.set()
        self._proc.kill()
        self._thread.join(timeout=1)


class BleReceiver:
    """Owns the BLE connection on a background thread; reconnects after the firmware resets."""

    def __init__(self, log: SessionLog, name: str, address: str | None):
        self.log = log
        self.name = name
        self.address = address
        self.status = "starting"
        self._lock = threading.Lock()
        self._latest: protocol.Frame | None = None
        self._stop = threading.Event()
        self.assembler = protocol.FrameAssembler(self._on_frame, lambda m: self.log("host", m))
        self._last_completed: float | None = None
        self._thread = threading.Thread(target=lambda: asyncio.run(self._run()), daemon=True)
        self._thread.start()

    def _on_frame(self, frame: protocol.Frame):
        interval = f"{frame.completed_at - self._last_completed:.2f}s" if self._last_completed else "-"
        self._last_completed = frame.completed_at
        self.log("host", f"frame #{frame.index} {frame.width}x{frame.height} {len(frame.pixels)}B "
                         f"packets={frame.packets} lost={frame.lost_packets} "
                         f"rx={frame.duration_s:.2f}s interval={interval}")
        with self._lock:
            self._latest = frame

    def take_latest(self) -> protocol.Frame | None:
        with self._lock:
            frame, self._latest = self._latest, None
        return frame

    async def _run(self):
        while not self._stop.is_set():
            try:
                await self._session()
            except Exception as e:  # BLE errors are routine here (device resetting, out of range)
                self.log("host", f"BLE error: {type(e).__name__}: {e}")
            if not self._stop.is_set():
                self.status = "reconnecting"
                await asyncio.sleep(2)

    async def _session(self):
        self.status = f"scanning for '{self.name}'"
        target = self.address or await BleakScanner.find_device_by_name(self.name, timeout=10.0)
        if target is None:
            self.log("host", f"device '{self.name}' not found")
            return
        disconnected = asyncio.Event()
        self.status = "connecting"
        async with BleakClient(target, disconnected_callback=lambda _: disconnected.set()) as client:
            addr = getattr(target, "address", target)
            self.log("host", f"connected to {addr}, mtu={client.mtu_size}")
            await client.start_notify(protocol.DATA_UUID, lambda _, d: self.assembler.feed(bytes(d)))
            await client.write_gatt_char(protocol.CONTROL_UUID, bytes([protocol.CMD_STREAM_START]), response=False)
            self.log("host", "stream start (0xB1) sent")
            self.status = "streaming"
            while not self._stop.is_set() and not disconnected.is_set():
                await asyncio.sleep(0.1)
        self.log("host", "disconnected")

    def close(self):
        self._stop.set()
        self._thread.join(timeout=5)


class FpsMeter:
    def __init__(self, window: int = 5):
        self._times = collections.deque(maxlen=window)

    def tick(self, t: float):
        self._times.append(t)

    def fps(self) -> float:
        if len(self._times) < 2:
            return 0.0
        return (len(self._times) - 1) / (self._times[-1] - self._times[0])

    def seconds_since_last(self) -> float:
        return time.monotonic() - self._times[-1] if self._times else float("inf")


def draw_overlay(canvas: np.ndarray, lines: list[str]):
    font, scale, thick, margin, pad, gap = cv2.FONT_HERSHEY_SIMPLEX, 0.45, 1, 8, 5, 6
    sizes = [cv2.getTextSize(t, font, scale, thick)[0] for t in lines]
    box_w = max(w for w, _ in sizes) + 2 * pad
    box_h = sum(h for _, h in sizes) + gap * (len(lines) - 1) + 2 * pad
    x1, y0 = canvas.shape[1] - margin, margin
    x0, y1 = x1 - box_w, y0 + box_h
    roi = canvas[y0:y1, x0:x1]
    roi[:] = (roi * 0.4).astype(np.uint8)
    y = y0 + pad
    for text, (tw, th) in zip(lines, sizes):
        y += th
        cv2.putText(canvas, text, (x1 - pad - tw, y), font, scale, (80, 255, 80), thick, cv2.LINE_AA)
        y += gap


def save_frame(frame: protocol.Frame, out_dir: pathlib.Path) -> pathlib.Path:
    """Save the frame at native resolution, without the overlay."""
    out_dir.mkdir(parents=True, exist_ok=True)
    stamp = f"{datetime.datetime.now():%Y%m%d-%H%M%S-%f}"[:-3]
    path = out_dir / f"frame-{stamp}.png"
    img = np.frombuffer(frame.pixels, np.uint8).reshape(frame.height, frame.width)
    cv2.imwrite(str(path), img)
    return path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", default=protocol.DEVICE_NAME)
    ap.add_argument("--address", help="BLE address; skips the name scan")
    ap.add_argument("--scale", type=int, default=2, help="display upscale factor")
    ap.add_argument("--rtt", action="store_true", help="also capture firmware logs over J-Link RTT")
    ap.add_argument("--reset", action="store_true", help="with --rtt: reset the board first so logs start at boot")
    ap.add_argument("--duration", type=float, help="quit automatically after N seconds")
    ap.add_argument("--snapshot", help="save the last rendered view to this PNG on exit")
    args = ap.parse_args()

    stamp = f"{datetime.datetime.now():%Y%m%d-%H%M%S}"
    log = SessionLog(HERE / "logs" / f"session-{stamp}.log")
    log("host", f"session log: {log.path}")

    rtt = RttTap(log, HERE / "logs" / f"rtt-{stamp}.raw.log", args.reset) if args.rtt else None
    rx = BleReceiver(log, args.name, args.address)
    fps = FpsMeter()

    w0, h0 = protocol.SIZE_HIGH_RES
    canvas = np.zeros((h0 * args.scale, w0 * args.scale, 3), np.uint8)
    last_frame: protocol.Frame | None = None
    saved_msg, saved_until = "", 0.0
    cv2.namedWindow(WINDOW, cv2.WINDOW_AUTOSIZE)
    t_end = time.monotonic() + args.duration if args.duration else None

    try:
        while True:
            frame = rx.take_latest()
            if frame is not None:
                fps.tick(frame.completed_at)
                last_frame = frame
            if last_frame is not None:
                img = np.frombuffer(last_frame.pixels, np.uint8).reshape(last_frame.height, last_frame.width)
                img = cv2.resize(img, None, fx=args.scale, fy=args.scale, interpolation=cv2.INTER_NEAREST)
                canvas = cv2.cvtColor(img, cv2.COLOR_GRAY2BGR)
                stalled = fps.seconds_since_last() > 5
                lines = [
                    f"FPS {fps.fps():.2f}" + (" (stalled)" if stalled else ""),
                    f"{last_frame.width}x{last_frame.height} {len(last_frame.pixels)} B",
                ]
                if time.monotonic() < saved_until:
                    lines.append(saved_msg)
                draw_overlay(canvas, lines)
            else:
                canvas[:] = 0
                draw_overlay(canvas, ["FPS --", rx.status])
            a = rx.assembler
            cv2.setWindowTitle(WINDOW, f"{WINDOW} - {rx.status} | frames {a.frames} | lost pkts {a.total_lost}")
            cv2.imshow(WINDOW, canvas)
            key = cv2.waitKey(30) & 0xFF
            if key in (ord("q"), 27) or cv2.getWindowProperty(WINDOW, cv2.WND_PROP_VISIBLE) < 1:
                break
            if key == ord(" ") and last_frame is not None:
                path = save_frame(last_frame, HERE / "captures")
                log("host", f"saved frame #{last_frame.index} -> {path}")
                saved_msg, saved_until = f"saved {path.name}", time.monotonic() + 1.5
            if t_end and time.monotonic() > t_end:
                break
    finally:
        if args.snapshot:
            cv2.imwrite(args.snapshot, canvas)
            log("host", f"snapshot saved: {args.snapshot}")
        a = rx.assembler
        log("host", f"summary: frames={a.frames} packets={a.total_packets} lost={a.total_lost} "
                    f"discarded_partial={a.discarded} last_fps={fps.fps():.2f}")
        rx.close()
        if rtt:
            rtt.close()
        cv2.destroyAllWindows()
        log.close()


if __name__ == "__main__":
    sys.exit(main())
