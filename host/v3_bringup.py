"""V3 board (balsamic3) bring-up, hub #83 stage 2: one subcommand per gate, run in order. Each
step prints its evidence and stops; nothing goes on to the next gate by itself. Results are
appended to logs/v3/summary.json (b4 builds the DK vs V3 table from it).

Setup (Xiaomeng): two V3 boards, each on its own external J-Link (debug FPC), powered by her;
camera modules NOT plugged in for B0 / B1; both DKs unplugged; the bridge DK stays.
Power: 5 V only on TP24 (VBUS_USB); battery pads BT1 only 3.0-4.2 V.
STOP and report on any power anomaly (current, a rail, heat): no blind retries.

  python v3_bringup.py b0 [--left SNR --right SNR]   # J-Links -> left / right, SWD reads the chip
  python v3_bringup.py b1                            # flash main (V3 image), boot log: PMIC, rails, PSRAM boot check,
                                                     #   "no camera", BLE address
  python v3_bringup.py b1-psram [--side L]           # HW_TEST PSRAM only: one line, 8 MB x 2, then four lines
  python v3_bringup.py b1-ble                        # main again; bridge connects both, R_STATUS, no-camera path
  -> post "cameras can be attached" on hub #83, wait for Xiaomeng (power off, plug, power on)
  python v3_bringup.py b2                            # camera ID + silicon revision, 0xB2 JPEG through the bridge,
                                                     #   wake -> frame (ae_check.py)
  python v3_bringup.py b3                            # m2_test.py tests 1-8 on V3
  python v3_bringup.py b4                            # DK vs V3 table (markdown)

Meter points for B1 (she measures, the firmware cannot): TP25 +1V8, TP26 +2V8 (firmware sets
2.80 V), TP27 +1V2, TP28 VSBB0, GND TP29.
"""
import argparse
import json
import pathlib
import re
import statistics
import subprocess
import sys
import time

import protocol as P
import devtools  # noqa: E402 (../tools, path set by protocol)

HERE = pathlib.Path(__file__).resolve().parent
LOGS = HERE / "logs" / "v3"
SUMMARY = LOGS / "summary.json"
DK5340 = devtools.REPO / "fw_nrf5340" / "tools" / "dk5340.py"
SIDES = {"L": "V3L", "R": "V3R"}
HOST_NAME = {"L": "v3-left", "R": "v3-right"}
KNOWN_PROBES = {"1050035314", "1050017384", "1050221517", "1050291681"}  # the two 5340 DKs, the bridge, the 52840 DK
DEVICE = "nRF5340_xxAA_APP"

# DK reference values for b4, with where they come from (DEVLOG). None: not measured that way on the DK.
DK_REF = {
    "psram_boot": ("ok both", "PR #60, 11-20"),
    "psram_single_kbs": ("~2700 (48 MHz, 32 B bursts)", "PR #60"),
    "psram_8mb": ("0 errors x 2, both boards", "PR #60 HW_TEST"),
    "psram_quad": ("fails on jumper wires (both)", "PR #60 [5d] / [5e]"),
    "snapshot_s": ("~1.5 s per picture (stereo.py --bridge)", "PR #56 era, 11-19"),
    "ble_kbs_two": ("110-118 KB/s together (bridge limit)", "PR #40 / 11-19"),
    "wake_to_fvld_ms": ("L 11-24 (follows exposure), R 120.7 fixed", "PR #62 test 5 / PR #63 AE check"),
    "silicon_rev": ("L 0x01, R 0x03", "PR #63"),
    "bad_frames": ("HM0360B VGA: occasional OVERFLOW BAD; QVGA 0", "CLAUDE.md"),
}


def local():
    return json.loads(P.V3_LOCAL.read_text("utf-8")) if P.V3_LOCAL.exists() else {}


def save_local(d):
    P.V3_LOCAL.write_text(json.dumps(d, indent=1), "utf-8")


def record(step, data):
    LOGS.mkdir(parents=True, exist_ok=True)
    s = json.loads(SUMMARY.read_text("utf-8")) if SUMMARY.exists() else {}
    s.setdefault(step, {}).update({"time": time.strftime("%Y-%m-%d %H:%M:%S"), **data})  # --side L, then R: keep both
    SUMMARY.write_text(json.dumps(s, indent=1, ensure_ascii=False), "utf-8")


