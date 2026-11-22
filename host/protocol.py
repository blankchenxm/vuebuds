"""BLE protocol of the vuebuds "mustard" firmware (see fw/ble_cus.c, fw/ble_manager.c).

control char 0x1403: write 0xB1 to start the camera stream,
                     0xB2 to take one picture (fw_nrf5340/ only)
data    char 0x1402: notifications of [seq, flags, payload...]
                     flags bit0 = start of frame; a start-of-frame payload begins with
                     width and height (uint16 little endian each), followed by pixels (v1, fw/)
                     flags bit1 = frame header v2 (fw_nrf5340, 2026-10-11): the payload begins
                     with HEADER_V2 (FrameHeaderV2 below), then `length` image bytes
                     (format 0 raw gray, 1 PNG, 2 JPEG)
control 0x1403: 0xB3 <format> <quality> image format; 0xB4 codec bench; 0xB6 00 auto exposure,
                0xB6 01 <integration u16> <analog gain u8> <digital gain u16> fixed exposure
M2 (hub INTERFACES C2, fw_nrf5340/src/ringcmd.h): 0xB8 2 / 0 ring buffer, 0xB9 status, 0xBA fetch;
                replies are data notifications with flags bit2: [seq, 0x04, type, ...] (Reply below)
"""
import json
import pathlib
import struct
import sys
import time
from dataclasses import dataclass
from typing import Callable, Optional

# ../tools/devtools.py (J-Link tools, board reset; shared with the firmware scripts) for the
# host scripts that import this module.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / "tools"))

DEVICE_NAME = "mustard"
# All DKs advertise as "mustard"; pick one by J-Link serial number and BLE address
# (ble_target(): on macOS a per-Mac UUID instead of the address).
# nRF52840 DKs run fw/, nRF5340 DKs run fw_nrf5340/ (same BLE protocol).
# rotate: degrees clockwise that make the picture upright (stereo.py), checked on real
# pictures with the modules as they sit on the two 5340 DKs (10-07; right board 270, 10-08).
# dk_board: the board's name for fw_nrf5340/tools/dk5340.py --board (stereo.py --flash).
BOARDS = {
    "hm0360": {"snr": "1050291681", "address": "C8:1A:80:9C:01:BC"},
    "hm01b0": {"snr": "1050221517", "address": "D6:25:79:FD:6A:6B"},
    "hm0360-5340": {"snr": "1050035314", "address": "DA:E7:DE:69:E3:5E",
                    "device": "nRF5340_xxAA_APP", "family": "nrf53", "rotate": 90, "dk_board": "HM0360"},
    "hm0360b-5340": {"snr": "1050017384", "address": "EA:F1:6D:2A:C0:BB",
                    "device": "nRF5340_xxAA_APP", "family": "nrf53", "rotate": 270,  # user 10-08: 90 + 180
                    "dk_board": "HM0360B"},
}
# macOS: CoreBluetooth hides BLE addresses; bleak connects by a UUID that this Mac assigns to
# each board. Kept per machine in boards.local.json (gitignored), {board: uuid}, written by
# `python ble_identify.py`. The bridge (bridge.py) still uses the addresses above.
LOCAL_BOARDS = pathlib.Path(__file__).resolve().parent / "boards.local.json"


def ble_target(board: str) -> str:
    """What bleak connects to for a board in BOARDS: its address, or on macOS its UUID."""
    if sys.platform != "darwin":
        return BOARDS[board]["address"]
    uuids = json.loads(LOCAL_BOARDS.read_text("utf-8")) if LOCAL_BOARDS.exists() else {}
    if board not in uuids:
        raise SystemExit(f"{board}: no CoreBluetooth UUID on this Mac yet; run `python ble_identify.py` "
                         f"(writes {LOCAL_BOARDS.name})")
    return uuids[board]


# J-Link device / nrfjprog family of the nRF52840 boards (entries without "device").
DEFAULT_DEVICE = "NRF52840_XXAA"
DEFAULT_FAMILY = "nrf52"
# As advertised by the device; the comment in fw/ble_cus.h lists these bytes in a different order.
_BASE = "47ea{:04x}-a0e4-554e-5282-0afcd3246970"
DATA_UUID = _BASE.format(0x1402)
CONTROL_UUID = _BASE.format(0x1403)
CMD_STREAM_START = 0xB1
CMD_SNAPSHOT = 0xB2
CMD_SET_FORMAT = 0xB3
CMD_CODEC_BENCH = 0xB4  # next picture sent raw, PNG, JPEG q95 / 90 / 85 / 75 / 60 (same frame id)
CMD_SET_EXPOSURE = 0xB6
CMD_TIME_REFERENCE = 0xB5   # from the bridge only (fw_nrf5340/src/timesync.h)
CMD_SNAPSHOT_AT = 0xB7      # picture at a bridge time T (bridge.Bridge.timed_write)
CMD_TIMED = 0xB8  # timed capture: <mode u8> <period u16 ms>; mode 1 start, 0 stop (INTERFACES C2, F5)
CMD_STATUS = 0xB9  # M2: -> R_STATUS
CMD_FETCH = 0xBA   # M2: 0 nearest / 1 at or before <t u64>, 2 range <t1> <t2> <n u8>, 3 frame number <u32>
FORMAT_RAW, FORMAT_PNG, FORMAT_JPEG = 0, 1, 2
FORMAT_NAMES = {FORMAT_RAW: "raw", FORMAT_PNG: "png", FORMAT_JPEG: "jpeg"}


