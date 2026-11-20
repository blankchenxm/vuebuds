"""Cross-platform helpers shared by the firmware scripts and host/ (Windows, macOS, Linux).

- ncs_env(): environment for west / cmake of NCS v3.2.1 (nrfutil sdk-manager, else the NCS_DIR layout)
- source_dir(): a path without spaces for the build (Zephyr's devicetree step breaks on spaces)
- jlink_tool(): SEGGER J-Link executables (JLINK_DIR, else the platform's install location / PATH)
- reset_board(): reset a DK by J-Link serial number (nrfutil device, else nrfjprog)
- RttLogger: RTT channel 0 into a file (J-Link Commander's RTT telnet server)

Standard library only, Python 3.8+: runs from the system Python as well as host/.venv.
"""
import hashlib
import json
import os
import pathlib
import platform
import shutil
import subprocess
import sys
import time

REPO = pathlib.Path(__file__).resolve().parent.parent
NCS_VERSION = "v3.2.1"
IS_WINDOWS = os.name == "nt"
IS_MAC = sys.platform == "darwin"
# Windows install of fw-win (2026-10): toolchain bundle 66cdf9b75e under D:\NRFSDK.
WINDOWS_TOOLCHAIN_ID = "66cdf9b75e"


def _nrfutil():
    return shutil.which("nrfutil")


def _has_nrfutil_command(name):
    exe = _nrfutil()
    if not exe:
        return False
    r = subprocess.run([exe, "list"], capture_output=True, text=True)
    return r.returncode == 0 and any(line.split()[:1] == [name] for line in r.stdout.splitlines())


def ncs_env():
    """os.environ plus the NCS toolchain (PATH, ZEPHYR_BASE, ...) for subprocesses.

    1. nrfutil sdk-manager, if it knows v3.2.1 (the macOS install, /opt/nordic/ncs):
       `nrfutil sdk-manager toolchain env --ncs-version v3.2.1`, ZEPHYR_BASE from `sdk-manager list`.
    2. Else (or with NCS_DIR set) the layout dk5340.ps1 used: <NCS_DIR>/v3.2.1 and
       <NCS_DIR>/toolchains/66cdf9b75e, NCS_DIR default D:\\NRFSDK on Windows.
    """
    global _NCS_ENV
    if _NCS_ENV is None:
        _NCS_ENV = _ncs_vars()
    env = dict(os.environ)
    env.update(_NCS_ENV)
    # A virtualenv's Python must not shadow the toolchain's (west and its packages live there).
    env.pop("VIRTUAL_ENV", None)
    env.pop("PYTHONHOME", None)
    return env


_NCS_ENV = None


def _ncs_vars():
    if not os.environ.get("NCS_DIR") and _has_nrfutil_command("sdk-manager"):
        data = _nrfutil_json(["sdk-manager", "toolchain", "env", "--ncs-version", NCS_VERSION]) or {}
        if data.get("env_variables"):
            out = {v["key"]: v["value"] for v in data["env_variables"]}
            out["ZEPHYR_BASE"] = str(_sdk_path() / "zephyr")
            return out
    ncs = pathlib.Path(os.environ.get("NCS_DIR", r"D:\NRFSDK" if IS_WINDOWS else "/opt/nordic/ncs"))
    tc = ncs / "toolchains" / os.environ.get("NCS_TOOLCHAIN_ID", WINDOWS_TOOLCHAIN_ID)
    if not (ncs / NCS_VERSION / "zephyr").is_dir() or not tc.is_dir():
        raise SystemExit(f"NCS {NCS_VERSION} not found: install it with `nrfutil sdk-manager install {NCS_VERSION}`, "
                         f"or set NCS_DIR (and NCS_TOOLCHAIN_ID) to an existing install (tried {ncs}, {tc})")
    out = {"ZEPHYR_BASE": str(ncs / NCS_VERSION / "zephyr"), "ZEPHYR_TOOLCHAIN_VARIANT": "zephyr",
           "ZEPHYR_SDK_INSTALL_DIR": str(tc / "opt" / "zephyr-sdk"), "NRFUTIL_HOME": str(tc / "nrfutil" / "home")}
    if IS_WINDOWS:
        out["PYTHONPATH"] = os.pathsep.join(str(tc / d) for d in ("opt/bin", "opt/bin/Lib", "opt/bin/Lib/site-packages"))
        dirs = ["", "mingw64/bin", "bin", "opt/bin", "opt/bin/Scripts", "opt/nanopb/generator-bin",
                "nrfutil/bin", "opt/zephyr-sdk/arm-zephyr-eabi/bin"]
    else:
        dirs = ["bin", "usr/bin", "usr/local/bin", "opt/bin", "opt/nanopb/generator-bin", "nrfutil/bin",
                "opt/zephyr-sdk/arm-zephyr-eabi/bin"]
    out["PATH"] = os.pathsep.join(str(tc / d) for d in dirs) + os.pathsep + os.environ.get("PATH", "")
    return out


