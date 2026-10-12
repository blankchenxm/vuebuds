/*
 * USB CDC ACM, interrupt driven, through two ring buffers. Zephyr's CDC ACM class sends
 * one 64-byte USB transfer at a time: about 212 KB/s to the PC (host/usb_test.py), above
 * what two BLE links deliver together (~170 KB/s at most).
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>

#include "usb.h"

static const struct device *const cdc = DEVICE_DT_GET(DT_NODELABEL(cdc_acm_uart0));

RING_BUF_DECLARE(rxRing, 4096);
RING_BUF_DECLARE(txRing, 16384);
static K_SEM_DEFINE(rxReady, 0, 1);
static K_SEM_DEFINE(txSpace, 0, 1);
static K_MUTEX_DEFINE(txLock);  // one frame at a time (BT RX thread and main thread both send)

static void cdcIrq(const struct device *dev, void *user_data)
{
  ARG_UNUSED(user_data);
  while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
    if (uart_irq_rx_ready(dev)) {
      uint8_t *p;
      uint32_t room = ring_buf_put_claim(&rxRing, &p, 512);
      if (room == 0) {
        uart_irq_rx_disable(dev);  // re-enabled by the reader
      } else {
        int n = uart_fifo_read(dev, p, room);
        ring_buf_put_finish(&rxRing, n > 0 ? n : 0);
      }
      k_sem_give(&rxReady);
    }
    if (uart_irq_tx_ready(dev)) {
      uint8_t *p;
      uint32_t avail = ring_buf_get_claim(&txRing, &p, 512);
      if (avail == 0) {
        ring_buf_get_finish(&txRing, 0);
        uart_irq_tx_disable(dev);
      } else {
        int n = uart_fifo_fill(dev, p, avail);
        ring_buf_get_finish(&txRing, n > 0 ? n : 0);
      }
      k_sem_give(&txSpace);
    }
  }
}

bool usbInit(void)
{
  if (!device_is_ready(cdc)) {
    return false;
  }
  uart_irq_callback_set(cdc, cdcIrq);
  return true;
}

bool usbHostOpen(void)
{
  uint32_t dtr = 0;
  return uart_line_ctrl_get(cdc, UART_LINE_CTRL_DTR, &dtr) == 0 && dtr;
}

void usbWaitHostOpen(void)
{
  while (!usbHostOpen()) {
    k_msleep(50);
  }
  uart_irq_rx_enable(cdc);
}

bool usbReadTimeout(void *buf, uint32_t len, int32_t timeout_ms)
{
  uint8_t *dst = buf;
  int64_t deadline = k_uptime_get() + timeout_ms;
  while (len > 0) {
    uint32_t n = ring_buf_get(&rxRing, dst, len);
    dst += n;
    len -= n;
    if (len > 0) {
      if (timeout_ms >= 0 && k_uptime_get() >= deadline) {
        return false;
      }
      uart_irq_rx_enable(cdc);
      k_sem_take(&rxReady, K_MSEC(10));
    }
  }
  return true;
}

void usbRead(void *buf, uint32_t len)
{
  usbReadTimeout(buf, len, -1);
}

static void writeLocked(const void *buf, uint32_t len)
{
  const uint8_t *src = buf;
  while (len > 0) {
    uint32_t n = ring_buf_put(&txRing, src, len);
    src += n;
    len -= n;
    uart_irq_tx_enable(cdc);
    if (len > 0) {
      if (!usbHostOpen()) {
        ring_buf_reset(&txRing);  // nobody reads: drop instead of blocking the BLE side forever
        return;
      }
      k_sem_take(&txSpace, K_MSEC(10));
    }
  }
}

void usbWrite(const void *buf, uint32_t len)
{
  k_mutex_lock(&txLock, K_FOREVER);
  writeLocked(buf, len);
  k_mutex_unlock(&txLock);
}

void usbSendFrame(uint8_t type, uint8_t slot, const void *payload, uint16_t len)
{
  uint8_t header[6] = {0xA5, 0x5A, type, slot, len & 0xFF, len >> 8};
  k_mutex_lock(&txLock, K_FOREVER);
  writeLocked(header, sizeof(header));
  writeLocked(payload, len);
  k_mutex_unlock(&txLock);
}
