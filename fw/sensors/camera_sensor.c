#include <stddef.h>

#include "nrf_log.h"
#include "i2c.h"
#include "timers.h"
#include "camera_sensor.h"
#include "hm01b0.h"
#include "hm0360.h"

#define DETECT_POLL_INTERVAL_MS 50

static const camera_sensor_t *const m_sensors[] = {
  &hm01b0_sensor,
  &hm0360_sensor,
};

static const camera_sensor_t *find_sensor(uint16_t model_id)
{
  for (size_t i = 0; i < sizeof(m_sensors) / sizeof(m_sensors[0]); i++) {
    if (m_sensors[i]->model_id == model_id) {
      return m_sensors[i];
    }
  }
  return NULL;
}

static uint16_t read_model_id(void)
{
  return ((uint16_t)i2cRead16(CAMERA_SENSOR_I2C_ADDR, CAMERA_SENSOR_REG_MODEL_ID_H) << 8) |
         i2cRead16(CAMERA_SENSOR_I2C_ADDR, CAMERA_SENSOR_REG_MODEL_ID_L);
}

ret_code_t camera_sensor_detect(const camera_sensor_t **sensor, uint32_t timeout_ms)
{
  // HM0360 needs some time after XSHUTDOWN before it answers, so poll instead of a fixed delay.
  uint32_t start = systemTimeGetMs();
  uint32_t tries = 0;
  uint16_t model_id;
  do {
    tries++;
    model_id = read_model_id();
    *sensor = find_sensor(model_id);
    if (*sensor != NULL) {
      NRF_LOG_RAW_INFO("[sensor] model id 0x%04X -> %s (try %u, %u ms)\n", model_id, (*sensor)->name,
                       tries, systemTimeGetMs() - start);
      return NRF_SUCCESS;
    }
    delayMs(DETECT_POLL_INTERVAL_MS);
  } while (systemTimeGetMs() - start < timeout_ms);

  NRF_LOG_RAW_INFO("[sensor] no known sensor after %u tries / %u ms, last id 0x%04X\n",
                   tries, systemTimeGetMs() - start, model_id);
  return NRF_ERROR_NOT_FOUND;
}

const char *camera_mode_name(camera_mode_t mode)
{
  switch (mode) {
    case CAMERA_MODE_QVGA:  return "QVGA";
    case CAMERA_MODE_QQVGA: return "QQVGA";
    case CAMERA_MODE_VGA:   return "VGA";
    default:                return "?";
  }
}
