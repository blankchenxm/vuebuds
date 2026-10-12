/*
 * 8-bit grayscale JPEG / PNG encoders (bitbank2 JPEGENC / PNGenc as C, lib/README.md).
 * Output goes through codec_io_t: sequential writes; PNG also reads back and seeks at the
 * end (chunk length and CRC), so the sink must be random access (PSRAM or RAM).
 * The encoders' state lives in caller-provided memory (codecInit), not in .bss.
 */
#ifndef CODEC_H_
#define CODEC_H_

#include <stddef.h>
#include <stdint.h>

typedef struct codec_io {
  int32_t (*write)(struct codec_io *io, const uint8_t *data, int32_t len);  // at the position; returns len
  int32_t (*read)(struct codec_io *io, uint8_t *data, int32_t len);         // at the position; returns len
  int32_t (*seek)(struct codec_io *io, int32_t position);                   // returns position
  void *ctx;
} codec_io_t;

size_t codecWorkspaceSize(void);
int codecInit(void *workspace, size_t size);  // 0, or -1 if too small

/* Return the encoded size in bytes, or a negative error. quality 1..100 (libjpeg scale). */
int32_t codecJpeg(const uint8_t *pixels, int stride, int width, int height, int quality, codec_io_t *io);
/* level 0..9 (zlib) */
int32_t codecPng(const uint8_t *pixels, int stride, int width, int height, int level, codec_io_t *io);

#endif