def _nrfutil_json(args):
    """Run nrfutil with --json; return the data of its last 'info' line (one JSON object per line)."""
    r = subprocess.run([_nrfutil(), *args, "--json"], capture_output=True, text=True)
    data = None
    for line in r.stdout.splitlines():
        try:
            obj = json.loads(line)
        except ValueError:
            continue
        if isinstance(obj, dict) and obj.get("type") == "info" and isinstance(obj.get("data"), dict):
            data = obj["data"]
    return data


def _sdk_path():
    """Path of NCS v3.2.1 from `nrfutil sdk-manager list` (default /opt/nordic/ncs/v3.2.1 on macOS)."""
    data = _nrfutil_json(["sdk-manager", "list"]) or {}
    for sdk in data.get("versions") or data.get("sdks") or []:
        if isinstance(sdk, dict) and sdk.get("version") == NCS_VERSION and sdk.get("path"):
            return pathlib.Path(sdk["path"])
    for default in (pathlib.Path("/opt/nordic/ncs") / NCS_VERSION, pathlib.Path.home() / "ncs" / NCS_VERSION):
        if (default / "zephyr").is_dir():
            return default
    raise SystemExit(f"nrfutil sdk-manager: SDK {NCS_VERSION} not found (nrfutil sdk-manager install {NCS_VERSION})")


def source_dir():
    """The repository at a path without spaces.

    Windows: drive V: mapped with subst (as dk5340.ps1 did). macOS / Linux: a symlink
    ~/.cache/vuebuds-src/<hash> -> repo. A repo path without spaces (git worktree) is used in place.
    """
    if " " not in str(REPO):
        return REPO
    if IS_WINDOWS:
        drive = os.environ.get("VUEBUDS_DRIVE", "V:")
        if not pathlib.Path(drive + "\\fw_nrf5340").exists():
            subprocess.run(f'subst {drive} "{REPO}"', shell=True, check=False)
        return pathlib.Path(drive + "\\")
    link_root = pathlib.Path.home() / ".cache" / "vuebuds-src"
    link = link_root / hashlib.md5(str(REPO).encode()).hexdigest()[:8]
    if " " in str(link):
        raise SystemExit(f"home directory has spaces, cannot make a space-free path: {link}")
    if not link.is_symlink() or pathlib.Path(os.readlink(link)) != REPO:
        link_root.mkdir(parents=True, exist_ok=True)
        if link.is_symlink() or link.exists():
            link.unlink()
        link.symlink_to(REPO, target_is_directory=True)
    return link


def jlink_dir():
    """JLINK_DIR, else the platform's default install (macOS /Applications/SEGGER/JLink)."""
    if os.environ.get("JLINK_DIR"):
        return pathlib.Path(os.environ["JLINK_DIR"])
    for d in ([r"C:\Program Files\SEGGER\JLink", r"C:\Program Files (x86)\SEGGER\JLink"] if IS_WINDOWS
              else ["/Applications/SEGGER/JLink", "/opt/SEGGER/JLink"]):
        if pathlib.Path(d).is_dir():
            return pathlib.Path(d)
    return None


def jlink_tool(name):
    """Path of a J-Link tool by its Windows name without .exe: 'JLinkRTTLogger', 'JLink'.

    Windows: JLinkRTTLogger.exe / JLink.exe. macOS / Linux: JLinkRTTLoggerExe / JLinkExe (the
    names without Exe are wrappers or missing on some versions). PATH is the last resort, but
    on Windows the 'jlink' on PATH is Java's, so only the SEGGER directory is trusted there.
    """
    d = jlink_dir()
    names = [name + ".exe"] if IS_WINDOWS else [name + "Exe", name]
    for n in names:
        if d and (d / n).is_file():
            return str(d / n)
    if not IS_WINDOWS:
        for n in names:
            found = shutil.which(n)
            if found:
                return found
    raise SystemExit(f"J-Link tool {name} not found (set JLINK_DIR to the SEGGER J-Link directory)")


def flash_runner():
    """west flash runner: nrfjprog where it is installed (Windows setup), else nrfutil."""
    if os.environ.get("WEST_RUNNER"):
        return os.environ["WEST_RUNNER"]
    return "nrfjprog" if shutil.which("nrfjprog") else "nrfutil"


def reset_board(snr, family="nrf53"):
    """Reset the DK with J-Link serial number snr; returns the tool's exit code.

    nrfjprog where installed (Windows, as before), else nrfutil device (nrfjprog is end-of-life
    and not installed on the Mac).
    """
    if shutil.which("nrfjprog"):
        cmd = ["nrfjprog", "-f", family, "--reset"] + (["--snr", str(snr)] if snr else [])
    elif _nrfutil():
        cmd = [_nrfutil(), "device", "reset"] + (["--serial-number", str(snr)] if snr else [])
    else:
        raise SystemExit("neither nrfjprog nor nrfutil found")
    return subprocess.run(cmd, capture_output=True).returncode


