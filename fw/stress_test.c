/*
 * Interrupt-latency stress test (build with -DCAPTURE_STRESS_US=<busy time>, streaming only):
 * RTC2 fires every ~7 ms (jittered) at interrupt priority 2, above every capture interrupt,
 * and busy-waits CAPTURE_STRESS_US inside. Any capture step that relies on an interrupt
 * meeting a line-blanking deadline then fails regularly; steps done by PPI do not.
 */
#ifdef CAPTURE_STRESS_US

#include "nrfx_rtc.h"
#include "nrf_delay.h"
#include "app_error.h"
#include "nrf_log.h"

#define STRESS_IRQ_PRIORITY 2
#define STRESS_PERIOD_TICKS 229u  // ~7 ms at 32.768 kHz

static const nrfx_rtc_t m_rtc = NRFX_RTC_INSTANCE(2);
static uint32_t m_jitter = 1;

static void stress_handler(nrfx_rtc_int_type_t int_type)
{
  // Vary the period (+0..31 ticks) so the busy windows sweep across every line of a frame.
  m_jitter = m_jitter * 1103515245u + 12345u;
  uint32_t next = (nrfx_rtc_counter_get(&m_rtc) + STRESS_PERIOD_TICKS + ((m_jitter >> 16) & 31u)) & 0xFFFFFFu;
  APP_ERROR_CHECK(nrfx_rtc_cc_set(&m_rtc, 0, next, true));
  nrf_delay_us(CAPTURE_STRESS_US);
}

void stressTestStart(void)
{
  nrfx_rtc_config_t config = NRFX_RTC_DEFAULT_CONFIG;
  config.prescaler = 0;
  config.interrupt_priority = STRESS_IRQ_PRIORITY;
  APP_ERROR_CHECK(nrfx_rtc_init(&m_rtc, &config, stress_handler));
  APP_ERROR_CHECK(nrfx_rtc_cc_set(&m_rtc, 0, STRESS_PERIOD_TICKS, true));
  nrfx_rtc_enable(&m_rtc);
  NRF_LOG_RAW_INFO("[stress] RTC2 every ~7 ms, %u us busy at priority %u\n", CAPTURE_STRESS_US, STRESS_IRQ_PRIORITY);
}

#endif
