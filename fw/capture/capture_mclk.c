/*
 * Camera MCLK: TIMER3 at 16 MHz with CC[0] = 1 and auto-clear, its COMPARE0
 * event toggling CAM_MCLK_IN_FROM_MCU through PPI + GPIOTE -> 8 MHz square wave.
 * Only 8/N MHz can be produced this way.
 */
#include "nrf_drv_gpiote.h"
#include "nrf_drv_ppi.h"
#include "nrf_drv_timer.h"
#include "app_error.h"

#include "gpio.h"
#include "capture.h"

static const nrf_drv_timer_t m_mclk_timer = NRF_DRV_TIMER_INSTANCE(3);
static bool m_initialized = false;

static void timer_dummy_handler(nrf_timer_event_t event_type, void *p_context) {}

void capture_mclk_init(void)
{
  if (m_initialized) {
    capture_mclk_enable(true);
    return;
  }

  ret_code_t err_code = nrf_drv_ppi_init();
  if (err_code != NRF_ERROR_MODULE_ALREADY_INITIALIZED) {
    APP_ERROR_CHECK(err_code);
  }

  nrf_drv_timer_config_t timer_cfg = NRF_DRV_TIMER_DEFAULT_CONFIG;
  err_code = nrf_drv_timer_init(&m_mclk_timer, &timer_cfg, timer_dummy_handler);
  APP_ERROR_CHECK(err_code);
  nrf_drv_timer_extended_compare(&m_mclk_timer, NRF_TIMER_CC_CHANNEL0, 1UL,
                                 NRF_TIMER_SHORT_COMPARE0_CLEAR_MASK, false);

  nrf_drv_gpiote_out_config_t out_config = GPIOTE_CONFIG_OUT_TASK_TOGGLE(false);
  err_code = nrf_drv_gpiote_out_init(CAM_MCLK_IN_FROM_MCU, &out_config);
  APP_ERROR_CHECK(err_code);

  nrf_ppi_channel_t ppi_channel;
  err_code = nrf_drv_ppi_channel_alloc(&ppi_channel);
  APP_ERROR_CHECK(err_code);
  err_code = nrf_drv_ppi_channel_assign(ppi_channel,
                                        nrf_drv_timer_event_address_get(&m_mclk_timer, NRF_TIMER_EVENT_COMPARE0),
                                        nrf_drv_gpiote_out_task_addr_get(CAM_MCLK_IN_FROM_MCU));
  APP_ERROR_CHECK(err_code);
  err_code = nrf_drv_ppi_channel_enable(ppi_channel);
  APP_ERROR_CHECK(err_code);

  nrf_drv_gpiote_out_task_enable(CAM_MCLK_IN_FROM_MCU);
  // High drive for clean 8 MHz edges.
  nrf_gpio_cfg(CAM_MCLK_IN_FROM_MCU, NRF_GPIO_PIN_DIR_OUTPUT, NRF_GPIO_PIN_INPUT_DISCONNECT,
               NRF_GPIO_PIN_NOPULL, NRF_GPIO_PIN_H0H1, NRF_GPIO_PIN_NOSENSE);

  m_initialized = true;
  capture_mclk_enable(true);
}

void capture_mclk_enable(bool enable)
{
  if (!m_initialized) {
    return;
  }
  if (enable) {
    nrf_drv_timer_enable(&m_mclk_timer);
  } else {
    nrf_drv_timer_disable(&m_mclk_timer);
  }
}

void capture_mclk_shutdown(void)
{
  if (!m_initialized) {
    return;
  }
  nrf_drv_timer_disable(&m_mclk_timer);
  m_mclk_timer.p_reg->TASKS_SHUTDOWN = 1;
}
