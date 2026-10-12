"""Reusable offline and hardware operations for the local Gradio demos."""
import atexit
import datetime
import json
import pathlib
import queue
import subprocess
import threading
import time
from zoneinfo import ZoneInfo

import cv2
import numpy as np

import bridge
import protocol
import stitch
import stereo

HERE = pathlib.Path(__file__).resolve().parent
BOARDS = {"left": "hm0360-5340", "right": "hm0360b-5340"}
MODES = {"QVGA": (320, 240), "VGA": (640, 480)}
FORMATS = {"raw": (0, 0), "png": (1, 0), **{f"jpeg:{q}": (2, q) for q in (95, 90, 85, 75, 60)}}


def worktree_hosts():
    roots = [HERE]
    try:
        text = subprocess.check_output(["git", "worktree", "list", "--porcelain"], cwd=HERE,
                                       encoding="utf-8", timeout=5)
        roots += [pathlib.Path(line[9:]) / "host" for line in text.splitlines() if line.startswith("worktree ")]
    except (OSError, subprocess.SubprocessError):
        pass
    return list(dict.fromkeys(roots))


def default_captures():
    for root in worktree_hosts():
        path = root / "captures" / "stereo"
        if any(path.glob("*QVGA/*/left.png")):
            return path
    return HERE / "captures" / "stereo"


def read_image(path, color=False):
    # imdecode handles Windows paths with non-ASCII characters across OpenCV versions.
    image = cv2.imdecode(np.fromfile(path, np.uint8), cv2.IMREAD_COLOR if color else cv2.IMREAD_GRAYSCALE)
    if image is None:
        raise ValueError(f"Cannot read image: {path}")
    return image


def write_image(path, image):
    ok, encoded = cv2.imencode(".png", image)
    if not ok:
        raise ValueError(f"Cannot save image: {path}")
    encoded.tofile(path)


def datasets(root):
    root = pathlib.Path(root)
    found = {}
    if not root.exists():
        return found
    loose = [p for p in root.iterdir() if p.is_dir() and (p / "left.png").is_file() and (p / "right.png").is_file()]
    if loose:
        found["Ungrouped captures"] = sorted(loose)
    for directory in sorted(root.iterdir()):
        if directory.is_dir():
            pairs = sorted(p.parent for p in directory.glob("*/left.png") if (p.parent / "right.png").is_file())
            if pairs:
                found[directory.name] = pairs
    return found


def resolve_pair(root, dataset, name):
    pairs = datasets(root).get(dataset, [])
    for pair in pairs:
        if pair.name == name:
            return pair
    raise ValueError("Select a valid dataset and image pair first.")


def run_stitch(left, right, method):
    if method not in stitch.DEFAULT_METHODS:
        raise ValueError("Unknown stitching method")
    t0 = time.perf_counter()
    try:
        if method in ("lightglue", "loftr") and not stitch.matcher_available(method):
            raise RuntimeError(f"Optional dependencies for {method} are missing. See host/requirements.txt.")
        return stitch.build_methods()[method](left, right)
    except Exception as exc:
        return stitch.StitchResult(method, False, None, (time.perf_counter() - t0) * 1000,
                                   {"reason": f"{type(exc).__name__}: {exc}"})


def cached_stitch(pair, method, force=False):
    if not force:
        try:
            rows = json.loads((pair / "stitch_results.json").read_text(encoding="utf-8"))
            row = next(r for r in rows if r["method"] == method)
            image_path = pair / f"stitch_{method}.png"
            if row["ok"] and image_path.is_file():
                result = stitch.StitchResult(method, True, read_image(image_path), row["ms"],
                    {k: v for k, v in row.items() if k not in ("method", "ok", "ms")})
                match_path = pair / f"matches_{method}.png"
                if match_path.is_file():
                    result.matches = read_image(match_path, True)
                return result, True
        except (OSError, ValueError, KeyError, StopIteration):
            pass
    # Leave the previous experiment's files untouched; newly computed results live in session state.
    return run_stitch(read_image(pair / "left.png"), read_image(pair / "right.png"), method), False


def auto_stitch(left, right, learned=False):
    attempts = []
    started = time.perf_counter()
    for method in ["sift", "asift", "orb"] + (["lightglue", "loftr"] if learned else []):
        result = run_stitch(left, right, method)
        attempts.append(result.summary())
        if result.ok and result.image is not None:
            return result, attempts, time.perf_counter() - started
    return None, attempts, time.perf_counter() - started