def set_format_command(fmt: int, quality: int = 0) -> bytes:
    return bytes([CMD_SET_FORMAT, fmt, quality])


def timed_command(period_ms: int) -> bytes:
    """0xB8: a picture every period_ms (100-60000); 0 stops."""
    if period_ms == 0:
        return bytes([CMD_TIMED, 0])
    return bytes([CMD_TIMED, 1]) + struct.pack("<H", period_ms)


def ring_command(period_ms: int) -> bytes:
    """0xB8 2: ring buffer, a picture at every multiple of period_ms on the bridge's clock; 0: 0xB8 0 (stop)."""
    if period_ms == 0:
        return bytes([CMD_TIMED, 0])
    return bytes([CMD_TIMED, 2]) + struct.pack("<H", period_ms)


def status_command() -> bytes:
    return bytes([CMD_STATUS])


def fetch_nearest_command(t_us: int) -> bytes:
    return bytes([CMD_FETCH, 0]) + struct.pack("<Q", t_us)


def fetch_before_command(t_us: int) -> bytes:
    return bytes([CMD_FETCH, 1]) + struct.pack("<Q", t_us)


def fetch_range_command(t1_us: int, t2_us: int, n: int) -> bytes:
    return bytes([CMD_FETCH, 2]) + struct.pack("<QQB", t1_us, t2_us, n)


def fetch_seq_command(seq: int) -> bytes:
    return bytes([CMD_FETCH, 3]) + struct.pack("<I", seq)


def set_exposure_command(integration: Optional[int] = None, analog_gain: int = 0, digital_gain: int = 0) -> bytes:
    """None: automatic exposure; else fixed register values (HM0360)."""
    if integration is None:
        return bytes([CMD_SET_EXPOSURE, 0])
    return bytes([CMD_SET_EXPOSURE, 1]) + struct.pack("<HBH", integration, analog_gain, digital_gain)

FLAG_START_OF_FRAME = 0x01
FLAG_HEADER_V2 = 0x02
FLAG_REPLY = 0x04  # M2 reply packet, not part of a frame
R_STATUS, R_FRAME, R_NONE, R_DONE, R_TIME = 1, 2, 3, 4, 5
R_NONE_REASONS = {0: "overwritten", 1: "not taken yet", 2: "ring empty", 3: "busy", 4: "bad arguments",
                  5: "not synced"}
STATUS_FIELDS = ("state", "period", "format", "quality", "frames", "oldest_seq", "oldest_us", "newest_seq",
                 "newest_us", "now_us", "used_kb", "total_kb", "skipped", "overwritten", "psram", "synced")
STATUS = struct.Struct("<BHBBHIQIQQHHHIBB")
R_FRAME_S = struct.Struct("<IQI")    # frame number, capture time, length
R_NONE_S = struct.Struct("<BQQ")     # reason, oldest time, newest time
FRAME_HEADER = struct.Struct("<HH")  # width, height (v1)
# v2: width, height, format, quality, length, capture_us, frame_id, integration, analog gain,
# digital gain, flags (fw_nrf5340/src/ble.h ble_frame_header_t)
HEADER_V2 = struct.Struct("<HHBBIIHHBHB")
HEADER_COMMON_CLOCK = 0x01
HEADER_MANUAL_EXPOSURE = 0x02
HEADER_SKIPPED = 0x04  # 0xB8: at least one period skipped before this picture
HEADER_FROM_RING = 0x08  # M2: sent out of the ring buffer (after R_FRAME)
HEADER_TIMED = 0x10    # 0xB8: taken by the timed capture, not a 0xB2 request
# Placeholder canvas size before the first frame header arrives.
DEFAULT_SIZE = (320, 240)


@dataclass
class Frame:
    index: int
    width: int
    height: int
    pixels: bytes  # 8-bit gray, width x height (decoded if the board sent PNG / JPEG)
    packets: int
    lost_packets: int
    duration_s: float
    completed_at: float
    # Frame header v2 (None / 0 for v1 boards)
    version: int = 1
    format: int = FORMAT_RAW
    quality: int = 0
    data: bytes = b""          # the bytes as sent (encoded image for PNG / JPEG)
    capture_us: Optional[int] = None
    frame_id: Optional[int] = None
    integration: Optional[int] = None
    analog_gain: Optional[int] = None
    digital_gain: Optional[int] = None
    header_flags: int = 0

    @property
    def exposure(self) -> dict:
        return {"integration": self.integration, "analog_gain": self.analog_gain,
                "digital_gain": self.digital_gain, "manual": bool(self.header_flags & HEADER_MANUAL_EXPOSURE)}


