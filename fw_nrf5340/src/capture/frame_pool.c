/*
 * Frame pool: one static block cut into equal slots of one transport frame each.
 * fw/ gives the pool all RAM the linker leaves between heap and stack; here the size is
 * fixed (FRAME_POOL_BYTES) and set to what is left of the application core's RAM.
 *
 * nRF5340 only: the SPIS DMA receives nothing when RXD.PTR is not word aligned (measured:
 * HM01B0 QQVGA, DMA start = slot + 1 line x 162 B, got 0 bytes per frame). So each slot is
 * shifted by 0-3 bytes such that slot + dma_offset (the first byte DMA writes) is aligned.
 */
#include <stddef.h>
#include <stdint.h>

#include <zephyr/sys/util.h>

#include "capture.h"

#ifndef FRAME_POOL_BYTES
#define FRAME_POOL_BYTES 384000u  // 5 HM0360 QVGA frames; ~28 KB of RAM left
#endif

static uint8_t m_pool[FRAME_POOL_BYTES] __aligned(4);
static uint32_t m_lead;  // bytes before each slot so that slot + dma_offset is word aligned
static uint32_t m_slot_size;
static uint32_t m_slot_count;

uint32_t frame_pool_init(uint32_t frame_bytes, uint32_t dma_offset)
{
  m_lead = (4u - dma_offset % 4u) % 4u;
  m_slot_size = (m_lead + frame_bytes + 3u) & ~3u;
  m_slot_count = frame_pool_size() / m_slot_size;
  return m_slot_count;
}

uint32_t frame_pool_size(void)
{
  return sizeof(m_pool);
}

uint32_t frame_pool_slot_count(void)
{
  return m_slot_count;
}

uint8_t *frame_pool_slot(uint32_t index)
{
  if (index >= m_slot_count) {
    return NULL;
  }
  return m_pool + index * m_slot_size + m_lead;
}
