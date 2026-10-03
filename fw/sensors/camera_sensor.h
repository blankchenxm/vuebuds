/*
 * Sensor-independent camera interface.
 *
 * Each sensor driver (sensors/<name>/) exports one camera_sensor_t. The capture
 * layer (capture/) and the BLE layer only see camera_mode_info_t, so changing
 * the sensor or the mode does not touch them.
 */
#ifndef CAMERA_SENSOR_H_
#define CAMERA_SENSOR_H_

#include <stdint.h>
#include "sdk_errors.h"

#define CAMERA_SENSOR_I2C_ADDR 0x24
#define CAMERA_SENSOR_REG_MODEL_ID_H 0x0000
#define CAMERA_SENSOR_REG_MODEL_ID_L 0x0001

typedef enum {
  CAMERA_MODE_QVGA = 0,
  CAMERA_MODE_QQVGA,
} camera_mode_t;

typedef enum {
  CAMERA_TEST_PATTERN_OFF = 0,
  CAMERA_TEST_PATTERN_COLOR_BAR,
  CAMERA_TEST_PATTERN_WALKING_1,
} camera_test_pattern_t;

typedef struct {
  uint16_t x;
  uint16_t y;
  uint16_t width;
  uint16_t height;
} camera_rect_t;

/* Geometry and capture timing of one sensor mode. */
typedef struct {
  // What the sensor clocks out per frame; the frame pool slot has this size.
  uint16_t transport_width;
  uint16_t transport_height;
  // Region of the transport frame sent over BLE (320x240 / 160x120).
  camera_rect_t standard;
  // VueBuds' delay from FVLD rise to CS low. Not used since PR #32: CS now goes low in
  // hardware right after line first_line - 1 (or on the FVLD edge when first_line is 0).
  uint16_t fvld_to_cs_us;
  // First transport line DMA receives: lines before it are over before CS is asserted.
  // Their rows in the slot stay zero; the standard region must start at or below it.
  uint16_t first_line;
} camera_mode_info_t;

typedef struct {
  const char *name;
  uint16_t model_id;
  // Configure the sensor for mode/pattern and leave it in standby. MCLK must be running.
  ret_code_t (*init)(camera_mode_t mode, camera_test_pattern_t pattern);
  ret_code_t (*stream_start)(void);
  ret_code_t (*stream_stop)(void);
  ret_code_t (*get_mode_info)(camera_mode_t mode, camera_mode_info_t *info);
} camera_sensor_t;

/* Poll the model ID over I2C until it matches a known driver or timeout_ms passes. MCLK must be running. */
ret_code_t camera_sensor_detect(const camera_sensor_t **sensor, uint32_t timeout_ms);

const char *camera_mode_name(camera_mode_t mode);

#endif /* CAMERA_SENSOR_H_ */