def snr(side):
    s = local().get(SIDES[side], {}).get("snr")
    if not s:
        raise SystemExit(f"{SIDES[side]}: no J-Link serial number yet: run `python v3_bringup.py b0` first")
    devtools.SWD_KHZ[str(s)] = 1000
    return str(s)


def sides(a):
    return [a.side] if getattr(a, "side", None) else ["L", "R"]


def dk(side, command, *extra, seconds=None):
    """fw_nrf5340/tools/dk5340.py for this V3 board; returns its output (also printed)."""
    cmd = [sys.executable, str(DK5340), command, "--board", SIDES[side], "--snr", snr(side), *extra]
    if seconds:
        cmd += ["--seconds", str(seconds)]
    print("+", " ".join(cmd), flush=True)
    r = subprocess.run(cmd, capture_output=True, text=True)
    out = r.stdout + r.stderr
    if r.returncode:
        print(out[-3000:])
        raise SystemExit(f"dk5340.py {command} failed on {SIDES[side]} ({r.returncode}): stop, check, report on hub #83")
    return out


def rtt_text(side, seconds):
    out = dk(side, "rtt", seconds=seconds)
    log = devtools.REPO / "fw_nrf5340" / "build_logs" / f"rtt_{SIDES[side].lower()}.log"
    keep = LOGS / f"{time.strftime('%H%M%S')}_{side}.log"
    LOGS.mkdir(parents=True, exist_ok=True)
    text = log.read_text(errors="replace") if log.exists() else out
    keep.write_text(text)
    print(f"  RTT log -> {keep}")
    return text


def grep(text, pattern):
    return [ln.strip() for ln in text.splitlines() if re.search(pattern, ln)]


# ---------------- B0 ----------------
def probes():
    out = subprocess.run([devtools._nrfutil(), "device", "list", "--json"], capture_output=True, text=True).stdout
    found = []
    for line in out.splitlines():
        try:
            d = json.loads(line)
        except ValueError:
            continue
        data = (d.get("data") or {}) if d.get("type") == "task_end" else {}
        for dev in (data.get("data") or data).get("devices", []):  # nrfutil 8: data.data.devices
            sn = dev.get("serialNumber", "").lstrip("0")
            if dev.get("traits", {}).get("jlink"):
                found.append((sn, (dev.get("devkit") or {}).get("boardVersion")))
    return found


def b0(a):
    found = probes()
    print("J-Links:", ", ".join(f"{s} ({v or 'external'})" for s, v in found) or "none")
    ext = sorted(s for s, v in found if s not in KNOWN_PROBES)
    dks = [s for s, _ in found if s in {"1050035314", "1050017384"}]
    if dks:
        print(f"NOTE: DK probe(s) still plugged in: {dks} (hub #83: both DKs unplugged)")
    left, right = a.left, a.right
    if not (left and right):
        if len(ext) != 2:
            raise SystemExit(f"expected 2 external J-Links, found {ext}: check the connections (or pass --left / --right)")
        left, right = ext  # lower serial number = left (our choice; she labels the boards)
    d = local()
    for side, s in (("L", left), ("R", right)):
        d.setdefault(SIDES[side], {})["snr"] = s
    save_local(d)
    res = {}
    for side in "LR":
        s = snr(side)
        out = devtools.jlink_commands(s, DEVICE, ["mem32 0x00FF020C 2", "mem32 0x00FF0204 2"], timeout=40)
        ok = "Cortex-M33 identified" in out or "Found SW-DP" in out
        prot = "protected" in out.lower() or "APPROTECT" in out
        part = re.search(r"00FF020C = ([0-9A-F]{8}) ([0-9A-F]{8})", out)
        vtref = re.search(r"VTref\s*=\s*([\d.]+)\s*V", out)
        res[side] = {"snr": s, "swd": ok, "protected": prot, "part": part.group(1) if part else None,
                     "variant": part.group(2) if part else None, "vtref": vtref.group(1) if vtref else None}
        print(f"{SIDES[side]} ({'left' if side == 'L' else 'right'}): J-Link {s}, VTref {res[side]['vtref']} V (expect ~1.8, "
              f"the board's +1V8), SWD {'ok' if ok else 'NO CONNECTION'}, FICR PART {res[side]['part']} "
              f"(expect 00005340), VARIANT {res[side]['variant']}{', ACCESS PORT PROTECTED' if prot else ''}")
        if not ok:
            (LOGS / f"b0_{side}_jlink.txt").parent.mkdir(parents=True, exist_ok=True)
            (LOGS / f"b0_{side}_jlink.txt").write_text(out)
    if any(r["protected"] for r in res.values()):
        print("A board is protected (APPROTECT): unlocking erases the chip (`nrfutil device recover`). "
              "Ask on hub #83 before doing it.")
    record("b0", res)


