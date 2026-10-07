/*
 * Camera MCLK: TIMER0 at 16 MHz with CC[0] = 1 and auto-clear, its COMPARE0 event
 * toggling CAM_MCLK_IN_FROM_MCU through DPPI + GPIOTE -> 8 MHz square wave.
 * Same as fw/ (which used TIMER3 + PPI); the application core has only TIMER0-2.
 */
#include <zephyr/kernel.h>
#include <nrfx_gpiote.h>

#include "pins.h"
#include "capture.h"

#define MCLK_TIMER NRF_TIMER0

static bool m_initialized = false;
static bool m_running = false;

void capture_mclk_init(void)
{
  if (m_initialized) {
    capture_mclk_enable(true);
    return;
  }
  nrfx_gpiote_t *gpiote = capture_gpiote();

  MCLK_TIMER->TASKS_STOP = 1;
  MCLK_TIMER->MODE = TIMER_MODE_MODE_Timer;
  MCLK_TIMER->BITMODE = TIMER_BITMODE_BITMODE_16Bit;
  MCLK_TIMER->PRESCALER = 0;  // 16 MHz
  MCLK_TIMER->CC[0] = 1;
  MCLK_TIMER->SHORTS = TIMER_SHORTS_COMPARE0_CLEAR_Msk;
  MCLK_TIMER->TASKS_CLEAR = 1;

  uint8_t ch;
  CAPTURE_CHECK(nrfx_gpiote_channel_alloc(gpiote, &ch) == 0);
  // High drive for clean 8 MHz edges.
  nrfx_gpiote_output_config_t out = {
    .drive = NRF_GPIO_PIN_H0H1,
    .input_connect = NRF_GPIO_PIN_INPUT_DISCONNECT,
    .pull = NRF_GPIO_PIN_NOPULL,
  };
  nrfx_gpiote_task_config_t task = {
    .task_ch = ch,
    .polarity = NRF_GPIOTE_POLARITY_TOGGLE,
    .init_val = NRF_GPIOTE_INITIAL_VALUE_LOW,
  };
  CAPTURE_CHECK(nrfx_gpiote_output_configure(gpiote, CAM_MCLK_IN_FROM_MCU, &out, &task) == 0);
  nrfx_gpiote_out_task_enable(gpiote, CAM_MCLK_IN_FROM_MCU);

  capture_dppi_publish((uint32_t)&MCLK_TIMER->EVENTS_COMPARE[0], CAPTURE_DPPI_MCLK);
  capture_dppi_subscribe(nrfx_gpiote_out_task_address_get(gpiote, CAM_MCLK_IN_FROM_MCU), CAPTURE_DPPI_MCLK);
  NRF_DPPIC->CHENSET = BIT(CAPTURE_DPPI_MCLK);

  m_initialized = true;
  capture_mclk_enable(true);
}

void capture_mclk_enable(bool enable)
{
  if (!m_initialized) {
    return;
  }
  if (enable) {
    MCLK_TIMER->TASKS_START = 1;
  } else {
    MCLK_TIMER->TASKS_STOP = 1;
  }
  m_running = enable;
}

bool capture_mclk_running(void)
{
  return m_initialized && m_running;  // TIMER has no status register on this chip
}

void capture_mclk_shutdown(void)
{
  if (!m_initialized) {
    return;
  }
  MCLK_TIMER->TASKS_STOP = 1;
  m_running = false;
}
