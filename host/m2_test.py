"""M2 commands on both boards through the bridge (hub #68 item 1; INTERFACES C2 / D;
fw_nrf5340/src/ringcmd.h): ring buffer 0xB8 2 / 0, status 0xB9, fetch 0xBA 0-3, replies R_*.
A reference for the client side (sw-mac): Board / Session below show how to send the commands
and match R_FRAME to the frame after it.

  python m2_test.py --rtt logs/m2/run1                      # tests 1-6, default setup
  python m2_test.py --tests 7 --duration 360 --rtt logs/m2/long_a
  python m2_test.py --tests 7 --duration 360 --no-reset --rtt logs/m2/long_b   # continue the same ring

Default setup (hub #68): QVGA firmware, JPEG q85 (0xB3), 1 s period, compressed when stored.
Tests:
  1  start / stop / new period: every R_STATUS field
  2  the four fetch modes: right frame, R_FRAME number / time / length = frame header, CRC = stored
  3  every R_NONE reason once
  4  fetching nonstop while the ring keeps capturing: skipped capture times
  5  0xB2 while the ring runs (R_FRAME + frame, stored too); both boards on the same multiple of P
  6  time to fetch one frame, and a range of 10
  7  stability: fetch every few seconds for --duration s
  8  0xB8 0 then 0xB1: the stream starts, the ring frames stay (leaves the boards streaming)
  3s R_NONE 5 (not synced): reboots the boards and fetches right after they connect
With --rtt each board's RTT log goes to <prefix>_<L|R>.log; the stored CRCs ("[ring] stored ...
crc X") are checked against the CRC32 of the fetched bytes. Results: <prefix>.json.
"""
import argparse
import json
import re
import statistics
import threading
import time
import zlib

import bridge
import protocol
import devtools  # noqa: E402 (protocol puts ../tools on sys.path)

P = protocol


class Board:
    """Replies and frames of one board, in arrival order; a frame after R_FRAME carries it."""

    def __init__(self, slot, name):
        self.slot, self.name = slot, name
        self.events = []  # (kind, obj): ("reply", Reply) / ("frame", Frame)
        self.cv = threading.Condition()
        self._rframe = None
        self.asm = P.FrameAssembler(self._frame, lambda m: self._log(m), self._reply)
        self.notes = []

    def _log(self, msg):
        self.notes.append(msg)

    def _reply(self, r):
        with self.cv:
            if r.kind == P.R_FRAME:
                self._rframe = r
            self.events.append(("reply", r))
            self.cv.notify_all()

    def _frame(self, f):
        with self.cv:
            f.rframe, self._rframe = self._rframe, None
            self.events.append(("frame", f))
            self.cv.notify_all()

    def mark(self):
        with self.cv:
            return len(self.events)

    def wait(self, since, pred, timeout=10.0):
        """First event at or after index `since` with pred(kind, obj); (index, obj) or (None, None)."""
        t_end = time.monotonic() + timeout
        with self.cv:
            i = since
            while True:
                while i < len(self.events):
                    k, o = self.events[i]
                    if pred(k, o):
                        return i, o
                    i += 1
                left = t_end - time.monotonic()
                if left <= 0:
                    return None, None
                self.cv.wait(left)


def is_reply(kind):
    return lambda k, o: k == "reply" and o.kind == kind


def is_reply_in(kinds):
    return lambda k, o: k == "reply" and o.kind in kinds


def is_frame(k, o):
    return k == "frame"


