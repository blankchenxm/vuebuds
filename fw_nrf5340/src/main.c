/*
 * nRF5340 DK port of fw/main.c: camera + BLE streaming (CAM_APP=STREAM, the default) or
 * Monitor (CAM_APP=MONITOR). The main thread runs the same event loop as fw/; it sleeps in
 * eventQueueWait() instead of nrf_pwr_mgmt_run(), and sends a frame in one call
 * (bleSendFrame returns once every packet is queued) instead of polling bleService().
 * The VueBuds board's PMU, IMU, buttons, LEDs and CLI are not ported.
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>

#include "nrf_log.h"
#include "event.h"
#include "camera.h"
#include "monitor.h"
#include "ble.h"
#include "i2c.h"
#include "timers.h"

#ifdef CAPTURE_STRESS_US
extern void stressTestStart(void);
#endif

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
      cameraInit();
      cameraStartStream();
#ifdef CAPTURE_STRESS_US
      stressTestStart();
#endif
      break;

    case EVENT_CAMERA_CAPTURE_DONE: {
#ifdef CAMERA_APP_MONITOR
      monitorFrameDone();
      break;
#endif
      NRF_LOG_RAW_INFO("%08d [cam] EVENT_CAMERA_CAPTURE_DONE\n", systemTimeGetMs());
      camera_frame_t frame;
      if (cameraGetFrame(&frame)) {
        bleSendFrame(frame.pixels, frame.stride, frame.width, frame.height);
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
#endif

    default:
      NRF_LOG_RAW_INFO("%08d [main] unhandled event:%d\n", systemTimeGetMs(), event);
      break;
  }
}

int main(void)
{
  NRF_LOG_RAW_INFO("%08d [mustard] booting... (nRF5340)\n", systemTimeGetMs());
  i2cInit();
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
