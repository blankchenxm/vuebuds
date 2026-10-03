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
#include "gpio.h"
#include "nrf_gpio.h"

// 0: no RTC, every frame is stored (the next slot is armed as soon as one is done).
#ifndef MONITOR_PERIOD_MS
#define MONITOR_PERIOD_MS 500
#endif
// Frames to let pass after each HM0360 wake before the one that is stored (user choice: 1).
#ifndef MONITOR_SKIP_FRAMES
#define MONITOR_SKIP_FRAMES 1
#endif

#define MONITOR_MAX_SLOTS 16
#define RTC_HZ 32768u
#define RTC_PERIOD_TICKS ((MONITOR_PERIOD_MS * RTC_HZ) / 1000u)
#define RTC_COUNTER_MASK 0xFFFFFFu  // 24-bit counter
#define HM0360_MODEL_ID 0x0360
#define MONITOR_FVLD_FALL_TIMEOUT_US 30000u  // > 36 blank lines (13.5 ms) + margin

typedef enum {
  SLOT_EMPTY = 0,
  SLOT_WRITING,
  SLOT_READY,
} slot_state_t;

typedef struct {
  slot_state_t state;
  uint32_t seq;       // frame number, counting from 1
  uint32_t time_ms;   // when the frame was complete
} monitor_slot_t;

static const nrfx_rtc_t m_rtc = NRFX_RTC_INSTANCE(2);
static uint32_t m_cc;

static monitor_slot_t m_slots[MONITOR_MAX_SLOTS];
static uint32_t m_slot_count;
static uint32_t m_next;        // oldest slot, written by the next frame
static uint32_t m_seq;
static bool m_running;
static bool m_s2;              // HM0360: S2 sleep between frames; HM01B0: MCLK and sensor always on
static bool m_busy;            // a wake is in progress (sensor awake, slot armed)
static uint32_t m_wake_ms;
static uint32_t m_overruns;
static uint32_t m_dropped;     // bad frames not stored
static bool m_was_asleep;      // XSLEEP low just before this wake
static bool m_mclk_was_on;     // MCLK running just before this wake
static uint32_t m_sleep_wait_us; // HM0360: last line -> FVLD fall, waited before sleeping

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
  if (cameraModelId() == 0) {
    NRF_LOG_RAW_INFO("[mon] not started: no camera sensor found\n");
    return;
  }
  m_s2 = (cameraModelId() == HM0360_MODEL_ID);

  m_slot_count = cameraSlotCount();
  if (m_slot_count > MONITOR_MAX_SLOTS) {
    m_slot_count = MONITOR_MAX_SLOTS;
  }

  // Sensor streams from here on. HM0360: S2 gates it with XSLEEP and MCLK only.
  // HM01B0 has no S2: it keeps streaming with MCLK on, a frame is stored when a slot is armed.
  cameraSensorStream();
  if (m_s2) {
    cameraSleep(true);
  }
  m_running = true;

  NRF_LOG_RAW_INFO("%08d [mon] start: %s %s, %u slots, period %u ms,", systemTimeGetMs(),
                   m_s2 ? "HM0360" : "HM01B0", cameraModeName(), m_slot_count, MONITOR_PERIOD_MS);
  if (m_s2) {
    NRF_LOG_RAW_INFO(" S2 sleep, skip %u frame(s)\n", MONITOR_SKIP_FRAMES);
  } else {
    NRF_LOG_RAW_INFO(" MCLK always on, no skip\n");
  }

  if (MONITOR_PERIOD_MS == 0) {
    eventQueuePush(EVENT_MONITOR_WAKE);
    return;
  }
  nrfx_rtc_config_t config = NRFX_RTC_DEFAULT_CONFIG;
  config.prescaler = RTC_FREQ_TO_PRESCALER(RTC_HZ);
  APP_ERROR_CHECK(nrfx_rtc_init(&m_rtc, &config, rtc_handler));
  m_cc = (nrfx_rtc_counter_get(&m_rtc) + RTC_PERIOD_TICKS) & RTC_COUNTER_MASK;
  APP_ERROR_CHECK(nrfx_rtc_cc_set(&m_rtc, 0, m_cc, true));
  nrfx_rtc_enable(&m_rtc);
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

  // State between frames, for the acceptance check (HM0360 expected: XSLEEP low, MCLK off).
  m_was_asleep = cameraIsAsleep();
  m_mclk_was_on = capture_mclk_running();

  m_wake_ms = systemTimeGetMs();
  m_slots[m_next].state = SLOT_WRITING;
  m_busy = true;
  cameraArmSlot(m_next, m_s2 ? MONITOR_SKIP_FRAMES : 0);
  if (m_s2) {
    cameraSleep(false);
  }
}

#ifdef MONITOR_FIRST_FRAMES_TEST
// Mean |a - b| over every 8th pixel of two slots.
static uint32_t frame_diff(uint32_t slot_a, uint32_t slot_b)
{
  camera_frame_t a, b;
  if (!cameraSlotFrame(slot_a, &a) || !cameraSlotFrame(slot_b, &b)) {
    return 0;
  }
  uint32_t sum = 0, n = 0;
  for (uint16_t y = 0; y < a.height; y += 8) {
    for (uint16_t x = 0; x < a.width; x += 8) {
      int32_t d = (int32_t)a.pixels[(uint32_t)y * a.stride + x] - b.pixels[(uint32_t)y * b.stride + x];
      sum += (uint32_t)(d < 0 ? -d : d);
      n++;
    }
  }
  return n ? sum / n : 0;
}

