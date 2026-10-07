#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/sys/printk.h>

#include "i2c.h"

static const struct device *const i2cDev = DEVICE_DT_GET(DT_NODELABEL(i2c1));

void i2cInit(void)
{
  if (!device_is_ready(i2cDev)) {
    printk("[i2c] device not ready\n");
  }
}

static int write16(uint8_t addr, uint16_t reg, uint8_t data)
{
  uint8_t bytes[3] = {(reg >> 8) & 0xFF, reg & 0xFF, data};
  int err = i2c_write(i2cDev, bytes, sizeof(bytes), addr);
  if (err) {
    printk("[i2c] write nack\n");
  }
  return err;
}

void i2cWrite16(uint8_t addr, uint16_t reg, uint8_t data)
{
  write16(addr, reg, data);
  if (i2cRead16(addr, reg) != data) {
    printk("[i2c] failed to write addr:0x%x reg:0x%04x data:0x%04x\n", addr, reg, data);
  }
}

void i2cWrite16NoVerify(uint8_t addr, uint16_t reg, uint8_t data)
{
  write16(addr, reg, data);
}

uint8_t i2cRead16(uint8_t addr, uint16_t reg)
{
  // fw/ sends the register address and the read as two transfers (STOP in between).
  uint8_t bytes[2] = {(reg >> 8) & 0xFF, reg & 0xFF};
  uint8_t rx = 0;
  if (i2c_write(i2cDev, bytes, sizeof(bytes), addr) || i2c_read(i2cDev, &rx, 1, addr)) {
    printk("[i2c] addr nack\n");
    return 0;
  }
  return rx;
}
