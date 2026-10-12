/*
 * VueBuds BLE-USB bridge (nRF52840 DK now, nRF5340 DK later): BLE central for the two camera
 * boards, USB CDC ACM to the PC ("nRF USB" connector). Protocol in host/bridge.py.
 *
 * PC -> bridge: [A5 5A][cmd][len u16 LE][payload]
 *   'I'  identify                                -> FRAME_TEXT "VueBuds bridge v1"
 *   'C'  [interval u16 (1.25 ms)][n][n x (type u8, addr 6 B LSB first)]: boards for slots 0..n-1
 *   'W'  [slot mask][bytes]: write the bytes to the control characteristic of those slots
 *   'X'  [slot mask]: disconnect (the boards reboot, the bridge reconnects)
 *   'T'  [slot mask][delay ms u16][camera command]: timed command at one bridge time T for all
 *        (central.h centralTimedWrite; 0xB7 = picture at T)
 * Bridge -> PC: [A5 5A][type][slot][len u16 LE][payload], types in central.h.
 *
 * Build with -Dfw_bridge_USB_TEST=1 for the USB link test instead (src/usb_test.c, host/usb_test.py).
 */
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "central.h"
#include "usb.h"

#ifdef USB_TEST
extern void usbTestRun(void);
#endif

#define CMD_MAX 64

static void handle(uint8_t cmd, const uint8_t *p, uint16_t len)
{
  switch (cmd) {
    case 'I': {
      static const char id[] = "VueBuds bridge v1";
      usbSendFrame(FRAME_TEXT, 0xFF, id, sizeof(id) - 1);
      break;
    }
    case 'C': {
      if (len < 3 || p[2] > CENTRAL_SLOTS || len < 3 + 7 * p[2]) {
        break;
      }
      bt_addr_le_t addrs[CENTRAL_SLOTS];
      for (int i = 0; i < p[2]; i++) {
        addrs[i].type = p[3 + 7 * i];
        memcpy(addrs[i].a.val, &p[4 + 7 * i], 6);
      }
      uint16_t interval = p[0] | (p[1] << 8);
      printk("[bridge] configure: %u board(s), interval %u x 1.25 ms\n", p[2], interval);
      centralConfigure(addrs, p[2], interval);
      break;
    }
    case 'W':
      if (len >= 2) {
        centralWrite(p[0], p + 1, len - 1);
      }
      break;
    case 'T':
      if (len >= 4) {
        centralTimedWrite(p[0], p[1] | (p[2] << 8), p[3]);
      }
      break;
    case 'X':
      if (len >= 1) {
        centralDisconnect(p[0]);
      }
      break;
    default:
      printk("[bridge] unknown command 0x%02x\n", cmd);
      break;
  }
}

int main(void)
{
  printk("[bridge] booting\n");
  if (!usbInit()) {
    printk("[bridge] CDC ACM not ready\n");
    return 0;
  }
#ifdef USB_TEST
  usbTestRun();
#endif
  centralInit();

  for (;;) {
    usbWaitHostOpen();
    printk("[bridge] host port open\n");
    while (usbHostOpen()) {
      uint8_t sync;
      if (!usbReadTimeout(&sync, 1, 100) || sync != 0xA5) {
        continue;
      }
      uint8_t h[4];
      if (!usbReadTimeout(h, 1, 100) || h[0] != 0x5A || !usbReadTimeout(h + 1, 3, 100)) {
        continue;
      }
      uint16_t len = h[2] | (h[3] << 8);
      static uint8_t payload[CMD_MAX];
      if (len > CMD_MAX || !usbReadTimeout(payload, len, 100)) {
        printk("[bridge] bad command frame (cmd 0x%02x, len %u)\n", h[1], len);
        continue;
      }
      handle(h[1], payload, len);
    }
    printk("[bridge] host port closed\n");
  }
  return 0;
}
