#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#include "nrf_log.h"

#include "ble.h"
#include "image.h"
#include "camera.h"
#include "capture.h"
#include "event.h"
#include "timers.h"
#include "timesync.h"
#include "ringcmd.h"
#include "ring.h"
#include "snapshot.h"

#define SNAPSHOT_SLOT 0
#define HM0360_MODEL_ID 0x0360
// Bad frame (wrong DMA byte count / overflow, not sent): take the next frame instead, so the
// host still gets a picture (seen 10-08 on HM0360B VGA: OVERFLOW with right byte counts, 5 of 25).
#define SNAPSHOT_MAX_RETAKES 2
// Ring buffer: the next capture time is at least this far ahead when it is armed (the hardware
// wake needs 3 ms: MCLK 2 ms before XSLEEP, timesync.c).
#define RING_LEAD_US 5000

typedef enum {
  PIC_REQUEST,  // 0xB2 / 0xB7 (stored in the ring too while the ring runs)
  PIC_TIMED,    // 0xB8 1
  PIC_RING,     // 0xB8 2
  PIC_WARMUP,   // first picture after boot for 0xB8 2: starts the sensor, not stored
} picture_kind_t;

static bool m_started;
static volatile bool m_busy;  // woken / armed, frame not done yet (read by the timed capture's timer)
static bool m_s2;         // HM0360: S2 sleep between pictures
static uint32_t m_count;
static uint8_t m_retakes;  // bad frames retaken for the current request
static uint32_t m_request_ms;
static picture_kind_t m_kind;  // of the picture in progress
static volatile bool m_skipped;  // a timed period was skipped since the last timed picture
static volatile uint32_t m_timed_skips, m_skips_logged;
static uint32_t m_timed_period_ms, m_timed_taken;
// Ring buffer (0xB8 2)
static uint32_t m_ring_period_ms;  // 0: off
static uint64_t m_ring_T;          // bridge time of the armed ring picture
static uint64_t m_ring_last_T;     // of the last ring picture taken (0: none since the start / period change)
static uint32_t m_ring_skips;
static bool m_ring_send;           // a 0xB2 came during the ring picture in progress: send it too
static bool m_ring_due_pending;    // fixed exposure: waiting for the due timer

static void request(picture_kind_t kind);
static void ringSchedule(void);

static uint64_t bridgeNowUs(void)
{
  uint64_t l = timesyncLocalUs(), b;
  return timesyncLocalToBridge(l, &b) ? b : l;
}

// Where a ring picture's time went (hub #68 item 2), ms after its capture time T: first FVLD
// (capture time), capture done, stored. host/ reads these lines.
static void ringTiming(uint64_t T, uint64_t done_us, bool boundary, uint32_t wait_us, bool stored, bool early)
{
  uint64_t cap;
  imageCaptureTime(&cap);
  uint64_t now = bridgeNowUs();
  int32_t fvld = (int32_t)(cap - T) / 1000, done = (int32_t)(done_us - T) / 1000, end = (int32_t)(now - T) / 1000;
  if (early) {
    NRF_LOG_RAW_INFO("%08d [ring] timing: fvld +%d done +%d asleep %s %u us, %s +%d ms (early arm)\n", systemTimeGetMs(),
                     fvld, done, boundary ? "boundary" : "timeout", wait_us, stored ? "stored" : "NOT stored", end);
  } else {
    NRF_LOG_RAW_INFO("%08d [ring] timing: fvld +%d done +%d, %s +%d ms\n", systemTimeGetMs(), fvld, done,
                     stored ? "stored" : "NOT stored", end);
  }
}

static void ringDue(struct k_timer *timer)
{
  eventQueuePush(EVENT_CAMERA_RING_DUE);
}
static K_TIMER_DEFINE(m_ring_due_timer, ringDue, NULL);

// Give up the armed (not yet started) ring picture; false if it is already under way.
static bool ringCancelArmed(void)
{
  if (!m_busy || m_kind != PIC_RING) {
    return false;
  }
  if (m_ring_due_pending) {
    k_timer_stop(&m_ring_due_timer);
    m_ring_due_pending = false;
  } else if (!cameraCancelWakeAt()) {
    return false;
  }
  m_busy = false;
  return true;
}

