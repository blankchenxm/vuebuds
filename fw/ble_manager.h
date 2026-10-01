#ifndef BLE_MANAGER_H
#define BLE_MANAGER_H_

void bleAdvertisingStart();
void bleInit(void);
void send(void);

/* Send a width x height frame whose rows are stride bytes apart. pixels must stay valid until
 * EVENT_CAMERA_READY_NEXT_FRAME. */
void bleSendFrame(const uint8_t *pixels, uint16_t stride, uint16_t width, uint16_t height);

ret_code_t bleDisconnect(void);
void bleService(void);

#endif
