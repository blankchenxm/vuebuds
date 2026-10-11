"""PC side of the BLE-USB bridge (fw_bridge/): the bridge is the BLE central for up to two
camera boards and forwards their notifications over USB CDC ACM ("nRF USB" port).

Frames (little endian):
  PC -> bridge  [A5 5A][cmd][len u16][payload]
      'I'  identify
      'C'  [interval u16, 1.25 ms units][n][n x (addr type u8, address 6 B LSB first)]
      'W'  [slot mask][bytes]  write to the control characteristic (0xB1 stream, 0xB2 picture)
      'X'  [slot mask]         disconnect (the boards reboot; the bridge reconnects)
  bridge -> PC  [A5 5A][type][slot][len u16][payload]
      'N'  one data notification ([seq, flags, ...], protocol.FrameAssembler)
      'E'  link event (central_event_t in fw_bridge/src/central.h)
      'S'  log text
      'W'  int8 result of a control write

Command line, e.g. stream both boards for 20 s and print per-board rates:
    python bridge.py --boards hm0360-5340 hm0360b-5340 --stream --duration 20
"""
import argparse
import struct
import threading
import time
from dataclasses import dataclass
from typing import Callable, Optional

import serial
from serial.tools import list_ports

import protocol

ZEPHYR_VID = 0x2FE3
SYNC = b"\xA5\x5A"
EVENT = struct.Struct("<BB6sHBBHHHB")
STATES = {0: "disconnected", 1: "connected", 2: "ready", 3: "updated"}
DEFAULT_INTERVAL_MS = 15.0


def find_port() -> str:
    for p in list_ports.comports():
        if p.vid == ZEPHYR_VID:
            return p.device
    raise RuntimeError("bridge USB port not found (cable in the DK's 'nRF USB' connector?)")


def release(port: Optional[str] = None) -> bool:
    """Make the bridge drop all boards (configure with none), so a PC tool can connect to them
    directly; the boards accept one connection only. False if no bridge is plugged in."""
    try:
        port = port or find_port()
    except RuntimeError:
        return False
    with serial.Serial(port, timeout=0.5) as ser:
        ser.dtr = True
        ser.write(SYNC + b"C" + struct.pack("<HBB", 3, 0, 0)[:2] + struct.pack("<HB", 12, 0))
        time.sleep(0.5)
    return True


def address_bytes(address: str) -> bytes:
    return bytes(int(x, 16) for x in reversed(address.split(":")))


@dataclass
class Link:
    address: str = ""
    connected: bool = False
    ready: bool = False
    interval_ms: float = 0.0
    phy: str = ""
    data_length: int = 0
    mtu: int = 0
    packets: int = 0
    ready_at: float = 0.0  # time.monotonic() of the last "ready" event