void snapshotRequest(void)
{
  if (m_busy && m_kind == PIC_RING && m_ring_period_ms) {
    if (ringCancelArmed()) {
      NRF_LOG_RAW_INFO("%08d [snap] 0xB2: ring picture at %u ms given up, taking one now\n", systemTimeGetMs(),
                       (uint32_t)(m_ring_T / 1000));
    } else {
      m_ring_send = true;  // the ring picture under way is the answer
      NRF_LOG_RAW_INFO("%08d [snap] 0xB2 during a ring picture: that one is sent\n", systemTimeGetMs());
      return;
    }
  }
  request(PIC_REQUEST);
}

static void request(picture_kind_t kind)
{
  if (m_busy) {
    NRF_LOG_RAW_INFO("%08d [snap] busy, request ignored\n", systemTimeGetMs());
    return;
  }
  m_request_ms = systemTimeGetMs();
  m_kind = kind;
  if (!m_started) {
    cameraInit();
    if (cameraModelId() == 0) {
      NRF_LOG_RAW_INFO("[snap] no camera sensor found\n");
      return;
    }
    m_s2 = (cameraModelId() == HM0360_MODEL_ID);
    // The sensor comes out of reset with XSLEEP high: arm, then let it stream. This first
    // picture includes the camera init, so it is later than the following ones.
    cameraArmSlot(SNAPSHOT_SLOT, 0);
    cameraSensorStream();
    m_started = true;
  } else {
    cameraArmSlot(SNAPSHOT_SLOT, 0);
    if (m_s2 && !cameraExposureManual()) {
      cameraWake();
    }
  }
  m_busy = true;
}

void snapshotRequestAt(uint64_t local_us)
{
  if (m_busy || !m_started || !m_s2 || cameraExposureManual() || !cameraIsAsleep() || m_ring_period_ms) {
    NRF_LOG_RAW_INFO("%08d [snap] timed request taken now (busy %d, started %d, asleep %d, ring %d)\n",
                     systemTimeGetMs(), m_busy, m_started, cameraIsAsleep(), m_ring_period_ms != 0);
    snapshotRequest();
    return;
  }
  m_request_ms = systemTimeGetMs();
  m_kind = PIC_REQUEST;
  cameraArmSlot(SNAPSHOT_SLOT, 0);
  if (!cameraWakeAt(local_us)) {
    NRF_LOG_RAW_INFO("%08d [snap] wake time too close, waking now\n", systemTimeGetMs());
    cameraWake();
  }
  m_busy = true;
}

