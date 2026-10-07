#ifndef MONITOR_H_
#define MONITOR_H_

/*
 * Monitor (cache only), built with CAM_APP=MONITOR; the sensor found at boot picks the flow:
 *   HM0360: a periodic k_timer (fw/: RTC2) wakes the sensor every MONITOR_PERIOD_MS (S2: MCLK on, XSLEEP high),
 *           one frame is written into the oldest frame-pool slot, then the sensor
 *           sleeps again (XSLEEP low, MCLK off).
 *   HM01B0: no S2; MCLK and the sensor stay on, and every MONITOR_PERIOD_MS the next
 *           frame is written into the oldest slot (the streaming flow without BLE).
 * MONITOR_PERIOD_MS=0 stores every frame. Bad frames (wrong DMA byte count) are not stored.
 */

void monitorStart(void);      // EVENT_MONITOR_START: init the camera, start the wake timer
void monitorWake(void);       // EVENT_MONITOR_WAKE: wake the sensor (HM0360), arm the oldest slot
void monitorFrameDone(void);
void monitorAsleep(void);     // EVENT_CAMERA_ASLEEP: XSLEEP went low on the frame boundary, stop MCLK  // EVENT_CAMERA_CAPTURE_DONE: sleep the sensor (HM0360), mark the slot ready

#endif
