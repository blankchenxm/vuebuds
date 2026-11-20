#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>

#include "nrf_log.h"

#include "ble.h"
#include "psram.h"
#include "timers.h"
#include "ring.h"

#define VERIFY_CHUNK 1024u

static ring_entry_t m_index[RING_INDEX_MAX];  // circular: m_head = oldest
static uint32_t m_head, m_count;
static uint32_t m_write_addr;                 // where the next frame goes
static uint64_t m_last_us;                    // for extending the 32-bit capture time
static ring_stats_t m_stats;
static uint8_t m_chunk[VERIFY_CHUNK];
static bool m_ready;

bool ringInit(void)
{
  m_ready = psramInit();
  NRF_LOG_RAW_INFO("%08d [ring] %s: %u KB, index %u frames\n", systemTimeGetMs(),
                   m_ready ? "PSRAM ready" : "NO PSRAM, ring off", RING_BYTES / 1024, RING_INDEX_MAX);
  return m_ready;
}

static ring_entry_t *slot(uint32_t i)
{
  return &m_index[(m_head + i) % RING_INDEX_MAX];
}

static void evictOldest(void)
{
  m_stats.used_bytes -= slot(0)->len;
  m_head = (m_head + 1) % RING_INDEX_MAX;
  m_count--;
  m_stats.evicted++;
}

// Does [a, a + n) overlap the oldest entry?
static bool overlapsOldest(uint32_t a, uint32_t n)
{
  const ring_entry_t *o = slot(0);
  return a < o->addr + o->len && o->addr < a + n;
}

bool ringAppend(const camera_frame_t *frame, uint32_t seq, uint32_t capture_us32, uint32_t *write_us)
{
  *write_us = 0;
  if (!m_ready) {
    return false;
  }
  uint32_t len = (uint32_t)frame->width * frame->height;
  if (len > RING_BYTES) {
    return false;
  }
  uint32_t addr = m_write_addr;
  if (addr + len > RING_BYTES) {
    addr = 0;  // a frame never wraps
    m_stats.wraps++;
  }
  // Drop the oldest frames this one will overwrite (and one more if the index is full).
  while (m_count > 0 && (m_count == RING_INDEX_MAX || overlapsOldest(addr, len))) {
    evictOldest();
  }

  uint32_t t0 = systemTimeGetUs();
  uint32_t crc = 0;
  bool ok = true;
  if (frame->stride == frame->width) {
    crc = crc32_ieee_update(crc, frame->pixels, len);
    ok = psramWrite(addr, frame->pixels, len);
  } else {
    for (uint16_t y = 0; y < frame->height && ok; y++) {
      const uint8_t *row = frame->pixels + (uint32_t)y * frame->stride;
      crc = crc32_ieee_update(crc, row, frame->width);
      ok = psramWrite(addr + (uint32_t)y * frame->width, row, frame->width);
    }
  }
  *write_us = systemTimeGetUs() - t0;
  if (!ok) {
    m_stats.write_errors++;
    return false;
  }

  // 64-bit capture time: the 32-bit us clock wraps every ~71.6 min.
  uint64_t t = (m_last_us & ~0xFFFFFFFFull) | capture_us32;
  if (m_count + m_stats.appended > 0 && t < m_last_us) {
    t += 1ull << 32;
  }
  m_last_us = t;

  ring_entry_t *e = &m_index[(m_head + m_count) % RING_INDEX_MAX];
  *e = (ring_entry_t){.seq = seq, .capture_us = t, .addr = addr, .len = len, .crc = crc,
                      .width = frame->width, .height = frame->height, .format = BLE_FORMAT_RAW};
  m_count++;
  m_write_addr = addr + len;
  m_stats.appended++;
  m_stats.used_bytes += len;
  m_stats.frames = m_count;
  return true;
}

bool ringVerify(const ring_entry_t *e, uint32_t *read_us)
{
  uint32_t t0 = systemTimeGetUs();
  uint32_t crc = 0;
  bool ok = true;
  for (uint32_t off = 0; off < e->len && ok; off += VERIFY_CHUNK) {
    uint32_t n = MIN(VERIFY_CHUNK, e->len - off);
    ok = psramRead(e->addr + off, m_chunk, n);
    crc = crc32_ieee_update(crc, m_chunk, n);
  }
  *read_us = systemTimeGetUs() - t0;
  return ok && crc == e->crc;
}

uint32_t ringCount(void)
{
  return m_count;
}

const ring_entry_t *ringAt(uint32_t i)
{
  return i < m_count ? slot(i) : NULL;
}

const ring_entry_t *ringNewest(void)
{
  return m_count ? slot(m_count - 1) : NULL;
}

// Capture times only grow, so the index is sorted: binary search for the last entry <= t.
const ring_entry_t *ringFindAtOrBefore(uint64_t t_us)
{
  if (m_count == 0 || slot(0)->capture_us > t_us) {
    return NULL;
  }
  uint32_t lo = 0, hi = m_count - 1;
  while (lo < hi) {
    uint32_t mid = (lo + hi + 1) / 2;
    if (slot(mid)->capture_us <= t_us) {
      lo = mid;
    } else {
      hi = mid - 1;
    }
  }
  return slot(lo);
}

const ring_entry_t *ringFindNearest(uint64_t t_us)
{
  const ring_entry_t *before = ringFindAtOrBefore(t_us);
  if (before == NULL) {
    return m_count ? slot(0) : NULL;
  }
  uint32_t i = (uint32_t)((before - m_index + RING_INDEX_MAX - m_head) % RING_INDEX_MAX);
  if (i + 1 < m_count) {
    const ring_entry_t *after = slot(i + 1);
    if (after->capture_us - t_us < t_us - before->capture_us) {
      return after;
    }
  }
  return before;
}

const ring_stats_t *ringStats(void)
{
  return &m_stats;
}
