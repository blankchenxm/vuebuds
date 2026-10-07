/* BLE peripheral, same protocol as fw/ble_manager.c + ble_cus.c (see host/protocol.py). */
#ifndef BLE_H_
#define BLE_H_

#include <stdint.h>

void bleInit(void);  // starts advertising
// Sends one frame (row by row from the capture slot) and returns when every packet is queued.
void bleSendFrame(const uint8_t *pixels, uint16_t stride, uint16_t width, uint16_t height);

#endif
