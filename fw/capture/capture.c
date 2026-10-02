#include <string.h>

#include "nrf_drv_gpiote.h"
#include "nrf_drv_spis.h"
#include "nrfx_spis_patch.h"
#include "nrf_drv_timer.h"
#include "nrf_delay.h"
#include "app_error.h"
#include "nrf_log.h"

#include "gpio.h"
#include "event.h"
#include "timers.h"
#include "capture.h"

#define SPIS_INSTANCE 2
#define SPIS_IRQ_PRIORITY 5
#define DMA_MAX_BYTES 0xFFFFu  // EasyDMA MAXCNT is 16 bits on nRF52840

static nrfx_spis_t m_spis = NRF_DRV_SPIS_INSTANCE(SPIS_INSTANCE);
// Asserts CS fvld_to_cs_us after FVLD rises (VueBuds' "LVLD timer").
static const nrf_drv_timer_t m_cs_timer = NRF_DRV_TIMER_INSTANCE(4);

static uint8_t m_tx_dummy[1];
static camera_mode_info_t m_info;
static uint8_t *m_slot;
static uint16_t m_rx_lines;  // lines DMA receives per frame: first_line .. transport_height - 1
static uint8_t m_seg_count;
static uint16_t m_seg_rows;  // lines per segment; the last one may be shorter

// Shared with the GPIOTE / SPIS / timer interrupts
static volatile bool m_in_frame;
static volatile uint8_t m_skip;            // frames still to let pass before capturing
static volatile bool m_skipping;           // the current frame is one of them (no CS, no DMA)
static volatile bool m_seen_fvld;          // an FVLD rise happened since arming
static volatile uint32_t m_armed_us;
static volatile uint16_t m_lines;          // LVLD falling edges since FVLD rose
static volatile uint16_t m_next_boundary;  // LVLD count at which CS is pulsed
static volatile uint8_t m_seg;
static volatile uint32_t m_frame_start_us;
static capture_stats_t m_stats;

static uint32_t segment_lines(uint8_t seg)
{
  uint32_t lines = m_rx_lines - (uint32_t)seg * m_seg_rows;
  return (lines > m_seg_rows) ? m_seg_rows : lines;
}

static void set_segment_buffer(uint8_t seg)
{
  uint32_t first = (uint32_t)seg * m_seg_rows;
  APP_ERROR_CHECK(nrfx_spis_buffers_set(&m_spis, m_tx_dummy, 0,
                                        m_slot + (m_info.first_line + first) * m_info.transport_width,
                                        segment_lines(seg) * m_info.transport_width));
}

static void spis_handler(nrf_drv_spis_event_t event)
{
  if (event.evt_type != NRF_DRV_SPIS_XFER_DONE) {
    return;
  }

  uint8_t seg = m_seg;
  if (seg < CAPTURE_MAX_SEGMENTS) {
    m_stats.segment_bytes[seg] = event.rx_amount;
  }
  if (m_spis.p_reg->STATUS & SPIS_STATUS_OVERFLOW_Msk) {
    m_stats.overflow = true;
    m_spis.p_reg->STATUS = SPIS_STATUS_OVERFLOW_Msk;
  }

  m_seg = ++seg;
  if (seg < m_seg_count) {
    set_segment_buffer(seg);
  } else {
    bool ok = !m_stats.overflow;
    for (uint8_t i = 0; i < m_seg_count; i++) {
      ok = ok && (m_stats.segment_bytes[i] == segment_lines(i) * m_info.transport_width);
    }
    m_stats.ok = ok;
    m_stats.duration_us = (uint32_t)systemTimeGetUs() - m_frame_start_us;
    m_stats.frame++;
    eventQueuePush(EVENT_CAMERA_CAPTURE_DONE);
  }
}

static void cs_timer_handler(nrf_timer_event_t event_type, void *p_context)
{
  nrf_gpio_pin_clear(CAM_SPI_CS_OUT);
  nrf_drv_timer_disable(&m_cs_timer);
}

