"""BLE protocol of the vuebuds "mustard" firmware (see fw/ble_cus.c, fw/ble_manager.c).

control char 0x1403: write 0xB1 to start the camera stream
data    char 0x1402: notifications of [seq, flags, payload...]
                     flags bit0 = start of frame; a start-of-frame payload begins with
                     width and height (uint16 little endian each), followed by pixels
"""
import struct
import time
from dataclasses import dataclass
from typing import Callable, Optional

DEVICE_NAME = "mustard"
# All DKs advertise as "mustard"; pick one by J-Link serial number and BLE address.
# nRF52840 DKs run fw/, nRF5340 DKs run fw_nrf5340/ (same BLE protocol).
BOARDS = {
    "hm0360": {"snr": "1050291681", "address": "C8:1A:80:9C:01:BC"},
    "hm01b0": {"snr": "1050221517", "address": "D6:25:79:FD:6A:6B"},
    "hm0360-5340": {"snr": "1050035314", "address": "DA:E7:DE:69:E3:5E",
                    "device": "nRF5340_xxAA_APP", "family": "nrf53"},
    "hm01b0-5340": {"snr": "1050017384", "address": "EA:F1:6D:2A:C0:BB",
                    "device": "nRF5340_xxAA_APP", "family": "nrf53"},
}
# J-Link device / nrfjprog family of the nRF52840 boards (entries without "device").
DEFAULT_DEVICE = "NRF52840_XXAA"
DEFAULT_FAMILY = "nrf52"
# As advertised by the device; the comment in fw/ble_cus.h lists these bytes in a different order.
_BASE = "47ea{:04x}-a0e4-554e-5282-0afcd3246970"
DATA_UUID = _BASE.format(0x1402)
CONTROL_UUID = _BASE.format(0x1403)
CMD_STREAM_START = 0xB1

FLAG_START_OF_FRAME = 0x01
FRAME_HEADER = struct.Struct("<HH")  # width, height
# Placeholder canvas size before the first frame header arrives.
DEFAULT_SIZE = (320, 240)


@dataclass
class Frame:
    index: int
    width: int
    height: int
    pixels: bytes
    packets: int
    lost_packets: int
    duration_s: float
    completed_at: float


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
            if len(payload) < FRAME_HEADER.size:
                self.on_event(f"start-of-frame packet too short: {len(payload)} bytes")
                return
            self._size = FRAME_HEADER.unpack_from(payload)
            payload = payload[FRAME_HEADER.size:]
            self._t_start = now
        if self._size is None:
            return

        self._packets += 1
        self._buf += payload
        w, h = self._size
        if len(self._buf) >= w * h:
            self.frames += 1
            self.on_frame(Frame(
                index=self.frames, width=w, height=h, pixels=bytes(self._buf[: w * h]),
                packets=self._packets, lost_packets=self._lost,
                duration_s=now - self._t_start, completed_at=now,
            ))
            self._reset_frame()

    def partial_bytes(self) -> int:
        return len(self._buf)
