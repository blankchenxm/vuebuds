#!/usr/bin/env python3
"""nRF5340 DK helper (Windows / macOS / Linux; replaces dk5340.ps1): build, flash, RTT logs, camera.

  python3 tools/dk5340.py build                      # west build (app core + ipc_radio net core)
  python3 tools/dk5340.py flash                      # build + program both cores + reset
  python3 tools/dk5340.py rtt --seconds 20           # reset + capture RTT channel 0
  python3 tools/dk5340.py run --seconds 20           # build + flash + capture RTT
  python3 tools/dk5340.py run --board HM0360B        # pick the DK by name (J-Link serial number below)
  python3 tools/dk5340.py run --mode QQVGA --color-bar   # camera mode / color bar test pattern
  python3 tools/dk5340.py run --monitor --period-ms 0    # Monitor build (CAM_APP=MONITOR)
  python3 tools/dk5340.py run --cflags 'CAPTURE_STRESS_US=300'   # extra defines, ';'-separated, no -D
  python3 tools/dk5340.py camera --seconds 20        # reset + host/viewer.py over BLE with RTT, saves a snapshot
  python3 tools/dk5340.py run --board V3L --snr 801234567   # V3 board (balsamic3, hub #83), external J-Link

V3 boards (--board V3L / V3R): same firmware with CAM_BOARD=V3_L / V3_R (PMIC init, camera probe
at boot) and boards/balsamic3.overlay; flashed and logged through the external J-Link given by
--snr, or the serial number saved for that board in host/v3_boards.local.json
({"V3L": {"snr": ..., "address": ...}}, written by host/v3_bringup.py b0). RTT runs at 1 MHz
there (SWD over the debug FPC).

Build directories: fw_nrf5340/build_<variant> (same names as dk5340.ps1). Zephyr's devicetree
step breaks on the spaces in the repo path, so a repo path with spaces is built through
drive V: (Windows, subst) or a symlink under ~/.cache/vuebuds-src (macOS / Linux).
NCS v3.2.1 comes from nrfutil sdk-manager (macOS) or NCS_DIR (default D:\\NRFSDK, Windows).
J-Link tools: JLINK_DIR, else the platform's SEGGER directory.
"""
import argparse
import hashlib
import json
import pathlib
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "tools"))
import devtools  # noqa: E402

BOARDS = {
    "HM0360": {"snr": "1050035314", "host": "hm0360-5340"},
    "HM0360B": {"snr": "1050017384", "host": "hm0360b-5340"},  # had the HM01B0 until 2026-10-06
    # V3 boards (hub #83): external J-Links, serial numbers from host/v3_boards.local.json or --snr.
    "V3L": {"snr": None, "host": "v3-left", "v3": "L", "swd_khz": 1000},
    "V3R": {"snr": None, "host": "v3-right", "v3": "R", "swd_khz": 1000},
}
V3_LOCAL = devtools.REPO / "host" / "v3_boards.local.json"
DEVICE = "nRF5340_xxAA_APP"


def snr(a):
    if a.snr:
        return a.snr
    s = BOARDS[a.board]["snr"]
    if s is None and V3_LOCAL.exists():
        s = json.loads(V3_LOCAL.read_text("utf-8")).get(a.board, {}).get("snr")
    if s is None:
        raise SystemExit(f"{a.board}: no J-Link serial number; pass --snr (or run host/v3_bringup.py b0)")
    return str(s)


def variant(a):
    v = a.mode.lower()
    if "v3" in BOARDS[a.board]:
        v = f"v3{BOARDS[a.board]['v3'].lower()}_" + v
    if a.color_bar:
        v += "_colorbar"
    if a.monitor:
        v += "_monitor"
    if a.period_ms >= 0:
        v += f"_p{a.period_ms}"
    if a.cflags:  # short hash: long build paths break the Windows 260-character limit in ninja
        v += "_x" + hashlib.md5(a.cflags.encode()).hexdigest()[:8]
    return v


