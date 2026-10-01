#include "i2c.h"
#include "timers.h"
#include "hm0360.h"
#include "private_include/hm0360_private.h"

/* i2cWrite16 reads every register back and logs a mismatch, so failures show up in RTT. */

uint8_t hm0360_reg_read(uint16_t addr)
{
  return i2cRead16(HM0360_I2C_ADDRESS, addr);
}

void hm0360_reg_write(uint16_t addr, uint8_t value)
{
  i2cWrite16(HM0360_I2C_ADDRESS, addr, value);
}

void hm0360_reg_update_bits(uint16_t addr, uint8_t mask, uint8_t value)
{
  uint8_t current = hm0360_reg_read(addr);
  uint8_t updated = (current & (uint8_t)~mask) | (value & mask);
  if (updated != current) {
    hm0360_reg_write(addr, updated);
  }
}

ret_code_t hm0360_write_table(const hm0360_regval_t *table, size_t count)
{
  if (hm0360_dev.state != HM0360_STATE_STANDBY) {
    return NRF_ERROR_INVALID_STATE;
  }

  for (size_t i = 0; i < count; i++) {
    if (table[i].mask == UINT8_MAX) {
      hm0360_reg_write(table[i].addr, table[i].value);
    } else {
      hm0360_reg_update_bits(table[i].addr, table[i].mask, table[i].value);
    }
    if (table[i].delay_ms > 0) {
      delayMs(table[i].delay_ms);
    }
  }
  return NRF_SUCCESS;
}
