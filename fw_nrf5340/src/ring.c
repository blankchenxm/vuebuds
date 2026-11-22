#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>

#include "nrf_log.h"

#include "ble.h"
#include "codec.h"
#include "psram.h"
#include "timers.h"
#include "ring.h"

#define VERIFY_CHUNK 1024u

static ring_entry_t m_index[RING_INDEX_MAX];  // circular: m_head = oldest
static uint32_t m_head, m_count;
static uint32_t m_write_addr;                 // where the next frame goes
static ring_stats_t m_stats;
static uint8_t m_chunk[VERIFY_CHUNK];
static uint8_t *m_ram;                        // NULL: PSRAM
static uint32_t m_cap;                        // store bytes (0: no ring)
static uint32_t m_pinned;                     // frame being sent (ringPin)
K_MUTEX_DEFINE(ringIndexLock);

uint32_t ringInit(uint8_t *ram, uint32_t ram_bytes)
{
  if (psramCheckFinish()) {  // boot check (psram.h): waits until 2 s after boot
    m_ram = NULL;
    m_cap = RING_BYTES;
  } else {
    m_ram = ram;
    m_cap = ram ? ram_bytes : 0;
  }
  NRF_LOG_RAW_INFO("%08d [ring] %s: %u KB, index %u frames\n", systemTimeGetMs(),
                   m_cap == 0 ? "NO PSRAM, ring off" : (m_ram ? "PSRAM not usable, ring in RAM" : "PSRAM ready"),
                   m_cap / 1024, RING_INDEX_MAX);
  return m_cap;
}

bool ringInPsram(void)
{
  return m_cap != 0 && m_ram == NULL;
}

uint32_t ringCapacity(void)
{
  return m_cap;
}

static bool storeWrite(uint32_t addr, const void *data, uint32_t n)
{
  if (addr + n > m_cap) {
    return false;
  }
  if (m_ram) {
    memcpy(m_ram + addr, data, n);
    return true;
  }
  return psramWrite(addr, data, n);
}

static bool storeRead(uint32_t addr, void *data, uint32_t n)
{
  if (addr + n > m_cap) {
    return false;
  }
  if (m_ram) {
    memcpy(data, m_ram + addr, n);
    return true;
  }
  return psramRead(addr, data, n);
}

void ringLock(void)
{
  k_mutex_lock(&ringIndexLock, K_FOREVER);
}

void ringUnlock(void)
{
  k_mutex_unlock(&ringIndexLock);
}

void ringPin(uint32_t seq)
{
  ringLock();
  m_pinned = seq;
  ringUnlock();
}

static ring_entry_t *slot(uint32_t i)
{
  return &m_index[(m_head + i) % RING_INDEX_MAX];
}

// Drop the oldest entry, unless it is the one being sent.
static bool evictOldest(void)
{
  ringLock();
  bool ok = m_pinned == 0 || slot(0)->seq != m_pinned;
  if (ok) {
    m_stats.used_bytes -= slot(0)->len;
    m_head = (m_head + 1) % RING_INDEX_MAX;
    m_count--;
    m_stats.frames = m_count;
    m_stats.evicted++;
  } else {
    m_stats.pinned_fails++;
  }
  ringUnlock();
  return ok;
}

// Does [a, a + n) overlap the oldest entry?
static bool overlapsOldest(uint32_t a, uint32_t n)
{
  const ring_entry_t *o = slot(0);
  return a < o->addr + o->len && o->addr < a + n;
}

// Make room for [a, a + n): drop the oldest frames it covers (and one more if the index is full).
static bool makeRoom(uint32_t a, uint32_t n)
{
  while (m_count > 0 && (m_count == RING_INDEX_MAX || overlapsOldest(a, n))) {
    if (!evictOldest()) {
      return false;
    }
  }
  return true;
}

static void indexAdd(uint32_t seq, uint64_t capture_us, uint32_t addr, uint32_t len, uint32_t crc,
                     const camera_frame_t *frame, uint8_t format, uint8_t quality, const ring_meta_t *meta)
{
  ringLock();
  ring_entry_t *e = &m_index[(m_head + m_count) % RING_INDEX_MAX];
  *e = (ring_entry_t){.seq = seq, .capture_us = capture_us, .addr = addr, .len = len, .crc = crc,
                      .width = frame->width, .height = frame->height, .format = format, .quality = quality};
  if (meta) {
    e->integration = meta->integration;
    e->digital_gain = meta->digital_gain;
    e->analog_gain = meta->analog_gain;
    e->flags = meta->flags;
  }
  m_count++;
  m_write_addr = addr + len;
  m_stats.appended++;
  m_stats.used_bytes += len;
  m_stats.frames = m_count;
  ringUnlock();
}

