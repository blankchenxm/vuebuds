#include "i2c.h"
#include "timers.h"
#include "hm01b0.h"
#include "private_include/hm01b0_private.h"

/* i2cWrite16 reads every register back and logs a mismatch, so failures show up in RTT. */

uint8_t hm01b0_reg_read(uint16_t addr)
{
  return i2cRead16(HM01B0_I2C_ADDRESS, addr);
}

void hm01b0_reg_write(uint16_t addr, uint8_t value)
{
  i2cWrite16(HM01B0_I2C_ADDRESS, addr, value);
}

void hm01b0_reg_update_bits(uint16_t addr, uint8_t mask, uint8_t value)
{
  uint8_t current = hm01b0_reg_read(addr);
  uint8_t updated = (current & (uint8_t)~mask) | (value & mask);
  if (updated != current) {
    hm01b0_reg_write(addr, updated);
  }
}

ret_code_t hm01b0_write_table(const hm01b0_regval_t *table, size_t count)
{
  if (hm01b0_dev.state != HM01B0_STATE_STANDBY) {
    return NRF_ERROR_INVALID_STATE;
  }

  for (size_t i = 0; i < count; i++) {
    if (table[i].mask == UINT8_MAX) {
      hm01b0_reg_write(table[i].addr, table[i].value);
    } else {
      hm01b0_reg_update_bits(table[i].addr, table[i].mask, table[i].value);
    }
    if (table[i].delay_ms > 0) {
      delayMs(table[i].delay_ms);
    }
  }
  return NRF_SUCCESS;
}
