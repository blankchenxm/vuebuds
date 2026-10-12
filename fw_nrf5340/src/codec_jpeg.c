/* JPEGENC (lib/jpegenc) compiled as C: grayscale baseline JPEG through codec_io_t. */
#define __MCUXPRESSO 1  // JPEGENC.h: plain C headers instead of Arduino.h
#include "JPEGENC.h"
#include "jpegenc.inl"

#include "codec.h"

extern void *codecJpegState;  // codec.c

size_t codecJpegStateSize(void)
{
  return sizeof(JPEGE_IMAGE);
}

static int32_t jpegWrite(JPEGE_FILE *file, uint8_t *data, int32_t len)
{
  codec_io_t *io = file->fHandle;
  return io->write(io, data, len);
}

int32_t codecJpeg(const uint8_t *pixels, int stride, int width, int height, int quality, codec_io_t *io)
{
  JPEGE_IMAGE *jpg = codecJpegState;
  if (jpg == NULL || quality < 1 || quality > 100) {
    return -JPEGE_INVALID_PARAMETER;
  }
  // As JPEGENC::open() for file output: our write callback, 2 KB staging buffer.
  memset(jpg, 0, sizeof(*jpg));
  jpg->pfnWrite = jpegWrite;
  jpg->JPEGFile.fHandle = io;
  jpg->pHighWater = &jpg->ucFileBuf[JPEGE_FILE_BUF_SIZE - 512];

  JPEGENCODE enc;
  int rc = JPEGEncodeBegin(jpg, &enc, width, height, JPEGE_PIXEL_GRAYSCALE, JPEGE_SUBSAMPLE_444,
                           JPEGE_Q_IJG + quality);
  if (rc != JPEGE_SUCCESS) {
    return -rc;
  }
  for (int mcu = 0; mcu < jpg->iMCUWidth * jpg->iMCUHeight; mcu++) {
    rc = JPEGAddMCU(jpg, &enc, (uint8_t *)pixels + enc.y * stride + enc.x, stride);
    if (rc != JPEGE_SUCCESS) {
      return -rc;
    }
  }
  int size = JPEGEncodeEnd(jpg);
  return size > 0 ? size : -jpg->iError - 100;
}
