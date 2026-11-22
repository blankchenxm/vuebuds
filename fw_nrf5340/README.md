# fw_nrf5340 — VueBuds camera firmware for the nRF5340 DK

nRF Connect SDK (Zephyr) port of `../fw` (nRF52840, nRF5 SDK). Same features and BLE
protocol: HM01B0 / HM0360 detected at boot, QVGA / QQVGA chosen at compile time, BLE
streaming to `host/viewer.py`, or Monitor (frames cached in RAM, HM0360 S2 sleep).

- `../fw/sensors/` (drivers and register tables) is compiled from `fw/` directly, not copied;
  `src/compat/` provides the nRF5 SDK headers it includes (I2C, time, log, delay).
- `src/capture/`: MCLK (TIMER0), line counter (TIMER1), 2 µs CS pulse (TIMER2), chip select
  and XSLEEP fully in hardware through DPPI + GPIOTE + EGU0, SPIS2 at register level in
  mode 1 (D0 sampled on the falling PCLK edge; mode 0 as in `fw/` gives the HM0360 random
  bit errors on this chip). The channel map is at the top of `capture.c`.
- Frame pool: all RAM after the image (`_end` .. `__kernel_ram_end`, about 413 KB); libc
  malloc is disabled so nothing else claims it. The build's RAM figure excludes the pool.
- `src/camera.c`, `src/monitor.c`: ports of `fw/camera.c`, `fw/monitor.c`.
- `src/ble.c`: Zephyr GATT service with the same UUIDs and packet format as `fw/ble_cus.c`,
  except the start-of-frame header (**v2**, below) and ATT MTU 498 (`fw/`: 247).
  The network core runs Nordic's `ipc_radio` image (`sysbuild/ipc_radio/prj.conf`).
- `src/image.c` (not in `fw/`): frame output with **frame header v2** (`ble_frame_header_t` in
  `ble.h`, 22 bytes; packet flags bit1): width, height, format, quality, length, capture time
  (FVLD rise, µs), frame id, exposure registers read after the frame (integration 0x0202/03,
  analog gain 0x0205, digital gain 0x020E/0F), flags (common clock, manual exposure).
  Control commands (multi-byte writes, queued for the main thread):
  - `B3 <format> <quality>`: image format, 0 raw, 1 PNG, 2 JPEG (quality 1–100, libjpeg scale),
    encoded on the board (`src/codec*.c`, `lib/README.md`); output in PSRAM (`src/psram.c`) or,
    without PSRAM, in the frame pool after slot 0. `B4`: codec bench (`host/codec_bench.py`).
  - `B6 00` automatic exposure; `B6 01 <integration u16> <analog gain> <digital gain u16>` fixed
    exposure (HM0360). Fixed values only hold while the sensor streams: a wake from S2 meters
    the scene itself, so with fixed exposure the picture app keeps the sensor awake (no S2).
