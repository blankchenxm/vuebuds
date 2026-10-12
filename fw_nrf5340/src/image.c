#include <string.h>

#include <zephyr/kernel.h>

#include "nrf_log.h"
#include "capture.h"
#include "timers.h"
#include "image.h"


static uint8_t m_format = BLE_FORMAT_RAW;
static uint8_t m_quality;
static uint16_t m_frame_id;
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

void imagePrepare(const camera_frame_t *frame)
{
  const capture_stats_t *stats = capture_stats();
  memset(&m_header, 0, sizeof(m_header));
  m_header.width = frame->width;
  m_header.height = frame->height;
  m_header.capture_us = stats->start_us;
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
  m_header.format = BLE_FORMAT_RAW;
  m_header.length = (uint32_t)frame->width * frame->height;
  NRF_LOG_RAW_INFO("%08d [img] frame %u: %ux%u raw, exposure %u / 0x%02x / 0x%04x\n", systemTimeGetMs(),
                   m_header.frame_id, frame->width, frame->height, m_header.integration, m_header.analog_gain,
                   m_header.digital_gain);
  return bleSendImage(&m_header, readRaw, (void *)frame);
}

void imageCommand(const ble_command_t *cmd)
{
  const uint8_t *d = cmd->data;
  switch (d[0]) {
    case IMAGE_CMD_SET_FORMAT:
      if (cmd->len < 3) {
        break;
      }
      if (d[1] != BLE_FORMAT_RAW) {
        NRF_LOG_RAW_INFO("%08d [img] format %u not available yet, keeping raw\n", systemTimeGetMs(), d[1]);
        break;
      }
      m_format = d[1];
      m_quality = d[2];
      NRF_LOG_RAW_INFO("%08d [img] format %u, quality %u\n", systemTimeGetMs(), m_format, m_quality);
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
