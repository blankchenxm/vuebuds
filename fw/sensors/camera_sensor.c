#include <stddef.h>

#include "nrf_log.h"
#include "i2c.h"
#include "camera_sensor.h"
#include "hm01b0.h"

static const camera_sensor_t *const m_sensors[] = {
  &hm01b0_sensor,
};

ret_code_t camera_sensor_detect(const camera_sensor_t **sensor)
{
  uint16_t model_id = ((uint16_t)i2cRead16(CAMERA_SENSOR_I2C_ADDR, CAMERA_SENSOR_REG_MODEL_ID_H) << 8) |
                      i2cRead16(CAMERA_SENSOR_I2C_ADDR, CAMERA_SENSOR_REG_MODEL_ID_L);

  for (size_t i = 0; i < sizeof(m_sensors) / sizeof(m_sensors[0]); i++) {
    if (m_sensors[i]->model_id == model_id) {
      NRF_LOG_RAW_INFO("[sensor] model id 0x%04X -> %s\n", model_id, m_sensors[i]->name);
      *sensor = m_sensors[i];
      return NRF_SUCCESS;
    }
  }

  NRF_LOG_RAW_INFO("[sensor] unknown model id 0x%04X\n", model_id);
  *sensor = NULL;
  return NRF_ERROR_NOT_FOUND;
}

const char *camera_mode_name(camera_mode_t mode)
{
  switch (mode) {
    case CAMERA_MODE_QVGA:  return "QVGA";
    case CAMERA_MODE_QQVGA: return "QQVGA";
    default:                return "?";
  }
}
