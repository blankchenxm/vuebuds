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
- `src/ble.c`: Zephyr GATT service with the same UUIDs and packet format as `fw/ble_cus.c`.
  The network core runs Nordic's `ipc_radio` image (`sysbuild/ipc_radio/prj.conf`).

## Wiring

Obsidian note `02-daily_raw/10.5~10.11/nrf5340迁移.md` §2, `src/pins.h`. Same as the nRF52840
DK except FVLD → P1.09, LVLD → P1.10, CS loopback jumper P1.06 → P1.07.

## Build / flash / log (PowerShell, NCS v3.2.1 in `D:\NRFSDK`)

```powershell
.\tools\dk5340.ps1 run -Seconds 15                 # build + flash + RTT (HM0360 board)
.\tools\dk5340.ps1 run -Board HM0360B -Mode QQVGA  # other board / mode
.\tools\dk5340.ps1 run -ColorBar                   # test pattern
.\tools\dk5340.ps1 run -Monitor -Mode QQVGA        # Monitor (-PeriodMs 0: every frame)
.\tools\dk5340.ps1 run -Mode VGA                   # HM0360 VGA: ~4 s per frame over BLE;
                                                   # Monitor keeps 1 slot, default period 1000 ms
.\tools\dk5340.ps1 run -Cflags 'CAPTURE_STRESS_US=300'   # interrupt stress test
.\tools\dk5340.ps1 build -Pristine                 # after adding a .conf / overlay file
..\host\.venv\Scripts\python.exe ..\host\viewer.py --board hm0360-5340 --rtt --reset
```

- Zephyr's devicetree step fails on the spaces in the repo path, so the script maps the repo
  to drive `V:` (`subst`) and builds in `V:\fw_nrf5340\build_<variant>`.
- Options reach the app image as `-Dfw_nrf5340_CAM_MODE=…` (sysbuild image prefix), read in
  `CMakeLists.txt` with `zephyr_get(... SYSBUILD LOCAL)`.
- Bash: `source tools/ncs_env.sh` sets up the same toolchain.

| Board | J-Link SN | BLE address |
|---|---|---|
| HM0360 | 1050035314 | DA:E7:DE:69:E3:5E |
| HM0360B (HM01B0 until 10-06) | 1050017384 | EA:F1:6D:2A:C0:BB |

## Not ported

`fw/wiring_test.c`, and the VueBuds board's PMU, IMU, buttons, LEDs, CLI and power-off path.
