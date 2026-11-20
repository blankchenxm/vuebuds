/*
 * PSRAM ring buffer with a time index (M2 / F3, hub #26, 2026-11-19; branch only, not in main).
 * Monitor (CAM_APP=MONITOR, -Cflags MONITOR_RING) stores every good frame here after it is
 * captured into the frame pool: frames lie back to back in PSRAM, a frame never wraps (it starts
 * at 0 again when it does not fit at the end), and the oldest frames whose bytes the new frame
 * covers are dropped from the index. The index (RAM) holds, per frame: frame number, capture
 * time (FVLD rise, 64-bit us), PSRAM address, length, size, format and a CRC32 of the bytes.
 *
 * Not yet: compression (frames are stored raw, RING_FORMAT is recorded for later), BLE commands
 * to query / fetch (need an INTERFACES change), a common clock (capture time is the board's).
 */
#ifndef RING_H_
#define RING_H_

#include <stdbool.h>
#include <stdint.h>

#include "camera.h"

#ifndef RING_BYTES
#define RING_BYTES (8u * 1024 * 1024)  // all of the APS6404L
#endif
#ifndef RING_INDEX_MAX
#define RING_INDEX_MAX 512  // frames the index can hold (raw QVGA fills 8 MB with 109)
#endif

typedef struct {
  uint32_t seq;         // frame number from the Monitor, counting from 1
  uint64_t capture_us;  // FVLD rise of the frame (board clock, extended past the 32-bit wrap)
  uint32_t addr;        // PSRAM address
  uint32_t len;         // bytes
  uint32_t crc;         // CRC32 (IEEE) of the stored bytes
  uint16_t width, height;
  uint8_t format;       // BLE_FORMAT_* (raw only for now)
} ring_entry_t;

typedef struct {
  uint32_t frames;        // frames in the ring now
  uint32_t appended;      // since start
  uint32_t evicted;       // dropped as the oldest
  uint32_t wraps;         // writes that went back to address 0
  uint32_t write_errors;  // PSRAM writes that failed (frame not indexed)
  uint32_t used_bytes;    // bytes of the frames in the ring
} ring_stats_t;

bool ringInit(void);  // false: no PSRAM
/* Copy the frame's standard region into the ring; *write_us = time spent writing PSRAM. */
bool ringAppend(const camera_frame_t *frame, uint32_t seq, uint32_t capture_us32, uint32_t *write_us);
/* Read the entry back from PSRAM and compare its CRC; *read_us = time spent. */
bool ringVerify(const ring_entry_t *e, uint32_t *read_us);

uint32_t ringCount(void);
const ring_entry_t *ringAt(uint32_t i);  // 0 = oldest
const ring_entry_t *ringNewest(void);
/* Newest frame captured at or before t (the "not later than t" lookup); NULL if none. */
const ring_entry_t *ringFindAtOrBefore(uint64_t t_us);
/* Frame whose capture time is closest to t; NULL if the ring is empty. */
const ring_entry_t *ringFindNearest(uint64_t t_us);
const ring_stats_t *ringStats(void);

#endif
