/*
 * USB link test (build with -Dfw_bridge_USB_TEST=1, host/usb_test.py). Raw commands, little-endian:
 *   'I'                 -> "VueBuds bridge\n"
 *   'T' <u32 n>         -> n bytes of u32 counter words 0, 1, 2, ..., then 'D' <u32 device ms>
 *   'R' <u32 n> <n B>   -> the bridge checks the pattern, then 'r' <u32 bad words> <u32 device ms>
 */
#ifdef USB_TEST

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "usb.h"

static uint32_t readU32(void)
{
  uint32_t v;
  usbRead(&v, sizeof(v));
  return v;
}

static void sendPattern(uint32_t bytes)
{
  static uint32_t words[256];
  uint32_t counter = 0;
  int64_t t0 = k_uptime_get();
  while (bytes > 0) {
    uint32_t chunk = MIN(bytes, sizeof(words));
    for (uint32_t i = 0; i < chunk / 4; i++) {
      words[i] = counter++;
    }
    usbWrite(words, chunk);
    bytes -= chunk;
  }
  uint32_t ms = (uint32_t)(k_uptime_get() - t0);
  uint8_t done = 'D';
  usbWrite(&done, 1);
  usbWrite(&ms, sizeof(ms));
  printk("[usb] sent pattern, %u ms\n", ms);
}

static void checkPattern(uint32_t bytes)
{
  static uint32_t words[256];
  uint32_t counter = 0, bad = 0;
  int64_t t0 = k_uptime_get();
  while (bytes > 0) {
    uint32_t chunk = MIN(bytes, sizeof(words));
    usbRead(words, chunk);
    for (uint32_t i = 0; i < chunk / 4; i++) {
      if (words[i] != counter++) {
        bad++;
      }
    }
    bytes -= chunk;
  }
  uint32_t ms = (uint32_t)(k_uptime_get() - t0);
  uint8_t reply = 'r';
  usbWrite(&reply, 1);
  usbWrite(&bad, sizeof(bad));
  usbWrite(&ms, sizeof(ms));
  printk("[usb] received pattern, %u bad words, %u ms\n", bad, ms);
}

void usbTestRun(void)
{
  for (;;) {
    usbWaitHostOpen();
    printk("[usb] host port open\n");
    while (usbHostOpen()) {
      uint8_t cmd;
      if (!usbReadTimeout(&cmd, 1, 100)) {
        continue;
      }
      switch (cmd) {
        case 'I': {
          static const char id[] = "VueBuds bridge\n";
          usbWrite(id, sizeof(id) - 1);
          break;
        }
        case 'T':
          sendPattern(readU32() & ~3u);
          break;
        case 'R':
          checkPattern(readU32() & ~3u);
          break;
        default:
          printk("[usb] unknown command 0x%02x\n", cmd);
          break;
      }
    }
    printk("[usb] host port closed\n");
  }
}

#endif /* USB_TEST */
