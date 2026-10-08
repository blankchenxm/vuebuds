"""Two-board pictures: connect to the left and right nRF5340 boards at once, take a picture
on both (control command 0xB2) at the same time, rotate each, stitch them, and save the set.

  python stereo.py                          # Space: take a pair, q / Esc: quit
  python stereo.py --auto 5 --interval 3    # take 5 pairs without the keyboard, then quit
  python stereo.py --rotate-left 180        # override a board's rotation (degrees clockwise)

Each pair goes to captures/stereo/<time>/: left.png, right.png (rotated), stitch_opencv.png
and stitch_orb.png when that method worked, info.json (timings, stitch results).
The first pair after a board (re)connects includes the camera init and is not saved (warm-up).
"""
import argparse
import asyncio
import collections
import datetime
import json
import pathlib
import sys
import threading
import time
from typing import Callable, Optional

import cv2
import numpy as np
from bleak import BleakClient

import protocol
import stitch
from viewer import SessionLog

HERE = pathlib.Path(__file__).resolve().parent
WINDOW = "vuebuds stereo"
PAIR_TIMEOUT_S = 20.0  # VGA from two boards over Windows: ~8 s expected
SIDES = ("left", "right")
ROTATIONS = {0: None, 90: cv2.ROTATE_90_CLOCKWISE, 180: cv2.ROTATE_180, 270: cv2.ROTATE_90_COUNTERCLOCKWISE}


class BoardLink:
    """One board's BLE connection; reconnects after the firmware reboots."""

    def __init__(self, side: str, board: str, log: SessionLog, on_frame: Callable[[str, protocol.Frame], None]):
        self.side = side
        self.board = board
        self.address = protocol.BOARDS[board]["address"]
        self.log = log
        self.status = "starting"
        self.fresh = False  # connected and no picture taken yet: the next one includes the camera init
        self.client: Optional[BleakClient] = None
        self.assembler = protocol.FrameAssembler(lambda f: on_frame(side, f), lambda m: log(side, m))

    @property
    def ready(self) -> bool:
        return self.client is not None and self.client.is_connected

    async def run(self, stop: threading.Event):
        while not stop.is_set():
            try:
                await self._session(stop)
            except Exception as e:  # routine: the board reboots on disconnect, Windows cancels connects
                self.log(self.side, f"BLE error: {type(e).__name__}: {e}")
            self.client = None
            if not stop.is_set():
                self.status = "reconnecting"
                await asyncio.sleep(2)

    async def _session(self, stop: threading.Event):
        self.status = "connecting"
        disconnected = asyncio.Event()
        async with BleakClient(self.address, disconnected_callback=lambda _: disconnected.set()) as client:
            self.log(self.side, f"connected to {self.board} {self.address}, mtu={client.mtu_size}")
            await client.start_notify(protocol.DATA_UUID, lambda _, d: self.assembler.feed(bytes(d)))
            self.client = client
            self.fresh = True
            self.status = "ready"
            while not stop.is_set() and not disconnected.is_set():
                await asyncio.sleep(0.1)
        self.log(self.side, "disconnected")

    async def snap(self) -> float:
        await self.client.write_gatt_char(protocol.CONTROL_UUID, bytes([protocol.CMD_SNAPSHOT]), response=False)
        return time.monotonic()


class Pair:
    def __init__(self, index: int, warmup: bool):
        self.index = index
        self.warmup = warmup
        self.t_request = time.monotonic()
        self.t_written: dict[str, float] = {}
        self.frames: dict[str, protocol.Frame] = {}


