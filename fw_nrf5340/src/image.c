#include <string.h>

#include <zephyr/kernel.h>

#include "nrf_log.h"
#include "capture.h"
#include "codec.h"
#include "psram.h"
#include "timers.h"
#include "image.h"
#include "timesync.h"

#define PNG_LEVEL 6  // zlib default; the PC check (tools/codec_test) matched OpenCV's PNG size
#define BENCH_FORMATS 7

static uint8_t m_format = BLE_FORMAT_RAW;
static uint8_t m_quality;
static uint16_t m_frame_id;
static bool m_bench;  // 0xB4: the next frame goes out raw, PNG and JPEG q95 / 90 / 85 / 75 / 60
static ble_frame_header_t m_header;

/* Raw frames: the standard region, row by row out of the capture slot. */
static size_t readRaw(void *ctx, uint32_t offset, uint8_t *dst, size_t n)
{
  const camera_frame_t *f = ctx;
  size_t done = 0;
  while (done < n) {
    uint32_t row = (offset + done) / f->width, col = (offset + done) % f->width;
    size_t k = MIN(n - done, (size_t)(f->width - col));
    memcpy(dst + done, f->pixels + row * f->stride + col, k);
    done += k;
  }
  return done;
}

/*
 * Encoder output: random access (PNG seeks back), in PSRAM when the board has one, else in the
 * frame pool past slot 0 and the encoders' workspace (stream / picture builds use slot 0 only).
 */
typedef struct {
  codec_io_t io;
  uint8_t *ram;   // NULL: PSRAM
  int32_t cap, pos, size;
  bool failed;
} sink_t;

static sink_t m_sink;
static bool m_codec_ready;

static int32_t sinkWrite(codec_io_t *io, const uint8_t *data, int32_t len)
{
  sink_t *s = io->ctx;
  if (s->pos + len > s->cap || (s->ram == NULL && !psramWrite(s->pos, data, len))) {
    s->failed = true;
    return 0;
  }
  if (s->ram != NULL) {
    memcpy(s->ram + s->pos, data, len);
  }
  s->pos += len;
  s->size = MAX(s->size, s->pos);
  return len;
}

static int32_t sinkRead(codec_io_t *io, uint8_t *data, int32_t len)
{
  sink_t *s = io->ctx;
  len = MIN(len, s->size - s->pos);
  if (len <= 0 || (s->ram == NULL && !psramRead(s->pos, data, len))) {
    return 0;
  }
  if (s->ram != NULL) {
    memcpy(data, s->ram + s->pos, len);
  }
  s->pos += len;
  return len;
}

static int32_t sinkSeek(codec_io_t *io, int32_t position)
{
  sink_t *s = io->ctx;
  s->pos = position;
  return position;
}

/* BLE reader over the sink. */
static size_t readEncoded(void *ctx, uint32_t offset, uint8_t *dst, size_t n)
{
  sink_t *s = ctx;
  s->pos = offset;
  return (size_t)sinkRead(&s->io, dst, n);
}

static bool codecReady(void)
{
  if (m_codec_ready) {
    return true;
  }
  uint32_t spare;
  uint8_t *mem = frame_pool_after(1, &spare);
  size_t ws = codecWorkspaceSize();
  if (mem == NULL || spare < ws || codecInit(mem, ws) != 0) {
    NRF_LOG_RAW_INFO("[img] no room for the encoders (%u B after slot 0, %u needed)\n", spare, (unsigned)ws);
    return false;
  }
  m_sink.io = (codec_io_t){sinkWrite, sinkRead, sinkSeek, &m_sink};
  if (psramInit()) {
    m_sink.ram = NULL;
    m_sink.cap = PSRAM_SIZE;
  } else {
    m_sink.ram = mem + ((ws + 3) & ~3u);
    m_sink.cap = spare - ((ws + 3) & ~3u);
  }
  NRF_LOG_RAW_INFO("[img] encoders ready: workspace %u B, output in %s (%u KB)\n", (unsigned)ws,
                   m_sink.ram ? "RAM" : "PSRAM", m_sink.cap / 1024);
  m_codec_ready = true;
  return true;
}

/* Encode into the sink; returns the size, or <= 0 (then the frame goes out raw). */
static int32_t encode(const camera_frame_t *frame, uint8_t format, uint8_t quality, uint32_t *us)
{
  if (!codecReady()) {
    return -1;
  }
  m_sink.pos = m_sink.size = 0;
  m_sink.failed = false;
  int64_t t0 = k_uptime_ticks();
  int32_t n = format == BLE_FORMAT_JPEG
                  ? codecJpeg(frame->pixels, frame->stride, frame->width, frame->height, quality, &m_sink.io)
                  : codecPng(frame->pixels, frame->stride, frame->width, frame->height, PNG_LEVEL, &m_sink.io);
  *us = (uint32_t)k_ticks_to_us_floor64(k_uptime_ticks() - t0);
  if (m_sink.failed) {
    NRF_LOG_RAW_INFO("[img] output full (%u KB)\n", m_sink.cap / 1024);
    return -1;
  }
  return n;
}

static bool sendRaw(const camera_frame_t *frame)
{
  m_header.format = BLE_FORMAT_RAW;
  m_header.quality = 0;
  m_header.length = (uint32_t)frame->width * frame->height;
  return bleSendImage(&m_header, readRaw, (void *)frame);
}

