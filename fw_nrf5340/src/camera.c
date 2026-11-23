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
#include "i2c.h"
#include "timesync.h"

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
// Longest wait for a frame boundary. Right after a capture it is 36 blank lines away
// (QQVGA / QVGA 376 us lines: 13.5 ms; VGA 768 us lines: 27.6 ms); requested at another
// time it is up to two frames away (QVGA 2 x 103.8 ms, VGA 2 x 396 ms).
// After-frame timeout: fw/ waits 30 ms. A frame in which the HM0360's AE changed the gain
// has no FVLD low gap at its end at all (measured 10-07: 30 of 30 gain changes, 0 of 28
// unchanged frames), so that wait was always spent in full. 16 ms is just past the 13.5 ms
// boundary; XSLEEP low then does not delay the next wake (user choice B, 10-07).
#ifdef CAM_MODE_VGA  // from CMakeLists (CAMERA_MODE is an enum value, invisible to #if)
#define CAMERA_SLEEP_AFTER_FRAME_TIMEOUT_MS 30
#define CAMERA_SLEEP_ANYTIME_TIMEOUT_MS 850
#else
#define CAMERA_SLEEP_AFTER_FRAME_TIMEOUT_MS 16
#define CAMERA_SLEEP_ANYTIME_TIMEOUT_MS 250
#endif

static void applyPendingExposure(void);  // set before the sensor started (cameraSetExposure)

static bool cameraInitialized = false;
static uint8_t m_state = CAMERA_STATE_UNCHECKED;  // cameraState()
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
    m_state = CAMERA_STATE_MISSING;
    capture_mclk_enable(false);
    return;
  }
  m_state = CAMERA_STATE_PRESENT;

  if (sensor->get_mode_info(CAMERA_MODE, &modeInfo) != NRF_SUCCESS) {
    // e.g. VGA on an HM01B0 (no such mode): leave the camera off.
    NRF_LOG_RAW_INFO("[camera] %s has no %s mode, camera not started\n", sensor->name,
                     camera_mode_name(CAMERA_MODE));
    sensor = NULL;
    capture_mclk_enable(false);
    return;
  }
  ret_code_t err_code = sensor->init(CAMERA_MODE, CAMERA_TEST_PATTERN);
  if (err_code != NRF_SUCCESS) {
    NRF_LOG_RAW_INFO("[camera] %s init failed: 0x%x\n", sensor->name, err_code);
  }
  NRF_LOG_RAW_INFO("[camera] %s %s, test pattern %d, sent as %ux%u\n", sensor->name,
                   camera_mode_name(CAMERA_MODE), CAMERA_TEST_PATTERN,
                   modeInfo.standard.width, modeInfo.standard.height);
#ifdef CAMERA_REG_DUMP
  // hub #68 item 3: compare the two boards' sensors (ID, revision, AE settings), read after init.
  {
    static const uint16_t blocks[][2] = {{0x0000, 0x0003}, {0x0100, 0x0104}, {0x0202, 0x0205}, {0x020E, 0x020F},
                                         {0x2000, 0x203F}, {0x3024, 0x3030}};
    for (int b = 0; b < (int)ARRAY_SIZE(blocks); b++) {
      for (uint16_t r = blocks[b][0]; r <= blocks[b][1]; r += 8) {
        NRF_LOG_RAW_INFO("[regs] %04x:", r);
        for (uint16_t k = r; k < r + 8 && k <= blocks[b][1]; k++) {
          NRF_LOG_RAW_INFO(" %02x", i2cRead16(CAMERA_I2C_ADDR, k));
        }
        NRF_LOG_RAW_INFO("\n");
        k_msleep(5);  // RTT buffer is 512 B
      }
    }
  }
#endif

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
  applyPendingExposure();
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

#ifdef BOARD_V3
/*
 * V3 boot (hub #83): is a camera module plugged in? MCLK on, reset pulse, read the model ID and
 * revision quietly, then MCLK off and XSHUTDOWN low again (the module's reset state, 10 k
 * pull-down on the board), so cameraInit() later starts from the same state as on the DK.
 */