- `src/snapshot.c` (not in `fw/`): control command **0xB2 takes one picture** in stream builds.
  HM0360 sleeps (S2) between pictures; a request wakes it, the first frame is sent with the
  usual frame packets, then it sleeps again. A bad frame is retaken (up to 2 times). One boot
  takes either 0xB1 (stream) or 0xB2 (pictures); 0xB1 after `B8 00` is allowed (M2). Used by `host/stereo.py` (two boards;
  `--exposure auto | fixed I A D | sync`).
  **0xB8 timed capture** (F5, hub INTERFACES C2, 2026-11-19): `B8 01 <period u16 ms>` takes a
  picture every period (100–60000 ms, 32.768 kHz clock), the first right away, like 0xB2 and in
  the format of `B3`; `B8 00` stops; `B8 01 <P'>` while running changes the period. A period that
  comes while the previous picture is still being taken or sent is skipped at the timer, not
  queued: pictures stay on the period grid, and the next one has header flag `0x04`. Timed
  pictures carry flag `0x10` (a 0xB2 in between does not). Same group as 0xB2 (not with 0xB1).
  `host/timed_capture.py` measures the reached period. Without PSRAM (or when its boot check
  fails) the encoder output lives in the RAM left after the frame slot: ~45 KB with VGA, so VGA
  JPEG q85 does not fit and goes out raw (q60 fits).
  **Time sync** (plan E, `src/timesync.c`, hub INTERFACES D): RTC0 counts the BLE controller's
  µs; the bridge's `0xB5` references (anchor time + connection event counter) map it to the
  bridge's clock. FVLD edges are captured in hardware (RTC0 CAPTURE2): frame headers carry the FVLD
  rise on the bridge's clock (flag `0x01`, low 32 bits). `0xB7 … <T u64>` wakes the sensor at T in
  hardware (RTC0 COMPARE0 → DPPI → XSLEEP). Uses RTC0, so not with `CAPTURE_STRESS_US`.
  **M2: ring buffer, status, fetch** (`src/ringcmd.c`, `src/ring.c`; hub INTERFACES C2,
  Xiaomeng 11-21): `B8 02 <P u16>` takes a picture at every multiple of P on the bridge's clock
  (hardware wake), stores it in the 0xB3 format (PNG: raw) in the PSRAM ring (first 7.5 MB,
  index 512 frames; without PSRAM in the RAM after the encoders) and does not send it; `B8 00`
  stops (frames stay until the next start); `B9` status; `BA 00 / 01 / 02 / 03` fetch the frame
  nearest to t / at or before t / in [t1, t2] (≤ 32) / by number. Replies are data notifications
  `[seq, 0x04, type, …]`: `R_STATUS`, `R_FRAME` (before each frame sent from the ring, header flag
  `0x08`), `R_NONE <reason>`, `R_DONE`. Fetched frames go out from a sender thread below the main
  thread, so the ring keeps capturing (`R_STATUS` counts skipped capture times). 0xB2 while the
  ring runs is stored too and sent with `R_FRAME`. `B8 00` then `B1` hands the sensor to the stream.
  `host/m2_test.py` tests all of it (hub #68).

## Encoders and PSRAM (2026-10-11)

- `lib/jpegenc`, `lib/pngenc`: bitbank2 JPEGENC / PNGenc compiled as C (`src/codec_jpeg.c`,
  `src/codec_png.c`), JPEGENC patched for numeric quality (`lib/README.md`); state (~50 KB)
  in the frame pool after slot 0, built with `-O2`. PC check against OpenCV:
  `tools/codec_test/` (same sources, MinGW gcc).
- `src/psram.c`: APS6404L on the QSPI pins (CE# P0.18, SCK P0.17, SIO0–3 P0.13–P0.16; since
  2026-10-14, was SPIM4 on P1.12–P1.15), single line 48 MHz in 32 B bursts (CE# low ~6.6 µs <
  tCEM 8 µs), ~2.7 MB/s. Quad works in principle but breaks on the DK jumper wires. The DK's
  MX25R64 flash is cut off these pins (SB10–SB15 open, SB20–SB25 shorted).
  **Boot check** (2026-11-20): 4 × 1 KB written across the chip, read back at once and 2 s
  later; failed → not used, output in on-chip RAM, RTT `[psram] boot check: …`. BLE starts
  without waiting; a picture compressed within 2 s of boot waits for the result.
  `-Cflags HW_TEST` checks the whole chip ([5a]–[5j]).
- Measured (VGA 640×480, two boards through the bridge, 64 MHz CPU): JPEG q85 11 KB / 49 dB,
  encode 0.32 s + BLE 0.28 s (raw: 300 KB, 5.1 s); PNG 96 KB, encode 4.2–4.9 s. QVGA JPEG q85
  5.7 KB, encode 65 ms. The application core runs at 64 MHz by default; `-Cflags CPU_128MHZ`
  halves the encode times (VGA JPEG q85 0.16 s, PNG 2.2 s).

## Wiring

Obsidian note `02-daily_raw/10.5~10.11/nrf5340迁移.md` §2, `src/pins.h`. Same as the nRF52840
DK except FVLD → P1.09, LVLD → P1.10, CS loopback jumper P1.06 → P1.07.

## Build / flash / log (macOS / Windows / Linux, NCS v3.2.1)

`tools/dk5340.py` (Python 3.8+, standard library; replaces `dk5340.ps1` since 2026-11-18). On
Windows write `python` / `..\host\.venv\Scripts\python.exe` for `python3` / `../host/.venv/bin/python`.

```bash
python3 tools/dk5340.py run --seconds 15                 # build + flash + RTT (HM0360 board)
python3 tools/dk5340.py run --board HM0360B --mode QQVGA # other board (by J-Link SN) / mode
python3 tools/dk5340.py run --color-bar                  # test pattern
python3 tools/dk5340.py run --monitor --mode QQVGA       # Monitor (--period-ms 0: every frame)
python3 tools/dk5340.py run --mode VGA                   # HM0360 VGA: ~4 s per frame over BLE;
                                                         # Monitor keeps 1 slot, default period 1000 ms
python3 tools/dk5340.py run --cflags 'CAPTURE_STRESS_US=300'   # interrupt stress test
python3 tools/dk5340.py build --pristine                 # after adding a .conf / overlay file
python3 tools/dk5340.py camera --seconds 20              # reset + viewer.py over BLE with RTT + snapshot
../host/.venv/bin/python ../host/viewer.py --board hm0360-5340 --rtt --reset
# both boards (stream build flashed on each): Space takes a left + right pair, S saves it
../host/.venv/bin/python ../host/stereo.py      # --flash VGA: flash VGA to both first; --auto 5: no keyboard
../host/.venv/bin/python ../host/stitch.py ../host/captures/stereo   # stitch saved pairs offline
```

- NCS: `nrfutil sdk-manager install v3.2.1` (macOS: `/opt/nordic/ncs`), found through
  `nrfutil sdk-manager toolchain env`; otherwise `NCS_DIR` (default `D:\NRFSDK`) with toolchain
  `NCS_TOOLCHAIN_ID` (default `66cdf9b75e`), the Windows install. Shared code: `../tools/devtools.py`.
- Flash runner: `nrfjprog` where installed (Windows), else `nrfutil` (macOS; `WEST_RUNNER` overrides).
- J-Link tools from `JLINK_DIR`, else `/Applications/SEGGER/JLink` or `C:\Program Files\SEGGER\JLink`.
- Zephyr's devicetree step fails on the spaces in the repo path, so the script builds through
  drive `V:` (`subst`, Windows) or a symlink `~/.cache/vuebuds-src/<hash>` (macOS / Linux):
  `<that>/fw_nrf5340/build_<variant>`. A repo path without spaces is built in place.
- Options reach the app image as `-Dfw_nrf5340_CAM_MODE=…` (sysbuild image prefix), read in
  `CMakeLists.txt` with `zephyr_get(... SYSBUILD LOCAL)`.
- Shell: `source tools/ncs_env.sh` sets up the same toolchain for `west` by hand.

| Board | J-Link SN | BLE address |
|---|---|---|
| HM0360 | 1050035314 | DA:E7:DE:69:E3:5E |
| HM0360B (HM01B0 until 10-06) | 1050017384 | EA:F1:6D:2A:C0:BB |

## Not ported

`fw/wiring_test.c`, and the VueBuds board's PMU, IMU, buttons, LEDs, CLI and power-off path.
