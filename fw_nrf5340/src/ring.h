/*
 * Ring buffer with a time index (M2 / F3, hub #26; INTERFACES C2, Xiaomeng 11-21).
 * Frames lie back to back in the store, a frame never wraps (it starts at 0 again when it does not
 * fit at the end), and the oldest frames whose bytes the new frame covers are dropped from the
 * index. The index (RAM) holds, per frame: frame number, capture time (64-bit us; the bridge's
 * clock in stream builds, ringcmd.h), store address, length, size, format, the frame header's
 * exposure fields and flags, and a CRC32 of the bytes. A frame never changes once stored.
 *
 * Store: the first RING_BYTES of the PSRAM (the rest is image.c's encoder output for pictures
 * sent directly); without a PSRAM that passed the boot check, the RAM given to ringInit().
 *
 * Compression (hub #39, 11-20): ringAppendJpeg() encodes the frame straight into the ring ("store
 * compressed"); ringFetchJpeg() encodes a raw entry in 8-row bands ("compress on fetch", only a
 * counting sink for the Monitor test).
 *
 * Threads: the main thread appends; the ring sender (ringcmd.c) looks up and reads. Index changes
 * take the ring lock; a reader holds ringLock() while it looks up and copies entries, and pins the
 * frame it is sending (ringPin): an append that would overwrite the pinned frame fails instead.
 */
#ifndef RING_H_
#define RING_H_

#include <stdbool.h>
#include <stdint.h>

#include "camera.h"

#ifndef RING_BYTES
#define RING_BYTES (7680u * 1024)  // PSRAM 8 MB minus 512 KB for image.c's encoder output
#endif
#ifndef RING_INDEX_MAX
#define RING_INDEX_MAX 512  // frames the index can hold (Xiaomeng 11-20: not enlarged)
#endif

typedef struct {
  uint32_t seq;         // frame number, counting from 1
  uint64_t capture_us;  // FVLD rise of the frame
  uint32_t addr;        // store address
  uint32_t len;         // bytes
  uint32_t crc;         // CRC32 (IEEE) of the stored bytes
  uint16_t width, height;
  uint8_t format;       // BLE_FORMAT_RAW or BLE_FORMAT_JPEG
  uint8_t quality;      // JPEG quality (0 for raw)
  // Frame header v2 fields of the picture (image.c), sent again with the frame.
  uint16_t integration;
  uint16_t digital_gain;
  uint8_t analog_gain;
  uint8_t flags;        // BLE_HEADER_*
} ring_entry_t;

typedef struct {
  uint32_t frames;        // frames in the ring now
  uint32_t appended;      // since start
  uint32_t evicted;       // dropped as the oldest
  uint32_t wraps;         // writes that went back to address 0
  uint32_t write_errors;  // writes that failed (frame not indexed)
  uint32_t pinned_fails;  // appends refused: they would have overwritten the frame being sent
  uint32_t used_bytes;    // bytes of the frames in the ring
} ring_stats_t;

/* Exposure fields and flags of a picture, stored with it (may be NULL). */
typedef struct {
  uint16_t integration;
  uint16_t digital_gain;
  uint8_t analog_gain;
  uint8_t flags;
} ring_meta_t;

/* PSRAM if it passed the boot check, else ram / ram_bytes (NULL / 0: no ring). Returns the store size. */
uint32_t ringInit(uint8_t *ram, uint32_t ram_bytes);
bool ringInPsram(void);
uint32_t ringCapacity(void);  // store bytes
/* Copy the frame's standard region into the ring; *write_us = time spent writing. */
bool ringAppend(const camera_frame_t *frame, uint32_t seq, uint64_t capture_us, const ring_meta_t *meta,
                uint32_t *write_us);
/* Encode the frame as JPEG into the ring (frees the oldest frames as the output grows; space for
 * one raw frame is reserved at the write position). *encode_us = encode + write. */
bool ringAppendJpeg(const camera_frame_t *frame, uint8_t quality, uint32_t seq, uint64_t capture_us,
                    const ring_meta_t *meta, uint32_t *encode_us);
/* JPEG-encode a raw entry (8-row bands into band, width x 8 bytes); the output is only counted.
 * Returns the size (< 0: error); *us = read + encode. */
int32_t ringFetchJpeg(const ring_entry_t *e, uint8_t quality, uint8_t *band, uint32_t *us);
void ringReset(void);  // empty the ring and the statistics
bool ringDropNewest(void);  // undo the last append (its bytes turned out bad)
/* Read the entry back and compare its CRC; *read_us = time spent. */
bool ringVerify(const ring_entry_t *e, uint32_t *read_us);
/* Bytes [offset, offset + n) of the entry. */
bool ringRead(const ring_entry_t *e, uint32_t offset, uint8_t *dst, uint32_t n);

void ringLock(void);    // around lookups from another thread (recursive)
void ringUnlock(void);
void ringPin(uint32_t seq);  // 0: none
bool ringHas(const ring_entry_t *e);  // still in the ring (same seq at the same place)

uint32_t ringCount(void);
const ring_entry_t *ringAt(uint32_t i);  // 0 = oldest
const ring_entry_t *ringNewest(void);
/* Newest frame captured at or before t (the "not later than t" lookup); NULL if none. */
const ring_entry_t *ringFindAtOrBefore(uint64_t t_us);
/* Frame whose capture time is closest to t; NULL if the ring is empty. */
const ring_entry_t *ringFindNearest(uint64_t t_us);
/* Index (0 = oldest) of the first frame captured at or after t; ringCount() if none. */
uint32_t ringFirstAtOrAfter(uint64_t t_us);
/* Frame with this frame number; NULL if not in the ring. */
const ring_entry_t *ringFindSeq(uint32_t seq);
const ring_stats_t *ringStats(void);

#endif