void snapshotFrameDone(void)
{
  if (!m_busy) {
    return;
  }
  uint32_t now = systemTimeGetMs();
  const capture_stats_t *stats = capture_stats();
  NRF_LOG_RAW_INFO("%08d [snap] picture %u: request -> 1st FVLD %u ms -> done %u ms\n", now, m_count + 1,
                   stats->arm_to_fvld_us / 1000, now - m_request_ms);
  camera_frame_t frame;
  bool ok = cameraGetFrame(&frame);  // logs the DMA byte counts; false for a bad frame
  if (!ok && m_retakes < SNAPSHOT_MAX_RETAKES) {
    // Keep the sensor awake and take the next frame (~1 frame later than the other board).
    m_retakes++;
    NRF_LOG_RAW_INFO("%08d [snap] bad frame, retake %u\n", now, m_retakes);
    cameraArmSlot(SNAPSHOT_SLOT, 0);
    return;
  }
  uint64_t done_us = bridgeNowUs();
  bool sleep = m_s2 && !cameraExposureManual();
  // Capture ends ~13 ms (VGA ~28 ms) before the frame boundary, where DPPI pulls XSLEEP low.
  if (ok) {
#ifdef SNAP_SLEEP_FIRST
    // Experiment (hub #68 item 2): every picture's sleep ran into the 16 ms timeout (no FVLD fall)
    // while Monitor's, armed right at the end of the capture, mostly did not. Here the sleep is
    // armed before the exposure registers are read over I2C.
    imagePrepareTime(&frame);
    if (sleep) {
      cameraSleepOnFrameBoundary();
    }
    imagePrepareExposure();
#else
    imagePrepare(&frame);  // exposure over I2C: before S2, which turns I2C off (~3 ms of the 13)
    if (sleep) {
      cameraSleepOnFrameBoundary();
    }
#endif
    if (m_kind == PIC_TIMED) {
      imageAddFlags(BLE_HEADER_TIMED | (m_skipped ? BLE_HEADER_SKIPPED : 0));
      m_skipped = false;
    }
  } else if (sleep) {
    cameraSleepOnFrameBoundary();
  }
  picture_kind_t kind = m_kind;
  uint64_t T = m_ring_T;
  if (kind == PIC_RING) {
    m_ring_last_T = T;
  }
#ifdef RING_EARLY_ARM
  if (kind == PIC_RING && ok && sleep && m_ring_period_ms) {
    // Experiment (hub #68 item 2, VGA): wait for the boundary here and arm the next wake before
    // encoding, so the encode overlaps the next wake's ~0.3 s to its first FVLD. The slot is
    // overwritten from that FVLD on: if it came before the frame was stored, the entry is dropped.
    uint32_t wait_us;
    bool boundary = cameraWaitAsleep(&wait_us);
    m_busy = false;
    m_retakes = 0;
    uint64_t fvld0 = timesyncLastFvldUs();
    ringSchedule();
    bool send = m_ring_send;
    m_ring_send = false;
    bool stored = ringcmdStore(&frame, send);
    if (stored && timesyncLastFvldUs() != fvld0) {
      ringDropNewest();
      stored = false;
      NRF_LOG_RAW_INFO("%08d [ring] next frame started before this one was stored: dropped\n", systemTimeGetMs());
    }
    if (!stored) {
      m_ring_skips++;
    }
    ringTiming(T, done_us, boundary, wait_us, stored, true);
    return;
  }
#endif
  if (!ok) {
    if (kind == PIC_RING) {
      m_ring_skips++;
    }
  } else if (kind == PIC_WARMUP) {
    NRF_LOG_RAW_INFO("%08d [snap] sensor started for the ring (picture not stored)\n", systemTimeGetMs());
  } else if ((kind == PIC_RING || kind == PIC_REQUEST) && m_ring_period_ms) {
    // Ring buffer running: stored; a 0xB2 picture also goes out (R_FRAME + frame).
    bool send = kind == PIC_REQUEST || m_ring_send;
    m_ring_send = false;
    bool stored = ringcmdStore(&frame, send);
    if (!stored && kind == PIC_RING) {
      m_ring_skips++;
    }
    if (kind == PIC_RING) {
      ringTiming(T, done_us, false, 0, stored, false);
    }
  } else if (kind == PIC_RING) {
    // Stopped (0xB8 0) while this picture was under way: not stored.
  } else {
    m_count++;
    imageSend(&frame);
    NRF_LOG_RAW_INFO("%08d [snap] picture %u sent, %u ms after the request\n", systemTimeGetMs(), m_count,
                     systemTimeGetMs() - m_request_ms);
  }
  m_busy = false;
  m_retakes = 0;
  if (!sleep) {
    ringSchedule();  // fixed exposure / HM01B0: no sleep to wait for
  }
}

void snapshotAsleep(void)
{
  cameraAsleep();
  if (cameraIsAsleep()) {
    ringSchedule();
  }
}

// The wake meters the scene and can overwrite the values written right after it (10-11: one
// board read back integration 272 instead of 200): write them again once frames are flowing.
#define SNAPSHOT_EXPOSURE_REWRITE_MS 300
static void reapplyExposure(struct k_work *work)
{
  cameraReapplyExposure();
}
static K_WORK_DELAYABLE_DEFINE(reapplyWork, reapplyExposure);