class Session:
    def __init__(self, br, names):
        self.br = br
        self.boards = [Board(i, n) for i, n in enumerate(names)]
        self.mask = (1 << len(names)) - 1
        br.on_notify = lambda s, d: self.boards[s].asm.feed(d) if s < len(self.boards) else None
        self.clock = None  # (offset_us, rtt_us, n): bridge_us ~= monotonic * 1e6 + offset

    def sync(self):
        c = self.br.bridge_clock(samples=8)
        if c:
            self.clock = c
        return c

    def bridge_now(self):
        return int(time.monotonic() * 1e6 + self.clock[0])

    def send(self, data, mask=None):
        self.br.write(self.mask if mask is None else mask, data)

    def ask(self, data, kinds, timeout=5.0, boards=None):
        """Send to the boards, return each board's first reply of these kinds (None: none)."""
        boards = boards or self.boards
        marks = [b.mark() for b in boards]
        self.send(data, sum(1 << b.slot for b in boards))
        return [b.wait(m, is_reply_in(kinds), timeout)[1] for b, m in zip(boards, marks)]

    def status(self, boards=None):
        return [r.fields if r else None for r in self.ask(P.status_command(), (P.R_STATUS,), boards=boards)]

    def fetch(self, board, data, timeout=15.0):
        """One fetch on one board. Returns (frames, final reply, seconds to the last event):
        frames = [(R_FRAME fields, Frame)], final = R_NONE / R_DONE / None (single frame)."""
        m = board.mark()
        t0 = time.monotonic()
        self.send(data, 1 << board.slot)
        frames = []
        single = data[1] != 2
        i = m
        while True:
            idx, o = board.wait(i, lambda k, o: k == "frame" or (k == "reply" and o.kind in (P.R_NONE, P.R_DONE)),
                                timeout)
            if idx is None:
                return frames, None, time.monotonic() - t0
            i = idx + 1
            if isinstance(o, P.Reply):
                return frames, o, o.received_at - t0
            if getattr(o, "rframe", None) is None:
                continue  # a frame not from this fetch (should not happen)
            frames.append((o.rframe.fields, o))
            if single:
                return frames, None, o.completed_at - t0


def check_frame(rf, f, crcs=None):
    """R_FRAME vs the frame after it; problems as a list of strings."""
    bad = []
    if rf["seq"] & 0xFFFF != f.frame_id:
        bad.append(f"frame_id {f.frame_id} != number {rf['seq']}")
    if rf["capture_us"] & 0xFFFFFFFF != f.capture_us:
        bad.append("capture_us != R_FRAME time")
    if rf["length"] != len(f.data):
        bad.append(f"length {len(f.data)} != {rf['length']}")
    want = P.HEADER_FROM_RING | P.HEADER_COMMON_CLOCK
    if f.header_flags & want != want:
        bad.append(f"flags 0x{f.header_flags:02x}")
    if f.lost_packets:
        bad.append(f"{f.lost_packets} packets lost")
    f.crc = zlib.crc32(f.data) & 0xFFFFFFFF
    return bad


RESULT = {}


def say(*a):
    print(*a, flush=True)


def fmt_status(s):
    if s is None:
        return "no R_STATUS"
    return (f"state {s['state']} period {s['period']} fmt {s['format']}/q{s['quality']} frames {s['frames']} "
            f"#{s['oldest_seq']}..#{s['newest_seq']} t {s['oldest_us'] / 1e6:.3f}..{s['newest_us'] / 1e6:.3f} "
            f"now {s['now_us'] / 1e6:.3f} {s['used_kb']}/{s['total_kb']} KB skipped {s['skipped']} "
            f"overwritten {s['overwritten']} psram {s['psram']} synced {s['synced']} camera {s.get('camera')}")


def phase_of(t_us, period_ms):
    return t_us % (period_ms * 1000) / 1000  # ms after the multiple of P


# ---------------- tests ----------------