static void line_valid_handler(nrf_drv_gpiote_pin_t pin, nrf_gpiote_polarity_t action)
{
  // End of a line. At a segment boundary, pulse CS so SPIS ends the current
  // DMA transfer; the SPIS handler then points DMA at the next segment.
  // This runs in horizontal blanking (~52 us at Sensor_Core 1 MHz).
  if (++m_lines == m_next_boundary) {
    nrf_gpio_pin_set(CAM_SPI_CS_OUT);
    nrf_delay_us(1);
    nrf_gpio_pin_clear(CAM_SPI_CS_OUT);
    m_next_boundary += m_seg_rows;
    if (m_next_boundary >= m_info.transport_height) {
      nrf_drv_gpiote_in_event_disable(CAM_LINE_VALID);
    }
  }
}

static void frame_valid_handler(nrf_drv_gpiote_pin_t pin, nrf_gpiote_polarity_t action)
{
  if (nrf_gpio_pin_read(CAM_FRAME_VALID)) {
    if (!m_in_frame) {
      m_in_frame = true;
      if (!m_seen_fvld) {
        m_seen_fvld = true;
        m_stats.arm_to_fvld_us = (uint32_t)systemTimeGetUs() - m_armed_us;
      }
      if (m_skip > 0) {
        // Let this frame pass: CS stays high, so SPIS receives nothing.
        m_skip--;
        m_skipping = true;
        return;
      }
      m_frame_start_us = (uint32_t)systemTimeGetUs();
      nrf_drv_timer_enable(&m_cs_timer);
      if (m_seg_count > 1) {
        // LVLD edges count from line 0, including the lines skipped before CS.
        m_lines = 0;
        m_next_boundary = m_info.first_line + m_seg_rows;
        nrf_drv_gpiote_in_event_enable(CAM_LINE_VALID, true);
      }
    }
  } else if (m_in_frame && m_skipping) {
    m_skipping = false;
    m_in_frame = false;
  } else if (m_in_frame) {
    // A falling edge without a preceding rise (armed mid-frame) is ignored above.
    nrf_gpio_pin_set(CAM_SPI_CS_OUT);
    nrf_drv_gpiote_in_event_disable(CAM_FRAME_VALID);
    m_in_frame = false;
  }
}

