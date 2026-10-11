#ifndef SNAPSHOT_H_
#define SNAPSHOT_H_

#include <stdbool.h>

/*
 * Take one picture (control command 0xB2, stream builds only). The first request
 * initializes the camera and starts the sensor; from then on:
 *   HM0360: the sensor sleeps between pictures (S2: XSLEEP low, MCLK off). A request wakes
 *           it, the first frame after the wake is captured into slot 0 and sent over BLE,
 *           and the sensor goes back to sleep on the frame boundary (the Monitor wake path).
 *   HM01B0: no S2; MCLK and the sensor stay on, and a request captures the next frame.
 * A bad frame (wrong DMA byte count / overflow) is not sent: the sensor stays awake and the
 * next frame is taken instead (up to SNAPSHOT_MAX_RETAKES times).
 * A request while a picture is still being taken or sent is ignored.
 */

void snapshotRequest(void);    // EVENT_CAMERA_SNAPSHOT
void snapshotFrameDone(void);  // EVENT_CAMERA_CAPTURE_DONE: sleep the sensor, send the frame
void snapshotAsleep(void);     // EVENT_CAMERA_ASLEEP: XSLEEP went low on the frame boundary, stop MCLK
bool snapshotActive(void);     // a picture has been requested since boot

#endif
