#include <stdbool.h>
#include <stdint.h>

#include "nrf_log.h"

#include "ble.h"
#include "camera.h"
#include "capture.h"
#include "timers.h"
#include "snapshot.h"

#define SNAPSHOT_SLOT 0
#define HM0360_MODEL_ID 0x0360
// Bad frame (wrong DMA byte count / overflow, not sent): take the next frame instead, so the
// host still gets a picture (seen 10-08 on HM0360B VGA: OVERFLOW with right byte counts, 5 of 25).
#define SNAPSHOT_MAX_RETAKES 2

static bool m_started;
static bool m_busy;       // woken / armed, frame not sent yet
static bool m_s2;         // HM0360: S2 sleep between pictures
static uint32_t m_count;
static uint8_t m_retakes;  // bad frames retaken for the current request
static uint32_t m_request_ms;

void snapshotRequest(void)
{
  if (m_busy) {
    NRF_LOG_RAW_INFO("%08d [snap] busy, request ignored\n", systemTimeGetMs());
    return;
  }
  m_request_ms = systemTimeGetMs();
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
    if (m_s2) {
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
  if (m_s2) {
    // Capture ends ~13 ms (VGA ~28 ms) before the frame boundary, where DPPI pulls XSLEEP low.
    cameraSleepOnFrameBoundary();
  }
  if (ok) {
    m_count++;
    bleSendFrame(frame.pixels, frame.stride, frame.width, frame.height);
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

bool snapshotActive(void)
{
  return m_started;
}
