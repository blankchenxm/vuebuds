/*
 * HM0360 driver, laid out like sensors/hm01b0 (and the ESP32 HM01B0 driver).
 * Power pins (XSHUTDOWN / XSLEEP) and the capture path live outside the driver:
 * camera.c resets the sensor with MCLK running before the driver is used.
 */
#ifndef HM0360_H
#define HM0360_H

#include <stddef.h>
#include <stdint.h>

#include "sdk_errors.h"
#include "hm0360_types.h"

#define HM0360_I2C_ADDRESS       CAMERA_SENSOR_I2C_ADDR
#define HM0360_EXPECTED_MODEL_ID 0x0360U

extern const camera_sensor_t hm0360_sensor;

/* Read MODEL_ID_H/L and verify it is 0x0360. */
ret_code_t hm0360_probe(uint16_t *model_id);

/* Probe, enter standby, then apply the base, test pattern, mode and 1-bit interface tables. */
ret_code_t hm0360_init(camera_mode_t mode, camera_test_pattern_t pattern);

/* Enter Sleep1 (software standby). Configuration changes are allowed in this state. */
ret_code_t hm0360_standby(void);

/* Start continuous streaming from standby. */
ret_code_t hm0360_stream_start(void);

/* Stop streaming and return to standby. */
ret_code_t hm0360_stream_stop(void);

/* Apply a QVGA/QQVGA mode table while in standby. */
ret_code_t hm0360_set_mode(camera_mode_t mode);

/* Apply a data interface table while in standby. Only 1-bit is wired on this board. */
ret_code_t hm0360_set_interface(hm0360_interface_t interface);

/* Select test pattern off / color bar / walking 1 while in standby. */
ret_code_t hm0360_set_test_pattern(camera_test_pattern_t pattern);

/* Transport and standard geometry of a mode. Does not touch the bus. */
ret_code_t hm0360_get_mode_info(camera_mode_t mode, camera_mode_info_t *info);

hm0360_state_t hm0360_get_state(void);

uint8_t hm0360_reg_read(uint16_t addr);
void hm0360_reg_write(uint16_t addr, uint8_t value);
void hm0360_reg_update_bits(uint16_t addr, uint8_t mask, uint8_t value);
/* Apply a register table; only allowed in standby. */
ret_code_t hm0360_write_table(const hm0360_regval_t *table, size_t count);

#endif /* HM0360_H */