void capture_init(const camera_mode_info_t *info)
{
  m_info = *info;
  uint32_t frame_bytes = (uint32_t)m_info.transport_width * m_info.transport_height;
  m_rx_lines = m_info.transport_height - m_info.first_line;
  uint32_t rx_bytes = (uint32_t)m_rx_lines * m_info.transport_width;
  m_seg_count = (rx_bytes + DMA_MAX_BYTES - 1) / DMA_MAX_BYTES;
  m_seg_rows = (m_rx_lines + m_seg_count - 1) / m_seg_count;
  APP_ERROR_CHECK_BOOL(m_seg_count <= CAPTURE_MAX_SEGMENTS);
  APP_ERROR_CHECK_BOOL((uint32_t)m_seg_rows * m_info.transport_width <= DMA_MAX_BYTES);
  APP_ERROR_CHECK_BOOL(m_info.standard.y >= m_info.first_line);

  uint32_t slots = frame_pool_init(frame_bytes);
  APP_ERROR_CHECK_BOOL(slots >= 1);
  m_slot = frame_pool_slot(0);
  memset(&m_stats, 0, sizeof(m_stats));
  m_stats.segments = m_seg_count;

  NRF_LOG_RAW_INFO("[cap] transport %ux%u, DMA lines %u-%u in %u segment(s) of %u lines\n",
                   m_info.transport_width, m_info.transport_height, m_info.first_line,
                   m_info.transport_height - 1, m_seg_count, m_seg_rows);
  NRF_LOG_RAW_INFO("[cap] frame pool 0x%08X, %u B = %u slot(s) of %u B\n",
                   (uint32_t)frame_pool_slot(0), frame_pool_size(), slots, frame_bytes);

  // Software chip select, looped back to the SPIS CSN pin by a jumper.
  nrf_gpio_cfg_output(CAM_SPI_CS_OUT);
  nrf_gpio_pin_set(CAM_SPI_CS_OUT);

  ret_code_t err_code;
  nrf_drv_gpiote_in_config_t fvld_config = NRFX_GPIOTE_CONFIG_IN_SENSE_TOGGLE(true);
  fvld_config.pull = NRF_GPIO_PIN_NOPULL;
  err_code = nrf_drv_gpiote_in_init(CAM_FRAME_VALID, &fvld_config, frame_valid_handler);
  APP_ERROR_CHECK(err_code);

  nrf_drv_gpiote_in_config_t lvld_config = NRFX_GPIOTE_CONFIG_IN_SENSE_HITOLO(true);
  lvld_config.pull = NRF_GPIO_PIN_NOPULL;
  err_code = nrf_drv_gpiote_in_init(CAM_LINE_VALID, &lvld_config, line_valid_handler);
  APP_ERROR_CHECK(err_code);

  nrf_drv_timer_config_t timer_cfg = NRF_DRV_TIMER_DEFAULT_CONFIG;
  err_code = nrf_drv_timer_init(&m_cs_timer, &timer_cfg, cs_timer_handler);
  APP_ERROR_CHECK(err_code);
  nrf_drv_timer_extended_compare(&m_cs_timer, NRF_TIMER_CC_CHANNEL4,
                                 nrf_drv_timer_us_to_ticks(&m_cs_timer, m_info.fvld_to_cs_us),
                                 NRF_TIMER_SHORT_COMPARE4_CLEAR_MASK, true);

  nrfx_spis_config_t spis_config = {
    .miso_pin = NRFX_SPIS_PIN_NOT_USED,
    .mosi_pin = CAM_D0,
    .sck_pin = CAM_PCLK_OUT_TO_MCU,
    .csn_pin = CAM_SPI_CS_IN,
    .mode = NRF_SPIS_MODE_0,
    .bit_order = NRF_SPIS_BIT_ORDER_MSB_FIRST,
    .csn_pullup = NRFX_SPIS_DEFAULT_CSN_PULLUP,
    .miso_drive = NRFX_SPIS_DEFAULT_MISO_DRIVE,
    .def = NRFX_SPIS_DEFAULT_DEF,
    .orc = NRFX_SPIS_DEFAULT_ORC,
    .irq_priority = SPIS_IRQ_PRIORITY,
  };
  APP_ERROR_CHECK(nrf_drv_spis_init(&m_spis, &spis_config, spis_handler));
}

void capture_uninit(void)
{
  nrfx_spis_uninit(&m_spis);
  nrf_drv_timer_disable(&m_cs_timer);
  capture_mclk_shutdown();
}

void capture_arm(void)
{
  capture_arm_slot(0, 0);
}

void capture_arm_slot(uint32_t slot, uint8_t skip_frames)
{
  m_slot = frame_pool_slot(slot);
  APP_ERROR_CHECK_BOOL(m_slot != NULL);
  m_skip = skip_frames;
  m_skipping = false;
  m_seen_fvld = false;
  m_armed_us = (uint32_t)systemTimeGetUs();
  m_stats.ok = false;
  m_seg = 0;
  m_stats.overflow = false;
  memset(m_stats.segment_bytes, 0, sizeof(m_stats.segment_bytes));
  m_spis.p_reg->STATUS = SPIS_STATUS_OVERFLOW_Msk | SPIS_STATUS_OVERREAD_Msk;
  // Zero the slot so lines DMA never wrote (line 0, or a short frame) are black, not stale.
  memset(m_slot, 0, (uint32_t)m_info.transport_width * m_info.transport_height);
  set_segment_buffer(0);
  nrf_drv_gpiote_in_event_enable(CAM_FRAME_VALID, true);
}

uint8_t *capture_frame(void)
{
  return m_slot;
}

const capture_stats_t *capture_stats(void)
{
  return &m_stats;
}
