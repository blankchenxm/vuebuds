#!/usr/bin/env python3
"""balsamic3 (V3) hardware test (Windows / macOS / Linux; replaces v3test.ps1): build, flash
(application core only) and read the RTT log.

  python3 tools/v3test.py build | flash | rtt | run   [--seconds 70] [--snr 53001982] [--pristine]

Same NCS setup as fw_nrf5340/tools/dk5340.py (../../tools/devtools.py; paths with spaces are
built through drive V: / a symlink). The PSRAM pass takes ~20 s; with shorter captures the log
was cut there, so keep --seconds at 70 or more. The J-Link EDU Mini on the debug FPC runs SWD
at 1 MHz.
"""
import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "tools"))
import devtools  # noqa: E402

SNR = "53001982"  # J-Link EDU Mini used with the debug FPC
DEVICE = "nRF5340_xxAA_APP"
SWD_KHZ = 1000


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", nargs="?", default="run", choices=["build", "flash", "rtt", "run"])
    ap.add_argument("--seconds", type=int, default=70)
    ap.add_argument("--snr", default=SNR)
    ap.add_argument("--pristine", action="store_true")
    a = ap.parse_args()
    src = devtools.source_dir() / "fw_v3_test"
    bdir = src / "build"
    if a.command in ("build", "flash", "run"):
        devtools.run(["west", "build", "-p", "always" if a.pristine else "auto", "-b", "nrf5340dk/nrf5340/cpuapp",
                      "-d", bdir, src], env=devtools.ncs_env(), cwd=src)  # src: see dk5340.py
    if a.command in ("flash", "run"):
        devtools.run(["west", "flash", "-d", bdir, "--runner", devtools.flash_runner(), "--dev-id", a.snr],
                     env=devtools.ncs_env())
    if a.command in ("rtt", "run"):
        devtools.reset_board(a.snr, "nrf53")
        devtools.capture_rtt(a.snr, DEVICE, devtools.REPO / "fw_v3_test" / "build_logs" / "rtt.log",
                             a.seconds, "v3", speed=SWD_KHZ)


if __name__ == "__main__":
    main()