/*
 * Problem 3 experiment (HM0360, build with MONITOR_FIRST_FRAMES_TEST and MONITOR_SKIP_FRAMES=0):
 * keep the 1st, 2nd and 3rd frame after each wake (the 3rd overwrites the 1st's slot) and
 * compare |1st - 2nd| with |2nd - 3rd|, the frame-to-frame noise of a settled sensor.
 * If they match (pre-meter already set the exposure), the skipped frame can go.
 * Returns true while more frames of this wake are to come.
 */
static bool first_frames_test(void)
{
  static uint8_t frame;  // frames of this wake done so far
  static uint32_t fvld1_us, done1_ms, done2_ms, mean1, mean2, diff12;
  const capture_stats_t *stats = capture_stats();
  uint32_t a = m_next, b = (m_next + 1) % m_slot_count;
  if (!stats->ok) {
    frame = 0;
    return false;  // handled below as a bad frame
  }
  uint32_t t = systemTimeGetMs() - m_wake_ms;
  if (frame == 0) {
    fvld1_us = stats->arm_to_fvld_us;
    done1_ms = t;
    mean1 = frame_mean(a);
    frame = 1;
    cameraArmSlot(b, 0);  // sensor stays awake for the next frame
    return true;
  }
  if (frame == 1) {
    done2_ms = t;
    mean2 = frame_mean(b);
    diff12 = frame_diff(a, b);
    frame = 2;
    cameraArmSlot(a, 0);
    return true;
  }
  frame = 0;
  cameraSleep(true);
  NRF_LOG_RAW_INFO("%08d [mon] first-frames: wake -> 1st FVLD %u ms, done 1st %u 2nd %u 3rd %u ms\n",
                   systemTimeGetMs(), fvld1_us / 1000, done1_ms, done2_ms, t);
  NRF_LOG_RAW_INFO("%08d [mon] first-frames: mean %u %u %u, |1st-2nd| %u, |2nd-3rd| %u\n", systemTimeGetMs(), mean1,
                   mean2, frame_mean(a), diff12, frame_diff(b, a));
  m_next = (b + 1) % m_slot_count;
  m_busy = false;
  return true;
}
#endif

void monitorFrameDone(void)
{
  if (!m_busy) {
    return;
  }
#ifdef MONITOR_FIRST_FRAMES_TEST
  if (m_s2 && first_frames_test()) {
    return;
  }
#endif
  if (m_s2) {
    // Sleep on a frame boundary: the capture now ends after the last line, ~13 ms before
    // FVLD falls. Measured (QVGA): XSLEEP low inside the 65 us FVLD-low gap -> the next wake
    // gives its first FVLD after 131 ms; low anywhere inside a frame -> 235 ms (one frame more).
    uint32_t t0 = (uint32_t)systemTimeGetUs();
    while (nrf_gpio_pin_read(CAM_FRAME_VALID) && (uint32_t)systemTimeGetUs() - t0 < MONITOR_FVLD_FALL_TIMEOUT_US) {
    }
    m_sleep_wait_us = (uint32_t)systemTimeGetUs() - t0;
    cameraSleep(true);
  }

  const capture_stats_t *stats = capture_stats();
  uint32_t now = systemTimeGetMs();
  monitor_slot_t *slot = &m_slots[m_next];
  m_busy = false;
  if (MONITOR_PERIOD_MS == 0) {
    eventQueuePush(EVENT_MONITOR_WAKE);  // continuous: arm the next slot right after this frame
  }

  if (!stats->ok) {
    // Bad frame (wrong DMA byte count / overflow): not stored; the next wake rewrites this slot.
    slot->state = SLOT_EMPTY;
    m_dropped++;
    NRF_LOG_RAW_INFO("%08d [mon] slot %u: BAD frame dropped (%u us, dma %u %u%s),", now, m_next, stats->duration_us,
                     stats->segment_bytes[0], stats->segment_bytes[1], stats->overflow ? " OVERFLOW" : "");
    NRF_LOG_RAW_INFO(" %u dropped so far\n", m_dropped);
    return;
  }
  slot->state = SLOT_READY;
  slot->seq = ++m_seq;
  slot->time_ms = now;

  // NRF_LOG takes at most 6 arguments per call.
  NRF_LOG_RAW_INFO("%08d [mon] slot %u seq %u: wake -> 1st FVLD %u ms -> done %u ms,", now, m_next, slot->seq,
                   stats->arm_to_fvld_us / 1000, now - m_wake_ms);
  NRF_LOG_RAW_INFO(" mean %u, ok, before wake XSLEEP %s MCLK %s\n", frame_mean(m_next), m_was_asleep ? "low" : "HIGH",
                   m_mclk_was_on ? "ON" : "off");
  if (m_s2) {
    NRF_LOG_RAW_INFO("%08d [mon]   slept at FVLD fall, %u us after the last line\n", now, m_sleep_wait_us);
  }

  m_next = (m_next + 1) % m_slot_count;
}