def test1(ss, period):
    say("== 1 start / stop / new period")
    rows = []
    ok = True

    def row(what, sts, expect):
        nonlocal ok
        for b, s in zip(ss.boards, sts):
            problems = [k for k, v in expect.items() if s is None or not v(s)]
            ok &= not problems
            rows.append({"step": what, "board": b.name, "status": s, "problems": problems})
            say(f"  {what:24s} {b.name:13s} {fmt_status(s)} {'OK' if not problems else 'BAD ' + ','.join(problems)}")

    common = {"synced": lambda s: s["synced"] == 1, "psram": lambda s: s["psram"] == 0,
              "total_kb": lambda s: s["total_kb"] == 7680, "format": lambda s: s["format"] == 2 and s["quality"] == 85,
              # hub #92: 1 present; 0 = not checked yet is fine on a DK before its first picture
              "camera": lambda s: s.get("camera") in (0, 1)}
    ss.sync()
    # Within 2 s of boot the reply waits for the PSRAM check.
    row("0xB9 after boot", ss.status(), {**common, "state": lambda s: s["state"] == 0, "frames": lambda s: s["frames"] == 0})
    sts = [r.fields if r else None for r in ss.ask(P.ring_command(period), (P.R_STATUS,))]
    row(f"0xB8 2 {period}", sts, {**common, "state": lambda s: s["state"] == 2, "period": lambda s: s["period"] == period})
    time.sleep(6.5)
    now = ss.bridge_now()
    sts = ss.status()
    row("0xB9 after 6.5 s", sts, {
        **common, "frames": lambda s: 4 <= s["frames"] <= 7, "camera_present": lambda s: s.get("camera") == 1,
        "spacing": lambda s: abs((s["newest_us"] - s["oldest_us"]) - (s["frames"] - 1) * period * 1000) < period * 500,
        "phase": lambda s: phase_of(s["newest_us"], period) < 300,
        "now": lambda s: abs(s["now_us"] - now) < 100_000,
        "fresh": lambda s: 0 < s["now_us"] - s["newest_us"] < period * 1000 + 400_000,
        "numbers": lambda s: s["newest_seq"] - s["oldest_seq"] + 1 == s["frames"],
        "used": lambda s: 0 < s["used_kb"] < s["frames"] * 40,
        "skipped": lambda s: s["skipped"] == 0})
    if all(sts):
        say(f"  same multiple of P: newest L {sts[0]['newest_us'] // (period * 1000)} "
            f"R {sts[-1]['newest_us'] // (period * 1000)}, phases "
            + ", ".join(f"{phase_of(s['newest_us'], period):.1f} ms" for s in sts))
    p2 = period // 2
    sts = [r.fields if r else None for r in ss.ask(P.ring_command(p2), (P.R_STATUS,))]
    row(f"0xB8 2 {p2} (new period)", sts, {**common, "state": lambda s: s["state"] == 2, "period": lambda s: s["period"] == p2,
                                          "kept": lambda s: s["frames"] >= 4})
    before = sts
    time.sleep(3.2)
    sts = ss.status()
    row(f"0xB9 after 3.2 s", sts, {
        **common, "more": lambda s: s["frames"] >= 9,
        "phase": lambda s: phase_of(s["newest_us"], p2) < 300})
    for b, s0 in zip(ss.boards, before):
        if s0:
            frames, final, _ = ss.fetch(b, P.fetch_range_command(s0["newest_us"] + 1, ss.bridge_now(), 32))
            gaps = [(b2[0]["capture_us"] - a2[0]["capture_us"]) / 1000 for a2, b2 in zip(frames, frames[1:])]
            say(f"  {b.name}: frames after the period change, gaps {[round(g) for g in gaps]} ms")
            ok &= bool(gaps) and all(abs(g - p2) < 20 for g in gaps)
    sts = [r.fields if r else None for r in ss.ask(P.ring_command(0), (P.R_STATUS,))]
    row("0xB8 0", sts, {**common, "state": lambda s: s["state"] == 0})
    stopped = sts
    time.sleep(2.0)
    sts = ss.status()
    row("0xB9 2 s after stop", sts, {**common, "state": lambda s: s["state"] == 0,
                                       "unchanged": lambda s: True})
    for s0, s1 in zip(stopped, sts):
        if s0 and s1 and s1["newest_seq"] != s0["newest_seq"]:
            ok = False
            say("  BAD: frames stored after 0xB8 0")
    sts = [r.fields if r else None for r in ss.ask(P.ring_command(period), (P.R_STATUS,))]
    row(f"0xB8 2 {period} (restart)", sts, {**common, "state": lambda s: s["state"] == 2, "emptied": lambda s: s["frames"] == 0})
    RESULT["test1"] = {"ok": ok, "rows": rows}
    return ok


def collect_ring(ss, b):
    """All frames in the ring (range fetch, 32 at a time)."""
    s = ss.status([b])[0]
    out = []
    t1 = s["oldest_us"]
    while True:
        frames, final, _ = ss.fetch(b, P.fetch_range_command(t1, s["newest_us"], 32))
        out += frames
        if len(frames) < 32:
            return out, s
        t1 = frames[-1][0]["capture_us"] + 1


