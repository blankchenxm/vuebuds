/* Main-loop event queue (fw/event.h). Push is safe from interrupts. */
#ifndef EVENT_H_
#define EVENT_H_

#include <stdbool.h>

typedef enum {
  EVENT_NONE = 0,
  EVENT_BLE_DATA_STREAM_START,
  EVENT_BLE_DATA_STREAM_STOP,
  EVENT_BLE_DISCONNECTED,
  EVENT_CAMERA_STREAM_START,
  EVENT_CAMERA_CAPTURE_DONE,
  EVENT_CAMERA_READY_NEXT_FRAME,
  EVENT_MONITOR_START,
  EVENT_MONITOR_WAKE,
  EVENT_CAMERA_ASLEEP,
  EVENT_CAMERA_SNAPSHOT,
  EVENT_BLE_COMMAND,  // a multi-byte control command is queued (bleNextCommand)
  EVENT_CAMERA_TIMED,  // 0xB8 timed capture: the period's timer expired
  EVENT_PSRAM_CHECK,  // PSRAM_CHECK_WAIT_MS after the boot check started: read it back (psram.h)
} event_t;

void eventQueuePush(event_t event);
event_t eventQueueWait(void);  // blocks the calling thread until an event arrives

#endif