class Stereo:
    """Owns both connections on one asyncio loop (background thread)."""

    def __init__(self, log: SessionLog, boards: dict[str, str]):
        self.log = log
        self.links = {side: BoardLink(side, boards[side], log, self._on_frame) for side in SIDES}
        self.pending: Optional[Pair] = None
        self.done: collections.deque[Pair] = collections.deque()
        self.pairs = 0
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._loop = asyncio.new_event_loop()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def _run(self):
        asyncio.set_event_loop(self._loop)
        # Connect one after the other: Windows is unreliable with two connects at once.
        async def main():
            left = asyncio.ensure_future(self.links["left"].run(self._stop))
            while not self.links["left"].ready and not self._stop.is_set():
                await asyncio.sleep(0.2)
            await asyncio.gather(left, self.links["right"].run(self._stop))
        self._loop.run_until_complete(main())

    @property
    def ready(self) -> bool:
        return all(l.ready for l in self.links.values())

    @property
    def needs_warmup(self) -> bool:
        return any(l.fresh for l in self.links.values())

    def status(self) -> str:
        return " | ".join(f"{s}: {l.status}" for s, l in self.links.items())

    def trigger(self, warmup: bool = False) -> bool:
        with self._lock:
            if self.pending is not None or not self.ready:
                return False
            self.pairs += 1
            self.pending = Pair(self.pairs, warmup)
            for l in self.links.values():
                l.fresh = False
        asyncio.run_coroutine_threadsafe(self._trigger(self.pending), self._loop)
        return True

    async def _trigger(self, pair: Pair):
        try:
            # Both writes go out together; each still waits for its own connection event.
            t_left, t_right = await asyncio.gather(self.links["left"].snap(), self.links["right"].snap())
            pair.t_written = {"left": t_left, "right": t_right}
            self.log("host", f"pair {pair.index}{' (warm-up)' if pair.warmup else ''}: 0xB2 written, "
                             f"left +{(t_left - pair.t_request) * 1000:.0f} ms, "
                             f"right +{(t_right - pair.t_request) * 1000:.0f} ms")
        except Exception as e:
            self.log("host", f"pair {pair.index}: writing 0xB2 failed: {type(e).__name__}: {e}")
            with self._lock:
                if self.pending is pair:
                    self.pending = None

    def _on_frame(self, side: str, frame: protocol.Frame):
        with self._lock:
            pair = self.pending
            if pair is None or side in pair.frames:
                self.log(side, f"frame #{frame.index} without a request, ignored")
                return
            pair.frames[side] = frame
            self.log(side, f"pair {pair.index}: {frame.width}x{frame.height} lost={frame.lost_packets} "
                           f"rx {frame.duration_s:.2f}s, +{(frame.completed_at - pair.t_request):.2f}s after request")
            if len(pair.frames) == 2:
                self.done.append(pair)
                self.pending = None

    def check_timeout(self):
        with self._lock:
            if self.pending and time.monotonic() - self.pending.t_request > PAIR_TIMEOUT_S:
                got = ",".join(self.pending.frames) or "nothing"
                self.log("host", f"pair {self.pending.index}: timed out after {PAIR_TIMEOUT_S:.0f} s (got {got})")
                self.pending = None

    def close(self):
        self._stop.set()
        self._thread.join(timeout=5)


def to_image(frame: protocol.Frame, rotate: int) -> np.ndarray:
    img = np.frombuffer(frame.pixels, np.uint8).reshape(frame.height, frame.width)
    return cv2.rotate(img, ROTATIONS[rotate]) if ROTATIONS[rotate] is not None else img.copy()


def fit_height(img: np.ndarray, h: int) -> np.ndarray:
    return cv2.resize(img, (max(1, round(img.shape[1] * h / img.shape[0])), h), interpolation=cv2.INTER_AREA)


def label(img: np.ndarray, text: str) -> np.ndarray:
    out = cv2.cvtColor(img, cv2.COLOR_GRAY2BGR) if img.ndim == 2 else img
    cv2.putText(out, text, (6, 18), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 0, 0), 3, cv2.LINE_AA)
    cv2.putText(out, text, (6, 18), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (80, 255, 80), 1, cv2.LINE_AA)
    return out


def compose(left: np.ndarray, right: np.ndarray, results: list[stitch.StitchResult], h: int = 240) -> np.ndarray:
    """Top row: left | right. Bottom row: each stitch result (or its failure reason)."""
    top = np.hstack([label(fit_height(left, h), "left"), label(fit_height(right, h), "right")])
    box_w = top.shape[1] // 2
    tiles = []
    for r in results:
        tile = np.zeros((h, box_w), np.uint8)
        if r.ok:  # fit inside an h x box_w tile (a panorama can be very wide)
            scale = min(h / r.image.shape[0], box_w / r.image.shape[1])
            img = cv2.resize(r.image, (max(1, round(r.image.shape[1] * scale)), max(1, round(r.image.shape[0] * scale))),
                             interpolation=cv2.INTER_AREA)
            tile[:img.shape[0], :img.shape[1]] = img
            text = f"{r.method} {r.ms:.0f} ms"
        else:
            text = f"{r.method}: FAILED ({r.detail.get('reason') or r.detail.get('status')})"
        tiles.append(label(tile, text))
    bottom = np.hstack(tiles)
    width = max(top.shape[1], bottom.shape[1])
    pad = lambda im: cv2.copyMakeBorder(im, 0, 0, 0, width - im.shape[1], cv2.BORDER_CONSTANT)
    return np.vstack([pad(top), pad(bottom)])