# ---------------- B1 ----------------
def boot_summary(side, text):
    lines = {
        "boot": grep(text, r"\[mustard\] booting"),
        "pmic": grep(text, r"\[pmic\]"),
        "psram": grep(text, r"\[psram\]"),
        "camera": grep(text, r"\[camera\]"),
        "ble": grep(text, r"\[ble\] address"),
    }
    for k, v in lines.items():
        for ln in v or [f"(no {k} line)"]:
            print(f"  {side} {ln}")
    addr = re.search(r"address -> ([0-9A-F:]{17})", text)
    erc = re.search(r"ERCFLAG 0x([0-9a-f]{2})", text)
    sbb2 = re.search(r"SBB2 0x[0-9a-f]{2} -> 0x([0-9a-f]{2})", text)
    psram_ok = any("ok" in ln.lower() and "check" in ln.lower() for ln in lines["psram"])
    res = {"address": addr.group(1) if addr else None, "pmic_ok": "PMIC ok" in " ".join(lines["boot"]),
           "sbb2_reg": sbb2.group(1) if sbb2 else None, "ercflag": erc.group(1) if erc else None,
           "psram_lines": lines["psram"], "camera": lines["camera"], "psram_boot_ok": psram_ok}
    if erc and int(erc.group(1), 16) & 0x07:
        print(f"  {side} STOP: ERCFLAG 0x{erc.group(1)} has a power fault bit (thermal / SYS over- or undervoltage): "
              "report on hub #83 before going on")
    return res


def b1(a):
    d = local()
    res = {}
    for side in sides(a):
        if not a.no_flash:
            dk(side, "flash")
        text = rtt_text(side, a.seconds)
        res[side] = boot_summary(side, text)
        if res[side]["address"]:
            d.setdefault(SIDES[side], {})["address"] = res[side]["address"]
    save_local(d)
    record("b1", res)
    print("\nMeter (Xiaomeng): TP25 +1V8, TP26 +2V8 (expect 2.80 V after boot), TP27 +1V2, TP28 VSBB0; GND TP29.")


def b1_psram(a):
    res = {}
    for side in sides(a):
        dk(side, "flash", "--cflags", "HW_TEST;HW_TEST_PSRAM_ONLY")
        text = rtt_text(side, a.seconds)
        items = grep(text, r"\[hwtest\] \[\d")
        for ln in items:
            print(f"  {side} {ln}")
        speeds = grep(text, r"\[hwtest\]\s+(single|quad)\s+\d+ MHz")
        for ln in speeds:
            print(f"  {side}   {ln}")
        done = grep(text, r"===== done")
        print(f"  {side} {done[0] if done else 'NOT FINISHED: raise --seconds'}")
        res[side] = {"items": items, "speeds": speeds, "done": done[0] if done else None}
    record("b1_psram", res)
    print("\nNext: python v3_bringup.py b1-ble (flashes the main image again)")


class Link:
    """Bridge session with the two V3 boards (m2_test.Session)."""

    def __enter__(self):
        import bridge
        from m2_test import Session
        names = [HOST_NAME[s] for s in "LR"]
        missing = [n for n in names if not P.BOARDS[n].get("address")]
        if missing:
            raise SystemExit(f"no BLE address for {missing}: run b1 first")
        self.br = bridge.Bridge(on_text=lambda s, t: None, on_event=lambda s, l: None).__enter__()
        self.ss = Session(self.br, names)
        self.br.configure([P.BOARDS[n]["address"] for n in names])
        t_x = time.monotonic()
        self.br.disconnect(self.ss.mask)
        time.sleep(0.3)
        if not self.br.wait_ready(range(2), timeout=30, after=t_x):
            self.br.__exit__(None, None, None)
            raise SystemExit(f"boards not ready through the bridge: {self.br.links}")
        time.sleep(1.0)
        self.ss.sync()
        return self.ss

    def __exit__(self, *exc):
        self.br.__exit__(*exc)


