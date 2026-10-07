/*
 * Interrupt-latency stress test, port of fw/stress_test.c (build with
 * -Cflags 'CAPTURE_STRESS_US=<busy time>', streaming only): RTC0 fires every ~7 ms
 * (jittered) at interrupt priority 0, above every capture interrupt (line counter and
 * GPIOTE: 1), and busy-waits CAPTURE_STRESS_US inside. Any capture step that relies on an
 * interrupt meeting a line-blanking deadline then fails regularly; steps done by DPPI do not.
 * fw/ used RTC2 at priority 2; the application core has only RTC0 / RTC1 (kernel clock).
 */
#ifdef CAPTURE_STRESS_US

#include <zephyr/kernel.h>
#include <zephyr/irq.h>

#include "nrf_log.h"

#define STRESS_RTC NRF_RTC0
#define STRESS_IRQ_PRIORITY 0
#define STRESS_PERIOD_TICKS 229u  // ~7 ms at 32.768 kHz

static uint32_t m_jitter = 1;

static void stress_isr(const void *arg)
{
  STRESS_RTC->EVENTS_COMPARE[0] = 0;
  // Vary the period (+0..31 ticks) so the busy windows sweep across every line of a frame.
  m_jitter = m_jitter * 1103515245u + 12345u;
  STRESS_RTC->CC[0] = (STRESS_RTC->COUNTER + STRESS_PERIOD_TICKS + ((m_jitter >> 16) & 31u)) & 0xFFFFFFu;
  k_busy_wait(CAPTURE_STRESS_US);
}

void stressTestStart(void)
{
  STRESS_RTC->PRESCALER = 0;
  STRESS_RTC->CC[0] = STRESS_PERIOD_TICKS;
  STRESS_RTC->EVTENSET = RTC_EVTEN_COMPARE0_Msk;
  STRESS_RTC->INTENSET = RTC_INTENSET_COMPARE0_Msk;
  IRQ_CONNECT(RTC0_IRQn, STRESS_IRQ_PRIORITY, stress_isr, NULL, 0);
  irq_enable(RTC0_IRQn);
  STRESS_RTC->TASKS_CLEAR = 1;
  STRESS_RTC->TASKS_START = 1;
  NRF_LOG_RAW_INFO("[stress] RTC0 every ~7 ms, %u us busy at priority %u\n", CAPTURE_STRESS_US, STRESS_IRQ_PRIORITY);
}

#endif
