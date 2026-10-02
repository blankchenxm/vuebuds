#include <stdbool.h>
#include <stdint.h>

#include "nrfx_rtc.h"
#include "app_error.h"
#include "nrf_log.h"

#include "camera.h"
#include "capture.h"
#include "event.h"
#include "timers.h"
#include "monitor.h"

#ifndef MONITOR_PERIOD_MS
#define MONITOR_PERIOD_MS 500
#endif
// Frames to let pass after each wake before the one that is stored (user choice: 1).
#ifndef MONITOR_SKIP_FRAMES
#define MONITOR_SKIP_FRAMES 1
#endif

#define MONITOR_MAX_SLOTS 16
#define RTC_HZ 32768u
#define RTC_PERIOD_TICKS ((MONITOR_PERIOD_MS * RTC_HZ) / 1000u)
#define RTC_COUNTER_MASK 0xFFFFFFu  // 24-bit counter
#define HM0360_MODEL_ID 0x0360

typedef enum {
  SLOT_EMPTY = 0,
  SLOT_WRITING,
  SLOT_READY,
} slot_state_t;

typedef struct {
  slot_state_t state;
  uint32_t seq;       // frame number, counting from 1
  uint32_t time_ms;   // when the frame was complete
  bool ok;            // DMA byte counts right, no overflow
} monitor_slot_t;

static const nrfx_rtc_t m_rtc = NRFX_RTC_INSTANCE(2);
static uint32_t m_cc;

static monitor_slot_t m_slots[MONITOR_MAX_SLOTS];
static uint32_t m_slot_count;
static uint32_t m_next;        // oldest slot, written by the next frame
static uint32_t m_seq;
static bool m_running;
static bool m_busy;            // a wake is in progress (sensor awake, slot armed)
static uint32_t m_wake_ms;
static uint32_t m_overruns;
static uint32_t m_dropped;     // bad frames not stored
static bool m_was_asleep;      // XSLEEP low just before this wake
static bool m_mclk_was_on;     // MCLK running just before this wake

static void rtc_handler(nrfx_rtc_int_type_t int_type)
{
  if (int_type == NRFX_RTC_INT_COMPARE0) {
    // Fixed period from the previous compare value, so wake-ups do not drift.
    m_cc = (m_cc + RTC_PERIOD_TICKS) & RTC_COUNTER_MASK;
    APP_ERROR_CHECK(nrfx_rtc_cc_set(&m_rtc, 0, m_cc, true));
    eventQueuePush(EVENT_MONITOR_WAKE);
  }
}

// Mean brightness of the stored frame (every 8th pixel), to spot dark / wrong exposure.
static uint32_t frame_mean(uint32_t slot)
{
  camera_frame_t f;
  if (!cameraSlotFrame(slot, &f)) {
    return 0;
  }
  uint32_t sum = 0, n = 0;
  for (uint16_t y = 0; y < f.height; y += 8) {
    for (uint16_t x = 0; x < f.width; x += 8) {
      sum += f.pixels[(uint32_t)y * f.stride + x];
      n++;
    }
  }
  return n ? sum / n : 0;
}

void monitorStart(void)
{
  cameraInit();
  if (cameraModelId() != HM0360_MODEL_ID) {
    NRF_LOG_RAW_INFO("[mon] not started: Monitor needs an HM0360 (found model id 0x%04X)\n", cameraModelId());
    return;
  }

  m_slot_count = cameraSlotCount();
  if (m_slot_count > MONITOR_MAX_SLOTS) {
    m_slot_count = MONITOR_MAX_SLOTS;
  }

  // Sensor streams from here on; S2 gates it with XSLEEP and MCLK only.
  cameraSensorStream();
  cameraSleep(true);

  nrfx_rtc_config_t config = NRFX_RTC_DEFAULT_CONFIG;
  config.prescaler = RTC_FREQ_TO_PRESCALER(RTC_HZ);
  APP_ERROR_CHECK(nrfx_rtc_init(&m_rtc, &config, rtc_handler));
  m_cc = (nrfx_rtc_counter_get(&m_rtc) + RTC_PERIOD_TICKS) & RTC_COUNTER_MASK;
  APP_ERROR_CHECK(nrfx_rtc_cc_set(&m_rtc, 0, m_cc, true));
  nrfx_rtc_enable(&m_rtc);
  m_running = true;

  NRF_LOG_RAW_INFO("%08d [mon] start: HM0360 %s, %u slots, period %u ms, skip %u frame(s)\n",
                   systemTimeGetMs(), cameraModeName(), m_slot_count, MONITOR_PERIOD_MS, MONITOR_SKIP_FRAMES);
}

void monitorWake(void)
{
  if (!m_running) {
    return;
  }
  if (m_busy) {
    // The previous frame is not done yet: skip this wake rather than re-arm mid-capture.
    m_overruns++;
    NRF_LOG_RAW_INFO("%08d [mon] overrun %u: previous frame not done\n", systemTimeGetMs(), m_overruns);
    return;
  }

  // State between frames, for the acceptance check (expected: XSLEEP low, MCLK off).
  m_was_asleep = cameraIsAsleep();
  m_mclk_was_on = capture_mclk_running();

  m_wake_ms = systemTimeGetMs();
  m_slots[m_next].state = SLOT_WRITING;
  m_busy = true;
  cameraArmSlot(m_next, MONITOR_SKIP_FRAMES);
  cameraSleep(false);
}

void monitorFrameDone(void)
{
  if (!m_busy) {
    return;
  }
  cameraSleep(true);

  const capture_stats_t *stats = capture_stats();
  uint32_t now = systemTimeGetMs();
  monitor_slot_t *slot = &m_slots[m_next];
  if (!stats->ok) {
    // Bad frame (wrong DMA byte count / overflow): not stored; the next wake rewrites this slot.
    slot->state = SLOT_EMPTY;
    m_dropped++;
    NRF_LOG_RAW_INFO("%08d [mon] slot %u: BAD frame dropped (dma %u %u%s), %u dropped so far\n", now, m_next,
                     stats->segment_bytes[0], stats->segment_bytes[1], stats->overflow ? " OVERFLOW" : "", m_dropped);
    m_busy = false;
    return;
  }
  slot->state = SLOT_READY;
  slot->seq = ++m_seq;
  slot->time_ms = now;
  slot->ok = stats->ok;

  // NRF_LOG takes at most 6 arguments per call.
  NRF_LOG_RAW_INFO("%08d [mon] slot %u seq %u: wake -> 1st FVLD %u ms -> done %u ms,", now, m_next, slot->seq,
                   stats->arm_to_fvld_us / 1000, now - m_wake_ms);
  NRF_LOG_RAW_INFO(" mean %u, ok, before wake XSLEEP %s MCLK %s\n", frame_mean(m_next), m_was_asleep ? "low" : "HIGH",
                   m_mclk_was_on ? "ON" : "off");

  m_next = (m_next + 1) % m_slot_count;
  m_busy = false;
}
