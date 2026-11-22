#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#include "nrf_log.h"

#include "camera.h"
#include "capture.h"
#include "event.h"
#include "timers.h"
#include "monitor.h"
#ifdef MONITOR_RING
#include "ring.h"
#include "codec.h"
#endif

// 0: no RTC, every frame is stored (the next slot is armed as soon as one is done).
#ifndef MONITOR_PERIOD_MS
#ifdef CAM_MODE_VGA  // from CMakeLists (CAMERA_MODE is an enum value, invisible to #if)
// A VGA wake keeps MCLK on ~682 ms (285 ms to the first FVLD + 370 ms of lines + the
// boundary), so 500 ms would skip every other wake (measured 10-07: 19 overruns in 20 s).
#define MONITOR_PERIOD_MS 1000
#else
#define MONITOR_PERIOD_MS 500
#endif
#endif
// Frames to let pass after each HM0360 wake before the one that is stored. 0 since PR #28
// (user choice 10-02): the 1st frame after a wake matched the 2nd and 3rd (was 1).
#ifndef MONITOR_SKIP_FRAMES
#define MONITOR_SKIP_FRAMES 0
#endif

#define MONITOR_MAX_SLOTS 16
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
} monitor_slot_t;

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

// fw/ uses RTC2 compares; a periodic k_timer (kernel RTC1, 32.768 kHz crystal) also keeps
// a fixed period from the previous expiry, so wake-ups do not drift.
static void wake_timer_handler(struct k_timer *timer)
{
  eventQueuePush(EVENT_MONITOR_WAKE);
}
K_TIMER_DEFINE(m_wake_timer, wake_timer_handler, NULL);

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

#ifdef MONITOR_RING
// hub #26: copy the stored frame into the PSRAM ring, read the newest and the oldest frame back
// (CRC32), and log timing, the time index and, every 10 frames, a lookup check.
static uint32_t m_ring_verify_errors;
static uint64_t m_ring_prev_us;
// The frame waiting to go into the ring: stored once the sensor is asleep and MCLK is off, so the
// PSRAM work (~150 ms for raw QVGA) does not keep MCLK on (it did: 235 -> 422 ms per wake).
static bool m_ring_pending;
static uint32_t m_ring_slot, m_ring_seq, m_ring_start_us;

/*
 * hub #39 step 2: stored format. RING_JPEG_Q=<q> stores JPEG q (0 = raw, the default);
 * RING_TEST_CYCLE runs RING_TEST_FRAMES frames each of raw, q60, q75, q85, q95 stored compressed,
 * emptying the ring between them; at the end of the raw run the newest frame is JPEG-encoded from
 * PSRAM at each quality (compress on fetch).
 */
#ifndef RING_TEST_FRAMES
#define RING_TEST_FRAMES 30
#endif
#ifdef RING_TEST_CYCLE
static const uint8_t m_cfg_q[] = {0, 60, 75, 85, 95};
#elif defined(RING_JPEG_Q)
static const uint8_t m_cfg_q[] = {RING_JPEG_Q};
#else
static const uint8_t m_cfg_q[] = {0};
#endif
static const uint8_t m_fetch_q[] = {60, 75, 85, 95};
static uint32_t m_cfg, m_cfg_frames, m_cfg_overruns0, m_cfg_dropped0, m_cfg_verify0;
static uint8_t *m_band;  // 8 rows for the fetch test, after the encoder workspace

static void ringCfgLog(const char *what)
{
  NRF_LOG_RAW_INFO("%08d [ring] === %s cfg %u: %s%u, %u frames, overruns %u, bad frames %u, verify errors %u, write errors %u\n",
                   systemTimeGetMs(), what, m_cfg, m_cfg_q[m_cfg] ? "store JPEG q" : "raw ", m_cfg_q[m_cfg], m_cfg_frames,
                   m_overruns - m_cfg_overruns0, m_dropped - m_cfg_dropped0, m_ring_verify_errors - m_cfg_verify0,
                   ringStats()->write_errors);
}