uint16_t cameraProbe(void)
{
  capture_mclk_init();
  delayMs(CAMERA_POR_DELAY_MS);
  cameraHardwareReset();
  uint8_t id[4] = {0};
  int err = 0;
  for (int i = 0; i < 4 && !err; i++) {
    err = i2cTryRead16(CAMERA_I2C_ADDR, (uint16_t)i, &id[i]);
  }
  capture_mclk_enable(false);
  nrf_gpio_pin_clear(CAM_XSHUTDOWN);
  uint16_t model = ((uint16_t)id[0] << 8) | id[1];
  if (err) {
    NRF_LOG_RAW_INFO("[camera] no camera: nothing answers at 0x%02x (%d); pictures and the ring stay off\n",
                     CAMERA_I2C_ADDR, err);
    m_state = CAMERA_STATE_MISSING;
    return 0;
  }
  m_state = CAMERA_STATE_PRESENT;
  NRF_LOG_RAW_INFO("[camera] found model 0x%04x, silicon revision 0x%02x (0x0003: 0x%02x)\n", model, id[2], id[3]);
  return model;
}
#endif

uint8_t cameraState(void)
{
  return m_state;
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
  applyPendingExposure();
}

// HM0360 S2: MCLK runs whenever XSLEEP is high (datasheet: MCLK first, then XSLEEP).
static enum { CAMERA_AWAKE, CAMERA_FALLING_ASLEEP, CAMERA_ASLEEP } sleepState = CAMERA_AWAKE;
static uint32_t sleepTimeoutMs;
static uint64_t wakeAtUs;  // cameraWakeAt() armed for this local time (0: none)

// No frame boundary in time (seen on the first wake after the stream starts, when FVLD
// stays high from one frame to the next): sleep anyway, like PR #26's 30 ms timeout.
static volatile bool sleepTimedOut;
static void sleepTimeout(struct k_timer *timer)
{
  if (sleepState == CAMERA_FALLING_ASLEEP) {
    sleepTimedOut = true;
    capture_xsleep(false);
    NRF_LOG_RAW_INFO("%08d [cam] no frame boundary within %u ms, XSLEEP low anyway\n", systemTimeGetMs(),
                     sleepTimeoutMs);
    eventQueuePush(EVENT_CAMERA_ASLEEP);
  }
}
K_TIMER_DEFINE(sleepTimeoutTimer, sleepTimeout, NULL);

// Exposure: AE (the HM0360 table's AE_CTRL 0x1F) or fixed values with AE off (as OpenMV's
// set_auto_exposure: AE_CTRL bit0 off, then integration / gains, then COMMAND_UPDATE).
// Fixed values only hold while the sensor keeps streaming: waking from S2 meters the scene
// itself and the first frames ignore the registers (10-11: registers read back the fixed values,
// the picture stayed at the AE target; pre-meter off, Context A AE_CTRL, AE update off, writing
// 40 ms after the wake and skipping a frame changed nothing). snapshot.c therefore keeps the
// sensor awake while the exposure is manual.
#define HM0360_AE_CTRL 0x2000
#define HM0360_AE_CTRL_AUTO 0x1F    // hm0360_modes.c
#define HM0360_AE_CTRL_MANUAL 0x1E  // bit0 = AE enable (datasheet 10.13)
#define SENSOR_COMMAND_UPDATE 0x0104
static bool exposureManual;
static bool exposurePending;  // set while asleep: written right after the next wake
static camera_exposure_t exposureValues;

static bool sensorAwake(void)
{
  return cameraInitialized && sensor != NULL && sleepState == CAMERA_AWAKE && capture_mclk_running();
}

static void writeExposure(void)
{
  if (exposureManual) {
    i2cWrite16(CAMERA_I2C_ADDR, HM0360_AE_CTRL, HM0360_AE_CTRL_MANUAL);
    i2cWrite16(CAMERA_I2C_ADDR, 0x0202, exposureValues.integration >> 8);
    i2cWrite16(CAMERA_I2C_ADDR, 0x0203, exposureValues.integration & 0xFF);
    i2cWrite16(CAMERA_I2C_ADDR, 0x0205, exposureValues.analog_gain);
    i2cWrite16(CAMERA_I2C_ADDR, 0x020E, exposureValues.digital_gain >> 8);
    i2cWrite16(CAMERA_I2C_ADDR, 0x020F, exposureValues.digital_gain & 0xFF);
  } else {
    i2cWrite16(CAMERA_I2C_ADDR, HM0360_AE_CTRL, HM0360_AE_CTRL_AUTO);
  }
  // Latch the double-buffered registers; write-only, so no read-back check.
  i2cWrite16NoVerify(CAMERA_I2C_ADDR, SENSOR_COMMAND_UPDATE, 0x01);
  exposurePending = false;
}

