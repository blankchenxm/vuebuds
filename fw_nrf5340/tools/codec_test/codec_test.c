/*
 * PC check of src/codec*.c (the same files the firmware builds), see run_codec_test.py:
 *   codec_test <raw 8-bit gray file> <width> <height> <out dir>
 * writes q95.jpg q90.jpg q85.jpg q75.jpg q60.jpg and l6.png into <out dir>, prints sizes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "codec.h"

typedef struct {
  uint8_t *buf;
  int32_t cap, pos, size;
} mem_t;

static int32_t memWrite(codec_io_t *io, const uint8_t *d, int32_t n)
{
  mem_t *m = io->ctx;
  if (m->pos + n > m->cap) return 0;
  memcpy(m->buf + m->pos, d, n);
  m->pos += n;
  if (m->pos > m->size) m->size = m->pos;
  return n;
}

static int32_t memRead(codec_io_t *io, uint8_t *d, int32_t n)
{
  mem_t *m = io->ctx;
  if (m->pos + n > m->size) n = m->size - m->pos;
  memcpy(d, m->buf + m->pos, n);
  m->pos += n;
  return n;
}

static int32_t memSeek(codec_io_t *io, int32_t p)
{
  mem_t *m = io->ctx;
  m->pos = p;
  return p;
}

static void save(const char *dir, const char *name, mem_t *m)
{
  char path[512];
  snprintf(path, sizeof(path), "%s/%s", dir, name);
  FILE *f = fopen(path, "wb");
  fwrite(m->buf, 1, m->size, f);
  fclose(f);
}

int main(int argc, char **argv)
{
  if (argc != 5) {
    fprintf(stderr, "usage: codec_test raw width height outdir\n");
    return 2;
  }
  int w = atoi(argv[2]), h = atoi(argv[3]);
  uint8_t *pix = malloc(w * h);
  FILE *f = fopen(argv[1], "rb");
  if (!f || fread(pix, 1, w * h, f) != (size_t)(w * h)) {
    fprintf(stderr, "cannot read %s\n", argv[1]);
    return 1;
  }
  fclose(f);

  size_t ws = codecWorkspaceSize();
  if (codecInit(malloc(ws), ws) != 0) return 1;
  printf("workspace %u bytes\n", (unsigned)ws);

  mem_t m = {.buf = malloc(4 * w * h + 4096), .cap = 4 * w * h + 4096};
  codec_io_t io = {memWrite, memRead, memSeek, &m};
  static const int qs[] = {95, 90, 85, 75, 60};
  for (int i = 0; i < 5; i++) {
    m.pos = m.size = 0;
    clock_t t = clock();
    int32_t n = codecJpeg(pix, w, w, h, qs[i], &io);
    char name[32];
    snprintf(name, sizeof(name), "q%d.jpg", qs[i]);
    printf("%s %d bytes %.1f ms\n", name, (int)n, 1000.0 * (clock() - t) / CLOCKS_PER_SEC);
    if (n > 0) save(argv[4], name, &m);
  }
  m.pos = m.size = 0;
  clock_t t = clock();
  int32_t n = codecPng(pix, w, w, h, 6, &io);
  printf("l6.png %d bytes %.1f ms\n", (int)n, 1000.0 * (clock() - t) / CLOCKS_PER_SEC);
  if (n > 0) save(argv[4], "l6.png", &m);
  return 0;
}
