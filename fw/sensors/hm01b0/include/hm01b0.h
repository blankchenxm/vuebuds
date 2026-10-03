/*
 * HM01B0 driver, laid out like components/hm01b0 in blankchenxm/hm01b0-esp-idf-driver.
 * The nRF capture path (MCLK, SPIS, frame sync) lives in capture/, not here.
 */
#ifndef HM01B0_H
#define HM01B0_H

#include <stddef.h>
#include <stdint.h>

#include "sdk_errors.h"
#include "hm01b0_types.h"

#define HM01B0_I2C_ADDRESS       CAMERA_SENSOR_I2C_ADDR
#define HM01B0_EXPECTED_MODEL_ID 0x01B0U

extern const camera_sensor_t hm01b0_sensor;

/* Read MODEL_ID_H/L and verify it is 0x01B0. */
ret_code_t hm01b0_probe(uint16_t *model_id);

/* Probe, enter standby and apply the base, mode, common and 1-bit interface tables. */
ret_code_t hm01b0_init(camera_mode_t mode, camera_test_pattern_t pattern);

/* Enter software standby. Configuration changes are allowed in this state. */
ret_code_t hm01b0_standby(void);

/* Start continuous streaming from standby. */
ret_code_t hm01b0_stream_start(void);

/* Stop streaming and return to standby. */
ret_code_t hm01b0_stream_stop(void);

/* Apply a QVGA/QQVGA mode table while in standby. */
ret_code_t hm01b0_set_mode(camera_mode_t mode);

/* Apply a data interface table while in standby. Only 1-bit is wired on this board. */
ret_code_t hm01b0_set_interface(hm01b0_interface_t interface);

/* Select test pattern off / color bar / walking 1 while in standby. */
ret_code_t hm01b0_set_test_pattern(camera_test_pattern_t pattern);

/* Transport and standard geometry of a mode. Does not touch the bus. */
ret_code_t hm01b0_get_mode_info(camera_mode_t mode, camera_mode_info_t *info);

hm01b0_state_t hm01b0_get_state(void);

/* Software reset (all registers to defaults, standby). The module has no reset pin. */
ret_code_t hm01b0_reset(void);

uint8_t hm01b0_reg_read(uint16_t addr);
void hm01b0_reg_write(uint16_t addr, uint8_t value);
void hm01b0_reg_update_bits(uint16_t addr, uint8_t mask, uint8_t value);
/* Apply a register table; only allowed in standby. */
ret_code_t hm01b0_write_table(const hm01b0_regval_t *table, size_t count);

#endif /* HM01B0_H */
