/*
 * Frame output over BLE (frame header v2, ble.h): the image format chosen at run time
 * (control command 0xB3) and the exposure settings (0xB6). Commands, little endian:
 *   B3 <format> <quality>                    format: 0 raw, 1 PNG, 2 JPEG (quality 1-100)
 *   B4                                       codec bench: the next frame (a picture in picture
 *                                            builds) goes out 7 times with the same frame id:
 *                                            raw, PNG, JPEG q95 / 90 / 85 / 75 / 60
 *   B6 00                                    automatic exposure
 *   B6 01 <integration u16> <again u8> <dgain u16>   fixed exposure (HM0360), register values
 */
#ifndef IMAGE_H_
#define IMAGE_H_

#include <stdbool.h>

#include "ble.h"
#include "camera.h"

#define IMAGE_CMD_SET_FORMAT   0xB3
#define IMAGE_CMD_CODEC_BENCH  0xB4
#define IMAGE_CMD_SET_EXPOSURE 0xB6

/* Right after a good frame, while the sensor is still awake: header fields and exposure. */
void imagePrepare(const camera_frame_t *frame);
/* Sends the prepared frame in the current format; false if it was not sent. */
bool imageSend(const camera_frame_t *frame);
void imageCommand(const ble_command_t *cmd);

#endif
