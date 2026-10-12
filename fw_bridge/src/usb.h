/* USB CDC ACM link to the PC (the DK's "nRF USB" connector). */
#ifndef USB_H_
#define USB_H_

#include <stdbool.h>
#include <stdint.h>

bool usbInit(void);
bool usbHostOpen(void);              // the PC has the port open (DTR)
void usbWaitHostOpen(void);
void usbRead(void *buf, uint32_t len);  // blocks until len bytes arrived
bool usbReadTimeout(void *buf, uint32_t len, int32_t timeout_ms);
void usbWrite(const void *buf, uint32_t len);  // blocks while the TX ring is full

/* Bridge frames to the PC: [A5 5A][type][slot][len u16 LE][payload]. Thread-safe. */
void usbSendFrame(uint8_t type, uint8_t slot, const void *payload, uint16_t len);

#endif
