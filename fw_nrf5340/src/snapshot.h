#ifndef SNAPSHOT_H_
#define SNAPSHOT_H_

#include <stdbool.h>
#include <stdint.h>

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
 * Manual exposure (0xB6): the HM0360 stays awake (no S2) and a request takes the next frame,
 * because a wake from S2 meters the scene itself and ignores fixed exposure values (camera.c).
 */

void snapshotRequest(void);    // EVENT_CAMERA_SNAPSHOT
/* 0xB7 (timesync.h): the HM0360 wakes at this local time in hardware (plan E1); asleep sensor
 * only, otherwise (first picture, fixed exposure, still falling asleep) as snapshotRequest. */
void snapshotRequestAt(uint64_t local_us);
void snapshotFrameDone(void);  // EVENT_CAMERA_CAPTURE_DONE: sleep the sensor, send the frame
void snapshotAsleep(void);     // EVENT_CAMERA_ASLEEP: XSLEEP went low on the frame boundary, stop MCLK
bool snapshotActive(void);     // a picture has been requested since boot

/*
 * Timed capture (0xB8 <mode> <period u16 ms>, F5, hub INTERFACES C2, 2026-11-19): mode 1 takes
 * a picture every period (100-60000 ms) on the 32.768 kHz clock, the first one right away, the
 * same way as 0xB2; mode 0 stops. A period that comes while the previous picture is still being
 * taken or sent is skipped (not queued): the next picture has BLE_HEADER_SKIPPED. Timed pictures
 * carry BLE_HEADER_TIMED, so a 0xB2 in between can be told apart. Mode 2 (ring buffer) is not here.
 */
#define SNAPSHOT_CMD_TIMED 0xB8
void snapshotTimedCommand(const uint8_t *data, uint8_t len);
void snapshotTimedTick(void);  // EVENT_CAMERA_TIMED
void snapshotExposureChanged(void);  // after 0xB6: manual -> wake and stay awake, auto -> sleep again

#endif
