/* BLE peripheral, same protocol as fw/ble_manager.c + ble_cus.c (see host/protocol.py). */
#ifndef BLE_H_
#define BLE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Frame header v2 (2026-10-11, host/protocol.py): the start-of-frame packet has flags bit1 set
 * and its payload starts with this header instead of v1's width, height. Little endian.
 */
#define BLE_FORMAT_RAW  0  // 8-bit gray, width x height
#define BLE_FORMAT_PNG  1
#define BLE_FORMAT_JPEG 2

#define BLE_HEADER_COMMON_CLOCK    0x01  // capture_us is on the bridge's common clock (time sync)
#define BLE_HEADER_MANUAL_EXPOSURE 0x02

typedef struct __packed {
  uint16_t width;
  uint16_t height;
  uint8_t format;         // BLE_FORMAT_*
  uint8_t quality;        // JPEG quality, else 0
  uint32_t length;        // image bytes after the header (raw: width x height)
  uint32_t capture_us;    // FVLD rise of this frame (board clock unless BLE_HEADER_COMMON_CLOCK)
  uint16_t frame_id;      // frames sent since boot
  uint16_t integration;   // exposure registers read after the frame: 0x0202 / 0x0203 (rows)
  uint8_t analog_gain;    // 0x0205
  uint16_t digital_gain;  // 0x020E << 8 | 0x020F
  uint8_t flags;          // BLE_HEADER_*
} ble_frame_header_t;

/* Copies image bytes [offset, offset + n) to dst; returns the bytes copied (0: source failed). */
typedef size_t (*ble_reader_t)(void *ctx, uint32_t offset, uint8_t *dst, size_t n);

/* Multi-byte control commands (0xB3 ...), queued for the main thread (EVENT_BLE_COMMAND). */
#define BLE_COMMAND_MAX 16
typedef struct {
  uint8_t len;
  uint8_t data[BLE_COMMAND_MAX];
} ble_command_t;

void bleInit(void);  // starts advertising
// Sends header + header->length bytes from read() and returns when every packet is queued.
bool bleSendImage(const ble_frame_header_t *header, ble_reader_t read, void *ctx);
bool bleNextCommand(ble_command_t *cmd);  // false when the queue is empty

#endif
