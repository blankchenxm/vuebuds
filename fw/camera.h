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
void cameraDeInit(void);
void cameraStartStream(void);
void cameraReadyNextFrame(void);
bool cameraGetFrame(camera_frame_t *frame);
void cameraEnableStandbyMode(bool);

#endif