static void ringCfgNext(void)
{
  if (m_cfg_q[m_cfg] == 0 && m_band != NULL) {
    const ring_entry_t *e = ringNewest();
    for (int i = 0; i < ARRAY_SIZE(m_fetch_q); i++) {
      uint32_t us;
      int32_t n = ringFetchJpeg(e, m_fetch_q[i], m_band, &us);
      NRF_LOG_RAW_INFO("%08d [ring] fetch seq %u as JPEG q%u: %d B, read + encode %u us\n", systemTimeGetMs(), e ? e->seq : 0,
                       m_fetch_q[i], n, us);
    }
  }
  ringCfgLog("end");
  m_cfg = (m_cfg + 1) % ARRAY_SIZE(m_cfg_q);
  m_cfg_frames = 0;
  m_cfg_overruns0 = m_overruns;
  m_cfg_dropped0 = m_dropped;
  m_cfg_verify0 = m_ring_verify_errors;
  m_ring_prev_us = 0;
  ringReset();
  ringCfgLog("start");
}

static void ringStore(uint32_t s, uint32_t seq, uint32_t start_us)
{
  camera_frame_t f;
  uint32_t write_us, new_us = 0, old_us = 0;
  uint8_t q = m_cfg_q[m_cfg];
  bool stored = cameraSlotFrame(s, &f) &&
                (q ? ringAppendJpeg(&f, q, seq, start_us, NULL, &write_us) : ringAppend(&f, seq, start_us, NULL, &write_us));
  m_cfg_frames++;
  if (!stored) {
    NRF_LOG_RAW_INFO("%08d [ring] seq %u: NOT stored (write errors %u)\n", systemTimeGetMs(), seq,
                     ringStats()->write_errors);
#ifdef RING_TEST_CYCLE
    if (m_cfg_frames >= RING_TEST_FRAMES) {
      ringCfgNext();
    }
#endif
    return;
  }
  const ring_entry_t *n = ringNewest(), *o = ringAt(0);
  bool new_ok = ringVerify(n, &new_us);
  bool old_checked = o != n && (seq % 10) == 0;  // the oldest every 10th frame (VGA raw: ~230 ms per read-back)
  bool old_ok = !old_checked || ringVerify(o, &old_us);
  if (!new_ok || !old_ok) {
    m_ring_verify_errors++;
  }
  uint32_t gap_ms = m_ring_prev_us ? (uint32_t)((n->capture_us - m_ring_prev_us) / 1000) : 0;
  m_ring_prev_us = n->capture_us;
  const ring_stats_t *st = ringStats();
  NRF_LOG_RAW_INFO("%08d [ring] seq %u t %u ms (+%u) @0x%06x: %s%u %u B, %s %u us,", systemTimeGetMs(), seq,
                   (uint32_t)(n->capture_us / 1000), gap_ms, n->addr, q ? "q" : "raw", q, n->len,
                   q ? "encode + write" : "write", write_us);
  NRF_LOG_RAW_INFO(" read back new %s %u us, oldest seq %u %s %u us,", new_ok ? "ok" : "BAD", new_us, o->seq,
                   old_checked ? (old_ok ? "ok" : "BAD") : "-", old_us);
  NRF_LOG_RAW_INFO(" wake -> stored %u ms\n", systemTimeGetMs() - m_wake_ms);
  NRF_LOG_RAW_INFO("%08d [ring]   %u frames, span %u ms, %u KB, wraps %u,", systemTimeGetMs(), st->frames,
                   (uint32_t)((n->capture_us - o->capture_us) / 1000), st->used_bytes / 1024, st->wraps);
  NRF_LOG_RAW_INFO(" evicted %u, verify errors %u", st->evicted, m_ring_verify_errors);
  NRF_LOG_RAW_INFO(", overruns %u, bad frames %u\n", m_overruns, m_dropped);
  if (seq % 10 == 0 && st->frames > 3) {
    // Lookup check: a time between two frames 2 frames back, and one before the oldest frame.
    const ring_entry_t *a = ringAt(st->frames - 3), *b = ringAt(st->frames - 2);
    uint64_t t = a->capture_us + (b->capture_us - a->capture_us) / 3;
    const ring_entry_t *le = ringFindAtOrBefore(t), *near = ringFindNearest(t);
    bool ok = le == a && near == a && ringFindAtOrBefore(o->capture_us - 1) == NULL &&
              ringFindNearest(b->capture_us - (b->capture_us - a->capture_us) / 4) == b;
    NRF_LOG_RAW_INFO("%08d [ring]   lookup t %u ms: at-or-before seq %u, nearest seq %u (expect %u): %s\n",
                     systemTimeGetMs(), (uint32_t)(t / 1000), le ? le->seq : 0, near ? near->seq : 0, a->seq,
                     ok ? "ok" : "WRONG");
  }
#ifdef RING_TEST_CYCLE
  if (m_cfg_frames >= RING_TEST_FRAMES) {
    ringCfgNext();
  }
#endif
}
#endif

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
    cameraSleepOnFrameBoundary();
  }
