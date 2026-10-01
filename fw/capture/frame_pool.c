/*
 * Frame pool: all RAM the linker left between the heap and the stack
 * (__frame_pool_start/__frame_pool_end in ble_app_template_gcc_nrf52.ld),
 * cut into equal slots of one transport frame each.
 */
#include <stddef.h>
#include <stdint.h>

#include "capture.h"

extern uint8_t __frame_pool_start[];
extern uint8_t __frame_pool_end[];

static uint32_t m_slot_size;
static uint32_t m_slot_count;

uint32_t frame_pool_init(uint32_t frame_bytes)
{
  m_slot_size = (frame_bytes + 3u) & ~3u;
  m_slot_count = frame_pool_size() / m_slot_size;
  return m_slot_count;
}

uint32_t frame_pool_size(void)
{
  return (uint32_t)(__frame_pool_end - __frame_pool_start);
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
  return __frame_pool_start + index * m_slot_size;
}