def test2(ss, period):
    say("== 2 fetch modes (ring running; waiting until ~12 frames)")
    time.sleep(max(0.0, 12 * period / 1000 - 1))
    rows = []
    ok = True
    for b in ss.boards:
        allf, s = collect_ring(ss, b)
        seqs = [rf["seq"] for rf, _ in allf]
        for rf, f in allf:
            bad = check_frame(rf, f)
            ok &= not bad
            rows.append({"board": b.name, "mode": "2 all", "seq": rf["seq"], "t": rf["capture_us"], "len": rf["length"],
                         "crc": f.crc, "problems": bad})
        contiguous = seqs == list(range(s["oldest_seq"], s["newest_seq"] + 1))
        ok &= contiguous
        say(f"  {b.name}: range [oldest, newest] -> {len(allf)} frames #{seqs[0] if seqs else '-'}..#{seqs[-1] if seqs else '-'}"
            f" ({'= status' if contiguous else 'NOT = status ' + str(s['oldest_seq']) + '..' + str(s['newest_seq'])})")
        if len(allf) < 4:
            ok = False
            continue
        k = len(allf) // 2
        a, nxt = allf[k][0], allf[k + 1][0]
        gap = nxt["capture_us"] - a["capture_us"]
        cases = [
            ("0 nearest, t = t_k + 0.4 gap", P.fetch_nearest_command(a["capture_us"] + int(0.4 * gap)), a["seq"]),
            ("0 nearest, t = t_k + 0.6 gap", P.fetch_nearest_command(a["capture_us"] + int(0.6 * gap)), nxt["seq"]),
            ("1 at/before, t = t_k", P.fetch_before_command(a["capture_us"]), a["seq"]),
            ("1 at/before, t = t_k + 0.9 gap", P.fetch_before_command(a["capture_us"] + int(0.9 * gap)), a["seq"]),
            ("1 at/before, t = t_k - 1 us", P.fetch_before_command(a["capture_us"] - 1), allf[k - 1][0]["seq"]),
            ("3 number k", P.fetch_seq_command(a["seq"]), a["seq"]),
            ("2 range [t_k, t_k+2] n 32", P.fetch_range_command(a["capture_us"], allf[k + 2][0]["capture_us"], 32),
             [a["seq"], nxt["seq"], allf[k + 2][0]["seq"]]),
            ("2 range all, n 2", P.fetch_range_command(allf[0][0]["capture_us"], allf[-1][0]["capture_us"], 2),
             [allf[0][0]["seq"], allf[1][0]["seq"]]),
        ]
        for what, cmd, want in cases:
            frames, final, dt = ss.fetch(b, cmd)
            got = [rf["seq"] for rf, _ in frames]
            problems = sum((check_frame(rf, f) for rf, f in frames), [])
            if isinstance(want, list):
                right = got == want and final is not None and final.kind == P.R_DONE and final.fields["count"] == len(want)
            else:
                right = got == [want] and final is None
            ok &= right and not problems
            rows.append({"board": b.name, "mode": what, "want": want, "got": got, "problems": problems,
                         "final": final.fields if final else None, "s": round(dt, 3),
                         "crc": [f.crc for _, f in frames], "seqs": got})
            say(f"  {b.name}: {what:32s} want {want} got {got} {'OK' if right and not problems else 'BAD ' + str(problems)}")
    RESULT["test2"] = {"ok": ok, "rows": rows}
    return ok


def test3(ss, period):
    say("== 3 every R_NONE reason")
    rows = []
    ok = True

    def expect(what, b, cmd, reason, pre=None):
        nonlocal ok
        m = b.mark()
        if pre:
            pre()
        ss.send(cmd, 1 << b.slot)
        _, r = b.wait(m, is_reply(P.R_NONE), 8.0)
        got = r.fields["reason"] if r else None
        right = got == reason
        ok &= right
        rows.append({"board": b.name, "case": what, "want": reason, "got": got})
        say(f"  {b.name}: {what:42s} want {reason} ({P.R_NONE_REASONS[reason]}) got {got} {'OK' if right else 'BAD'}")

    for b in ss.boards:
        s = ss.status([b])[0]
        now = ss.bridge_now()
        expect("0: at/before, t = oldest - 10 s", b, P.fetch_before_command(s["oldest_us"] - 10_000_000), 0)
        expect("0: number before the oldest", b, P.fetch_seq_command(max(0, s["oldest_seq"] - 1)), 0) \
            if s["oldest_seq"] > 1 else None
        expect("1: at/before, t = now + 10 s", b, P.fetch_before_command(now + 10_000_000), 1)
        expect("1: number after the newest", b, P.fetch_seq_command(s["newest_seq"] + 100), 1)
        expect("4: range with n = 0", b, P.fetch_range_command(s["oldest_us"], s["newest_us"], 0), 4)
        expect("4: range with t1 > t2", b, P.fetch_range_command(s["newest_us"], s["oldest_us"], 5), 4)
        expect("4: unknown mode 7", b, bytes([P.CMD_FETCH, 7, 0, 0, 0, 0]), 4)
        expect("3: second fetch while a range goes out", b, P.fetch_seq_command(s["newest_seq"]), 3,
               pre=lambda: ss.send(P.fetch_range_command(s["oldest_us"], s["newest_us"], 10), 1 << b.slot))
        time.sleep(3.0)  # let the range go out
    # 2: empty right after a new start (the first picture is up to one period away)
    for attempt in range(3):
        ss.ask(P.ring_command(0), (P.R_STATUS,))
        marks = [b.mark() for b in ss.boards]
        ss.send(P.ring_command(period))
        ss.send(P.fetch_seq_command(1))
        got = [b.wait(m, is_reply(P.R_NONE), 5.0)[1] for b, m in zip(ss.boards, marks)]
        reasons = [g.fields["reason"] if g else None for g in got]
        if all(r == 2 for r in reasons) or attempt == 2:
            break
        time.sleep(0.6)
    for b, r in zip(ss.boards, reasons):
        right = r == 2
        ok &= right
        rows.append({"board": b.name, "case": "2: fetch right after 0xB8 2 (new start)", "want": 2, "got": r})
        say(f"  {b.name}: {'2: fetch right after 0xB8 2 (new start)':42s} want 2 (ring empty) got {r} {'OK' if right else 'BAD'}")
    RESULT["test3"] = {"ok": ok, "rows": rows}
    return ok


