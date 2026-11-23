/*
 * MAX77650 on the V3 board (pmic.h). Register addresses and the SBB2 code come from
 * fw_v3_test/src/main.c (MAX77650/51 programmer's guide; SBB2 verified on V3 2026-10). The other
 * rails' voltages are decoded with the datasheet's step sizes; the LDO step is confirmed by its OTP
 * default (0x28 = 1.85 V read back on V3, hub #83). Not metered (Xiaomeng: register read-back only).
 */
#ifdef BOARD_V3

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/sys/printk.h>

#include "pmic.h"

#define REG_STAT_CHG_B  0x03
#define REG_ERCFLAG     0x04
#define REG_STAT_GLBL   0x05  // DIDM [7:6] (0 = MAX77650), STAT_EN [2] (nEN low), STAT_PWR_HLD [1]
#define REG_CNFG_GLBL   0x10
#define REG_CID         0x11  // [3:0] OTP option
#define REG_CNFG_SBB0_A 0x29  // TV_SBB0 [5:0]: 0.8 V + 25 mV x code
#define REG_CNFG_SBB0_B 0x2A
#define REG_CNFG_SBB1_A 0x2B  // TV_SBB1 [5:0]: 0.8 V + 12.5 mV x code
#define REG_CNFG_SBB1_B 0x2C
#define REG_CNFG_SBB2_A 0x2D  // IP [7:6], TV_SBB2 [5:0]: 0.8 V + 50 mV x code (0x28 = 2.80 V, 0x32 = 3.30 V)
#define REG_CNFG_SBB2_B 0x2E
#define REG_CNFG_LDO_A  0x38  // TV_LDO [6:0]: 1.35 V + 12.5 mV x code
#define REG_CNFG_LDO_B  0x39

#define TV_SBB2_2V8 0x28
#define TV_LDO_1V8  0x24  // 1.35 V + 36 x 12.5 mV; OTP default 0x28 = 1.85 V (read back on both V3 boards, hub #83)

static const struct device *const i2c = DEVICE_DT_GET(DT_NODELABEL(i2c1));

static int rd(uint8_t reg, uint8_t *val)
{
  return i2c_reg_read_byte(i2c, PMIC_I2C_ADDR, reg, val);
}

bool pmicInit(void)
{
  if (!device_is_ready(i2c)) {
    printk("[pmic] i2c1 not ready\n");
    return false;
  }
  // First: SBB2 (camera AVDD) from 3.3 V to 2.8 V, keeping the inductor peak current bits.
  uint8_t sbb2Boot = 0, sbb2 = 0;
  int err = rd(REG_CNFG_SBB2_A, &sbb2Boot);
  if (!err) {
    err = i2c_reg_write_byte(i2c, PMIC_I2C_ADDR, REG_CNFG_SBB2_A, (sbb2Boot & 0xc0) | TV_SBB2_2V8);
  }
  if (!err) {
    err = rd(REG_CNFG_SBB2_A, &sbb2);
  }
  if (err) {
    printk("[pmic] MAX77650 at 0x48 does not answer (%d): +2V8 not set\n", err);
    return false;
  }

  // Then the LDO (+1V8: nRF VDD, PSRAM, camera IOVDD, J-Link VTref) from 1.85 V down to 1.8 V
  // (board.yaml "fw sets 1.8V"; hub #92). Lower only, inside every load's range (PSRAM 1.62-1.98 V,
  // nRF 1.7-3.6 V). Bit 7 is kept as read.
  uint8_t ldoBoot = 0;
  err = rd(REG_CNFG_LDO_A, &ldoBoot);
  if (!err) {
    err = i2c_reg_write_byte(i2c, PMIC_I2C_ADDR, REG_CNFG_LDO_A, (ldoBoot & 0x80) | TV_LDO_1V8);
  }
  if (err) {
    printk("[pmic] LDO not set (%d)\n", err);
  }

  uint8_t stat = 0, cid = 0, glbl = 0, erc = 0, chgB = 0, s0a = 0, s0b = 0, s1a = 0, s1b = 0, s2b = 0, la = 0, lb = 0;
  err = rd(REG_STAT_GLBL, &stat) | rd(REG_CID, &cid) | rd(REG_CNFG_GLBL, &glbl) | rd(REG_ERCFLAG, &erc) |
        rd(REG_STAT_CHG_B, &chgB) | rd(REG_CNFG_SBB0_A, &s0a) | rd(REG_CNFG_SBB0_B, &s0b) | rd(REG_CNFG_SBB1_A, &s1a) |
        rd(REG_CNFG_SBB1_B, &s1b) | rd(REG_CNFG_SBB2_B, &s2b) | rd(REG_CNFG_LDO_A, &la) | rd(REG_CNFG_LDO_B, &lb);
  uint8_t didm = stat >> 6;
  printk("[pmic] %s: STAT_GLBL 0x%02x (DIDM %u, nEN %s, PWR_HLD %s), CID 0x%x, CNFG_GLBL 0x%02x, ERCFLAG 0x%02x, "
         "STAT_CHG_B 0x%02x (CHGIN_DTLS %u)%s\n",
         didm == 0 ? "MAX77650" : "not a MAX77650", stat, didm, (stat & 0x04) ? "low" : "high",
         (stat & 0x02) ? "high" : "low", cid & 0x0f, glbl, erc, chgB, (chgB >> 2) & 3, err ? " (some reads failed)" : "");
  printk("[pmic] +2V8 SBB2 0x%02x -> 0x%02x: %u -> %u mV (B 0x%02x); measure TP26, expect ~2.80 V\n", sbb2Boot, sbb2,
         800 + 50 * (sbb2Boot & 0x3f), 800 + 50 * (sbb2 & 0x3f), s2b);
  printk("[pmic] +1V8 LDO 0x%02x -> 0x%02x: %u -> %u mV (B 0x%02x)\n", ldoBoot, la, 1350 + (125 * (ldoBoot & 0x7f)) / 10,
         1350 + (125 * (la & 0x7f)) / 10, lb);
  printk("[pmic] +1V8 LDO 0x%02x ~%u mV (B 0x%02x), +1V2 SBB1 0x%02x ~%u mV (B 0x%02x), VSBB0 0x%02x ~%u mV (B 0x%02x); "
         "datasheet steps, not yet checked against TP25 / TP27 / TP28\n",
         la, 1350 + (125 * (la & 0x7f)) / 10, lb, s1a, 800 + (125 * (s1a & 0x3f)) / 10, s1b, s0a, 800 + 25 * (s0a & 0x3f),
         s0b);
  return didm == 0 && (sbb2 & 0x3f) == TV_SBB2_2V8 && (la & 0x7f) == TV_LDO_1V8;
}

#endif /* BOARD_V3 */
