#!/usr/bin/env python3
"""Build the four V3 images (V3L / V3R x QVGA / VGA) from one commit into a fixed folder (hub #104).

  python3 tools/v3_images.py                 # -> ~/vuebuds-fw/v3/
  python3 tools/v3_images.py --out DIR

Writes V3L_QVGA.hex, V3L_VGA.hex, V3R_QVGA.hex, V3R_VGA.hex (application core), net_ipc_radio.hex
(network core: the same for all four, only needed after a full erase), manifest.json (commit, board,
mode, sha256) and a copy of v3_flash.py (the flash command for sw-mac's settings page). Default
firmware options only (JPEG / ring as in main, 64 MHz, no RING_EARLY_ARM); every build is pristine.
Refuses to run with uncommitted changes under fw_nrf5340/ or ../fw/sensors/, so the manifest's
commit is what is in the images. Rerun after every firmware change that should reach the V3 boards.
"""
import argparse
import datetime
import hashlib
import json
import pathlib
import shutil
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
FW = HERE.parent
REPO = FW.parent
VARIANTS = [("V3L", "QVGA"), ("V3L", "VGA"), ("V3R", "QVGA"), ("V3R", "VGA")]


def git(*args):
    return subprocess.run(["git", "-C", str(REPO), *args], capture_output=True, text=True, check=True).stdout.strip()


def sha256(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", default=str(pathlib.Path.home() / "vuebuds-fw" / "v3"))
    ap.add_argument("--allow-dirty", action="store_true", help="for trying the script only; the manifest says so")
    a = ap.parse_args()

    dirty = "\n".join(ln for ln in git("status", "--porcelain", "--", "fw_nrf5340", "fw/sensors").splitlines()
                      if "__pycache__" not in ln)
    if dirty and not a.allow_dirty:
        sys.exit(f"uncommitted changes, commit first:\n{dirty}")
    commit = git("rev-parse", "--short=7", "HEAD")
    out = pathlib.Path(a.out).expanduser()
    out.mkdir(parents=True, exist_ok=True)

    images, net = [], None
    for board, mode in VARIANTS:
        print(f"== {board} {mode}", flush=True)
        r = subprocess.run([sys.executable, str(HERE / "dk5340.py"), "build", "--board", board, "--mode", mode,
                            "--pristine"], capture_output=True, text=True)
        if r.returncode:
            sys.exit(f"build {board} {mode} failed:\n{(r.stdout + r.stderr)[-3000:]}")
        build = FW / f"build_{board.lower()}_{mode.lower()}"
        app, netcore = build / "merged.hex", build / "merged_CPUNET.hex"
        dst = out / f"{board}_{mode}.hex"
        shutil.copyfile(app, dst)
        images.append({"file": dst.name, "board": board, "mode": mode, "commit": commit, "sha256": sha256(dst),
                       "core": "application"})
        if net is None:
            net = sha256(netcore)
            shutil.copyfile(netcore, out / "net_ipc_radio.hex")
        elif sha256(netcore) != net:
            sys.exit(f"{board} {mode}: network core differs from the first build; flash both cores (not handled)")
    shutil.copyfile(HERE / "v3_flash.py", out / "v3_flash.py")
    manifest = {
        "commit": commit + ("-dirty" if dirty else ""),
        "built": datetime.datetime.now().isoformat(timespec="seconds"),
        "options": "default: JPEG / ring as in main, app core 64 MHz, PSRAM quad 48 MHz on V3, no RING_EARLY_ARM",
        "boards": {"V3L": "53001962", "V3R": "53001982"},
        "flash_command": f"python3 {out / 'v3_flash.py'} --serial {{serial}} --image {{image}}",
        "images": images,
        "network_core": {"file": "net_ipc_radio.hex", "sha256": net, "note": "same for all four; v3_flash.py uses it "
                         "only after a recover"},
    }
    (out / "manifest.json").write_text(json.dumps(manifest, indent=1), "utf-8")
    for e in images:
        print(f"{out / e['file']}  {e['board']} {e['mode']}  {e['sha256'][:16]}")
    print(f"manifest: {out / 'manifest.json'} (commit {manifest['commit']})")


if __name__ == "__main__":
    main()
