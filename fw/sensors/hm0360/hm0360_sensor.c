#include "nrf_log.h"
#include "timers.h"
#include "hm0360.h"
#include "private_include/hm0360_private.h"
#include "private_include/hm0360_regs.h"
#include "private_include/hm0360_tables.h"

hm0360_dev_t hm0360_dev;

#define RETURN_IF_ERROR(expr)              \
  do {                                     \
    ret_code_t err_ = (expr);              \
    if (err_ != NRF_SUCCESS) return err_;  \
  } while (0)

static ret_code_t require_standby(void)
{
  return hm0360_dev.state == HM0360_STATE_STANDBY ? NRF_SUCCESS : NRF_ERROR_INVALID_STATE;
}

ret_code_t hm0360_probe(uint16_t *model_id)
{
  uint16_t id = ((uint16_t)hm0360_reg_read(HM0360_REG_MODEL_ID_H) << 8) |
                hm0360_reg_read(HM0360_REG_MODEL_ID_L);
  if (model_id != NULL) {
    *model_id = id;
  }
  if (id != HM0360_EXPECTED_MODEL_ID) {
    NRF_LOG_RAW_INFO("[hm0360] unexpected model id 0x%04X\n", id);
    return NRF_ERROR_NOT_FOUND;
  }
  return NRF_SUCCESS;
}

ret_code_t hm0360_standby(void)
{
  hm0360_reg_write(HM0360_REG_MODE_SELECT, HM0360_MODE_SLEEP);
  hm0360_dev.state = HM0360_STATE_STANDBY;
  return NRF_SUCCESS;
}

ret_code_t hm0360_stream_start(void)
{
  RETURN_IF_ERROR(require_standby());
  hm0360_reg_write(HM0360_REG_MODE_SELECT, HM0360_MODE_STREAMING);
  hm0360_dev.state = HM0360_STATE_STREAMING;
  return NRF_SUCCESS;
}

ret_code_t hm0360_stream_stop(void)
{
  if (hm0360_dev.state == HM0360_STATE_UNINITIALIZED) {
    return NRF_ERROR_INVALID_STATE;
  }
  if (hm0360_dev.state == HM0360_STATE_STANDBY) {
    return NRF_SUCCESS;
  }
  return hm0360_standby();
}

ret_code_t hm0360_set_mode(camera_mode_t mode)
{
  RETURN_IF_ERROR(require_standby());
  switch (mode) {
    case CAMERA_MODE_QVGA:
      RETURN_IF_ERROR(hm0360_write_table(hm0360_mode_qvga, hm0360_mode_qvga_count));
      break;
    case CAMERA_MODE_QQVGA:
      RETURN_IF_ERROR(hm0360_write_table(hm0360_mode_qqvga, hm0360_mode_qqvga_count));
      break;
    default:
      return NRF_ERROR_INVALID_PARAM;
  }
  hm0360_dev.mode = mode;
  return NRF_SUCCESS;
}

ret_code_t hm0360_set_interface(hm0360_interface_t interface)
{
  RETURN_IF_ERROR(require_standby());
  // Only D0 is wired to the nRF (SPIS MOSI), so 1-bit is the only usable interface.
  if (interface != HM0360_DATA_INTERFACE_1_BIT) {
    return NRF_ERROR_NOT_SUPPORTED;
  }
  RETURN_IF_ERROR(hm0360_write_table(hm0360_interface_1bit, hm0360_interface_1bit_count));
  hm0360_dev.interface = interface;
  return NRF_SUCCESS;
}

ret_code_t hm0360_set_test_pattern(camera_test_pattern_t pattern)
{
  RETURN_IF_ERROR(require_standby());
  uint8_t value;
  switch (pattern) {
    case CAMERA_TEST_PATTERN_OFF:       value = HM0360_TEST_PATTERN_DISABLED; break;
    case CAMERA_TEST_PATTERN_COLOR_BAR: value = HM0360_TEST_PATTERN_COLOR_BAR; break;
    case CAMERA_TEST_PATTERN_WALKING_1: value = HM0360_TEST_PATTERN_WALKING_1; break;
    default: return NRF_ERROR_INVALID_PARAM;
  }
  hm0360_reg_write(HM0360_REG_TEST_PATTERN_MODE, value);
  hm0360_dev.test_pattern = pattern;
  return NRF_SUCCESS;
}

ret_code_t hm0360_init(camera_mode_t mode, camera_test_pattern_t pattern)
{
  RETURN_IF_ERROR(hm0360_probe(NULL));
  // No software reset: camera.c has just reset the sensor through XSHUTDOWN (registers are at
  // their defaults), and i2cWrite16's immediate read-back of the write-only SW_RESET is unsafe.
  RETURN_IF_ERROR(hm0360_standby());
  RETURN_IF_ERROR(hm0360_write_table(hm0360_base_init, hm0360_base_init_count));
  RETURN_IF_ERROR(hm0360_set_test_pattern(pattern));
  RETURN_IF_ERROR(hm0360_set_mode(mode));
  RETURN_IF_ERROR(hm0360_set_interface(HM0360_DATA_INTERFACE_1_BIT));
  return NRF_SUCCESS;
}

hm0360_state_t hm0360_get_state(void)
{
  return hm0360_dev.state;
}

const camera_sensor_t hm0360_sensor = {
  .name = "HM0360",
  .model_id = HM0360_EXPECTED_MODEL_ID,
  .init = hm0360_init,
  .stream_start = hm0360_stream_start,
  .stream_stop = hm0360_stream_stop,
  .get_mode_info = hm0360_get_mode_info,
};
