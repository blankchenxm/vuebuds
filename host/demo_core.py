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
        raise ValueError(f"图片无法读取: {path}")
    return image


def write_image(path, image):
    ok, encoded = cv2.imencode(".png", image)
    if not ok:
        raise ValueError(f"图片保存失败: {path}")
    encoded.tofile(path)


def datasets(root):
    root = pathlib.Path(root)
    found = {}
    if not root.exists():
        return found
    loose = [p for p in root.iterdir() if p.is_dir() and (p / "left.png").is_file() and (p / "right.png").is_file()]
    if loose:
        found["未分组拍摄"] = sorted(loose)
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
    raise ValueError("请先选择有效的数据集和一组图片。")


def run_stitch(left, right, method):
    if method not in stitch.DEFAULT_METHODS:
        raise ValueError("未知拼接方法")
    t0 = time.perf_counter()
    try:
        if method in ("lightglue", "loftr") and not stitch.matcher_available(method):
            raise RuntimeError(f"未安装 {method} 的可选依赖，请查看 host/requirements.txt")
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

    def close(self):
        if self.stereo is not None:
            self.stereo.close()
            self.stereo = None
        self.settings = None

    def _take(self, warmup=False):
        st = self.stereo
        st.done.clear()
        if not st.trigger(warmup=warmup):
            raise RuntimeError("无法触发拍摄；请检查双板连接。")
        deadline = time.monotonic() + 45
        while time.monotonic() < deadline:
            if st.done:
                pair = st.done.popleft()
                if any(f.lost_packets for f in pair.frames.values()):
                    raise RuntimeError("本对图像丢包，已丢弃；请重新拍摄。")
                return pair
            if not st.ready:
                raise RuntimeError("拍摄时开发板断开，请检查 USB 和 BLE。")
            if st.pending is None:
                raise RuntimeError("拍照命令失败，请重新连接。")
            time.sleep(0.03)
        raise TimeoutError("双板收图超时（45 秒），连接已关闭，可重新拍摄。")

    def capture(self, mode, fmt, exposure, clicked):
        """Yield progress strings, then (images, timing metadata)."""
        if mode not in MODES or fmt not in FORMATS or exposure not in ("auto", "sync"):
            raise ValueError("无效的拍摄设置")
        if not self.lock.acquire(blocking=False):
            raise RuntimeError("硬件正在拍摄或烧录，请等待当前操作完成。")
        try:
            if self.requires_flash:
                raise RuntimeError("上次双板烧录未完成，请先重新烧录两块板。")
            if self.stereo is None:
                yield "正在连接 BLE-USB 桥和两块相机板…"
                self.stereo = stereo.Stereo(self.log, BOARDS, bridge_port=self.port)
            st = self.stereo
            deadline = time.monotonic() + 35
            while not st.ready and time.monotonic() < deadline:
                time.sleep(0.1)
            if not st.ready:
                raise TimeoutError("双板未就绪，请检查供电、桥的 nRF USB 线，关闭 stereo.py 等占用串口的程序。")
            changed = self.settings != (fmt, exposure)
            if changed or st.needs_warmup:
                st.image_format, st.exposure = FORMATS[fmt], (exposure,)
                st.apply_exposure()
                # B3/B6 are queued writes on the bridge. Give both links several connection events.
                time.sleep(0.15)
                yield "正在预热并应用压缩/曝光设置；预热图片不计入拍照耗时…"
                self._take(warmup=True)
                # In sync mode the previous left exposure is now applied to the right board.
                time.sleep(0.15)
                self.settings = (fmt, exposure)
            yield "正在同时请求左右板拍照并接收图片…"
            pair = self._take()
            if any((f.width, f.height) != MODES[mode] for f in pair.frames.values()):
                sizes = ", ".join(f"{s}: {f.width}×{f.height}" for s, f in pair.frames.items())
                raise RuntimeError(f"实际尺寸 {sizes} 与所选 {mode} 不同。请先按「烧录所选分辨率到双板」。")
            expected_fmt, expected_q = FORMATS[fmt]
            if any(f.format != expected_fmt or (expected_fmt == 2 and f.quality != expected_q) for f in pair.frames.values()):
                raise RuntimeError("板端返回格式与选择不同，可能压缩失败或固件过旧，请检查板端日志/重新烧录。")
            images = {s: stereo.to_image(f, protocol.BOARDS[BOARDS[s]]["rotate"]) for s, f in pair.frames.items()}
            yield images, pair_info(pair, clicked)
        except BaseException:
            self.close()  # Never reuse an incomplete request or receive a stale frame as a new pair.
            raise
        finally:
            self.lock.release()

    def flash(self, mode):
        if mode not in MODES:
            raise ValueError("请选择 QVGA 或 VGA")
        if not self.lock.acquire(blocking=False):
            raise RuntimeError("硬件正在使用，请等待。")
        process = None
        try:
            self.close()
            self.requires_flash = True
            script = HERE.parent / "fw_nrf5340" / "tools" / "dk5340.ps1"
            for side, board in BOARDS.items():
                yield f"正在编译并烧录 {side} / {mode}，请保持 USB 连接…"
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
                        yield f"{side} / {mode} · {time.monotonic() - start:.0f} 秒\n" + "\n".join(tail)
                        last = time.monotonic()
                code = process.wait()
                worker.join()
                process.stdout.close()
                if code:
                    raise RuntimeError(f"{side} 烧录失败 ({code})\n" + "\n".join(tail))
                yield f"{side} / {mode} 烧录完成。"
            self.requires_flash = False
            yield f"双板 {mode} 烧录完成，可点击拍摄。首次拍摄会重新连接和预热。"
        finally:
            # Flash must finish once started; do not terminate a programmer mid-write on a UI disconnect.
            if process is not None and process.poll() is None:
                process.wait()
            self.lock.release()