void snapshotExposureChanged(void)
{
  if (!m_started || !m_s2 || m_busy) {
    return;  // applied when the sensor starts / after the picture in progress
  }
  if (cameraExposureManual()) {
    cameraWake();  // writes the fixed values; the sensor streams from now on
    k_work_reschedule(&reapplyWork, K_MSEC(SNAPSHOT_EXPOSURE_REWRITE_MS));
    NRF_LOG_RAW_INFO("%08d [snap] manual exposure: sensor stays awake\n", systemTimeGetMs());
  } else {
    cameraSleepOnFrameBoundary();
    NRF_LOG_RAW_INFO("%08d [snap] auto exposure: sensor sleeps between pictures again\n", systemTimeGetMs());
  }
}

bool snapshotActive(void)
{
  return m_started;
}

uint8_t snapshotMode(void)
{
  return m_ring_period_ms ? SNAPSHOT_MODE_RING : (m_timed_period_ms ? SNAPSHOT_MODE_TIMED : SNAPSHOT_MODE_STOPPED);
}

uint16_t snapshotPeriodMs(void)
{
  return (uint16_t)(m_ring_period_ms ? m_ring_period_ms : m_timed_period_ms);
}

uint32_t snapshotRingSkips(void)
{
  return m_ring_skips;
}

bool snapshotHandOver(void)
{
  if (!m_started) {
    return true;
  }
  if (m_busy || m_ring_period_ms || m_timed_period_ms || !(cameraIsAsleep() || cameraExposureManual() || !m_s2)) {
    return false;
  }
  if (m_s2 && cameraIsAsleep()) {
    cameraWake();
  }
  m_started = false;
  NRF_LOG_RAW_INFO("%08d [snap] pictures stopped: handing the sensor over to the stream (0xB1)\n", systemTimeGetMs());
  return true;
}

// ---- timed capture (0xB8 1) ----

// Decided at the expiry (timer interrupt), not when the main thread gets to the event: the main
// thread is blocked while a picture is queued for BLE, so a queued expiry would take the next
// picture late instead of skipping it (11-19: raw QVGA at 500 / 1000 ms came every ~1.3 s).
static void timedExpiry(struct k_timer *timer)
{
  if (m_busy) {
    m_skipped = true;
    m_timed_skips++;
    return;
  }
  eventQueuePush(EVENT_CAMERA_TIMED);
}
static K_TIMER_DEFINE(m_timed_timer, timedExpiry, NULL);

static void timedStop(void)
{
  if (m_timed_period_ms) {
    k_timer_stop(&m_timed_timer);
    NRF_LOG_RAW_INFO("%08d [timed] stop: %u taken, %u skipped\n", systemTimeGetMs(), m_timed_taken, m_timed_skips);
    m_timed_period_ms = 0;
  }
}

static void ringStop(void)
{
  if (m_ring_period_ms) {
    ringCancelArmed();  // an armed picture is dropped; one already under way is not stored
    NRF_LOG_RAW_INFO("%08d [ring] stop: %u skipped\n", systemTimeGetMs(), m_ring_skips);
    m_ring_period_ms = 0;
  }
}

void snapshotStop(void)
{
  timedStop();
  ringStop();
}

void snapshotTimedCommand(const uint8_t *data, uint8_t len)
{
  uint8_t mode = len >= 2 ? data[1] : 0xFF;
  if (mode == 0) {
    snapshotStop();
    return;
  }
  uint16_t period = len >= 4 ? (uint16_t)(data[2] | (data[3] << 8)) : 0;
  if (mode != 1 || period < SNAPSHOT_PERIOD_MIN_MS || period > SNAPSHOT_PERIOD_MAX_MS) {
    NRF_LOG_RAW_INFO("%08d [timed] bad command: mode %u, period %u ms (%u bytes)\n", systemTimeGetMs(), mode, period,
                     len);
    return;
  }
  ringStop();  // 0xB8 1 and 0xB8 2 exclude each other: the later one wins
  // From stopped: the first picture right away. A new period while running: from now on.
  bool running = m_timed_period_ms != 0;
  m_timed_period_ms = period;
  m_timed_taken = m_timed_skips = m_skips_logged = 0;
  m_skipped = false;
  k_timer_start(&m_timed_timer, running ? K_MSEC(period) : K_NO_WAIT, K_MSEC(period));
  NRF_LOG_RAW_INFO("%08d [timed] every %u ms%s\n", systemTimeGetMs(), period, running ? " (new period)" : "");
}

