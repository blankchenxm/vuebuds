#include "nrf_log.h"
#include "nrf_delay.h"
#include "i2c.h"
#include "hm01b0.h"
#include "private_include/hm01b0_private.h"
#include "private_include/hm01b0_regs.h"
#include "private_include/hm01b0_tables.h"

hm01b0_dev_t hm01b0_dev;

#define RETURN_IF_ERROR(expr)              \
  do {                                     \
    ret_code_t err_ = (expr);              \
    if (err_ != NRF_SUCCESS) return err_;  \
  } while (0)

static ret_code_t require_standby(void)
{
  return hm01b0_dev.state == HM01B0_STATE_STANDBY ? NRF_SUCCESS : NRF_ERROR_INVALID_STATE;
}

ret_code_t hm01b0_probe(uint16_t *model_id)
{
  uint16_t id = ((uint16_t)hm01b0_reg_read(HM01B0_REG_MODEL_ID_H) << 8) |
                hm01b0_reg_read(HM01B0_REG_MODEL_ID_L);
  if (model_id != NULL) {
    *model_id = id;
  }
  if (id != HM01B0_EXPECTED_MODEL_ID) {
    NRF_LOG_RAW_INFO("[hm01b0] unexpected model id 0x%04X\n", id);
    return NRF_ERROR_NOT_FOUND;
  }
  return NRF_SUCCESS;
}

ret_code_t hm01b0_reset(void)
{
  // If the previous firmware left the sensor streaming, let the current frame finish
  // first: a reset written mid-frame sometimes took effect later and wiped the tables
  // written right after it (measured: 1 of 3 QVGA -> QQVGA switches).
  hm01b0_reg_write(HM01B0_REG_MODE_SELECT, HM01B0_MODE_STANDBY);
  nrf_delay_ms(HM01B0_FRAME_DRAIN_MS);
  // Write-only, so no read-back (i2cWrite16 would verify it).
  i2cWrite16NoVerify(HM01B0_I2C_ADDRESS, HM01B0_REG_SW_RESET, HM01B0_SOFTWARE_RESET);
  nrf_delay_ms(HM01B0_RESET_RECOVERY_MS);
  hm01b0_dev.state = HM01B0_STATE_STANDBY;
  return NRF_SUCCESS;
}

ret_code_t hm01b0_standby(void)
{
  hm01b0_reg_write(HM01B0_REG_MODE_SELECT, HM01B0_MODE_STANDBY);
  hm01b0_dev.state = HM01B0_STATE_STANDBY;
  return NRF_SUCCESS;
}

ret_code_t hm01b0_stream_start(void)
{
  RETURN_IF_ERROR(require_standby());
  hm01b0_reg_write(HM01B0_REG_MODE_SELECT, HM01B0_MODE_STREAMING);
  hm01b0_dev.state = HM01B0_STATE_STREAMING;
  return NRF_SUCCESS;
}

ret_code_t hm01b0_stream_stop(void)
{
  if (hm01b0_dev.state == HM01B0_STATE_UNINITIALIZED) {
    return NRF_ERROR_INVALID_STATE;
  }
  if (hm01b0_dev.state == HM01B0_STATE_STANDBY) {
    return NRF_SUCCESS;
  }
  return hm01b0_standby();
}

ret_code_t hm01b0_set_mode(camera_mode_t mode)
{
  RETURN_IF_ERROR(require_standby());
  switch (mode) {
    case CAMERA_MODE_QVGA:
      RETURN_IF_ERROR(hm01b0_write_table(hm01b0_mode_qvga, hm01b0_mode_qvga_count));
      break;
    case CAMERA_MODE_QQVGA:
      RETURN_IF_ERROR(hm01b0_write_table(hm01b0_mode_qqvga, hm01b0_mode_qqvga_count));
      break;
    default:
      return NRF_ERROR_INVALID_PARAM;
  }
  hm01b0_dev.mode = mode;
  return NRF_SUCCESS;
}

ret_code_t hm01b0_set_interface(hm01b0_interface_t interface)
{
  RETURN_IF_ERROR(require_standby());
  // Only D0 is wired to the nRF (SPIS MOSI), so 1-bit is the only usable interface.
  if (interface != HM01B0_DATA_INTERFACE_1_BIT) {
    return NRF_ERROR_NOT_SUPPORTED;
  }
  RETURN_IF_ERROR(hm01b0_write_table(hm01b0_interface_1bit, hm01b0_interface_1bit_count));
  hm01b0_dev.interface = interface;
  return NRF_SUCCESS;
}

ret_code_t hm01b0_set_test_pattern(camera_test_pattern_t pattern)
{
  RETURN_IF_ERROR(require_standby());
  uint8_t value;
  switch (pattern) {
    case CAMERA_TEST_PATTERN_OFF:       value = HM01B0_TEST_PATTERN_REG_DISABLED; break;
    case CAMERA_TEST_PATTERN_COLOR_BAR: value = HM01B0_TEST_PATTERN_REG_COLOR_BAR; break;
    case CAMERA_TEST_PATTERN_WALKING_1: value = HM01B0_TEST_PATTERN_REG_WALKING_1; break;
    default: return NRF_ERROR_INVALID_PARAM;
  }
  hm01b0_reg_write(HM01B0_REG_TEST_PATTERN_MODE, value);
  hm01b0_dev.test_pattern = pattern;
  return NRF_SUCCESS;
}

ret_code_t hm01b0_init(camera_mode_t mode, camera_test_pattern_t pattern)
{
  RETURN_IF_ERROR(hm01b0_probe(NULL));
  // The module has no reset pin and a DK reset does not power-cycle it: without this the
  // previous firmware's mode (e.g. QVGA) can survive the first init of a QQVGA build.
  RETURN_IF_ERROR(hm01b0_reset());
  RETURN_IF_ERROR(hm01b0_standby());
  RETURN_IF_ERROR(hm01b0_write_table(hm01b0_base_init, hm01b0_base_init_count));
  RETURN_IF_ERROR(hm01b0_set_test_pattern(pattern));
  RETURN_IF_ERROR(hm01b0_set_mode(mode));
  RETURN_IF_ERROR(hm01b0_write_table(hm01b0_common_init, hm01b0_common_init_count));
  RETURN_IF_ERROR(hm01b0_set_interface(HM01B0_DATA_INTERFACE_1_BIT));
  return NRF_SUCCESS;
}

hm01b0_state_t hm01b0_get_state(void)
{
  return hm01b0_dev.state;
}

const camera_sensor_t hm01b0_sensor = {
  .name = "HM01B0",
  .model_id = HM01B0_EXPECTED_MODEL_ID,
  .init = hm01b0_init,
  .stream_start = hm01b0_stream_start,
  .stream_stop = hm01b0_stream_stop,
  .get_mode_info = hm01b0_get_mode_info,
};
