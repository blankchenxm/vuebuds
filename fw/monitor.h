#ifndef MONITOR_H_
#define MONITOR_H_

/*
 * HM0360 Monitor (cache only): RTC2 wakes the sensor every MONITOR_PERIOD_MS (S2:
 * MCLK on, XSLEEP high), one frame is written into the oldest frame-pool slot,
 * then the sensor sleeps again (XSLEEP low, MCLK off). Built with CAM_APP=MONITOR.
 */

void monitorStart(void);      // EVENT_MONITOR_START: init the camera, start RTC2
void monitorWake(void);       // EVENT_MONITOR_WAKE (RTC2): wake the sensor, arm the oldest slot
void monitorFrameDone(void);  // EVENT_CAMERA_CAPTURE_DONE: sleep the sensor, mark the slot ready

#endif
