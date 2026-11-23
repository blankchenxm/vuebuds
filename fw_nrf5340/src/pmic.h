/*
 * V3 board (balsamic3, hub #83) power: MAX77650 PMIC at 0x48 on the camera I2C (TWIM1). Only
 * built with BOARD_V3 (tools/dk5340.py --board V3L / V3R); the DK has no PMIC.
 *
 * Rails (kicad-agent-flow boards/balsamic3/board.yaml): LDO = +1V8 (nRF, PSRAM, camera IOVDD),
 * SBB1 = +1V2 (camera DVDD), SBB2 = +2V8 (camera AVDD), SBB0 = LDO input. SBB2 powers up at
 * 3.3 V (OTP default of the MAX77650A), above the HM0360's AVDD range, so pmicInit() lowers it to
 * 2.8 V first thing in main(), as fw_v3_test did on V3 (verified 2026-10), then the LDO from its
 * 1.85 V default to 1.8 V (board.yaml "fw sets 1.8V", hub #92). Nothing else is changed: SBB0 stays
 * at 2.05 V (board.yaml allows 2.0 V; not done, it only cuts the LDO headroom), VSYS_REG, LEDs,
 * charger and the POWER button are not touched (F7 is outside hub #83 / #92). XSHUTDN has a 10 k pull-down, so the camera stays in reset
 * until the firmware raises it (camera.c, after this).
 */
#ifndef PMIC_H_
#define PMIC_H_

#include <stdbool.h>
#include <stdint.h>

#define PMIC_I2C_ADDR 0x48

/* SBB2 -> 2.8 V, LDO -> 1.8 V, then one log line per register group ([pmic] ...). False if the PMIC does not
 * answer or a read-back is not the target. */
bool pmicInit(void);

#endif
