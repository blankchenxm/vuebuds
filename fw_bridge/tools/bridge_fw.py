#!/usr/bin/env python3
"""BLE-USB bridge firmware helper (Windows / macOS / Linux; replaces bridge.ps1): build, flash, RTT.

  python3 tools/bridge_fw.py build              # west build (nrf52840dk/nrf52840)
  python3 tools/bridge_fw.py flash              # build + program + reset
  python3 tools/bridge_fw.py rtt --seconds 20   # attach the RTT logger without a reset (the bridge keeps its links)
  python3 tools/bridge_fw.py flash --usb-test   # USB link test build (host/usb_test.py)

Build directories: fw_bridge/build (bridge), fw_bridge/build_usbtest. Paths with spaces are
built through drive V: / a symlink, like fw_nrf5340/tools/dk5340.py.
"""
import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "tools"))
import devtools  # noqa: E402

SNR = "1050221517"  # the 52840 DK that was the HM01B0 board until 2026-10-10
DEVICE = "NRF52840_XXAA"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", nargs="?", default="flash", choices=["build", "flash", "rtt"])
    ap.add_argument("--seconds", type=int, default=15)
    ap.add_argument("--usb-test", action="store_true")
    ap.add_argument("--pristine", action="store_true")
    ap.add_argument("--snr", default=SNR)
    a = ap.parse_args()
    src = devtools.source_dir() / "fw_bridge"
    bdir = src / ("build_usbtest" if a.usb_test else "build")
    if a.command in ("build", "flash"):
        extra = ["--", "-Dfw_bridge_USB_TEST=1"] if a.usb_test else []
        devtools.run(["west", "build", "-p", "always" if a.pristine else "auto", "-b", "nrf52840dk/nrf52840",
                      "-d", bdir, src, *extra], env=devtools.ncs_env(), cwd=src)  # src: see dk5340.py
    if a.command == "flash":
        devtools.run(["west", "flash", "-d", bdir, "--runner", devtools.flash_runner(), "--dev-id", a.snr],
                     env=devtools.ncs_env())
    if a.command == "rtt":
        devtools.capture_rtt(a.snr, DEVICE, devtools.REPO / "fw_bridge" / "build_logs" / "rtt_bridge.log",
                             a.seconds, "bridge")


if __name__ == "__main__":
    main()
