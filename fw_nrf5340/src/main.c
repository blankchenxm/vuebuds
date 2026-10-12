/*
 * nRF5340 DK port of fw/main.c: camera + BLE streaming (CAM_APP=STREAM, the default; it
 * also takes single pictures on 0xB2, snapshot.h) or Monitor (CAM_APP=MONITOR). The main thread runs the same event loop as fw/; it sleeps in
 * eventQueueWait() instead of nrf_pwr_mgmt_run(), and sends a frame in one call
 * (imageSend / bleSendImage return once every packet is queued) instead of polling bleService().
 * The VueBuds board's PMU, IMU, buttons, LEDs and CLI are not ported.
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#ifdef CPU_128MHZ
#include <nrfx_clock.h>
#endif

#include "nrf_log.h"
#include "event.h"
#include "camera.h"
#include "monitor.h"
#include "snapshot.h"
#include "ble.h"
#include "image.h"
#include "i2c.h"
#include "timers.h"

#ifdef CAPTURE_STRESS_US
extern void stressTestStart(void);
#endif

// Stream builds take either a continuous stream (0xB1) or single pictures (0xB2) per boot.
static bool streaming;

static void processEvent(event_t event)
{
  switch (event) {
    case EVENT_BLE_DATA_STREAM_START:
      NRF_LOG_RAW_INFO("%08d [ble] stream start\n", systemTimeGetMs());
      break;

    case EVENT_BLE_DATA_STREAM_STOP:
    case EVENT_BLE_DISCONNECTED:
      sys_reboot(SYS_REBOOT_COLD);  // fw/: NVIC_SystemReset()
      break;

    case EVENT_CAMERA_STREAM_START:
#ifdef CAMERA_APP_MONITOR
      NRF_LOG_RAW_INFO("%08d [main] Monitor build: BLE streaming is disabled\n", systemTimeGetMs());
      break;
#endif
      if (snapshotActive()) {
        NRF_LOG_RAW_INFO("%08d [main] taking single pictures (0xB2): stream start ignored\n", systemTimeGetMs());
        break;
      }
      streaming = true;
      cameraInit();
      cameraStartStream();
#ifdef CAPTURE_STRESS_US
      stressTestStart();
#endif
      break;

    case EVENT_CAMERA_SNAPSHOT:
#ifdef CAMERA_APP_MONITOR
      NRF_LOG_RAW_INFO("%08d [main] Monitor build: single pictures are disabled\n", systemTimeGetMs());
      break;
#endif
      if (streaming) {
        NRF_LOG_RAW_INFO("%08d [main] streaming (0xB1): picture request ignored\n", systemTimeGetMs());
        break;
      }
      snapshotRequest();
      break;

    case EVENT_CAMERA_CAPTURE_DONE: {
#ifdef CAMERA_APP_MONITOR
      monitorFrameDone();
      break;
#endif
      if (snapshotActive()) {
        snapshotFrameDone();
        break;
      }
      NRF_LOG_RAW_INFO("%08d [cam] EVENT_CAMERA_CAPTURE_DONE\n", systemTimeGetMs());
      camera_frame_t frame;
      if (cameraGetFrame(&frame)) {
        imagePrepare(&frame);
        imageSend(&frame);
      }
      // Sent (or a bad frame, not sent): capture the next one.
      eventQueuePush(EVENT_CAMERA_READY_NEXT_FRAME);
      break;
    }

    case EVENT_CAMERA_READY_NEXT_FRAME:
      NRF_LOG_RAW_INFO("%08d [cam] EVENT_CAMERA_READY_NEXT_FRAME\n", systemTimeGetMs());
      cameraReadyNextFrame();
      break;

#ifdef CAMERA_APP_MONITOR
    case EVENT_MONITOR_START:
      monitorStart();
      break;

    case EVENT_MONITOR_WAKE:
      monitorWake();
      break;

    case EVENT_CAMERA_ASLEEP:
      monitorAsleep();
      break;
#else
    case EVENT_CAMERA_ASLEEP:
      snapshotAsleep();
      break;
#endif

    case EVENT_BLE_COMMAND: {
      ble_command_t cmd;
      while (bleNextCommand(&cmd)) {
        imageCommand(&cmd);
#ifndef CAMERA_APP_MONITOR
        if (cmd.data[0] == IMAGE_CMD_SET_EXPOSURE) {
          snapshotExposureChanged();
        } else if (cmd.data[0] == IMAGE_CMD_CODEC_BENCH && !streaming) {
          snapshotRequest();  // the bench picture (while streaming: the next streamed frame)
        }
#endif
      }
      break;
    }

    default:
      NRF_LOG_RAW_INFO("%08d [main] unhandled event:%d\n", systemTimeGetMs(), event);
      break;
  }
}

int main(void)
{
#ifdef CPU_128MHZ
  // Experiment (10-11): the application core runs at 64 MHz by default (HFCLK / 2); 128 MHz as
  // in nrf/samples/bluetooth/nrf_auraconfig, for the JPEG / PNG encoders.
  int divErr = nrfx_clock_divider_set(NRF_CLOCK_DOMAIN_HFCLK, NRF_CLOCK_HFCLK_DIV_1);
  SystemCoreClockUpdate();
  NRF_LOG_RAW_INFO("[mustard] CPU %u MHz (%d)\n", SystemCoreClock / 1000000, divErr);
#endif
  NRF_LOG_RAW_INFO("%08d [mustard] booting... (nRF5340)\n", systemTimeGetMs());
  i2cInit();
#ifdef HW_TEST
  extern void hwTestRun(void);
  hwTestRun();  // hardware check only (src/hw_test.c): no BLE, no camera application
  for (;;) {
    k_sleep(K_FOREVER);
  }
#endif
  bleInit();
  NRF_LOG_RAW_INFO("%08d [mustard] booted\n", systemTimeGetMs());

#ifdef CAMERA_APP_MONITOR
  eventQueuePush(EVENT_MONITOR_START);
#endif

  for (;;) {
    processEvent(eventQueueWait());
  }
  return 0;
}