def pair_info(pair, clicked):
    frames = pair.frames
    info = {"pair": pair.index, "warmup": pair.warmup,
            "frame_done_s": {s: f.completed_at - pair.t_request for s, f in frames.items()},
            "click_to_frame_s": {s: f.completed_at - clicked for s, f in frames.items()},
            "size": {s: [f.width, f.height] for s, f in frames.items()},
            "bytes": {s: len(f.data or f.pixels) for s, f in frames.items()},
            "format": {s: protocol.FORMAT_NAMES[f.format] for s, f in frames.items()},
            "quality": {s: f.quality for s, f in frames.items()},
            "lost_packets": {s: f.lost_packets for s, f in frames.items()},
            "frame_id": {s: f.frame_id for s, f in frames.items()},
            "capture_us": {s: f.capture_us for s, f in frames.items()},
            "exposure": {s: f.exposure for s, f in frames.items()},
            "arrival_delta_ms": abs(frames["left"].completed_at - frames["right"].completed_at) * 1000,
            "capture_delta_us": None}
    if all(f.header_flags & protocol.HEADER_COMMON_CLOCK and f.capture_us is not None for f in frames.values()):
        diff = (frames["left"].capture_us - frames["right"].capture_us + 2**31) % 2**32 - 2**31
        info["capture_delta_us"] = abs(diff)
    return info


def save_capture(root, mode, images, info):
    now = datetime.datetime.now(ZoneInfo("America/Los_Angeles"))
    out = pathlib.Path(root) / f"demo_{now:%Y%m%d}_{mode}" / f"{now:%Y%m%d-%H%M%S-%f}"
    out.mkdir(parents=True)
    for side, img in images.items():
        write_image(out / f"{side}.png", img)
    info = dict(info, time=now.isoformat(), boards=BOARDS,
                rotate={s: protocol.BOARDS[b]["rotate"] for s, b in BOARDS.items()})
    (out / "info.json").write_text(json.dumps(info, indent=2, ensure_ascii=False), encoding="utf-8")
    return out