def b1_ble(a):
    from m2_test import is_frame
    if not a.no_flash:
        for side in "LR":
            dk(side, "flash")
    res = {}
    with Link() as ss:
        sts = ss.status()
        for b, s in zip(ss.boards, sts):
            print(f"  {b.name}: R_STATUS {s}")
        st_ring = ss.ask(P.ring_command(1000), (P.R_STATUS,), timeout=8)
        marks = [b.mark() for b in ss.boards]
        ss.send(bytes([P.CMD_SNAPSHOT]))
        frames = [b.wait(m, is_frame, 4.0)[1] for b, m in zip(ss.boards, marks)]
        for b, s, r, f in zip(ss.boards, sts, st_ring, frames):
            ok = bool(s) and s["psram"] == 0 and s["total_kb"] == 7680 and s["synced"] == 1 and s.get("camera") == 2
            state = r.fields["state"] if r else None
            print(f"  {b.name}: connected, PSRAM state {s and s['psram']}, total {s and s['total_kb']} KB, synced "
                  f"{s and s['synced']}, camera {s and s.get('camera')} (expect 2 = missing) -> {'ok' if ok else 'CHECK'}; 0xB8 2 without a camera -> state {state} "
                  f"(expect 0 = stopped); 0xB2 -> {'a frame?!' if f else 'no frame (expected)'}")
            res[b.name] = {"status": s, "ring_state_no_camera": state, "frame_without_camera": bool(f), "ok": ok}
    record("b1_ble", res)
    print("\nIf both are ok: post 'cameras can be attached' on hub #83 and wait for Xiaomeng.")


# ---------------- B2 ----------------
def b2(a):
    from m2_test import is_frame
    res = {}
    for side in "LR":
        text = rtt_text(side, a.seconds)  # reset: the boot probe reads model ID and silicon revision
        cam = grep(text, r"\[camera\]")
        print(f"  {side}: {cam or 'no [camera] line'}")
        rev = re.search(r"silicon revision 0x([0-9a-f]{2})", text)
        res[side] = {"camera": cam, "silicon_rev": rev.group(1) if rev else None}
    with Link() as ss:
        ss.send(P.set_format_command(P.FORMAT_JPEG, a.quality))
        time.sleep(0.2)
        times, sizes = {b.name: [] for b in ss.boards}, {b.name: [] for b in ss.boards}
        t_all0 = time.monotonic()
        for i in range(a.pictures + 1):  # the first one includes the camera init: not counted
            marks = [b.mark() for b in ss.boards]
            t0 = time.monotonic()
            ss.send(bytes([P.CMD_SNAPSHOT]))
            for b, m in zip(ss.boards, marks):
                _, f = b.wait(m, is_frame, 15.0)
                if i and f:
                    times[b.name].append(time.monotonic() - t0)
                    sizes[b.name].append(len(f.data) or len(f.pixels))
            if i == 0:
                t_all0 = time.monotonic()
        span = time.monotonic() - t_all0
        total = sum(sum(v) for v in sizes.values())
        for n in times:
            print(f"  {n}: {len(times[n])}/{a.pictures} pictures, request -> frame median "
                  f"{statistics.median(times[n]) if times[n] else float('nan'):.2f} s, "
                  f"JPEG q{a.quality} {statistics.mean(sizes[n]) / 1024 if sizes[n] else 0:.1f} KB")
        print(f"  both boards: {total / 1024 / span:.1f} KB/s over {span:.1f} s")
        res["snapshot"] = {n: {"n": len(times[n]), "median_s": statistics.median(times[n]) if times[n] else None,
                               "kb": statistics.mean(sizes[n]) / 1024 if sizes[n] else None} for n in times}
        res["kbs_two"] = total / 1024 / span
    record("b2", res)
    print("\nWake -> frame: python ae_check.py --boards v3-left v3-right --seconds 120 --period-ms 1000 "
          "--csv logs/v3/ae.csv --rtt logs/v3/ae   (same measurement as the DK, PR #63)")
    if not a.skip_ae:
        subprocess.run([sys.executable, str(HERE / "ae_check.py"), "--boards", "v3-left", "v3-right", "--seconds",
                        str(a.ae_seconds), "--period-ms", "1000", "--csv", str(LOGS / "ae.csv"), "--rtt",
                        str(LOGS / "ae")], cwd=HERE)


