"""Two-board pictures: connect to the left and right nRF5340 boards at once, take a picture
on both (control command 0xB2) at the same time, rotate each and show them side by side.

  python stereo.py                          # Space or left click: take a pair, S or right click: save, q / Esc: quit
  python stereo.py --flash VGA              # first flash VGA (or QVGA / QQVGA) to both boards
  python stereo.py --auto 5 --interval 3    # take and save 5 pairs without the keyboard, then quit
  python stereo.py --rotate-left 180        # override a board's rotation (degrees clockwise)
  python stereo.py --exposure sync          # left board AE, the right board copies its exposure
  python stereo.py --exposure fixed 200 0 0x100   # both boards the same fixed exposure (registers)
  python stereo.py --format jpeg:85         # the boards send JPEG q85 (encoded on the board; or png)
  python stereo.py --bridge                 # through the BLE-USB bridge (fw_bridge/) instead of
                                            # Windows Bluetooth; both 0xB2 go out in one bridge command

S saves the pair on screen to captures/stereo/<time>/: left.png, right.png (rotated, native
resolution) and info.json (timings). Stitch saved pairs offline with stitch.py.
The first pair after a board (re)connects includes the camera init and cannot be saved (warm-up).
"""
import argparse
import asyncio
import collections
import datetime
import json
import pathlib
import subprocess
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
TAKE_HINT = "Space / left click: new pair"
SAVE_HINT = "S / right click: save"
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


class BridgeBoardLink:
    """One board behind the BLE-USB bridge (slot 0 = left, 1 = right); the bridge reconnects."""

    def __init__(self, side: str, slot: int, board: str, log: SessionLog,
                 on_frame: Callable[[str, protocol.Frame], None]):
        self.side = side
        self.slot = slot
        self.board = board
        self.address = protocol.BOARDS[board]["address"]
        self.log = log
        self.link = None  # bridge.Link (updated in place by the bridge reader)
        self._ready = False
        self.fresh = False
        self.assembler = protocol.FrameAssembler(lambda f: on_frame(side, f), lambda m: log(side, m))

    @property
    def ready(self) -> bool:
        return self._ready

    @property
    def status(self) -> str:
        if self.link is None:
            return "connecting"
        return "ready" if self.link.ready else ("connected" if self.link.connected else "reconnecting")

    def on_event(self, link):
        was_ready, self._ready = self._ready, link.ready
        self.link = link
        if link.ready and not was_ready:
            self.fresh = True  # the board rebooted: the next picture includes the camera init
            self.log(self.side, f"ready via bridge: {self.board} {link.address}, interval {link.interval_ms:g} ms, "
                                f"PHY {link.phy}, data length {link.data_length}, MTU {link.mtu}")


class Pair:
    def __init__(self, index: int, warmup: bool):
        self.index = index
        self.warmup = warmup
        self.t_request = time.monotonic()
        self.t_written: dict[str, float] = {}
        self.frames: dict[str, protocol.Frame] = {}