def jlink_commands(snr, device, commands, timeout=30):
    """Run J-Link Commander with a list of commands (one session, connected without a halt and
    without a probe firmware update); returns stdout. Commander resumes a halted core on exit,
    so keep a halt inside the session (e.g. ["h", "sleep 5000", "g"])."""
    import tempfile
    with tempfile.NamedTemporaryFile("w", suffix=".jlink", delete=False) as f:
        f.write("\n".join(["exec DisableAutoUpdateFW", "connect"] + list(commands) + ["exit"]) + "\n")
        script = f.name
    try:
        r = subprocess.run([jlink_tool("JLink"), "-USB", str(snr), "-device", device, "-if", "SWD", "-speed", "4000",
                            "-autoconnect", "0", "-NoGui", "1", "-CommandFile", script],
                           capture_output=True, text=True, timeout=timeout, stdin=subprocess.DEVNULL)
        return r.stdout
    finally:
        os.unlink(script)


class RttLogger:
    """RTT channel 0 of a board into out_file; attaches without resetting the target.

    J-Link Commander connects (no halt) and serves RTT on a telnet port, read here. Not
    JLinkRTTLogger: V9.96 on macOS reports "RTT Control Block not found" on the nRF5340 even
    with -RTTAddress, while Commander finds it (2026-11-18). Each logger takes its own free
    port, so two boards can be logged at once (stereo.py). Firmware auto-update is disabled
    for these sessions (DisableAutoUpdateFW).
    """

    HEADER = (b"SEGGER J-Link", b"Process: ")  # the telnet server's banner lines

    def __init__(self, snr, device, out_file):
        import socket
        import tempfile
        import threading
        self.out_file = pathlib.Path(out_file)
        self.out_file.parent.mkdir(parents=True, exist_ok=True)
        if self.out_file.exists():
            self.out_file.unlink()
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            self.port = s.getsockname()[1]
        with tempfile.NamedTemporaryFile("w", suffix=".jlink", delete=False) as f:
            f.write("exec DisableAutoUpdateFW\nconnect\n" + "sleep 3600000\n" * 24 + "exit\n")  # up to 24 h
            self._script = f.name
        probe = ["-USB", str(snr)] if snr else []
        kw = {"creationflags": subprocess.CREATE_NO_WINDOW} if IS_WINDOWS else {}
        self.proc = subprocess.Popen([jlink_tool("JLink"), *probe, "-device", device, "-if", "SWD", "-speed", "4000",
                                      "-autoconnect", "0", "-NoGui", "1", "-RTTTelnetPort", str(self.port),
                                      "-CommandFile", self._script],
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL, **kw)
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._read, daemon=True)
        self._thread.start()

    def _read(self):
        import socket
        sock, deadline = None, time.monotonic() + 15
        while sock is None and not self._stop.is_set() and time.monotonic() < deadline:
            try:
                sock = socket.create_connection(("127.0.0.1", self.port), timeout=0.5)
            except OSError:
                time.sleep(0.2)
        if sock is None:
            return
        sock.settimeout(0.2)
        pending, banner = b"", True
        with sock, open(self.out_file, "ab") as out:
            while not self._stop.is_set():
                try:
                    chunk = sock.recv(4096)
                except socket.timeout:
                    continue
                except OSError:
                    break
                if not chunk:
                    break
                if banner:  # drop the banner lines, then pass everything through
                    pending += chunk
                    while banner and b"\n" in pending:
                        line, pending = pending.split(b"\n", 1)
                        if not line.startswith(self.HEADER):
                            out.write(line + b"\n")
                            banner = False
                    if banner:
                        continue
                    chunk, pending = pending, b""
                out.write(chunk)
                out.flush()

    def kill(self):
        self.stop()

    def stop(self):
        self._stop.set()
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self._thread.join(timeout=2)
        try:
            os.unlink(self._script)
        except OSError:
            pass

    def text(self):
        return self.out_file.read_text("utf-8", "replace") if self.out_file.exists() else ""


def capture_rtt(snr, device, out_file, seconds, label=""):
    """Attach the RTT logger for seconds, then print the log (as dk5340.ps1 rtt did)."""
    logger = RttLogger(snr, device, out_file)
    try:
        time.sleep(seconds)
    finally:
        logger.stop()
    print(f"===== RTT log ({label}, {seconds} s) -> {out_file} =====")
    print(logger.text() or "(no RTT data captured)")


def run(cmd, env=None, cwd=None):
    """Run a command, echo it, exit with its code on failure."""
    print("+ " + " ".join(str(c) for c in cmd), flush=True)
    r = subprocess.run([str(c) for c in cmd], env=env, cwd=cwd)
    if r.returncode != 0:
        raise SystemExit(f"{pathlib.Path(str(cmd[0])).name} failed ({r.returncode})")


def venv_python():
    """host/.venv's Python (Scripts\\python.exe on Windows, bin/python elsewhere)."""
    venv = REPO / "host" / ".venv"
    py = venv / ("Scripts/python.exe" if IS_WINDOWS else "bin/python")
    return str(py) if py.exists() else sys.executable


def machine():
    return f"{platform.system()} {platform.machine()}"