# ---------------- ring store / fetch time (hub #92: single line vs quad PSRAM) ----------------
def ringtime(a):
    """Ring at --period for --seconds, then --fetches fetches of the frame nearest to now - 2 s on each
    board. Store time from RTT ("[ring] timing: ... stored +N ms", "encode + write N us"), fetch time from
    the host. Same firmware mode as flashed (QVGA / VGA)."""
    fmt = {"jpeg": (P.FORMAT_JPEG, 85), "raw": (P.FORMAT_RAW, 0)}[a.format]
    prefix = LOGS / f"ringtime_{a.label}_{a.format}"
    LOGS.mkdir(parents=True, exist_ok=True)
    loggers = [devtools.RttLogger(snr(s), DEVICE, f"{prefix}_{s}.log") for s in "LR"]
    fetch = {}
    try:
        with Link() as ss:
            ss.send(P.set_format_command(*fmt))
            time.sleep(0.2)
            ss.ask(P.ring_command(a.period), (P.R_STATUS,), timeout=8)
            time.sleep(a.seconds)
            for b in ss.boards:
                fetch[b.name] = []
                for _ in range(a.fetches):
                    frames, _, dt = ss.fetch(b, P.fetch_nearest_command(ss.bridge_now() - 2_000_000))
                    if frames:
                        fetch[b.name].append((dt, len(frames[0][1].data) or len(frames[0][1].pixels)))
            pairs = []  # both boards asked for the frame nearest to the same time at once
            for _ in range(a.pairs):
                t = ss.bridge_now() - 2_000_000
                marks = [b.mark() for b in ss.boards]
                t0 = time.monotonic()
                ss.send(P.fetch_nearest_command(t))
                got = [b.wait(m, lambda k, o: k == "frame", 30.0)[1] for b, m in zip(ss.boards, marks)]
                if all(got):
                    pairs.append(max(f.completed_at for f in got) - t0)
                time.sleep(0.5)
            fetch["pairs"] = pairs
            ss.ask(P.ring_command(0), (P.R_STATUS,), timeout=8)
            st = ss.status()
            fetch["status"] = st
    finally:
        for lg in loggers:
            lg.stop()
    res = {}
    for side, b in zip("LR", [HOST_NAME[s] for s in "LR"]):
        text = pathlib.Path(f"{prefix}_{side}.log").read_text(errors="replace")
        stored = [int(x) for x in re.findall(r"stored \+(\d+) ms", text)]
        enc = [int(x) / 1000 for x in re.findall(r"(?:encode \+ )?write (\d+) us", text)]
        skipped = sum(int(x) for x in re.findall(r"skipped (\d+)", text)[-1:])
        f = fetch.get(b, [])
        med = lambda v: statistics.median(v) if v else float("nan")  # noqa: E731
        res[side] = {"stored_ms": med(stored), "stored_max": max(stored) if stored else None, "n": len(stored),
                     "write_ms": med(enc), "fetch_s": med([x[0] for x in f]), "bytes": med([x[1] for x in f])}
        r = res[side]
        print(f"  {b} {a.label} {a.format}: {r['n']} stored, wake -> stored median {r['stored_ms']:.0f} ms "
              f"(max {r['stored_max']}), encode/copy + PSRAM write {r['write_ms']:.1f} ms; fetch one median "
              f"{r['fetch_s']:.3f} s ({r['bytes']:.0f} B); skipped (last R_STATUS) {skipped}")
    pr = fetch.get("pairs", [])
    if pr:
        print(f"  pair fetch ({len(pr)}/{a.pairs}): median {statistics.median(pr):.3f} s, max {max(pr):.3f} s")
    res["pairs_s"] = pr
    for b, st in zip([HOST_NAME[s] for s in "LR"], fetch.get("status") or []):
        if st and st["frames"]:
            per = st["used_kb"] / st["frames"]
            n = min(512, int(st["total_kb"] / per)) if per else 0
            print(f"  {b}: {st['frames']} frames use {st['used_kb']} KB ({per:.1f} KB each): the ring holds ~{n} "
                  f"frames = {n * a.period / 1000:.0f} s at {a.period} ms (index max 512); skipped {st['skipped']}")
    record(f"ringtime_{a.label}_{a.format}", res)


def b3(a):
    cmd = [sys.executable, str(HERE / "m2_test.py"), "--boards", "v3-left", "v3-right", "--tests", a.tests,
           "--duration", str(a.duration), "--rtt", str(LOGS / "m2")]
    print("+", " ".join(cmd))
    r = subprocess.run(cmd, cwd=HERE)
    record("b3", {"returncode": r.returncode, "results": str(LOGS / "m2.json")})


