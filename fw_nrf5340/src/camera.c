/*
 * nRF5340 port of fw/camera.c. Same flow and constants; app_timer -> k_timer, and
 * cameraDeInit / cameraEnableStandbyMode (power-off and button paths of the VueBuds board,
 * unused on the DK) are not ported.
 */
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#include "nrf_log.h"
#include "nrf_delay.h"
#include "event.h"
#include "camera_sensor.h"
#include "capture.h"
#include "camera.h"
#include "pins.h"
#include "timers.h"

// Mode and test pattern are compile-time choices (no runtime switching).
#ifndef CAMERA_MODE
#define CAMERA_MODE CAMERA_MODE_QVGA
#endif
#ifndef CAMERA_TEST_PATTERN
#define CAMERA_TEST_PATTERN CAMERA_TEST_PATTERN_OFF
#endif

#define CAMERA_POR_DELAY_MS 100
#define CAMERA_XSHUTDOWN_LOW_MS 10
// Datasheet table 6.2: >= 200 us from reset release to XSLEEP, >= 10 us to the first I2C command.
// Measured: the ID reads back right away; detection polls anyway.
#define CAMERA_BOOT_DELAY_MS 1
#define CAMERA_DETECT_TIMEOUT_MS 1000
#define CAMERA_XSLEEP_SETTLE_US 100  // >= 10 us from XSLEEP high to I2C (datasheet table 6.2)
// Longest wait for a frame boundary. Right after a capture it is ~13 ms away (36 blank
// lines); requested at another time it is up to two QVGA frames (2 x 103.8 ms) away.
#define CAMERA_SLEEP_AFTER_FRAME_TIMEOUT_MS 30
#define CAMERA_SLEEP_ANYTIME_TIMEOUT_MS 250

static bool cameraInitialized = false;
static const camera_sensor_t *sensor = NULL;
static camera_mode_info_t modeInfo;

/*
 * Reset the HM0360 through XSHUTDOWN with MCLK already running, so it always starts
 * from a clean state (it does not answer I2C if it powered up without MCLK), and keep
 * XSLEEP high. Both pins are unconnected on HM01B0 boards, where this does nothing.
 */
static void cameraHardwareReset(void)
{
  nrf_gpio_cfg_output(CAM_XSLEEP);
  nrf_gpio_pin_set(CAM_XSLEEP);
  nrf_gpio_cfg_output(CAM_XSHUTDOWN);
  nrf_gpio_pin_clear(CAM_XSHUTDOWN);
  delayMs(CAMERA_XSHUTDOWN_LOW_MS);
  nrf_gpio_pin_set(CAM_XSHUTDOWN);
  delayMs(CAMERA_BOOT_DELAY_MS);
}

void cameraInit(void)
{
  if (cameraInitialized) {
    NRF_LOG_INFO("[camera] already initialized, skipping");
    return;
  }

  // The sensor runs from our MCLK, including its I2C slave, so start it first.
  capture_mclk_init();
  delayMs(CAMERA_POR_DELAY_MS);
  cameraHardwareReset();

  if (camera_sensor_detect(&sensor, CAMERA_DETECT_TIMEOUT_MS) != NRF_SUCCESS) {
    capture_mclk_enable(false);
    return;
  }

  CAPTURE_CHECK(sensor->get_mode_info(CAMERA_MODE, &modeInfo) == NRF_SUCCESS);
  ret_code_t err_code = sensor->init(CAMERA_MODE, CAMERA_TEST_PATTERN);
  if (err_code != NRF_SUCCESS) {
    NRF_LOG_RAW_INFO("[camera] %s init failed: 0x%x\n", sensor->name, err_code);
  }
  NRF_LOG_RAW_INFO("[camera] %s %s, test pattern %d, sent as %ux%u\n", sensor->name,
                   camera_mode_name(CAMERA_MODE), CAMERA_TEST_PATTERN,
                   modeInfo.standard.width, modeInfo.standard.height);

  capture_init(&modeInfo);

  // Clock gating: MCLK only runs while streaming.
  capture_mclk_enable(false);

  cameraInitialized = true;
}

void cameraStartStream(void)
{
  if (!cameraInitialized) {
    return;
  }
  capture_mclk_enable(true);
  capture_arm();
  sensor->stream_start();
  NRF_LOG_RAW_INFO("%08d [cam] stream started\n", systemTimeGetMs());
}

void cameraReadyNextFrame(void)
{
  capture_arm();
}

