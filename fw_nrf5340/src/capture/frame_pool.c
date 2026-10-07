/*
 * Frame pool: all application-core RAM the image leaves free, from the end of the image
 * (_end; Zephyr allocates every stack and heap statically inside it) to the end of the RAM
 * region (__kernel_ram_end; the last 64 KB of the 512 KB are shared with the network core).
 * Same idea as fw/'s __frame_pool_start .. __frame_pool_end, cut into equal slots of one
 * transport frame each. libc malloc is off (prj.conf), so nothing else uses this RAM.
 *
 * nRF5340 only: the SPIS DMA receives nothing when RXD.PTR is not word aligned (measured:
 * HM01B0 QQVGA, DMA start = slot + 1 line x 162 B, got 0 bytes per frame). So each slot is
 * shifted by 0-3 bytes such that slot + dma_offset (the first byte DMA writes) is aligned.
 */
#include <stddef.h>
#include <stdint.h>

#include <zephyr/linker/linker-defs.h>

#include "capture.h"

#define POOL_START ((uint8_t *)(((uintptr_t)_end + 3u) & ~(uintptr_t)3u))
#define POOL_END ((uint8_t *)__kernel_ram_end)

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
  return (uint32_t)(POOL_END - POOL_START);
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
  return POOL_START + index * m_slot_size + m_lead;
}
