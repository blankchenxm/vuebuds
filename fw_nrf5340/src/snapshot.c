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
#include "snapshot.h"

#define SNAPSHOT_SLOT 0
#define HM0360_MODEL_ID 0x0360
// Bad frame (wrong DMA byte count / overflow, not sent): take the next frame instead, so the
// host still gets a picture (seen 10-08 on HM0360B VGA: OVERFLOW with right byte counts, 5 of 25).
#define SNAPSHOT_MAX_RETAKES 2

static bool m_started;
static volatile bool m_busy;  // woken / armed, frame not sent yet (read by the timed capture's timer)
static bool m_s2;         // HM0360: S2 sleep between pictures
static uint32_t m_count;
static uint8_t m_retakes;  // bad frames retaken for the current request
static uint32_t m_request_ms;
static bool m_timed_picture;  // the picture in progress was started by the timed capture
static volatile bool m_skipped;  // a timed period was skipped since the last timed picture
static volatile uint32_t m_timed_skips, m_skips_logged;
static uint32_t m_timed_period_ms, m_timed_taken;

static void request(bool timed);

void snapshotRequest(void)
{
  request(false);
}

static void request(bool timed)
{
  if (m_busy) {
    NRF_LOG_RAW_INFO("%08d [snap] busy, request ignored\n", systemTimeGetMs());
    return;
  }
  m_request_ms = systemTimeGetMs();
  m_timed_picture = timed;
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
  if (ok) {
    imagePrepare(&frame);  // exposure over I2C: before S2, which turns I2C off (~3 ms of the 13)
    if (m_timed_picture) {
      imageAddFlags(BLE_HEADER_TIMED | (m_skipped ? BLE_HEADER_SKIPPED : 0));
      m_skipped = false;
    }
  }
  if (m_s2 && !cameraExposureManual()) {
    // Capture ends ~13 ms (VGA ~28 ms) before the frame boundary, where DPPI pulls XSLEEP low.
    cameraSleepOnFrameBoundary();
  }
  if (ok) {
    m_count++;
    imageSend(&frame);
    NRF_LOG_RAW_INFO("%08d [snap] picture %u sent, %u ms after the request\n", systemTimeGetMs(), m_count,
                     systemTimeGetMs() - m_request_ms);
  }
  m_busy = false;
  m_retakes = 0;
}

void snapshotAsleep(void)
{
  cameraAsleep();
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

// ---- timed capture (0xB8) ----

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

#define TIMED_MIN_MS 100
#define TIMED_MAX_MS 60000

void snapshotTimedCommand(const uint8_t *data, uint8_t len)
{
  uint8_t mode = len >= 2 ? data[1] : 0xFF;
  if (mode == 0) {
    k_timer_stop(&m_timed_timer);
    NRF_LOG_RAW_INFO("%08d [timed] stop: %u taken, %u skipped\n", systemTimeGetMs(), m_timed_taken, m_timed_skips);
    m_timed_period_ms = 0;
    return;
  }
  uint16_t period = len >= 4 ? (uint16_t)(data[2] | (data[3] << 8)) : 0;
  if (mode != 1 || period < TIMED_MIN_MS || period > TIMED_MAX_MS) {
    NRF_LOG_RAW_INFO("%08d [timed] bad command: mode %u, period %u ms (%u bytes)%s\n", systemTimeGetMs(), mode, period,
                     len, mode == 2 ? ", ring buffer not in this build" : "");
    return;
  }
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
  request(true);
}
