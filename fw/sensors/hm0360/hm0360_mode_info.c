#include "hm0360.h"

/*
 * In 640x480 window mode (0x3030[0] = 1) the sensor already outputs the
 * standard size, so transport = standard and nothing is cropped on send.
 *
 * CS timing uses the same VueBuds scheme as HM01B0 (TIMER4 delay after FVLD rises).
 * Measured on the DK: with a 20 us delay DMA receives every line from line 0, exactly
 * width x height bytes per frame, and the color bar is aligned in both modes.
 */
ret_code_t hm0360_get_mode_info(camera_mode_t mode, camera_mode_info_t *info)
{
  switch (mode) {
    case CAMERA_MODE_QVGA:
      *info = (camera_mode_info_t) {
        .transport_width = 320,
        .transport_height = 240,
        .standard = { .x = 0, .y = 0, .width = 320, .height = 240 },
        .fvld_to_cs_us = 20,
        .first_line = 0,
      };
      return NRF_SUCCESS;

    case CAMERA_MODE_QQVGA:
      *info = (camera_mode_info_t) {
        .transport_width = 160,
        .transport_height = 120,
        .standard = { .x = 0, .y = 0, .width = 160, .height = 120 },
        .fvld_to_cs_us = 20,
        .first_line = 0,
      };
      return NRF_SUCCESS;

    default:
      return NRF_ERROR_INVALID_PARAM;
  }
}