static bool sendFormat(const camera_frame_t *frame, uint8_t format, uint8_t quality)
{
  if (format == BLE_FORMAT_RAW) {
    return sendRaw(frame);
  }
  uint32_t us;
  int32_t n = encode(frame, format, quality, &us);
  if (n <= 0) {
    NRF_LOG_RAW_INFO("%08d [img] frame %u: %s q%u failed (%d), sent raw\n", systemTimeGetMs(), m_header.frame_id,
                     format == BLE_FORMAT_JPEG ? "JPEG" : "PNG", quality, n);
    return sendRaw(frame);
  }
  m_header.format = format;
  m_header.quality = format == BLE_FORMAT_JPEG ? quality : PNG_LEVEL;
  m_header.length = n;
  // host/codec_bench.py reads these lines (RTT) for the encode time.
  NRF_LOG_RAW_INFO("%08d [img] frame %u: %s q%u %u B, encode %u us\n", systemTimeGetMs(), m_header.frame_id,
                   format == BLE_FORMAT_JPEG ? "JPEG" : "PNG", m_header.quality, n, us);
  int64_t t0 = k_uptime_get();
  bool ok = bleSendImage(&m_header, readEncoded, &m_sink);
  NRF_LOG_RAW_INFO("%08d [img] frame %u: queued in %u ms\n", systemTimeGetMs(), m_header.frame_id,
                   (uint32_t)(k_uptime_get() - t0));
  return ok;
}

void imagePrepare(const camera_frame_t *frame)
{
  const capture_stats_t *stats = capture_stats();
  memset(&m_header, 0, sizeof(m_header));
  m_header.width = frame->width;
  m_header.height = frame->height;
  // FVLD rise: hardware time (RTC0 capture), on the bridge's clock once it sent a reference.
  uint64_t bridge_us;
  if (timesyncLocalToBridge(timesyncLastFvldUs(), &bridge_us)) {
    m_header.capture_us = (uint32_t)bridge_us;
    m_header.flags |= BLE_HEADER_COMMON_CLOCK;
  } else {
    m_header.capture_us = stats->start_us;
  }
  camera_exposure_t e;
  if (cameraReadExposure(&e)) {
    m_header.integration = e.integration;
    m_header.analog_gain = e.analog_gain;
    m_header.digital_gain = e.digital_gain;
  }
  if (cameraExposureManual()) {
    m_header.flags |= BLE_HEADER_MANUAL_EXPOSURE;
  }
}

bool imageSend(const camera_frame_t *frame)
{
  m_header.frame_id = ++m_frame_id;
  NRF_LOG_RAW_INFO("%08d [img] frame %u: %ux%u, exposure %u / 0x%02x / 0x%04x\n", systemTimeGetMs(),
                   m_header.frame_id, frame->width, frame->height, m_header.integration, m_header.analog_gain,
                   m_header.digital_gain);
  if (m_bench) {
    m_bench = false;
    static const struct {
      uint8_t format, quality;
    } bench[BENCH_FORMATS] = {{BLE_FORMAT_RAW, 0},   {BLE_FORMAT_PNG, 0},   {BLE_FORMAT_JPEG, 95}, {BLE_FORMAT_JPEG, 90},
                              {BLE_FORMAT_JPEG, 85}, {BLE_FORMAT_JPEG, 75}, {BLE_FORMAT_JPEG, 60}};
    for (int i = 0; i < BENCH_FORMATS; i++) {
      if (!sendFormat(frame, bench[i].format, bench[i].quality)) {
        return false;
      }
    }
    return true;
  }
  return sendFormat(frame, m_format, m_quality);
}

void imageCommand(const ble_command_t *cmd)
{
  const uint8_t *d = cmd->data;
  switch (d[0]) {
    case IMAGE_CMD_SET_FORMAT:
      if (cmd->len < 3 || d[1] > BLE_FORMAT_JPEG || (d[1] == BLE_FORMAT_JPEG && (d[2] < 1 || d[2] > 100))) {
        NRF_LOG_RAW_INFO("%08d [img] bad format command\n", systemTimeGetMs());
        break;
      }
      m_format = d[1];
      m_quality = d[2];
      NRF_LOG_RAW_INFO("%08d [img] format %u, quality %u\n", systemTimeGetMs(), m_format, m_quality);
      break;
    case IMAGE_CMD_CODEC_BENCH:
      m_bench = true;
      NRF_LOG_RAW_INFO("%08d [img] codec bench: next frame raw, PNG, JPEG q95 / 90 / 85 / 75 / 60\n",
                       systemTimeGetMs());
      break;
    case IMAGE_CMD_SET_EXPOSURE:
      if (cmd->len >= 2 && d[1] == 0) {
        cameraSetExposure(NULL);
      } else if (cmd->len >= 7 && d[1] == 1) {
        camera_exposure_t e = {
          .integration = d[2] | (d[3] << 8),
          .analog_gain = d[4],
          .digital_gain = d[5] | (d[6] << 8),
        };
        cameraSetExposure(&e);
      }
      break;
    default:
      NRF_LOG_RAW_INFO("%08d [img] unknown command 0x%02x (%u bytes)\n", systemTimeGetMs(), d[0], cmd->len);
      break;
  }
}