@dataclass
class Reply:
    """M2 reply packet. kind: R_*; fields: STATUS_FIELDS (R_STATUS), seq / capture_us / length
    (R_FRAME), reason / oldest_us / newest_us (R_NONE), count (R_DONE)."""
    kind: int
    fields: dict
    received_at: float


def parse_reply(payload: bytes, now: float) -> Optional[Reply]:
    if not payload:
        return None
    kind, body = payload[0], payload[1:]
    try:
        if kind == R_STATUS:
            return Reply(kind, dict(zip(STATUS_FIELDS, STATUS.unpack_from(body))), now)
        if kind == R_FRAME:
            return Reply(kind, dict(zip(("seq", "capture_us", "length"), R_FRAME_S.unpack_from(body))), now)
        if kind == R_NONE:
            return Reply(kind, dict(zip(("reason", "oldest_us", "newest_us"), R_NONE_S.unpack_from(body))), now)
        if kind == R_DONE:
            return Reply(kind, {"count": body[0]}, now)
        if kind == R_TIME:
            return Reply(kind, dict(zip(("token", "board_us"), struct.unpack_from("<IQ", body))), now)
    except (struct.error, IndexError):
        return None
    return Reply(kind, {"raw": bytes(body)}, now)


def decode_image(data: bytes, width: int, height: int) -> bytes:
    """PNG / JPEG bytes -> 8-bit gray pixels (OpenCV)."""
    import cv2
    import numpy as np
    img = cv2.imdecode(np.frombuffer(data, np.uint8), cv2.IMREAD_GRAYSCALE)
    if img is None or img.shape != (height, width):
        raise ValueError(f"cannot decode a {width}x{height} image from {len(data)} bytes")
    return img.tobytes()


class FrameAssembler:
    """Reassembles notification payloads into frames.

    on_frame is called with each complete frame; on_event with human-readable
    diagnostics (lost packets, discarded partial frames).
    """

    def __init__(self, on_frame: Callable[[Frame], None], on_event: Callable[[str], None] = print,
                 on_reply: Optional[Callable[[Reply], None]] = None):
        self.on_frame = on_frame
        self.on_event = on_event
        self.on_reply = on_reply  # M2 reply packets (flags bit2); ignored if None
        self.frames = 0
        self.total_packets = 0
        self.total_lost = 0
        self.discarded = 0
        self._last_seq: Optional[int] = None
        self._reset_frame()

    def _reset_frame(self):
        self._buf = bytearray()
        self._size = None
        self._v2 = None  # header tuple
        self._packets = 0
        self._lost = 0
        self._t_start = 0.0

    def feed(self, data: bytes):
        if len(data) < 2:
            return
        now = time.monotonic()
        self.total_packets += 1
        seq, flags, payload = data[0], data[1], data[2:]

        if self._last_seq is not None:
            gap = (seq - self._last_seq - 1) & 0xFF
            if gap:
                self.total_lost += gap
                self._lost += gap
                self.on_event(f"seq gap: {self._last_seq} -> {seq} ({gap} packet(s) lost)")
        self._last_seq = seq

        if flags & FLAG_REPLY:  # M2 reply: never part of a frame (may come between a frame's packets)
            r = parse_reply(payload, now)
            if r is None:
                self.on_event(f"bad reply packet: {payload.hex()}")
            elif self.on_reply:
                self.on_reply(r)
            return

        if flags & FLAG_START_OF_FRAME:
            if self._buf:
                self.discarded += 1
                self.on_event(f"discarding partial frame: {len(self._buf)} bytes")
            self._reset_frame()
            header = HEADER_V2 if flags & FLAG_HEADER_V2 else FRAME_HEADER
            if len(payload) < header.size:
                self.on_event(f"start-of-frame packet too short: {len(payload)} bytes")
                return
            if header is HEADER_V2:
                self._v2 = HEADER_V2.unpack_from(payload)
                self._size = self._v2[0], self._v2[1]
            else:
                self._size = FRAME_HEADER.unpack_from(payload)
            payload = payload[header.size:]
            self._t_start = now
        if self._size is None:
            return

        self._packets += 1
        self._buf += payload
        w, h = self._size
        length = self._v2[4] if self._v2 else w * h
        if len(self._buf) >= length:
            self.frames += 1
            data = bytes(self._buf[:length])
            frame = Frame(index=self.frames, width=w, height=h, pixels=data, packets=self._packets,
                          lost_packets=self._lost, duration_s=now - self._t_start, completed_at=now, data=data)
            if self._v2:
                (_, _, frame.format, frame.quality, _, frame.capture_us, frame.frame_id, frame.integration,
                 frame.analog_gain, frame.digital_gain, frame.header_flags) = self._v2
                frame.version = 2
                if frame.format != FORMAT_RAW:
                    try:
                        frame.pixels = decode_image(data, w, h)
                    except ValueError as e:
                        self.on_event(f"frame {self.frames}: {e}")
                        self._reset_frame()
                        return
            self.on_frame(frame)
            self._reset_frame()

    def partial_bytes(self) -> int:
        return len(self._buf)