class Stereo:
    """Owns both connections on one asyncio loop (background thread)."""

    def __init__(self, log: SessionLog, boards: dict[str, str], bridge_port: Optional[str] = None,
                 interval_ms: float = 15.0, exposure: tuple = ("auto",), image_format: tuple = (0, 0)):
        self.log = log
        self.exposure = exposure
        self.image_format = image_format  # (protocol.FORMAT_*, quality)
        self.bridge = None
        if bridge_port is not None:
            import bridge
            self.links = {side: BridgeBoardLink(side, i, boards[side], log, self._on_frame) for i, side in enumerate(SIDES)}
            by_slot = list(self.links.values())
            self.bridge = bridge.Bridge(
                bridge_port or None,
                on_notify=lambda slot, d: by_slot[slot].assembler.feed(d),
                on_text=lambda slot, t: log(by_slot[slot].side if slot < 2 else "bridge", f"bridge: {t}"),
                on_event=lambda slot, l: by_slot[slot].on_event(l))
            self.bridge.configure([l.address for l in by_slot], interval_ms)
            log("host", f"bridge on {self.bridge.port}, interval {interval_ms:g} ms")
        else:
            self.links = {side: BoardLink(side, boards[side], log, self._on_frame) for side in SIDES}
        self.pending: Optional[Pair] = None
        self.done: collections.deque[Pair] = collections.deque()
        self.pairs = 0
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._loop = asyncio.new_event_loop()
        self._thread = threading.Thread(target=self._run, daemon=True)
        if self.bridge is None:
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

    def send(self, sides, data: bytes):
        """Write a control command to these sides (one bridge command for both)."""
        if self.bridge is not None:
            self.bridge.write(sum(1 << self.links[s].slot for s in sides), data)
            return
        for s in sides:
            client = self.links[s].client
            if client is not None:
                asyncio.run_coroutine_threadsafe(
                    client.write_gatt_char(protocol.CONTROL_UUID, data, response=False), self._loop)

    def apply_exposure(self):
        """--format, then --exposure: auto (both AE), fixed (both the same values), sync (left AE, right follows)."""
        self.send(SIDES, protocol.set_format_command(*self.image_format))
        mode = self.exposure[0]
        if mode == "fixed":
            self.send(SIDES, protocol.set_exposure_command(*self.exposure[1:]))
        else:
            self.send(SIDES, protocol.set_exposure_command(None))
        self.log("host", f"exposure: {' '.join(str(x) for x in self.exposure)}")

    def _sync_exposure(self, left: protocol.Frame):
        # The right board takes the left board's last AE result for the next pair.
        if left.integration is not None:
            self.send(["right"], protocol.set_exposure_command(left.integration, left.analog_gain, left.digital_gain))

    def _trigger_bridge(self, pair: Pair):
        # One bridge command: the bridge writes 0xB2 to both links back to back.
        self.bridge.write(0b11, bytes([protocol.CMD_SNAPSHOT]))
        t = time.monotonic()
        pair.t_written = {"left": t, "right": t}
        self.log("host", f"pair {pair.index}{' (warm-up)' if pair.warmup else ''}: 0xB2 sent to the bridge")

    def trigger(self, warmup: bool = False) -> bool:
        with self._lock:
            if self.pending is not None or not self.ready:
                return False
            self.pairs += 1
            self.pending = Pair(self.pairs, warmup)
            for l in self.links.values():
                l.fresh = False
        if self.bridge is not None:
            self._trigger_bridge(self.pending)
        else:
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
            exp = (f", exposure {frame.integration}/{frame.analog_gain:#04x}/{frame.digital_gain:#06x}"
                   if frame.integration is not None else "")
            self.log(side, f"pair {pair.index}: {frame.width}x{frame.height} lost={frame.lost_packets} "
                           f"rx {frame.duration_s:.2f}s, +{(frame.completed_at - pair.t_request):.2f}s after request{exp}")
            if len(pair.frames) == 2:
                self.done.append(pair)
                self.pending = None
                if self.exposure[0] == "sync":
                    self._sync_exposure(pair.frames["left"])

    def check_timeout(self):
        with self._lock:
            if self.pending and time.monotonic() - self.pending.t_request > PAIR_TIMEOUT_S:
                got = ",".join(self.pending.frames) or "nothing"
                self.log("host", f"pair {self.pending.index}: timed out after {PAIR_TIMEOUT_S:.0f} s (got {got})")
                self.pending = None

    def close(self):
        self._stop.set()
        if self.bridge is not None:
            self.bridge.close()
        else:
            self._thread.join(timeout=5)


def to_image(frame: protocol.Frame, rotate: int) -> np.ndarray:
    img = np.frombuffer(frame.pixels, np.uint8).reshape(frame.height, frame.width)
    return cv2.rotate(img, ROTATIONS[rotate]) if ROTATIONS[rotate] is not None else img.copy()


def fit_height(img: np.ndarray, h: int) -> np.ndarray:
    interp = cv2.INTER_AREA if h < img.shape[0] else cv2.INTER_LINEAR
    return cv2.resize(img, (max(1, round(img.shape[1] * h / img.shape[0])), h), interpolation=interp)


def label(img: np.ndarray, text: str, y: int = 24) -> np.ndarray:
    out = cv2.cvtColor(img, cv2.COLOR_GRAY2BGR) if img.ndim == 2 else img
    return stitch._label(out, text, y)


def compose(images: dict[str, np.ndarray], h: int) -> np.ndarray:
    """left | right, both scaled to h pixels high (already rotated)."""
    gap = np.full((h, 6, 3), 255, np.uint8)
    return np.hstack([label(fit_height(images["left"], h), "left"), gap, label(fit_height(images["right"], h), "right")])


