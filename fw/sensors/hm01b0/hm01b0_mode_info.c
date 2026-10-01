#include "hm01b0.h"

/*
 * Transport sizes match the ESP32 driver (hm01b0_mode_info.c). The sensor keeps
 * 2 (QVGA) / 1 (QQVGA) border columns on each side; all rows are valid, and the
 * standard region trims rows only to reach exactly 320x240 / 160x120.
 *
 * Capture timing is VueBuds': CS goes low 5000 / 2500 TIMER4 ticks (16 MHz) after
 * FVLD rises. Line 0 starts right after FVLD and lasts ~324 us (QVGA), so it is
 * skipped on purpose; asserting CS at FVLD instead loses the first byte.
 */
ret_code_t hm01b0_get_mode_info(camera_mode_t mode, camera_mode_info_t *info)
{
  switch (mode) {
    case CAMERA_MODE_QVGA:
      *info = (camera_mode_info_t) {
        .transport_width = 324,
        .transport_height = 244,
        .standard = { .x = 2, .y = 2, .width = 320, .height = 240 },
        .fvld_to_cs_us = 312,
        .first_line = 1,
      };
      return NRF_SUCCESS;

    case CAMERA_MODE_QQVGA:
      *info = (camera_mode_info_t) {
        .transport_width = 162,
        .transport_height = 122,
        .standard = { .x = 1, .y = 1, .width = 160, .height = 120 },
        .fvld_to_cs_us = 156,
        .first_line = 1,
      };
      return NRF_SUCCESS;

    default:
      return NRF_ERROR_INVALID_PARAM;
  }
}