void snapshotTimedTick(void)
{
  if (m_timed_period_ms == 0) {
    return;  // stopped after this expiry was queued
  }
  if (m_busy) {  // a 0xB2 picture started after the expiry
    m_skipped = true;
    m_timed_skips++;
  }
  uint32_t skips = m_timed_skips;
  if (skips != m_skips_logged) {
    NRF_LOG_RAW_INFO("%08d [timed] %u period(s) skipped (picture in progress), %u so far\n", systemTimeGetMs(),
                     skips - m_skips_logged, skips);
    m_skips_logged = skips;
  }
  if (m_busy) {
    return;
  }
  m_timed_taken++;
  request(PIC_TIMED);
}

// ---- ring buffer (0xB8 2, ringcmd.h) ----

void snapshotRingStart(uint16_t period_ms)
{
  timedStop();
  bool running = m_ring_period_ms != 0;
  if (!running) {
    m_ring_skips = 0;
  }
  m_ring_period_ms = period_ms;
  m_ring_last_T = 0;  // no skips counted across a start / period change
  NRF_LOG_RAW_INFO("%08d [ring] every %u ms on the bridge's clock%s\n", systemTimeGetMs(), period_ms,
                   running ? " (new period)" : "");
  if (!m_started) {
    request(PIC_WARMUP);  // camera init; the ring starts once the sensor is asleep
    return;
  }
  if (m_busy && !ringCancelArmed()) {
    return;  // scheduled when the picture under way is done
  }
  if (!m_s2 || cameraExposureManual() || cameraIsAsleep()) {
    ringSchedule();
  }
}

// Arm the next ring picture: the next multiple of the period on the bridge's clock.
static void ringSchedule(void)
{
  if (!m_ring_period_ms || m_busy || !m_started) {
    return;
  }
  uint64_t now_l = timesyncLocalUs(), now_b, local;
  uint64_t P = (uint64_t)m_ring_period_ms * 1000;
  if (!timesyncLocalToBridge(now_l, &now_b)) {
    NRF_LOG_RAW_INFO("%08d [ring] no time reference, retry in 100 ms\n", systemTimeGetMs());
    m_ring_due_pending = false;
    k_timer_start(&m_ring_due_timer, K_MSEC(100), K_NO_WAIT);
    return;
  }
  uint64_t T = (now_b / P + 1) * P;
  if (T < now_b + RING_LEAD_US) {
    T += P;
  }
  if (m_ring_last_T && T > m_ring_last_T + P) {
    uint32_t missed = (uint32_t)((T - m_ring_last_T) / P) - 1;
    m_ring_skips += missed;
    NRF_LOG_RAW_INFO("%08d [ring] %u capture time(s) skipped, %u so far\n", systemTimeGetMs(), missed, m_ring_skips);
  }
  m_ring_T = T;
  timesyncBridgeToLocal(T, &local);
  m_kind = PIC_RING;
  m_request_ms = systemTimeGetMs() + (uint32_t)((local - now_l) / 1000);
  m_busy = true;
  if (m_s2 && !cameraExposureManual()) {
    cameraArmSlot(SNAPSHOT_SLOT, 0);
    if (!cameraWakeAt(local)) {
      NRF_LOG_RAW_INFO("%08d [ring] hardware wake failed, waking now\n", systemTimeGetMs());
      cameraWake();
    }
  } else {
    // Sensor awake (fixed exposure / HM01B0): the next frame after T.
    m_ring_due_pending = true;
    k_timer_start(&m_ring_due_timer, K_USEC(local - now_l), K_NO_WAIT);
  }
}

void snapshotRingDue(void)
{
  if (m_ring_due_pending) {
    m_ring_due_pending = false;
    cameraArmSlot(SNAPSHOT_SLOT, 0);
  } else if (!m_busy) {
    ringSchedule();  // retry after no time reference
  }
}
