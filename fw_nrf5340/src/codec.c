/* Encoder workspace (codec.h): one block, JPEG state then PNG state, word aligned. */
#include "codec.h"

extern size_t codecJpegStateSize(void);
extern size_t codecPngStateSize(void);

void *codecJpegState;
void *codecPngState;

static size_t align8(size_t n)
{
  return (n + 7) & ~(size_t)7;
}

size_t codecWorkspaceSize(void)
{
  return align8(codecJpegStateSize()) + align8(codecPngStateSize()) + 8;
}

int codecInit(void *workspace, size_t size)
{
  if (workspace == NULL || size < codecWorkspaceSize()) {
    codecJpegState = codecPngState = NULL;
    return -1;
  }
  uintptr_t p = ((uintptr_t)workspace + 7) & ~(uintptr_t)7;
  codecJpegState = (void *)p;
  codecPngState = (void *)(p + align8(codecJpegStateSize()));
  return 0;
}
