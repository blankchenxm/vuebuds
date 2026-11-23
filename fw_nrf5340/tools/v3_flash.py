#!/usr/bin/env python3
"""Flash one V3 board (balsamic3) with a prebuilt application-core image, no prompts (hub #104).

  python3 v3_flash.py --serial 53001962 --image ~/vuebuds-fw/v3/V3L_VGA.hex

For the sw-mac settings page: camera.flash.command =
  python3 ~/vuebuds-fw/v3/v3_flash.py --serial {serial} --image {image}

- Only the two V3 J-Links are accepted: 53001962 = left (V3L), 53001982 = right (V3R). Anything
  else (the bridge, a DK) is refused before anything is touched.
- The image must be for that board: the manifest.json next to it (written by v3_images.py) names
  the board of each file; an image of the other side is refused.
- Programs the application core only (the four images share the same network core), erasing just
  the ranges the image covers, reads back to verify, then resets. The ring is in PSRAM and is lost
  on any reset anyway.
- If the board's access port is protected (a new chip, or after a full erase), it recovers the
  chip (full erase of both cores; Xiaomeng agreed for these two boards on 2026-11-22, hub #83),
  programs the network core (net_ipc_radio.hex next to the image) and then the application core.
- Two boards can be flashed at the same time (one process per serial number).

Exit codes: 0 flashed + verified + reset; 2 refused (serial / image / board mismatch); 3
programming failed; 4 J-Link not on USB, or no target voltage (board off / FPC not plugged in). The last line is "OK ..." or "FAIL ..." with the reason.
Standard library only; it runs from the folder the images are in (no repo needed).
"""
import argparse
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys
import time

BOARDS = {"53001962": "V3L", "53001982": "V3R"}
NET_IMAGE = "net_ipc_radio.hex"


def say(msg):
    print(f"[v3_flash] {msg}", flush=True)


def fail(code, msg):
    print(f"FAIL {msg}", flush=True)
    sys.exit(code)


def nrfutil():
    exe = shutil.which("nrfutil") or str(pathlib.Path.home() / ".local" / "bin" / "nrfutil")
    if not os.path.exists(exe):
        fail(3, "nrfutil not found (PATH or ~/.local/bin)")
    return exe


def run(args, timeout=180):
    try:
        r = subprocess.run([nrfutil(), "device", *args], capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return 1, "timed out"
    return r.returncode, (r.stdout + r.stderr).strip()


def image_board(image):
    """Board of an image: manifest.json next to it (by sha256), else the file name (V3L_... / V3R_...)."""
    manifest = image.parent / "manifest.json"
    if manifest.exists():
        digest = hashlib.sha256(image.read_bytes()).hexdigest()
        for entry in json.loads(manifest.read_text("utf-8")).get("images", []):
            if entry.get("sha256") == digest:
                return entry.get("board"), entry
    name = image.name.upper()
    for board in BOARDS.values():
        if name.startswith(board + "_"):
            return board, None
    return None, None


def program(serial, image, core):
    return run(["program", "--serial-number", serial, "--firmware", str(image), "--core", core, "--family", "nrf53",
                "--options", "chip_erase_mode=ERASE_RANGES_TOUCHED_BY_FIRMWARE,verify=VERIFY_READ"])


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--serial", required=True, help="J-Link serial number: 53001962 (left) or 53001982 (right)")
    ap.add_argument("--image", required=True, help="application-core .hex (V3L_VGA.hex ...)")
    a = ap.parse_args()

    serial = a.serial.strip().lstrip("0")
    if serial not in BOARDS:
        fail(2, f"serial {a.serial} is not a V3 board (only {', '.join(f'{s} {b}' for s, b in BOARDS.items())})")
    image = pathlib.Path(os.path.expanduser(a.image)).resolve()
    if not image.is_file():
        fail(2, f"no image {image}")
    board, entry = image_board(image)
    if board is None:
        fail(2, f"{image.name}: unknown board (not in manifest.json, name not V3L_ / V3R_)")
    if board != BOARDS[serial]:
        fail(2, f"{image.name} is for {board}, but J-Link {serial} is {BOARDS[serial]}")
    if entry:
        say(f"{image.name}: {entry.get('board')} {entry.get('mode')}, commit {entry.get('commit')}")

    t0 = time.monotonic()
    code, out = run(["list"], timeout=30)  # this nrfutil's "device list" has no --serial-number
    if code or serial not in out:
        fail(4, f"J-Link {serial} not found on USB")

    say(f"programming the application core of {BOARDS[serial]} (J-Link {serial})")
    code, out = program(serial, image, "application")
    if code and "protect" in out.lower():
        say("access port protected: recover (full erase of both cores), then network + application core")
        net = image.parent / NET_IMAGE
        if not net.is_file():
            fail(3, f"recover needs {net} for the network core")
        code, out = run(["recover", "--serial-number", serial])
        if code:
            fail(3, f"recover failed: {out[-400:]}")
        code, out = program(serial, net, "network")
        if code:
            fail(3, f"network core failed after recover: {out[-400:]}")
        code, out = program(serial, image, "application")
    if code and "LOW_VOLTAGE" in out:
        fail(4, f"J-Link {serial} sees no target voltage (LOW_VOLTAGE): board not powered or debug FPC not plugged in")
    if code:
        fail(3, f"programming failed: {out[-400:]}")
    code, out = run(["reset", "--serial-number", serial], timeout=30)
    if code:
        fail(3, f"programmed and verified, but the reset failed: {out[-400:]}")
    print(f"OK {serial} {BOARDS[serial]} {image.name} {time.monotonic() - t0:.1f} s", flush=True)


if __name__ == "__main__":
    main()