bool cameraGetFrame(camera_frame_t *frame)
{
  if (!cameraInitialized) {
    return false;
  }

  const capture_stats_t *stats = capture_stats();
  NRF_LOG_RAW_INFO("%08d [cam] frame %u: %u us, dma", systemTimeGetMs(), stats->frame, stats->duration_us);
  for (uint8_t i = 0; i < stats->segments; i++) {
    NRF_LOG_RAW_INFO(" %u", stats->segment_bytes[i]);
  }
  NRF_LOG_RAW_INFO("%s%s\n", stats->overflow ? " OVERFLOW" : "", stats->ok ? "" : ", BAD, dropped");
  if (!stats->ok) {
    // Wrong byte count or overflow (a late CS edge shifted part of the frame): do not send it.
    return false;
  }

  frame->stride = modeInfo.transport_width;
  frame->width = modeInfo.standard.width;
  frame->height = modeInfo.standard.height;
  frame->pixels = capture_frame() + (uint32_t)modeInfo.standard.y * frame->stride + modeInfo.standard.x;
  return true;
}

uint16_t cameraModelId(void)
{
  return (cameraInitialized && sensor != NULL) ? sensor->model_id : 0;
}

const char *cameraModeName(void)
{
  return camera_mode_name(CAMERA_MODE);
}

uint32_t cameraSlotCount(void)
{
  return cameraInitialized ? frame_pool_slot_count() : 0;
}

void cameraSensorStream(void)
{
  if (!cameraInitialized) {
    return;
  }
  capture_mclk_enable(true);
  sensor->stream_start();
}

// HM0360 S2: MCLK runs whenever XSLEEP is high (datasheet: MCLK first, then XSLEEP).
static enum { CAMERA_AWAKE, CAMERA_FALLING_ASLEEP, CAMERA_ASLEEP } sleepState = CAMERA_AWAKE;
static uint32_t sleepTimeoutMs;

// No frame boundary in time (seen on the first wake after the stream starts, when FVLD
// stays high from one frame to the next): sleep anyway, like PR #26's 30 ms timeout.
static void sleepTimeout(struct k_timer *timer)
{
  if (sleepState == CAMERA_FALLING_ASLEEP) {
    capture_xsleep(false);
    NRF_LOG_RAW_INFO("%08d [cam] no frame boundary within %u ms, XSLEEP low anyway\n", systemTimeGetMs(),
                     sleepTimeoutMs);
    eventQueuePush(EVENT_CAMERA_ASLEEP);
  }
}
K_TIMER_DEFINE(sleepTimeoutTimer, sleepTimeout, NULL);

void cameraWake(void)
{
  k_timer_stop(&sleepTimeoutTimer);
  capture_mclk_enable(true);
  nrf_delay_us(CAMERA_XSLEEP_SETTLE_US);
  capture_xsleep(true);
  sleepState = CAMERA_AWAKE;
}

void cameraSleepOnFrameBoundary(void)
{
  // XSLEEP low inside the 65 us FVLD-low gap: low anywhere inside a frame makes the next
  // wake one frame slower (QVGA 131 -> 235 ms to the first FVLD). DPPI pulls it low on the
  // FVLD fall; cameraAsleep() then stops MCLK.
  sleepState = CAMERA_FALLING_ASLEEP;
  sleepTimeoutMs = capture_xsleep_on_frame_boundary() ? CAMERA_SLEEP_AFTER_FRAME_TIMEOUT_MS
                                                      : CAMERA_SLEEP_ANYTIME_TIMEOUT_MS;
  k_timer_start(&sleepTimeoutTimer, K_MSEC(sleepTimeoutMs), K_NO_WAIT);
}

void cameraAsleep(void)
{
  if (sleepState != CAMERA_FALLING_ASLEEP) {
    return;
  }
  k_timer_stop(&sleepTimeoutTimer);
  nrf_delay_us(CAMERA_XSLEEP_SETTLE_US);
  capture_mclk_enable(false);
  sleepState = CAMERA_ASLEEP;
}

bool cameraIsAsleep(void)
{
  return sleepState == CAMERA_ASLEEP;
}

void cameraArmSlot(uint32_t slot, uint8_t skipFrames)
{
  capture_arm_slot(slot, skipFrames);
}

bool cameraSlotFrame(uint32_t slot, camera_frame_t *frame)
{
  uint8_t *base = cameraInitialized ? frame_pool_slot(slot) : NULL;
  if (base == NULL) {
    return false;
  }
  frame->stride = modeInfo.transport_width;
  frame->width = modeInfo.standard.width;
  frame->height = modeInfo.standard.height;
  frame->pixels = base + (uint32_t)modeInfo.standard.y * frame->stride + modeInfo.standard.x;
  return true;
}
