"""BLE protocol of the vuebuds "mustard" firmware (see fw/ble_cus.c, fw/ble_manager.c).

control char 0x1403: write 0xB1 to start the camera stream,
                     0xB2 to take one picture (fw_nrf5340/ only)
data    char 0x1402: notifications of [seq, flags, payload...]
                     flags bit0 = start of frame; a start-of-frame payload begins with
                     width and height (uint16 little endian each), followed by pixels (v1, fw/)
                     flags bit1 = frame header v2 (fw_nrf5340, 2026-10-11): the payload begins
                     with HEADER_V2 (FrameHeaderV2 below), then `length` image bytes
                     (format 0 raw gray, 1 PNG, 2 JPEG)
control 0x1403: 0xB3 <format> <quality> image format; 0xB6 00 auto exposure,
                0xB6 01 <integration u16> <analog gain u8> <digital gain u16> fixed exposure
"""
import struct
import time
from dataclasses import dataclass
from typing import Callable, Optional

DEVICE_NAME = "mustard"
# All DKs advertise as "mustard"; pick one by J-Link serial number and BLE address.
# nRF52840 DKs run fw/, nRF5340 DKs run fw_nrf5340/ (same BLE protocol).
# rotate: degrees clockwise that make the picture upright (stereo.py), checked on real
# pictures with the modules as they sit on the two 5340 DKs (10-07; right board 270, 10-08).
# dk_board: the board's name for fw_nrf5340/tools/dk5340.ps1 -Board (stereo.py --flash).
BOARDS = {
    "hm0360": {"snr": "1050291681", "address": "C8:1A:80:9C:01:BC"},
    "hm01b0": {"snr": "1050221517", "address": "D6:25:79:FD:6A:6B"},
    "hm0360-5340": {"snr": "1050035314", "address": "DA:E7:DE:69:E3:5E",
                    "device": "nRF5340_xxAA_APP", "family": "nrf53", "rotate": 90, "dk_board": "HM0360"},
    "hm0360b-5340": {"snr": "1050017384", "address": "EA:F1:6D:2A:C0:BB",
                    "device": "nRF5340_xxAA_APP", "family": "nrf53", "rotate": 270,  # user 10-08: 90 + 180
                    "dk_board": "HM0360B"},
}
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
CMD_SET_EXPOSURE = 0xB6
FORMAT_RAW, FORMAT_PNG, FORMAT_JPEG = 0, 1, 2
FORMAT_NAMES = {FORMAT_RAW: "raw", FORMAT_PNG: "png", FORMAT_JPEG: "jpeg"}


def set_format_command(fmt: int, quality: int = 0) -> bytes:
    return bytes([CMD_SET_FORMAT, fmt, quality])


def set_exposure_command(integration: Optional[int] = None, analog_gain: int = 0, digital_gain: int = 0) -> bytes:
    """None: automatic exposure; else fixed register values (HM0360)."""
    if integration is None:
        return bytes([CMD_SET_EXPOSURE, 0])
    return bytes([CMD_SET_EXPOSURE, 1]) + struct.pack("<HBH", integration, analog_gain, digital_gain)

FLAG_START_OF_FRAME = 0x01
FLAG_HEADER_V2 = 0x02
FRAME_HEADER = struct.Struct("<HH")  # width, height (v1)
# v2: width, height, format, quality, length, capture_us, frame_id, integration, analog gain,
# digital gain, flags (fw_nrf5340/src/ble.h ble_frame_header_t)
HEADER_V2 = struct.Struct("<HHBBIIHHBHB")
HEADER_COMMON_CLOCK = 0x01
HEADER_MANUAL_EXPOSURE = 0x02
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

    def __init__(self, on_frame: Callable[[Frame], None], on_event: Callable[[str], None] = print):
        self.on_frame = on_frame
        self.on_event = on_event
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