#ifdef MONITOR_RING
  ringInit(NULL, 0);
#if defined(RING_TEST_CYCLE) || (defined(RING_JPEG_Q) && RING_JPEG_Q > 0)
  {
    // Encoder workspace + an 8-row band after the Monitor slots; give up slots until it fits.
    size_t ws = (codecWorkspaceSize() + 7) & ~7u, need = ws + 640 * 8 + 8;
    uint32_t spare = 0;
    uint8_t *mem;
    while ((mem = frame_pool_after(m_slot_count, &spare)) == NULL || spare < need) {
      if (m_slot_count <= 1) {
        break;
      }
      m_slot_count--;
    }
    if (mem != NULL && spare >= need && codecInit(mem, ws) == 0) {
      m_band = (uint8_t *)(((uintptr_t)mem + ws + 7) & ~(uintptr_t)7);
    }
    NRF_LOG_RAW_INFO("%08d [ring] encoder: workspace %u B, %u B after %u slots: %s\n", systemTimeGetMs(), (unsigned)ws,
                     spare, m_slot_count, m_band ? "ok" : "NO ROOM");
  }
#endif
#ifdef RING_TEST_CYCLE
  ringCfgLog("start");
#endif
#endif
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
  k_timer_start(&m_wake_timer, K_MSEC(MONITOR_PERIOD_MS), K_MSEC(MONITOR_PERIOD_MS));
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
    cameraWake();
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
  cameraSleepOnFrameBoundary();
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
    // The capture ends after the last line, ~13 ms before the frame boundary (FVLD fall),
    // where PPI pulls XSLEEP low; monitorAsleep() follows.
    cameraSleepOnFrameBoundary();
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
#ifdef MONITOR_RING
  m_ring_pending = true;  // monitorAsleep() stores it (HM01B0, no sleep: right away)
  m_ring_slot = m_next;
  m_ring_seq = slot->seq;
  m_ring_start_us = stats->start_us;
  if (!m_s2) {
    m_ring_pending = false;
    ringStore(m_ring_slot, m_ring_seq, m_ring_start_us);
  }
#endif
  m_next = (m_next + 1) % m_slot_count;
}

void monitorAsleep(void)
{
  cameraAsleep();
  if (m_running && m_s2 && m_wake_ms) {
    NRF_LOG_RAW_INFO("%08d [mon]   asleep, MCLK on %u ms\n", systemTimeGetMs(),
                     systemTimeGetMs() - m_wake_ms);
  }
#ifdef MONITOR_RING
  if (m_ring_pending) {
    m_ring_pending = false;
    ringStore(m_ring_slot, m_ring_seq, m_ring_start_us);  // the slot is not armed again before the next wake
  }
#endif
}