// Where a frame of up to n bytes goes (a frame never wraps); 0 bytes free at the end -> address 0.
static uint32_t placeFor(uint32_t n)
{
  uint32_t addr = m_write_addr;
  if (addr + n > m_cap) {
    addr = 0;
    m_stats.wraps++;
  }
  return addr;
}

bool ringAppend(const camera_frame_t *frame, uint32_t seq, uint64_t capture_us, const ring_meta_t *meta,
                uint32_t *write_us)
{
  *write_us = 0;
  uint32_t len = (uint32_t)frame->width * frame->height;
  if (m_cap == 0 || len > m_cap) {
    return false;
  }
  uint32_t addr = placeFor(len);
  if (!makeRoom(addr, len)) {
    return false;
  }

  uint32_t t0 = systemTimeGetUs();
  uint32_t crc = 0;
  bool ok = true;
  if (frame->stride == frame->width) {
    crc = crc32_ieee_update(crc, frame->pixels, len);
    ok = storeWrite(addr, frame->pixels, len);
  } else {
    for (uint16_t y = 0; y < frame->height && ok; y++) {
      const uint8_t *row = frame->pixels + (uint32_t)y * frame->stride;
      crc = crc32_ieee_update(crc, row, frame->width);
      ok = storeWrite(addr + (uint32_t)y * frame->width, row, frame->width);
    }
  }
  *write_us = systemTimeGetUs() - t0;
  if (!ok) {
    m_stats.write_errors++;
    return false;
  }

  indexAdd(seq, capture_us, addr, len, crc, frame, BLE_FORMAT_RAW, 0, meta);
  return true;
}

bool ringRead(const ring_entry_t *e, uint32_t offset, uint8_t *dst, uint32_t n)
{
  return offset + n <= e->len && storeRead(e->addr + offset, dst, n);
}