def process(pair: Pair, rotations: dict[str, int], boards: dict[str, str], log: SessionLog,
            save: bool) -> np.ndarray:
    images = {s: to_image(pair.frames[s], rotations[s]) for s in SIDES}
    results = stitch.stitch_all(images["left"], images["right"])
    for r in results:
        log("host", f"pair {pair.index}: {r.summary()}")
    if save:
        out = HERE / "captures" / "stereo" / f"{datetime.datetime.now():%Y%m%d-%H%M%S}-{pair.index:03d}"
        out.mkdir(parents=True, exist_ok=True)
        for s in SIDES:
            cv2.imwrite(str(out / f"{s}.png"), images[s])
        for r in results:
            if r.ok:
                cv2.imwrite(str(out / f"stitch_{r.method}.png"), r.image)
        written = pair.t_written or {}
        info = {
            "pair": pair.index,
            "time": datetime.datetime.now().isoformat(timespec="milliseconds"),
            "boards": boards,
            "rotate": rotations,
            "size": {s: [pair.frames[s].width, pair.frames[s].height] for s in SIDES},
            "write_done_ms": {s: round((written[s] - pair.t_request) * 1000, 1) for s in written},
            "frame_done_s": {s: round(pair.frames[s].completed_at - pair.t_request, 3) for s in SIDES},
            "lost_packets": {s: pair.frames[s].lost_packets for s in SIDES},
            "stitch": [{"method": r.method, "ok": r.ok, "ms": round(r.ms, 1), **r.detail} for r in results],
        }
        (out / "info.json").write_text(json.dumps(info, indent=2), encoding="utf-8")
        log("host", f"pair {pair.index}: saved {2 + sum(r.ok for r in results)} images -> {out}")
    return compose(images["left"], images["right"], results)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--left", default="hm0360-5340", choices=sorted(protocol.BOARDS))
    ap.add_argument("--right", default="hm0360b-5340", choices=sorted(protocol.BOARDS))
    ap.add_argument("--rotate-left", type=int, choices=sorted(ROTATIONS), help="degrees clockwise (default: BOARDS)")
    ap.add_argument("--rotate-right", type=int, choices=sorted(ROTATIONS), help="degrees clockwise (default: BOARDS)")
    ap.add_argument("--auto", type=int, default=0, help="take N pairs automatically, then quit")
    ap.add_argument("--interval", type=float, default=2.0, help="with --auto: seconds between pairs")
    ap.add_argument("--snapshot", help="save the last rendered view to this PNG on exit")
    args = ap.parse_args()
    boards = {"left": args.left, "right": args.right}
    rotations = {
        "left": args.rotate_left if args.rotate_left is not None else protocol.BOARDS[args.left].get("rotate", 0),
        "right": args.rotate_right if args.rotate_right is not None else protocol.BOARDS[args.right].get("rotate", 0),
    }

    log = SessionLog(HERE / "logs" / f"stereo-{datetime.datetime.now():%Y%m%d-%H%M%S}.log")
    log("host", f"session log: {log.path}; left {args.left} (rotate {rotations['left']}), "
                f"right {args.right} (rotate {rotations['right']})")
    st = Stereo(log, boards)
    canvas = np.zeros((480, 640, 3), np.uint8)
    saved = 0
    next_auto = 0.0
    cv2.namedWindow(WINDOW, cv2.WINDOW_AUTOSIZE)
    try:
        while True:
            st.check_timeout()
            while st.done:
                pair = st.done.popleft()
                canvas = process(pair, rotations, boards, log, save=not pair.warmup)
                if not pair.warmup:
                    saved += 1
                next_auto = time.monotonic() + args.interval
            if st.ready and st.pending is None and st.needs_warmup:
                if st.trigger(warmup=True):
                    log("host", "both boards connected: warm-up pair (includes the camera init, not saved)")
            elif args.auto and st.ready and st.pending is None and time.monotonic() >= next_auto:
                if saved >= args.auto:
                    break
                st.trigger()

            title = f"{WINDOW} - {st.status()} | pairs saved {saved}" + (" | taking..." if st.pending else "")
            cv2.setWindowTitle(WINDOW, title)
            cv2.imshow(WINDOW, canvas)
            key = cv2.waitKey(30) & 0xFF
            if key in (ord("q"), 27) or cv2.getWindowProperty(WINDOW, cv2.WND_PROP_VISIBLE) < 1:
                break
            if key == ord(" "):
                if not st.trigger():
                    log("host", "not ready (both boards connected, previous pair done?)")
    finally:
        if args.snapshot:
            cv2.imwrite(args.snapshot, canvas)
        log("host", f"summary: pairs saved {saved}")
        st.close()
        cv2.destroyAllWindows()
        log.close()


if __name__ == "__main__":
    sys.exit(main())
