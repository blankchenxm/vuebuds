#ifndef CAMERA_H_
#define CAMERA_H_

#include <stdbool.h>
#include <stdint.h>

/* A frame as sent over BLE: the standard region inside the transport frame. */
typedef struct {
  const uint8_t *pixels;  // first pixel of the standard region
  uint16_t stride;        // transport width
  uint16_t width;
  uint16_t height;
} camera_frame_t;

void cameraInit(void);

void cameraStartStream(void);
void cameraReadyNextFrame(void);
bool cameraGetFrame(camera_frame_t *frame);  // false for a bad frame (wrong DMA byte count / overflow)

/* Monitor (HM0360) */
uint16_t cameraModelId(void);              // 0 until cameraInit() found a sensor
const char *cameraModeName(void);
uint32_t cameraSlotCount(void);            // frame pool slots for the compiled mode
void cameraSensorStream(void);             // MCLK on + sensor streaming, nothing armed
void cameraWake(void);                     // S2 off: MCLK on, then XSLEEP high
bool cameraWakeAt(uint64_t local_us);      // the same at a timesync local time (hardware); false: too late
void cameraSleepOnFrameBoundary(void);     // S2 on: XSLEEP low on the next FVLD fall (PPI) ...
void cameraAsleep(void);                   // ... EVENT_CAMERA_ASLEEP: then MCLK off
bool cameraIsAsleep(void);                 // XSLEEP low and MCLK off
void cameraArmSlot(uint32_t slot, uint8_t skipFrames);
bool cameraSlotFrame(uint32_t slot, camera_frame_t *frame);

/* Exposure registers (same addresses on both sensors: 0x0202/0x0203, 0x0205, 0x020E/0x020F). */
typedef struct {
  uint16_t integration;   // rows
  uint8_t analog_gain;
  uint16_t digital_gain;  // 0x020E << 8 | 0x020F
} camera_exposure_t;

bool cameraReadExposure(camera_exposure_t *exposure);  // I2C; false while asleep (S2) or off
/* NULL: automatic exposure (AE). Otherwise AE off and these values (HM0360 only); applied now if
 * the sensor is awake, else right after the next wake. */
void cameraSetExposure(const camera_exposure_t *exposure);
bool cameraExposureManual(void);
void cameraReapplyExposure(void);  // manual and awake: write the fixed values again

#endif