def save_pair(pair: Pair, images: dict[str, np.ndarray], rotations: dict[str, int], boards: dict[str, str]) -> pathlib.Path:
    """left.png / right.png (rotated, native resolution) and info.json; stitch later with stitch.py."""
    out = HERE / "captures" / "stereo" / f"{datetime.datetime.now():%Y%m%d-%H%M%S}-{pair.index:03d}"
    out.mkdir(parents=True, exist_ok=True)
    for s in SIDES:
        cv2.imwrite(str(out / f"{s}.png"), images[s])
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
        # Frame header v2 (fw_nrf5340): board clock unless "common_clock" (time sync)
        "frame_id": {s: pair.frames[s].frame_id for s in SIDES},
        "capture_us": {s: pair.frames[s].capture_us for s in SIDES},
        "common_clock": {s: bool(pair.frames[s].header_flags & protocol.HEADER_COMMON_CLOCK) for s in SIDES},
        "exposure": {s: pair.frames[s].exposure for s in SIDES},
        "format": {s: protocol.FORMAT_NAMES.get(pair.frames[s].format, pair.frames[s].format) for s in SIDES},
        "bytes": {s: len(pair.frames[s].data) for s in SIDES},
    }
    (out / "info.json").write_text(json.dumps(info, indent=2), encoding="utf-8")
    return out