def test3_not_synced(ss):
    """5: right after the board reboots, before the bridge's first reference (0.2 s after ready)."""
    rows = []
    ok = True
    t_x = time.monotonic()
    ss.br.disconnect(ss.mask)
    time.sleep(0.3)
    marks = [b.mark() for b in ss.boards]
    waiting = set(range(len(ss.boards)))
    t_end = time.monotonic() + 30
    while waiting and time.monotonic() < t_end:
        for i in list(waiting):
            link = ss.br.links[i]
            if link.ready and link.ready_at > t_x:
                ss.send(P.fetch_seq_command(1), 1 << i)
                waiting.discard(i)
        time.sleep(0.005)
    for b, m in zip(ss.boards, marks):
        _, r = b.wait(m, is_reply(P.R_NONE), 5.0)
        got = r.fields["reason"] if r else None
        right = got == 5
        ok &= right
        rows.append({"board": b.name, "case": "5: fetch right after connect", "want": 5, "got": got})
        say(f"  {b.name}: {'5: fetch right after connect':42s} want 5 (not synced) got {got} {'OK' if right else 'BAD'}")
    RESULT["test3_not_synced"] = {"ok": ok, "rows": rows}
    return ok


def test4(ss, period, seconds=60):
    say(f"== 4 fetch nonstop for {seconds} s while the ring captures (every fetch: the newest frame)")
    s0 = ss.status()
    t_end = time.monotonic() + seconds
    counts = [0] * len(ss.boards)
    errors = [0] * len(ss.boards)
    times = [[] for _ in ss.boards]

    def run(b):
        while time.monotonic() < t_end:
            frames, final, dt = ss.fetch(b, P.fetch_before_command(ss.bridge_now()))
            if frames and not check_frame(*frames[0]):
                counts[b.slot] += 1
                times[b.slot].append(dt)
            else:
                errors[b.slot] += 1
                time.sleep(0.05)
    ths = [threading.Thread(target=run, args=(b,)) for b in ss.boards]
    for t in ths:
        t.start()
    for t in ths:
        t.join()
    s1 = ss.status()
    ok = True
    rows = []
    for b, a, z in zip(ss.boards, s0, s1):
        stored = z["newest_seq"] - a["newest_seq"]
        expect = seconds * 1000 / period
        skipped = z["skipped"] - a["skipped"]
        good = skipped == 0 and stored >= expect - 2
        ok &= good
        rows.append({"board": b.name, "fetched": counts[b.slot], "fetch_errors": errors[b.slot], "stored": stored,
                     "expected": expect, "skipped": skipped,
                     "fetch_s_median": round(statistics.median(times[b.slot]), 3) if times[b.slot] else None})
        say(f"  {b.name}: fetched {counts[b.slot]} frames ({errors[b.slot]} errors), stored {stored} "
            f"(expected ~{expect:.0f}), skipped {skipped} {'OK' if good else 'BAD'}")
    RESULT["test4"] = {"ok": ok, "rows": rows}
    return ok