def b4(a):
    s = json.loads(SUMMARY.read_text("utf-8")) if SUMMARY.exists() else {}

    def v3(path, default="-"):
        cur = s
        for k in path:
            if not isinstance(cur, dict) or k not in cur:
                return default
            cur = cur[k]
        return cur
    psram = {side: v3(["b1_psram", side, "items"], []) for side in "LR"}
    pick = lambda side, tag: next((ln.split(tag, 1)[1].strip() for ln in psram[side] if tag in ln), "-")  # noqa: E731
    rows = [
        ("PSRAM boot check", DK_REF["psram_boot"], f"L {v3(['b1', 'L', 'psram_boot_ok'])}, R {v3(['b1', 'R', 'psram_boot_ok'])}"),
        ("PSRAM default throughput", DK_REF["psram_single_kbs"], f"L {pick('L', '[5g]')} / R {pick('R', '[5g]')}"),
        ("PSRAM 8 MB x 2", DK_REF["psram_8mb"], f"L {pick('L', '[5h]')}; R {pick('R', '[5h]')}"),
        ("PSRAM four lines", DK_REF["psram_quad"], f"L {pick('L', '[5d]')}; R {pick('R', '[5d]')}"),
        ("0xB2 request -> frame", DK_REF["snapshot_s"], json.dumps(v3(["b2", "snapshot"]))),
        ("BLE, two boards", DK_REF["ble_kbs_two"], f"{v3(['b2', 'kbs_two'])}"),
        ("Silicon revision", DK_REF["silicon_rev"], f"L {v3(['b2', 'L', 'silicon_rev'])}, R {v3(['b2', 'R', 'silicon_rev'])}"),
        ("Wake -> FVLD", DK_REF["wake_to_fvld_ms"], "see ae_check output (logs/v3/ae.csv)"),
        ("M2 tests", ("8/8 both (PR #62)", "DEVLOG 11-21"), f"returncode {v3(['b3', 'returncode'])}, {v3(['b3', 'results'])}"),
        ("Bad frames", DK_REF["bad_frames"], "count [cam] BAD lines in logs/v3/*.log"),
    ]
    print("| item | DK | V3 |\n|---|---|---|")
    for name, (dkv, src), v in rows:
        print(f"| {name} | {dkv} ({src}) | {v} |")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="step", required=True)
    p = sub.add_parser("b0")
    p.add_argument("--left")
    p.add_argument("--right")
    p = sub.add_parser("b1")
    p.add_argument("--side", choices=["L", "R"])
    p.add_argument("--seconds", type=int, default=12)
    p.add_argument("--no-flash", action="store_true")
    p = sub.add_parser("b1-psram")
    p.add_argument("--side", choices=["L", "R"])
    p.add_argument("--seconds", type=int, default=240, help="the PSRAM-only HW_TEST takes a few minutes at 1 MHz RTT")
    p = sub.add_parser("b1-ble")
    p.add_argument("--no-flash", action="store_true")
    p = sub.add_parser("b2")
    p.add_argument("--seconds", type=int, default=10)
    p.add_argument("--pictures", type=int, default=10)
    p.add_argument("--quality", type=int, default=85)
    p.add_argument("--skip-ae", action="store_true")
    p.add_argument("--ae-seconds", type=int, default=120)
    p = sub.add_parser("b3")
    p.add_argument("--tests", default="1,2,3,4,5,6,7,8")
    p.add_argument("--duration", type=float, default=300.0, help="test 7 seconds")
    sub.add_parser("b4")
    p = sub.add_parser("ringtime")
    p.add_argument("--label", required=True, help="e.g. single / quad (goes into the log names)")
    p.add_argument("--format", choices=["jpeg", "raw"], default="jpeg")
    p.add_argument("--period", type=int, default=1000)
    p.add_argument("--seconds", type=float, default=12)
    p.add_argument("--fetches", type=int, default=5)
    p.add_argument("--pairs", type=int, default=0, help="also fetch N pairs (both boards at once)")
    a = ap.parse_args()
    {"b0": b0, "b1": b1, "b1-psram": b1_psram, "b1-ble": b1_ble, "b2": b2, "b3": b3, "b4": b4, "ringtime": ringtime}[a.step](a)


if __name__ == "__main__":
    main()