def build_dir(a):
    return devtools.source_dir() / "fw_nrf5340" / f"build_{variant(a)}"


def build(a):
    src = devtools.source_dir() / "fw_nrf5340"
    defs = ([f"MONITOR_PERIOD_MS={a.period_ms}"] if a.period_ms >= 0 else []) + ([a.cflags] if a.cflags else [])
    # Sysbuild passes -D<image>_<var> to that image only; the app image is named fw_nrf5340.
    side = BOARDS[a.board].get("v3")
    v3 = [f"-Dfw_nrf5340_CAM_BOARD=V3_{side}",
          f"-Dfw_nrf5340_EXTRA_DTC_OVERLAY_FILE={(src / 'boards' / 'balsamic3.overlay').as_posix()}"] if side else []
    devtools.run(["west", "build", "-p", "always" if a.pristine else "auto", "-b", "nrf5340dk/nrf5340/cpuapp",
                  "-d", build_dir(a), src, "--",  # src, not ".": cwd would resolve the symlink
                  *v3,
                  f"-Dfw_nrf5340_CAM_MODE={a.mode}",
                  f"-Dfw_nrf5340_CAM_TEST_PATTERN={'COLOR_BAR' if a.color_bar else 'OFF'}",
                  f"-Dfw_nrf5340_CAM_APP={'MONITOR' if a.monitor else 'STREAM'}",
                  f"-Dfw_nrf5340_EXTRA_DEFINES={';'.join(defs)}"],
                 env=devtools.ncs_env(), cwd=src)


def flash(a):
    devtools.run(["west", "flash", "-d", build_dir(a), "--runner", devtools.flash_runner(),
                  "--dev-id", snr(a)], env=devtools.ncs_env())


def rtt_log(a):
    return devtools.REPO / "fw_nrf5340" / "build_logs" / f"rtt_{a.board.lower()}.log"


def rtt(a):
    # Reset first, then attach (the reset tool and the logger cannot share the probe).
    devtools.reset_board(snr(a), "nrf53")
    devtools.capture_rtt(snr(a), DEVICE, rtt_log(a), a.seconds, a.board, speed=BOARDS[a.board].get("swd_khz", 4000))


def camera(a):
    """Reset + BLE frames through host/viewer.py, with the RTT log merged into its session log."""
    host = devtools.REPO / "host"
    shot = devtools.REPO / "fw_nrf5340" / "build_logs" / f"camera_{a.board.lower()}.png"
    shot.parent.mkdir(parents=True, exist_ok=True)
    r = subprocess.run([devtools.venv_python(), str(host / "viewer.py"), "--board", BOARDS[a.board]["host"],
                        "--rtt", "--reset", "--duration", str(a.seconds), "--snapshot", str(shot)], cwd=host)
    print(f"snapshot -> {shot}")
    if r.returncode:
        raise SystemExit(f"viewer failed ({r.returncode})")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", nargs="?", default="run", choices=["build", "flash", "rtt", "run", "camera"])
    ap.add_argument("--seconds", type=int, default=15)
    ap.add_argument("--mode", default="QVGA", type=str.upper, choices=["QVGA", "QQVGA", "VGA"])
    ap.add_argument("--color-bar", action="store_true")
    ap.add_argument("--monitor", action="store_true")
    ap.add_argument("--period-ms", type=int, default=-1)
    ap.add_argument("--board", default="HM0360", type=str.upper, choices=list(BOARDS))
    ap.add_argument("--snr", help="J-Link serial number (overrides the board's)")
    ap.add_argument("--cflags", default="", help="extra defines, ';'-separated, without -D")
    ap.add_argument("--pristine", action="store_true", help="rebuild from scratch (after adding a .conf / overlay)")
    a = ap.parse_args()
    if a.command in ("build", "flash", "run"):
        build(a)
    if a.command in ("flash", "run"):
        flash(a)
    if a.command in ("rtt", "run"):
        rtt(a)
    if a.command == "camera":
        camera(a)


if __name__ == "__main__":
    main()