bool ringVerify(const ring_entry_t *e, uint32_t *read_us)
{
  uint32_t t0 = systemTimeGetUs();
  uint32_t crc = 0;
  bool ok = true;
  for (uint32_t off = 0; off < e->len && ok; off += VERIFY_CHUNK) {
    uint32_t n = MIN(VERIFY_CHUNK, e->len - off);
    ok = storeRead(e->addr + off, m_chunk, n);
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

bool ringHas(const ring_entry_t *e)
{
  ringLock();
  bool found = false;
  for (uint32_t i = 0; i < m_count && !found; i++) {
    const ring_entry_t *s = slot(i);
    found = s->seq == e->seq && s->addr == e->addr;
  }
  ringUnlock();
  return found;
}

// Capture times only grow, so the index is sorted: binary search for the last entry <= t.
static int32_t lastAtOrBefore(uint64_t t_us)
{
  if (m_count == 0 || slot(0)->capture_us > t_us) {
    return -1;
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
  return (int32_t)lo;
}

const ring_entry_t *ringFindAtOrBefore(uint64_t t_us)
{
  int32_t i = lastAtOrBefore(t_us);
  return i < 0 ? NULL : slot(i);
}

uint32_t ringFirstAtOrAfter(uint64_t t_us)
{
  int32_t i = lastAtOrBefore(t_us);
  if (i >= 0 && slot(i)->capture_us == t_us) {
    return (uint32_t)i;
  }
  return (uint32_t)(i + 1);
}

const ring_entry_t *ringFindNearest(uint64_t t_us)
{
  int32_t i = lastAtOrBefore(t_us);
  if (i < 0) {
    return m_count ? slot(0) : NULL;
  }
  const ring_entry_t *before = slot(i);
  if ((uint32_t)i + 1 < m_count) {
    const ring_entry_t *after = slot(i + 1);
    if (after->capture_us - t_us < t_us - before->capture_us) {
      return after;
    }
  }
  return before;
}

// Frame numbers grow by one per stored frame, but a failed append leaves a gap: binary search.
const ring_entry_t *ringFindSeq(uint32_t seq)
{
  if (m_count == 0 || seq < slot(0)->seq || seq > slot(m_count - 1)->seq) {
    return NULL;
  }
  uint32_t lo = 0, hi = m_count - 1;
  while (lo < hi) {
    uint32_t mid = (lo + hi) / 2;
    if (slot(mid)->seq < seq) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return slot(lo)->seq == seq ? slot(lo) : NULL;
}

const ring_stats_t *ringStats(void)
{
  return &m_stats;
}

/* ---------------- compression (hub #39) ---------------- */

typedef struct {
  codec_io_t io;
  uint32_t base, pos, cap, crc;
  bool failed;
} ring_sink_t;

// Encoder output straight into the ring: free the oldest frames the bytes are about to cover.
static int32_t sinkWrite(codec_io_t *io, const uint8_t *data, int32_t len)
{
  ring_sink_t *s = io->ctx;
  if (s->failed || s->pos + len > s->cap) {
    s->failed = true;
    return 0;
  }
  uint32_t a = s->base + s->pos;
  if (!makeRoom(a, len) || !storeWrite(a, data, len)) {
    s->failed = true;
    return 0;
  }
  s->crc = crc32_ieee_update(s->crc, data, len);
  s->pos += len;
  return len;
}

// Fetch test: the output is only counted (and its CRC taken).
static int32_t countWrite(codec_io_t *io, const uint8_t *data, int32_t len)
{
  ring_sink_t *s = io->ctx;
  s->crc = crc32_ieee_update(s->crc, data, len);
  s->pos += len;
  return len;
}

static int32_t noRead(codec_io_t *io, uint8_t *data, int32_t len)
{
  return 0;
}

static int32_t noSeek(codec_io_t *io, int32_t position)
{
  return -1;
}

bool ringAppendJpeg(const camera_frame_t *frame, uint8_t quality, uint32_t seq, uint64_t capture_us,
                    const ring_meta_t *meta, uint32_t *encode_us)
{
  *encode_us = 0;
  uint32_t cap = (uint32_t)frame->width * frame->height;  // a JPEG larger than the raw frame fails
  if (m_cap == 0 || cap > m_cap) {
    return false;
  }
  uint32_t addr = placeFor(cap);
  ring_sink_t s = {.io = {sinkWrite, noRead, noSeek, NULL}, .base = addr, .cap = cap};
  s.io.ctx = &s;
  uint32_t t0 = systemTimeGetUs();
  int32_t n = codecJpeg(frame->pixels, frame->stride, frame->width, frame->height, quality, &s.io);
  *encode_us = systemTimeGetUs() - t0;
  if (n <= 0 || s.failed || (uint32_t)n != s.pos) {
    m_stats.write_errors++;
    NRF_LOG_RAW_INFO("[ring] JPEG q%u failed: %d, %u B written%s\n", quality, n, s.pos,
                     s.failed ? ", sink full / write error / frame being sent" : "");
    m_write_addr = addr;  // the partial output is garbage; evicted frames stay evicted
    return false;
  }
  indexAdd(seq, capture_us, addr, (uint32_t)n, s.crc, frame, BLE_FORMAT_JPEG, quality, meta);
  return true;
}

typedef struct {
  const ring_entry_t *e;
} fetch_ctx_t;

static int fetchRows(void *ctx, int y, uint8_t *band)
{
  const ring_entry_t *e = ((fetch_ctx_t *)ctx)->e;
  return storeRead(e->addr + (uint32_t)y * e->width, band, (uint32_t)e->width * 8) ? 0 : -1;
}

int32_t ringFetchJpeg(const ring_entry_t *e, uint8_t quality, uint8_t *band, uint32_t *us)
{
  if (e == NULL || e->format != BLE_FORMAT_RAW) {
    return -1;
  }
  ring_sink_t s = {.io = {countWrite, noRead, noSeek, NULL}};
  s.io.ctx = &s;
  fetch_ctx_t f = {e};
  uint32_t t0 = systemTimeGetUs();
  int32_t n = codecJpegBands(e->width, e->height, quality, &s.io, fetchRows, &f, band);
  *us = systemTimeGetUs() - t0;
  return n;
}

void ringReset(void)
{
  ringLock();
  m_head = m_count = 0;
  m_write_addr = 0;
  memset(&m_stats, 0, sizeof(m_stats));
  ringUnlock();
}
