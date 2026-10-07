/* Camera I2C (TWIM1 at 100 kHz on P0.26 SDA / P0.27 SCL), same functions as fw/i2c.h. */
#ifndef I2C_H_
#define I2C_H_

#include <stdbool.h>
#include <stdint.h>

#define CAMERA_I2C_ADDR 0x24

void i2cInit(void);
void i2cWrite16(uint8_t addr, uint16_t reg, uint8_t data);  // reads the register back, logs a mismatch
void i2cWrite16NoVerify(uint8_t addr, uint16_t reg, uint8_t data);
uint8_t i2cRead16(uint8_t addr, uint16_t reg);              // 0 on a NACK, like fw/

#endif