def flash_boards(mode: str, boards: dict[str, str], log: SessionLog) -> bool:
    """Build fw_nrf5340 (stream app) in the given mode and flash it to both boards via dk5340.ps1."""
    script = HERE.parent / "fw_nrf5340" / "tools" / "dk5340.ps1"
    for side in SIDES:
        dk_board = protocol.BOARDS[boards[side]].get("dk_board")
        if dk_board is None:
            log("host", f"--flash: {boards[side]} is not an nRF5340 board")
            return False
        log("host", f"flashing {mode} to the {side} board ({dk_board}); the first build of a mode takes 1-2 min...")
        t0 = time.monotonic()
        r = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(script),
                            "flash", "-Mode", mode, "-Board", dk_board],
                           cwd=script.parent.parent, capture_output=True, text=True, errors="replace")
        if r.returncode != 0:
            tail = "\n".join((r.stdout + r.stderr).strip().splitlines()[-15:])
            log("host", f"flashing the {side} board failed (exit {r.returncode}):\n{tail}")
            return False
        log("host", f"{side} board flashed with {mode} in {time.monotonic() - t0:.0f} s")
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--left", default="hm0360-5340", choices=sorted(protocol.BOARDS))
    ap.add_argument("--right", default="hm0360b-5340", choices=sorted(protocol.BOARDS))
    ap.add_argument("--rotate-left", type=int, choices=sorted(ROTATIONS), help="degrees clockwise (default: BOARDS)")
    ap.add_argument("--rotate-right", type=int, choices=sorted(ROTATIONS), help="degrees clockwise (default: BOARDS)")
    ap.add_argument("--flash", choices=["QQVGA", "QVGA", "VGA"],
                    help="first build and flash this mode to both boards (the mode is compile-time)")
    ap.add_argument("--bridge", nargs="?", const="", default=None, metavar="PORT",
                    help="use the BLE-USB bridge (COM port, default: found by USB VID)")
    ap.add_argument("--interval-ms", type=float, default=15.0, help="with --bridge: connection interval")
    ap.add_argument("--exposure", nargs="+", default=["auto"], metavar="MODE",
                    help="auto (both AE, default) | fixed INTEGRATION ANALOG_GAIN DIGITAL_GAIN (register values, "
                         "both boards) | sync (left AE, the right board takes the left's values for the next pair)")
    ap.add_argument("--format", default="raw", metavar="FMT",
                    help="image format the boards send: raw (default) | png | jpeg:QUALITY (1-100), encoded on the board")
    ap.add_argument("--height", type=int, default=640, help="display height of each picture (pixels)")
    ap.add_argument("--auto", type=int, default=0, help="take and save N pairs automatically, then quit")
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
    if args.flash and not flash_boards(args.flash, boards, log):
        log.close()
        return 1
    exposure = tuple(args.exposure)
    if exposure[0] == "fixed":
        if len(exposure) != 4:
            ap.error("--exposure fixed INTEGRATION ANALOG_GAIN DIGITAL_GAIN")
        exposure = ("fixed",) + tuple(int(x, 0) for x in exposure[1:])
    elif exposure[0] not in ("auto", "sync") or len(exposure) != 1:
        ap.error("--exposure auto | fixed I A D | sync")
    fmt = args.format.lower()
    if fmt == "raw":
        image_format = (protocol.FORMAT_RAW, 0)
    elif fmt == "png":
        image_format = (protocol.FORMAT_PNG, 0)
    elif fmt.startswith("jpeg:") and fmt[5:].isdigit() and 1 <= int(fmt[5:]) <= 100:
        image_format = (protocol.FORMAT_JPEG, int(fmt[5:]))
    else:
        ap.error("--format raw | png | jpeg:QUALITY")
    st = Stereo(log, boards, args.bridge, args.interval_ms, exposure, image_format)
    h = args.height
    canvas = np.zeros((h, h * 3 // 2, 3), np.uint8)
    shown: Optional[tuple[Pair, dict[str, np.ndarray]]] = None  # pair on screen, its rotated pictures
    shown_saved: Optional[pathlib.Path] = None
    message = "waiting for both boards..."
    saved = 0
    next_auto = 0.0
    cv2.namedWindow(WINDOW, cv2.WINDOW_AUTOSIZE)
    # Mouse clicks work even when an input method (IME) swallows key presses.
    clicks: collections.deque[str] = collections.deque()
    cv2.setMouseCallback(WINDOW, lambda event, *_: clicks.append(
        "take" if event == cv2.EVENT_LBUTTONDOWN else "save") if event in (cv2.EVENT_LBUTTONDOWN, cv2.EVENT_RBUTTONDOWN) else None)

    def save_shown():
        nonlocal shown_saved, saved, message
        pair, images = shown
        shown_saved = save_pair(pair, images, rotations, boards)
        saved += 1
        message = f"pair {pair.index} saved -> {shown_saved.name}  |  {TAKE_HINT}"
        log("host", f"pair {pair.index}: saved -> {shown_saved}")

    try:
        while True:
            st.check_timeout()
            while st.done:
                pair = st.done.popleft()
                images = {s: to_image(pair.frames[s], rotations[s]) for s in SIDES}
                shown, shown_saved = (pair, images), None
                canvas = compose(images, h)
                if pair.warmup:
                    message = f"warm-up pair (camera init, not saved)  |  {TAKE_HINT}"
                else:
                    message = f"pair {pair.index}  |  {SAVE_HINT}  {TAKE_HINT}"
                    if args.auto:
                        save_shown()
                next_auto = time.monotonic() + args.interval
            if st.ready and st.pending is None and st.needs_warmup:
                st.apply_exposure()  # the boards rebooted: they start in auto
                if st.trigger(warmup=True):
                    log("host", "both boards connected: warm-up pair (includes the camera init, not saved)")
                    message = "both boards connected: warm-up pair..."
            elif args.auto and st.ready and st.pending is None and time.monotonic() >= next_auto:
                if saved >= args.auto:
                    break
                st.trigger()

            view = canvas.copy()
            label(view, "taking..." if st.pending else message, y=view.shape[0] - 12)
            cv2.setWindowTitle(WINDOW, f"{WINDOW} - {st.status()} | pairs saved {saved}")
            cv2.imshow(WINDOW, view)
            key = cv2.waitKey(30) & 0xFF
            if key != 0xFF:
                log("host", f"key {key} ({chr(key) if 32 <= key < 127 else '-'})")
            if key in (ord("q"), 27) or cv2.getWindowProperty(WINDOW, cv2.WND_PROP_VISIBLE) < 1:
                break
            action = clicks.popleft() if clicks else None
            if key in (ord(" "), 13):
                action = "take"
            elif key in (ord("s"), ord("S")):
                action = "save"
            if action == "take":
                if st.trigger():
                    message = "taking..."
                else:
                    why = "previous pair not done yet" if st.pending else f"boards not ready ({st.status()})"
                    message = f"cannot take: {why}"
                    log("host", f"take ignored: {why}")
            elif action == "save":
                if shown is None or shown[0].warmup:
                    message = f"nothing to save yet  |  {TAKE_HINT}"
                elif shown_saved is not None:
                    message = f"pair {shown[0].index} already saved -> {shown_saved.name}"
                else:
                    save_shown()
    finally:
        if args.snapshot:
            cv2.imwrite(args.snapshot, canvas)
        log("host", f"summary: pairs saved {saved}")
        st.close()
        cv2.destroyAllWindows()
        log.close()


if __name__ == "__main__":
    sys.exit(main())