def test5(ss, period):
    say("== 5 0xB2 while the ring runs; both boards on the same multiple of P")
    ok = True
    rows = []
    s0 = ss.status()
    marks = [b.mark() for b in ss.boards]
    t_req = ss.bridge_now()
    ss.send(bytes([P.CMD_SNAPSHOT]))
    for b, m, a in zip(ss.boards, marks, s0):
        _, f = b.wait(m, is_frame, 6.0)
        rf = f.rframe.fields if f is not None and f.rframe else None
        bad = check_frame(rf, f) if rf else ["no R_FRAME before the frame" if f else "no frame"]
        lat = (rf["capture_us"] - t_req) / 1000 if rf else None
        ok &= not bad
        rows.append({"board": b.name, "seq": rf["seq"] if rf else None, "capture_minus_request_ms": lat, "problems": bad})
        say(f"  {b.name}: 0xB2 -> R_FRAME #{rf['seq'] if rf else '-'} + frame, capture {lat if lat is None else round(lat)} ms "
            f"after the request {'OK' if not bad else 'BAD ' + str(bad)}")
    time.sleep(1.0)
    s1 = ss.status()
    for b, a, z, r in zip(ss.boards, s0, s1, rows):
        stored = r["seq"] is not None and a["newest_seq"] < r["seq"] <= z["newest_seq"]
        ok &= stored
        say(f"  {b.name}: stored in the ring too: {'yes' if stored else 'NO'}")
    # Pair the ring frames of the last ~20 s by multiple of P.
    time.sleep(max(0.0, 8 * period / 1000))
    per = []
    for b in ss.boards:
        s = ss.status([b])[0]
        frames, _, _ = ss.fetch(b, P.fetch_range_command(s["newest_us"] - 20_000_000, s["newest_us"], 32))
        per.append({rf["capture_us"] // (period * 1000): rf["capture_us"] % (period * 1000) / 1000 for rf, _ in frames})
    pairs = []
    if len(per) == 2:
        for k in sorted(set(per[0]) & set(per[1])):
            pairs.append((k, per[0][k], per[1][k], per[0][k] - per[1][k]))
        say(f"  {len(pairs)} pairs on the same multiple of P (k = bridge time // P)")
        for k, l, r, d in pairs[-8:]:
            say(f"    k {k}: L {l:7.2f} ms after kP, R {r:7.2f} ms, L - R {d:+7.2f} ms")
        only = [len(set(per[0]) - set(per[1])), len(set(per[1]) - set(per[0]))]
        say(f"  multiples only on L / R: {only}")
        ok &= len(pairs) >= 5
    RESULT["test5"] = {"ok": ok, "rows": rows, "pairs": pairs}
    return ok


def test6(ss, period, n=10):
    say(f"== 6 fetch time through the bridge (one frame x {n}; a range of 10 x 3)")
    rows = []
    ok = True
    for b in ss.boards:
        one = []
        for _ in range(n):
            s = ss.status([b])[0]
            frames, final, dt = ss.fetch(b, P.fetch_before_command(s["newest_us"]))
            if frames and not check_frame(*frames[0]):
                one.append((dt, len(frames[0][1].data)))
        rng = []
        for _ in range(3):
            s = ss.status([b])[0]
            frames, final, dt = ss.fetch(b, P.fetch_range_command(s["newest_us"] - 9500 * period, s["newest_us"], 10),
                                         timeout=30)
            if final is not None and final.kind == P.R_DONE:
                rng.append((dt, len(frames), sum(len(f.data) for _, f in frames)))
        ok &= len(one) == n and len(rng) == 3
        r = {"board": b.name,
             "one_s_median": round(statistics.median(t for t, _ in one), 3) if one else None,
             "one_s_max": round(max(t for t, _ in one), 3) if one else None,
             "one_bytes_mean": round(statistics.mean(x for _, x in one)) if one else None,
             "range10_s": [round(t, 3) for t, _, _ in rng], "range10_frames": [k for _, k, _ in rng],
             "range10_bytes": [x for _, _, x in rng]}
        rows.append(r)
        say(f"  {b.name}: one frame median {r['one_s_median']} s (max {r['one_s_max']}, {r['one_bytes_mean']} B); "
            f"range of 10: {r['range10_s']} s, frames {r['range10_frames']}")
    RESULT["test6"] = {"ok": ok, "rows": rows}
    return ok


def test7(ss, period, duration, every):
    say(f"== 7 stability: {duration:.0f} s, both boards fetch the frame nearest to now - 2 s every {every} s")
    t0 = time.monotonic()
    stats = [{"fetches": 0, "ok": 0, "none": {}, "bad": 0, "lost": 0, "times": [], "seqs": [], "crc": []}
             for _ in ss.boards]
    statuses = []
    next_status = t0
    while time.monotonic() - t0 < duration:
        tick = time.monotonic()
        ss.sync()
        for b in ss.boards:
            st = stats[b.slot]
            st["fetches"] += 1
            frames, final, dt = ss.fetch(b, P.fetch_nearest_command(ss.bridge_now() - 2_000_000))
            if frames:
                bad = check_frame(*frames[0])
                if bad:
                    st["bad"] += 1
                    say(f"  {b.name}: {bad}")
                else:
                    st["ok"] += 1
                    st["times"].append(dt)
                    st["seqs"].append(frames[0][0]["seq"])
                    st["crc"].append(frames[0][1].crc)
            elif final is not None and final.kind == P.R_NONE:
                st["none"][final.fields["reason"]] = st["none"].get(final.fields["reason"], 0) + 1
            else:
                st["lost"] += 1
        if time.monotonic() >= next_status:
            sts = ss.status()
            statuses.append((round(time.monotonic() - t0), sts))
            say(f"  t {time.monotonic() - t0:5.0f} s: " + " | ".join(
                f"{b.name} {s['frames'] if s else '-'} frames, skipped {s['skipped'] if s else '-'}, "
                f"overwritten {s['overwritten'] if s else '-'}" for b, s in zip(ss.boards, sts)))
            next_status += 60
        time.sleep(max(0.0, every - (time.monotonic() - tick)))
    sts = ss.status()
    statuses.append((round(time.monotonic() - t0), sts))
    ok = True
    rows = []
    for b, st, s in zip(ss.boards, stats, sts):
        good = st["ok"] == st["fetches"] and s is not None and s["state"] == 2
        ok &= good
        rows.append({"board": b.name, **{k: v for k, v in st.items() if k not in ("times", "ok_seqs")},
                     "fetch_s_median": round(statistics.median(st["times"]), 3) if st["times"] else None,
                     "final_status": s})
        say(f"  {b.name}: {st['ok']}/{st['fetches']} fetched ok, R_NONE {st['none']}, bad {st['bad']}, no answer {st['lost']}; "
            f"{fmt_status(s)} {'OK' if good else 'BAD'}")
    RESULT["test7"] = {"ok": ok, "rows": rows, "statuses": statuses}
    return ok


def test8(ss):
    """0xB1 after 0xB8 0 (group switch, INTERFACES C2): the stream starts, the ring's frames stay."""
    say("== 8 0xB8 0, then 0xB1: the stream starts; the ring frames can still be fetched")
    ok = True
    rows = []
    time.sleep(4.0)  # a few frames in the ring first
    st = [r.fields if r else None for r in ss.ask(P.ring_command(0), (P.R_STATUS,))]
    time.sleep(0.5)
    marks = [b.mark() for b in ss.boards]
    ss.send(bytes([P.CMD_STREAM_START]))
    time.sleep(8.0)
    for b, m, s in zip(ss.boards, marks, st):
        with b.cv:
            fr = [o for k, o in b.events[m:] if k == "frame"]
        stream = [f for f in fr if not f.header_flags & P.HEADER_FROM_RING]
        frames, final, _ = ss.fetch(b, P.fetch_seq_command(s["newest_seq"])) if s and s["frames"] else ([], None, 0)
        good = len(stream) >= 3 and len(frames) == 1
        ok &= good
        rows.append({"board": b.name, "stream_frames_8s": len(stream), "ring_fetch_after": len(frames)})
        say(f"  {b.name}: {len(stream)} stream frames in 8 s, fetch #{s['newest_seq'] if s else '-'} from the ring after: "
            f"{'ok' if frames else 'NONE'} {'OK' if good else 'BAD'}")
    RESULT["test8"] = {"ok": ok, "rows": rows}
    return ok


# ---------------- CRC check against the RTT logs ----------------

STORED = re.compile(r"\[ring\] stored (\d+) t \S+ ms: \S+ (\d+) B crc ([0-9a-f]{8})")


def crc_check(prefix, names):
    out = {}
    for i, name in enumerate(names):
        path = f"{prefix}_{'LR'[i]}.log"
        try:
            text = open(path, errors="replace").read()
        except OSError:
            continue
        # The ring's numbers restart at each boot: keep the lines after the last boot.
        text = text.rsplit("[mustard] booting", 1)[-1]
        stored = {int(m[1]): (int(m[2]), int(m[3], 16)) for m in STORED.finditer(text)}
        out[name] = stored
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--boards", nargs="+", default=["hm0360-5340", "hm0360b-5340"], choices=sorted(P.BOARDS))
    ap.add_argument("--tests", default="1,2,3,4,5,6", help="comma-separated: 1-8, 3s (R_NONE 5: reboots the boards); 8 leaves the boards streaming")
    ap.add_argument("--period-ms", type=int, default=1000)
    ap.add_argument("--quality", type=int, default=85, help="JPEG quality (0: raw)")
    ap.add_argument("--duration", type=float, default=600.0, help="test 7: seconds")
    ap.add_argument("--every", type=float, default=5.0, help="test 7: seconds between fetches")
    ap.add_argument("--test4-seconds", type=float, default=60.0)
    ap.add_argument("--no-reset", action="store_true", help="keep the boards running (test 7 continuing a ring)")
    ap.add_argument("--rtt", help="RTT of each board into <prefix>_<L|R>.log; results into <prefix>.json")
    ap.add_argument("--port")
    args = ap.parse_args()
    tests = args.tests.split(",")

    loggers = []
    if args.rtt:
        for i, b in enumerate(args.boards):
            brd = P.BOARDS[b]
            loggers.append(devtools.RttLogger(brd["snr"], brd.get("device", P.DEFAULT_DEVICE), f"{args.rtt}_{'LR'[i]}.log"))
    results = {}
    with bridge.Bridge(args.port, on_text=lambda s, t: None, on_event=lambda s, l: None) as br:
        ss = Session(br, args.boards)
        br.configure([P.BOARDS[b]["address"] for b in args.boards])
        n = len(args.boards)
        t_x = 0.0 if args.no_reset else time.monotonic()
        if not args.no_reset:
            br.disconnect(ss.mask)
            time.sleep(0.3)
        if not br.wait_ready(range(n), timeout=30, after=t_x):
            raise SystemExit(f"boards not ready: {br.links}")
        time.sleep(1.0)  # first references (0xB5, 200 ms after ready)
        if not ss.sync():
            raise SystemExit("no bridge clock")
        fmt = (P.FORMAT_JPEG, args.quality) if args.quality else (P.FORMAT_RAW, 0)
        if not args.no_reset:
            ss.send(P.set_format_command(*fmt))
            time.sleep(0.2)
        if "1" in tests:
            results["1"] = test1(ss, args.period_ms)  # leaves the ring running (restarted, empty)
        else:
            sts = ss.status()
            if any(s is None or s["state"] != 2 for s in sts):
                ss.ask(P.ring_command(args.period_ms), (P.R_STATUS,))
        if "2" in tests:
            results["2"] = test2(ss, args.period_ms)
        if "3" in tests:
            time.sleep(3)
            results["3"] = test3(ss, args.period_ms)
        if "4" in tests:
            time.sleep(3)
            results["4"] = test4(ss, args.period_ms, args.test4_seconds)
        if "5" in tests:
            results["5"] = test5(ss, args.period_ms)
        if "6" in tests:
            time.sleep(10 * args.period_ms / 1000)
            results["6"] = test6(ss, args.period_ms)
        if "7" in tests:
            time.sleep(3 * args.period_ms / 1000)
            results["7"] = test7(ss, args.period_ms, args.duration, args.every)
        if "8" in tests:
            results["8"] = test8(ss)
        if "3s" in tests:
            say("== 3 (cont.) R_NONE 5: reboots the boards")
            results["3s"] = test3_not_synced(ss)
        notes = {b.name: b.notes[-20:] for b in ss.boards}
    for lg in loggers:
        lg.stop()

    if args.rtt:
        stored = crc_check(args.rtt, args.boards)
        checked = {b: [0, 0, 0] for b in args.boards}  # match, mismatch, unknown
        for key in ("test2", "test7"):
            for row in RESULT.get(key, {}).get("rows", []):
                st = stored.get(row["board"], {})
                for seq, crc in zip(row.get("seqs", [row.get("seq")]), row.get("crc", []) if isinstance(row.get("crc"), list)
                                    else [row.get("crc")]):
                    if seq is None or crc is None:
                        continue
                    if seq not in st:
                        checked[row["board"]][2] += 1
                    elif st[seq][1] == crc:
                        checked[row["board"]][0] += 1
                    else:
                        checked[row["board"]][1] += 1
        say("CRC of fetched frames vs stored (RTT): " + ", ".join(f"{b} {m} match / {x} mismatch / {u} not in log"
                                                                  for b, (m, x, u) in checked.items()))
        RESULT["crc"] = checked
        RESULT["notes"] = notes
        with open(f"{args.rtt}.json", "w") as fh:
            json.dump(RESULT, fh, indent=1, default=str)
    say("RESULT: " + ", ".join(f"test {k} {'OK' if v else 'BAD'}" for k, v in results.items()))


if __name__ == "__main__":
    main()