class Camera:
    """One persistent bridge connection. Capture/flash always hold the same process lock."""
    def __init__(self, port=None):
        self.port = port or ""
        self.stereo = None
        self.lock = threading.Lock()
        self.log_lines = []
        self.settings = None
        self.requires_flash = False
        atexit.register(self.close)

    def log(self, side, message):
        self.log_lines.append(f"[{side}] {message}")
        self.log_lines[:] = self.log_lines[-60:]

    def close(self, reset=False):
        if self.stereo is not None:
            if reset:
                try:
                    # Closing the COM port alone leaves the BLE links and pending capture alive.
                    # Drop both links before a retry so delayed old frames cannot become a new pair.
                    self.stereo.bridge.configure([])
                    time.sleep(0.2)
                except Exception:
                    pass  # An unplugged serial port must not prevent local resource cleanup.
            self.stereo.close()
            self.stereo = None
        self.settings = None

    def _take(self, warmup=False):
        st = self.stereo
        st.done.clear()
        requested = time.monotonic()
        if not st.trigger(warmup=warmup):
            raise RuntimeError("Cannot trigger capture; check both camera connections.")
        deadline = time.monotonic() + 45
        while time.monotonic() < deadline:
            errors = [(slot, err) for stamp, slot, err in st.bridge.write_results if stamp >= requested and err]
            if errors:
                raise RuntimeError(f"Bridge failed to forward capture command: {errors}")
            if st.done:
                pair = st.done.popleft()
                if any(f.lost_packets for f in pair.frames.values()):
                    raise RuntimeError("Packet loss detected. This pair was discarded; capture again.")
                return pair
            if not st.ready:
                raise RuntimeError("A board disconnected during capture. Check USB and BLE.")
            if st.pending is None and not st.done:
                raise RuntimeError("Capture command failed. Reconnect the cameras.")
            time.sleep(0.03)
        pending = st.pending
        missing = [s for s in stereo.SIDES if s not in pending.frames] if pending else list(stereo.SIDES)
        raise TimeoutError(f"Capture timed out after 45 seconds. Missing: {', '.join(missing)}; check the corresponding sensor connection.")

    def capture(self, mode, fmt, exposure, clicked):
        """Yield progress strings, then (images, timing metadata)."""
        if mode not in MODES or fmt not in FORMATS or exposure not in ("auto", "sync"):
            raise ValueError("Invalid capture settings")
        if not self.lock.acquire(blocking=False):
            raise RuntimeError("Hardware is capturing or flashing. Wait for the current operation to finish.")
        try:
            if self.requires_flash:
                raise RuntimeError("The previous flash was incomplete. Flash both boards again first.")
            if self.stereo is None:
                yield "Connecting the BLE-USB bridge and both cameras…"
                # A previous CLI/DEMO may have left streaming or an unfinished request on the bridge.
                # Start a fresh camera session, then keep it open across subsequent pictures.
                bridge.release(self.port or None)
                self.stereo = stereo.Stereo(self.log, BOARDS, bridge_port=self.port)
            st = self.stereo
            deadline = time.monotonic() + 35
            while not st.ready and time.monotonic() < deadline:
                time.sleep(0.1)
            if not st.ready:
                raise TimeoutError("Cameras are not ready. Check power and the bridge nRF USB cable; close other serial clients such as stereo.py.")
            changed = self.settings != (fmt, exposure)
            if changed or st.needs_warmup:
                st.image_format, st.exposure = FORMATS[fmt], (exposure,)
                st.apply_exposure()
                # B3/B6 are queued writes on the bridge. Give both links several connection events.
                time.sleep(0.15)
                yield "Warming up and applying compression / exposure settings. Warm-up frames are excluded from capture timing…"
                self._take(warmup=True)
                # In sync mode the previous left exposure is now applied to the right board.
                time.sleep(0.15)
                self.settings = (fmt, exposure)
            yield "Requesting both cameras simultaneously and receiving images…"
            pair = self._take()
            if any((f.width, f.height) != MODES[mode] for f in pair.frames.values()):
                sizes = ", ".join(f"{s}: {f.width}×{f.height}" for s, f in pair.frames.items())
                raise RuntimeError(f"Received dimensions {sizes} do not match {mode}. Select Flash both cameras first.")
            expected_fmt, expected_q = FORMATS[fmt]
            if any(f.format != expected_fmt or (expected_fmt == 2 and f.quality != expected_q) for f in pair.frames.values()):
                raise RuntimeError("The received format differs from the selection. Check board logs or reflash; compression may have failed or firmware may be outdated.")
            images = {s: stereo.to_image(f, protocol.BOARDS[BOARDS[s]]["rotate"]) for s, f in pair.frames.items()}
            yield images, pair_info(pair, clicked)
        except BaseException:
            self.close(reset=True)
            raise
        finally:
            self.lock.release()

    def flash(self, mode):
        if mode not in MODES:
            raise ValueError("Select QVGA or VGA")
        if not self.lock.acquire(blocking=False):
            raise RuntimeError("Hardware is busy. Please wait.")
        process = None
        try:
            self.close()
            self.requires_flash = True
            script = HERE.parent / "fw_nrf5340" / "tools" / "dk5340.ps1"
            for side, board in BOARDS.items():
                yield f"Building and flashing {side} / {mode}. Keep USB connected…"
                process = subprocess.Popen([
                    "powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(script),
                    "flash", "-Mode", mode, "-Board", protocol.BOARDS[board]["dk_board"]],
                    cwd=script.parent.parent, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                    text=True, encoding="utf-8", errors="replace",
                    creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
                lines = queue.Queue()
                def drain(proc=process):
                    for line in proc.stdout:
                        lines.put(line.rstrip())
                    lines.put(None)
                worker = threading.Thread(target=drain, daemon=True)
                worker.start()
                start, last, tail = time.monotonic(), 0, []
                while True:
                    try:
                        line = lines.get(timeout=0.3)
                    except queue.Empty:
                        line = ""
                    if line is None:
                        break
                    if line:
                        tail.append(line)
                        tail = tail[-20:]
                    if time.monotonic() - last > 0.5:
                        yield f"{side} / {mode} · {time.monotonic() - start:.0f} seconds\n" + "\n".join(tail)
                        last = time.monotonic()
                code = process.wait()
                worker.join()
                process.stdout.close()
                if code:
                    raise RuntimeError(f"{side} flash failed ({code})\n" + "\n".join(tail))
                yield f"{side} / {mode} flash complete."
            self.requires_flash = False
            yield f"Both cameras are flashed for {mode}. Ready to capture; the first capture reconnects and warms up."
        finally:
            # Flash must finish once started; do not terminate a programmer mid-write on a UI disconnect.
            if process is not None and process.poll() is None:
                process.wait()
            self.lock.release()
