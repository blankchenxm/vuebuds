/* BLE central: up to two "mustard" camera boards (fw_nrf5340/src/ble.c protocol). */
#ifndef CENTRAL_H_
#define CENTRAL_H_

#include <stdint.h>
#include <zephyr/bluetooth/addr.h>

#define CENTRAL_SLOTS 2

/* Bridge -> PC frame types (usbSendFrame) */
#define FRAME_NOTIFY 'N'  // payload: one data notification ([seq, flags, ...])
#define FRAME_EVENT  'E'  // payload: central_event_t
#define FRAME_TEXT   'S'  // payload: log text
#define FRAME_WRITE  'W'  // payload: int8 result of a control write (0 = queued)

enum {
  CENTRAL_DISCONNECTED = 0,
  CENTRAL_CONNECTED = 1,
  CENTRAL_READY = 2,    // notifications on, control handle known
  CENTRAL_UPDATED = 3,  // connection parameters / PHY / data length changed
};

typedef struct __packed {
  uint8_t state;
  uint8_t addr_type;
  uint8_t addr[6];          // LSB first, as bt_addr_t
  uint16_t interval_1250us;
  uint8_t tx_phy, rx_phy;   // 1 = 1M, 2 = 2M
  uint16_t tx_len, rx_len;  // data length
  uint16_t mtu;
  uint8_t reason;           // HCI reason on disconnect
} central_event_t;

void centralInit(void);
/* Connect to these boards (slot i = addrs[i]); slots whose address did not change keep their link. */
void centralConfigure(const bt_addr_le_t *addrs, uint8_t count, uint16_t interval_1250us);
/* Write without response to the control characteristic of every ready slot in mask. */
void centralWrite(uint8_t mask, const uint8_t *data, uint16_t len);
/* Drop the links in mask; they reconnect by themselves (the camera reboots on disconnect). */
void centralDisconnect(uint8_t mask);
/* Time sync: write [cmd][this link's anchor u64][counter u16][T u64] to every ready slot in mask,
 * T = the newest anchor point + delay_ms (the same T for all), e.g. cmd 0xB7 = picture at T. */
void centralTimedWrite(uint8_t mask, uint16_t delay_ms, uint8_t cmd);

#endif