class Bridge:
    """Serial link to the bridge; a reader thread calls on_notify(slot, data) for every
    notification, on_text(slot, text) for log lines and on_event(slot, link) on link changes."""

    def __init__(self, port: Optional[str] = None,
                 on_notify: Callable[[int, bytes], None] = lambda s, d: None,
                 on_text: Callable[[int, str], None] = lambda s, t: print(f"[bridge {s}] {t}"),
                 on_event: Callable[[int, Link], None] = lambda s, l: None):
        self.port = port or find_port()
        self.on_notify, self.on_text, self.on_event = on_notify, on_text, on_event
        self.links = [Link(), Link()]
        self.write_results: list[tuple[float, int, int]] = []  # (time, slot, err)
        self._ser = serial.Serial(self.port, timeout=0.1)
        self._ser.dtr = True
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._reader, daemon=True)
        self._thread.start()

    def close(self):
        self._stop.set()
        self._thread.join(timeout=2)
        self._ser.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    # ---- commands ----

    def _send(self, cmd: str, payload: bytes = b""):
        self._ser.write(SYNC + cmd.encode() + struct.pack("<H", len(payload)) + payload)

    def identify(self):
        self._send("I")

    def configure(self, addresses: list[str], interval_ms: float = DEFAULT_INTERVAL_MS):
        """Connect slot i to addresses[i] (random static addresses, as the boards use)."""
        payload = struct.pack("<HB", round(interval_ms / 1.25), len(addresses))
        for i, a in enumerate(addresses):
            payload += bytes([1]) + address_bytes(a)
            self.links[i].address = a
        self._send("C", payload)

    def write(self, mask: int, data: bytes):
        self._send("W", bytes([mask]) + data)

    def disconnect(self, mask: int):
        self._send("X", bytes([mask]))

    def wait_ready(self, slots, timeout: float = 30.0, after: float = 0.0) -> bool:
        """All slots ready; with after, only links that became ready after that time count
        (after a board reset the old link looks alive until its 4 s supervision timeout)."""
        t_end = time.monotonic() + timeout
        while time.monotonic() < t_end:
            if all(self.links[s].ready and self.links[s].ready_at > after for s in slots):
                return True
            time.sleep(0.05)
        return False

    # ---- reader ----

    def _reader(self):
        buf = bytearray()
        while not self._stop.is_set():
            chunk = self._ser.read(self._ser.in_waiting or 1)
            if not chunk:
                continue
            buf += chunk
            while True:
                i = buf.find(SYNC)
                if i < 0:
                    del buf[:-1]
                    break
                if i:
                    del buf[:i]
                if len(buf) < 6:
                    break
                ftype, slot, n = chr(buf[2]), buf[3], buf[4] | (buf[5] << 8)
                if len(buf) < 6 + n:
                    break
                payload = bytes(buf[6:6 + n])
                del buf[:6 + n]
                self._dispatch(ftype, slot, payload)

    def _dispatch(self, ftype: str, slot: int, payload: bytes):
        if ftype == "N":
            self.links[slot].packets += 1
            self.on_notify(slot, payload)
        elif ftype == "S":
            self.on_text(slot, payload.decode(errors="replace"))
        elif ftype == "W":
            self.write_results.append((time.monotonic(), slot, struct.unpack("<b", payload)[0]))
        elif ftype == "E" and len(payload) >= EVENT.size:
            state, _, addr, itv, tx_phy, rx_phy, tx_len, rx_len, mtu, reason = EVENT.unpack_from(payload)
            link = self.links[slot]
            link.address = ":".join(f"{b:02X}" for b in reversed(addr))
            link.connected = state != 0
            if state == 0:
                link.ready = False
            elif state == 2:
                link.ready = True
                link.ready_at = time.monotonic()
            if state:
                link.interval_ms = itv * 1.25
                link.phy = f"{tx_phy}M/{rx_phy}M"
                link.data_length = tx_len
                link.mtu = mtu
            self.on_event(slot, link)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port")
    ap.add_argument("--boards", nargs="+", default=["hm0360-5340", "hm0360b-5340"], choices=sorted(protocol.BOARDS))
    ap.add_argument("--interval-ms", type=float, default=DEFAULT_INTERVAL_MS)
    ap.add_argument("--stream", action="store_true", help="start streaming (0xB1) on all boards")
    ap.add_argument("--release", action="store_true", help="only drop all boards (for direct Windows tools)")
    ap.add_argument("--duration", type=float, default=20.0)
    args = ap.parse_args()

    if args.release:
        print("released" if release(args.port) else "no bridge found")
        return 0
    frames = [[] for _ in args.boards]
    asms = [protocol.FrameAssembler(lambda f, i=i: frames[i].append(f), lambda m, i=i: print(f"[{i}] {m}"))
            for i in range(len(args.boards))]
    with Bridge(args.port, on_notify=lambda s, d: asms[s].feed(d),
                on_event=lambda s, l: print(f"[{s}] {l}")) as br:
        br.identify()
        br.configure([protocol.BOARDS[b]["address"] for b in args.boards], args.interval_ms)
        if not br.wait_ready(range(len(args.boards))):
            print("boards not ready:", br.links)
            return 1
        if not args.stream:
            return 0
        br.write((1 << len(args.boards)) - 1, bytes([protocol.CMD_STREAM_START]))
        t0 = time.monotonic()
        time.sleep(args.duration)
        for i, b in enumerate(args.boards):
            fs = frames[i]
            span = fs[-1].completed_at - fs[0].completed_at if len(fs) > 1 else 0
            fps = (len(fs) - 1) / span if span else 0
            kbs = sum(len(f.pixels) for f in fs[1:]) / span / 1024 if span else 0
            print(f"{b}: {len(fs)} frames in {time.monotonic() - t0:.0f} s, {fps:.2f} fps, {kbs:.0f} KB/s, "
                  f"lost packets {asms[i].total_lost}, link {br.links[i]}")
        br.disconnect((1 << len(args.boards)) - 1)  # reboot the boards: no stream left running
        time.sleep(0.2)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