bool cameraReadExposure(camera_exposure_t *exposure)
{
  if (!sensorAwake()) {
    return false;
  }
  exposure->integration = (i2cRead16(CAMERA_I2C_ADDR, 0x0202) << 8) | i2cRead16(CAMERA_I2C_ADDR, 0x0203);
  exposure->analog_gain = i2cRead16(CAMERA_I2C_ADDR, 0x0205);
  exposure->digital_gain = (i2cRead16(CAMERA_I2C_ADDR, 0x020E) << 8) | i2cRead16(CAMERA_I2C_ADDR, 0x020F);
  return true;
}

void cameraSetExposure(const camera_exposure_t *exposure)
{
  if (sensor != NULL && sensor->model_id != 0x0360) {
    NRF_LOG_RAW_INFO("[cam] manual exposure is HM0360 only\n");
    return;
  }
  exposureManual = exposure != NULL;
  if (exposure != NULL) {
    exposureValues = *exposure;
  }
  exposurePending = true;
  if (sensorAwake()) {
    writeExposure();
  }
  NRF_LOG_RAW_INFO("%08d [cam] exposure %s: integration %u, analog gain 0x%02x, digital gain 0x%04x%s\n",
                   systemTimeGetMs(), exposureManual ? "manual" : "auto", exposureValues.integration,
                   exposureValues.analog_gain, exposureValues.digital_gain,
                   exposurePending ? " (after the next wake)" : "");
}

static void applyPendingExposure(void)
{
  if (exposurePending && sensorAwake()) {
    writeExposure();
  }
}

bool cameraExposureManual(void)
{
  return exposureManual;
}

void cameraReapplyExposure(void)
{
  if (exposureManual && sensorAwake()) {
    writeExposure();
  }
}

void cameraWake(void)
{
  k_timer_stop(&sleepTimeoutTimer);
  wakeAtUs = 0;
  capture_mclk_enable(true);
  nrf_delay_us(CAMERA_XSLEEP_SETTLE_US);
  capture_xsleep(true);
  sleepState = CAMERA_AWAKE;
  if (exposurePending) {
    nrf_delay_us(CAMERA_XSLEEP_SETTLE_US);  // >= 10 us from XSLEEP high to I2C
    writeExposure();
  }
}

bool cameraWakeAt(uint64_t local_us)
{
  if (sleepState != CAMERA_ASLEEP || !timesyncWakeAt(local_us)) {
    return false;
  }
  // MCLK starts 2 ms before local_us (kernel timer), DPPI sets XSLEEP at local_us. Fixed
  // exposure keeps the sensor awake, so nothing is pending here.
  sleepState = CAMERA_AWAKE;
  wakeAtUs = local_us;
  return true;
}

// Before the MCLK timer (2 ms ahead) with some margin, nothing has happened yet.
#define CAMERA_CANCEL_MARGIN_US 3000

bool cameraCancelWakeAt(void)
{
  if (wakeAtUs == 0 || sleepState != CAMERA_AWAKE || timesyncLocalUs() + CAMERA_CANCEL_MARGIN_US > wakeAtUs) {
    return false;
  }
  timesyncCancelWake();
  if (capture_mclk_running()) {  // the timer fired after all (should not happen with the margin)
    capture_mclk_enable(false);
  }
  sleepState = CAMERA_ASLEEP;
  wakeAtUs = 0;
  return true;
}

void cameraSleepOnFrameBoundary(void)
{
  // XSLEEP low inside the 65 us FVLD-low gap: low anywhere inside a frame makes the next
  // wake one frame slower (QVGA 131 -> 235 ms to the first FVLD). DPPI pulls it low on the
  // FVLD fall; cameraAsleep() then stops MCLK.
  sleepState = CAMERA_FALLING_ASLEEP;
  sleepTimedOut = false;
  wakeAtUs = 0;
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

bool cameraWaitAsleep(uint32_t *waited_us)
{
  uint32_t t0 = systemTimeGetUs();
  uint32_t count0 = capture_asleep_count();  // called right after arming: the boundary is ahead
  bool fired = false;
  for (int i = 0; i < 1000 && sleepState == CAMERA_FALLING_ASLEEP && !sleepTimedOut; i++) {
    if (capture_asleep_count() != count0) {
      fired = true;
      break;
    }
    k_busy_wait(200);
  }
  *waited_us = systemTimeGetUs() - t0;
  cameraAsleep();
  return fired;
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
