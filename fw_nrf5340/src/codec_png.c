/* PNGenc (lib/pngenc, its zlib subset in separate files) compiled as C: 8-bit grayscale PNG. */
#define __MCUXPRESSO 1  // PNGenc.h: plain C headers instead of Arduino.h (keeps MEM_SHRINK 3: 4 KB window)
// PNGenc.h's C (non-C++) prototypes have two mistakes (a PNGIMAGE type, a PNG_addRGB565Line
// without bBigEndian); the library is left as it is and the prototypes are steered aside.
#define PNGIMAGE PNGENCIMAGE
#define PNG_addRGB565Line PNG_addRGB565Line_prototype
#include "PNGenc.h"
#undef PNG_addRGB565Line
#include "png.inl"

#include "codec.h"

extern void *codecPngState;  // codec.c

size_t codecPngStateSize(void)
{
  return sizeof(PNGENCIMAGE);
}

static int32_t pngWrite(PNGFILE *file, uint8_t *data, int32_t len)
{
  codec_io_t *io = file->fHandle;
  return io->write(io, data, len);
}

static int32_t pngRead(PNGFILE *file, uint8_t *data, int32_t len)
{
  codec_io_t *io = file->fHandle;
  return io->read(io, data, len);
}

static int32_t pngSeek(PNGFILE *file, int32_t position)
{
  codec_io_t *io = file->fHandle;
  return io->seek(io, position);
}

int32_t codecPng(const uint8_t *pixels, int stride, int width, int height, int level, codec_io_t *io)
{
  PNGENCIMAGE *png = codecPngState;
  if (png == NULL || level < 0 || level > 9 || width > PNG_MAX_BUFFERED_PIXELS) {
    return -PNG_INVALID_PARAMETER;
  }
  // As PNG_openFile() without the open callback; the sink is already open.
  memset(png, 0, sizeof(*png));
  png->iTransparent = -1;
  png->pfnWrite = pngWrite;
  png->pfnRead = pngRead;
  png->pfnSeek = pngSeek;
  png->PNGFile.fHandle = io;
  int rc = PNG_encodeBegin(png, width, height, PNG_PIXEL_GRAYSCALE, 8, NULL, level);
  if (rc != PNG_SUCCESS) {
    return -rc;
  }
  for (int y = 0; y < height; y++) {
    rc = PNG_addLine(png, (uint8_t *)pixels + y * stride, y);
    if (rc != PNG_SUCCESS) {
      return -rc;
    }
  }
  return png->iDataSize > 0 ? png->iDataSize : -100;
}
