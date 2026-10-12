# fw_bridge: BLE-USB bridge

An nRF52840 DK (later an nRF5340 DK, same code) is the BLE central for the two camera boards
(`fw_nrf5340/`) and forwards their notifications to the PC over USB, like the VueBuds paper's
bridge (§3.3.1). The bridge, not Windows, sets the link, so both boards get the same, steady speed.

```
 left 5340 (hm0360-5340)  ── BLE link, slot 0 ──┐
                                                ├── 52840 DK bridge ── USB CDC ACM ("nRF USB" port) ── PC
 right 5340 (hm0360b-5340) ── BLE link, slot 1 ──┘
```

## Hardware

- 52840 DK, J-Link SN `1050221517` (was the HM01B0 board until 2026-10-10; `fw/` and the
  SoftDevice are erased, `fw/tools/dk.ps1 flash-sd` + `flash` bring them back).
- Two USB cables: the J-Link port (flash, RTT) and the **nRF USB** port on the side (data).
  The PC sees a COM port with Zephyr's VID `0x2FE3` (`host/bridge.py` finds it by VID).

## Build / flash (PowerShell, from `fw_bridge/`)

```powershell
.\tools\bridge.ps1 flash             # build + program + reset
.\tools\bridge.ps1 rtt -Seconds 20   # RTT log without a reset (links stay up)
.\tools\bridge.ps1 flash -UsbTest    # USB link test build; then python host\usb_test.py
```

NCS v3.2.1 under `D:\NRFSDK`, as `fw_nrf5340/`. Sysbuild passes image options as
`-Dfw_bridge_<NAME>=...` (e.g. `-Dfw_bridge_CONFIG_...`).

## Link setup (src/central.c, prj.conf)

- Connection interval from the PC (`bridge.Bridge.configure`, default **15 ms**), the same for
  both links; the boards' own requests (fw_nrf5340 asks for 7.5–15 ms after 5 s) are refused.
- 2M PHY, data length 251, ATT MTU 498 (fw_nrf5340 also 498 since 10-11; the smaller side decides).
- Connection event: default 7.5 ms reservation, extended while the radio is free.
- Connects one board at a time, reconnects after a drop (the boards reboot on disconnect).
- PHY / data length / MTU / discovery run on the system work queue: called from the connected
  callback (BT RX thread) they deadlock (`Controller unresponsive, opcode 0x2032`).

## USB protocol

See `host/bridge.py` (PC side) and `src/main.c`. Frames are `[A5 5A][type][slot][len u16][payload]`
to the PC (`N` notification, `E` link event, `S` text, `W` write result) and
`[A5 5A][cmd][len u16][payload]` from the PC (`I`, `C` configure, `W` control write to a slot mask,
`X` disconnect). Slot numbers replace the paper's MAC prefix; the `E` event carries the address.

## Measured (2026-10-11, QVGA unless noted, 0 lost packets in every run)

| | Windows direct | bridge |
|---|---|---|
| one board, MTU 247 | 1.00–1.05 fps (75–79 KB/s) | 1.37–1.40 fps (103–105 KB/s) |
| one board, MTU 498 | 1.07–1.08 fps (81 KB/s) | **1.56 fps (117 KB/s)** |
| two boards streaming | 65 KB/s together, left 2× slower (10-08) | ~55 KB/s each, equal |
| two boards, one VGA pair (`stereo.py`) | 9.2 s, left/right 3.7 s apart (10-08) | **5.6–5.9 s, ~0.1 s apart** |

- Two boards together stay at ~110–116 KB/s in total: that is the bridge's limit (it does not
  change without USB forwarding, nor with unlimited event length).
- 30 ms (and 10 ms) intervals make two links unequal; 7.5 and 15 ms keep them equal.
- USB CDC ACM: ~212 KB/s to the PC (Zephyr sends one 64-byte transfer at a time), above what
  the radio delivers.

## Known

- RTT shows `udc: Failed to allocate net_buf 4095` while Windows enumerates the port; the port works.
