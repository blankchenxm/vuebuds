#include <stdbool.h>
#include <stdint.h>

#include "nrf_log.h"
#include "nrf_delay.h"
#include "camera_sensor.h"
#include "capture.h"
#include "camera.h"
#include "gpio.h"
#include "timers.h"

// Mode and test pattern are compile-time choices (no runtime switching).
#ifndef CAMERA_MODE
#define CAMERA_MODE CAMERA_MODE_QVGA
#endif
#ifndef CAMERA_TEST_PATTERN
#define CAMERA_TEST_PATTERN CAMERA_TEST_PATTERN_OFF
#endif

#define CAMERA_POR_DELAY_MS 100
#define CAMERA_STANDBY_DELAY_MS 5
#define CAMERA_XSHUTDOWN_LOW_MS 10
// Datasheet table 6.2: >= 200 us from reset release to XSLEEP, >= 10 us to the first I2C command.
// Measured: the ID reads back right away; detection polls anyway.
#define CAMERA_BOOT_DELAY_MS 1
#define CAMERA_DETECT_TIMEOUT_MS 1000
#define CAMERA_XSLEEP_SETTLE_US 100  // >= 10 us from XSLEEP high to I2C (datasheet table 6.2)

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

  APP_ERROR_CHECK(sensor->get_mode_info(CAMERA_MODE, &modeInfo));
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

void cameraDeInit(void)
{
  cameraInitialized = false;

  gpioDisable(CAM_MCLK_IN_FROM_MCU);
  gpioDisable(CAM_INT);
  gpioDisable(CAM_LINE_VALID);
  gpioDisable(CAM_FRAME_VALID);
  gpioDisable(CAM_PCLK_OUT_TO_MCU);
  gpioDisable(CAM_D0);
  gpioDisable(CAM_D1);
  gpioDisable(CAM_D2);
  gpioDisable(CAM_D3);

  capture_uninit();
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
  NRF_LOG_RAW_INFO("%s\n", stats->overflow ? " OVERFLOW" : "");

  frame->stride = modeInfo.transport_width;
  frame->width = modeInfo.standard.width;
  frame->height = modeInfo.standard.height;
  frame->pixels = capture_frame() + (uint32_t)modeInfo.standard.y * frame->stride + modeInfo.standard.x;
  return true;
}

void cameraEnableStandbyMode(bool standby)
{
  static bool currentStandby = false;
  NRF_LOG_INFO("[camera] standby:%d", standby);
  if (!cameraInitialized) {
    return;
  }
  if (standby) {
    sensor->stream_stop();
    delayMs(CAMERA_STANDBY_DELAY_MS);
    capture_mclk_enable(false);
    currentStandby = true;
  } else if (currentStandby) {
    capture_mclk_enable(true);
    delayMs(CAMERA_STANDBY_DELAY_MS);
    sensor->stream_start();
    currentStandby = false;
  }
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

void cameraSleep(bool sleep)
{
  // HM0360 S2: MCLK runs whenever XSLEEP is high (datasheet: MCLK first, then XSLEEP).
  if (sleep) {
    nrf_gpio_pin_clear(CAM_XSLEEP);
    nrf_delay_us(CAMERA_XSLEEP_SETTLE_US);
    capture_mclk_enable(false);
  } else {
    capture_mclk_enable(true);
    nrf_delay_us(CAMERA_XSLEEP_SETTLE_US);
    nrf_gpio_pin_set(CAM_XSLEEP);
  }
}

bool cameraIsAsleep(void)
{
  return nrf_gpio_pin_out_read(CAM_XSLEEP) == 0;
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
