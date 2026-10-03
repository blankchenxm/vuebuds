#include <string.h>

#include "nrf_drv_gpiote.h"
#include "nrf_drv_spis.h"
#include "nrfx_spis_patch.h"
#include "nrf_drv_timer.h"
#include "nrfx_ppi.h"
#include "nrf_delay.h"
#include "app_error.h"
#include "nrf_log.h"

#include "gpio.h"
#include "event.h"
#include "timers.h"
#include "capture.h"

#define SPIS_INSTANCE 2
#define SPIS_IRQ_PRIORITY 5
#define DMA_MAX_BYTES 0xFFFFu   // EasyDMA MAXCNT is 16 bits on nRF52840
#define CS_PULSE_TICKS 32u      // 2 us at 16 MHz: CS high between two DMA segments
#define COUNTER_NEVER 0xFFFFu   // a line count the counter does not reach within a frame
#define SPIS_WAIT_US 50u        // upper bound for END / CSN to follow a hardware CS edge

static nrfx_spis_t m_spis = NRF_DRV_SPIS_INSTANCE(SPIS_INSTANCE);
// Counts LVLD falling edges (PPI), cleared on every FVLD edge. Its compares drive CS:
//   CC0 = transport_height  last line done      -> CS high (end of the frame)
//   CC1 = first_line        lines to skip done  -> CS low (HM01B0; HM0360 uses the FVLD edge)
//   CC2 = next boundary     segment boundary    -> CS high, TIMER4, CS low 2 us later
//   CC3 = first_line + 1    one line received   -> CPU preloads the next DMA segment
// (TIMER2 would also be used by the UART CLI, whose cliInit() is not called.)
static const nrf_drv_timer_t m_line_counter = NRF_DRV_TIMER_INSTANCE(2);
// One-shot 2 us CS pulse between DMA segments.
static const nrf_drv_timer_t m_pulse_timer = NRF_DRV_TIMER_INSTANCE(4);

// PPI channel groups, enabled and disabled by hardware events:
//   arm:   CS low at the start trigger (disables itself, enables run)
//   run:   segment boundaries and the end of the frame (the end disables run)
//   sleep: XSLEEP low on the next FVLD fall (disables itself)
static nrf_ppi_channel_group_t m_group_arm, m_group_run, m_group_sleep;
static nrf_ppi_channel_t m_ppi_sleep;

static uint8_t m_tx_dummy[1];
static camera_mode_info_t m_info;
static uint8_t *m_slot;
static uint16_t m_rx_lines;  // lines DMA receives per frame: first_line .. transport_height - 1
static uint8_t m_seg_count;
static uint16_t m_seg_rows;  // lines per segment; the last one may be shorter

typedef enum {
  CAPTURE_IDLE = 0,
  CAPTURE_ARMED,      // waiting for skipped frames / the start trigger
  CAPTURE_RUNNING,    // CS low, DMA receiving
} capture_state_t;

// Shared with the GPIOTE / SPIS / timer interrupts
static volatile capture_state_t m_state;
static volatile uint8_t m_skip;            // frames still to let pass before capturing
static volatile bool m_seen_fvld;          // an FVLD rise happened since arming
static volatile uint32_t m_armed_us;
static volatile uint32_t m_last_rise_us;
static volatile uint32_t m_frame_start_us;
static volatile uint8_t m_seg;             // segment being received
static volatile uint16_t m_next_boundary;
static volatile bool m_buffer_ready;       // driver has handed segment 0 to the SPIS
static volatile bool m_sleep_wanted;       // XSLEEP low wanted on the next FVLD fall
static volatile bool m_sleep_armed;        // ... and the PPI channel for it is enabled
static volatile bool m_after_last_line;    // a frame's last line has passed, FVLD not yet changed
static capture_stats_t m_stats;

static uint32_t segment_lines(uint8_t seg)
{
  uint32_t lines = m_rx_lines - (uint32_t)seg * m_seg_rows;
  return (lines > m_seg_rows) ? m_seg_rows : lines;
}

static uint8_t *segment_start(uint8_t seg)
{
  return m_slot + (m_info.first_line + (uint32_t)seg * m_seg_rows) * m_info.transport_width;
}

/*
 * Write the next segment's DMA pointer while the current one is being received. The SPIS
 * keeps the semaphore between segments (no END -> ACQUIRE) and reads RXD.PTR / MAXCNT at
 * the start of each transfer, so the switch at the boundary needs no CPU (measured on both
 * sensors: the running segment is untouched, the next one lands at the new pointer).
 */
static void preload_segment(uint8_t seg)
{
  m_spis.p_reg->RXD.PTR = (uint32_t)segment_start(seg);
  m_spis.p_reg->RXD.MAXCNT = segment_lines(seg) * m_info.transport_width;
}

static bool wait_us(bool (*done)(void), uint32_t limit_us)
{
  uint32_t t0 = (uint32_t)systemTimeGetUs();
  while (!done()) {
    if ((uint32_t)systemTimeGetUs() - t0 > limit_us) {
      return false;
    }
  }
  return true;
}

static bool spis_end_seen(void)
{
  return m_spis.p_reg->EVENTS_END != 0;
}

static bool csn_low(void)
{
  return nrf_gpio_pin_read(CAM_SPI_CS_IN) == 0;
}

// A segment has ended (CS went high in hardware): record what the DMA received.
static void segment_done(void)
{
  wait_us(spis_end_seen, SPIS_WAIT_US);
  m_spis.p_reg->EVENTS_END = 0;
  if (m_seg < CAPTURE_MAX_SEGMENTS) {
    m_stats.segment_bytes[m_seg] = m_spis.p_reg->RXD.AMOUNT;
  }
  if (m_spis.p_reg->STATUS & SPIS_STATUS_OVERFLOW_Msk) {
    m_stats.overflow = true;
    m_spis.p_reg->STATUS = SPIS_STATUS_OVERFLOW_Msk;
  }
}

static void frame_done(void)
{
  bool ok = !m_stats.overflow;
  for (uint8_t i = 0; i < m_seg_count; i++) {
    ok = ok && (m_stats.segment_bytes[i] == segment_lines(i) * m_info.transport_width);
  }
  m_stats.ok = ok;
  m_stats.duration_us = (uint32_t)systemTimeGetUs() - m_frame_start_us;
  m_stats.frame++;
  m_state = CAPTURE_IDLE;
  eventQueuePush(EVENT_CAMERA_CAPTURE_DONE);
}

static void spis_handler(nrf_drv_spis_event_t event)
{
  // Only buffer hand-over at arming is reported; segment ends are read in line_counter_handler.
  if (event.evt_type == NRF_DRV_SPIS_BUFFERS_SET_DONE) {
    m_buffer_ready = true;
  }
}

static void pulse_timer_handler(nrf_timer_event_t event_type, void *p_context) {}

static void line_counter_handler(nrf_timer_event_t event_type, void *p_context)
{
  switch (event_type) {
    case NRF_TIMER_EVENT_COMPARE3:
      // One line after the start trigger. If CS went low, the frame is being received.
      if (m_state == CAPTURE_ARMED && csn_low()) {
        m_state = CAPTURE_RUNNING;
        m_frame_start_us = m_last_rise_us;
        if (m_seg_count > 1) {
          preload_segment(1);
        }
      }
      break;

    case NRF_TIMER_EVENT_COMPARE2:
      // Segment boundary: hardware has pulsed CS. Note the finished segment and point
      // the DMA at the one after the segment now running.
      if (m_state == CAPTURE_RUNNING) {
        segment_done();
        m_seg++;
        wait_us(csn_low, SPIS_WAIT_US);  // the new segment has started (2 us pulse)
        if (m_seg + 1 < m_seg_count) {
          preload_segment(m_seg + 1);
          m_next_boundary += m_seg_rows;
        } else {
          m_next_boundary = COUNTER_NEVER;
        }
        nrf_drv_timer_compare(&m_line_counter, NRF_TIMER_CC_CHANNEL2, m_next_boundary, true);
      }
      break;

    case NRF_TIMER_EVENT_COMPARE0:
      // Last line of a frame. Hardware has raised CS if this frame was being received.
      m_after_last_line = true;
      if (m_state == CAPTURE_RUNNING) {
        segment_done();
        frame_done();
      } else if (m_state == CAPTURE_ARMED && m_skip > 0 && --m_skip == 0) {
        // The skipped frames are over: the next start trigger begins the capture.
        nrfx_ppi_group_enable(m_group_arm);
      }
      break;

    default:
      break;
  }
}

static void arm_sleep(void)
{
  m_sleep_armed = true;
  nrfx_ppi_group_enable(m_group_sleep);
}

static void frame_valid_handler(nrf_drv_gpiote_pin_t pin, nrf_gpiote_polarity_t action)
{
  m_after_last_line = false;
  if (m_sleep_armed && !(NRF_PPI->CHEN & (1UL << m_ppi_sleep))) {
    // The sleep channel has fired on the FVLD fall (it disables itself): XSLEEP is low.
    m_sleep_armed = false;
    m_sleep_wanted = false;
    eventQueuePush(EVENT_CAMERA_ASLEEP);
    return;
  }
  // The pin level can be stale if this interrupt ran late; only used for bookkeeping.
  if (nrf_gpio_pin_read(CAM_FRAME_VALID)) {
    m_last_rise_us = (uint32_t)systemTimeGetUs();
    if (m_state != CAPTURE_IDLE && !m_seen_fvld) {
      m_seen_fvld = true;
      m_stats.arm_to_fvld_us = m_last_rise_us - m_armed_us;
    }
    if (m_sleep_wanted && !m_sleep_armed) {
      arm_sleep();  // mid-frame now, so the next FVLD edge is the fall
    }
  }
}

static void gpiote_out_init(nrfx_gpiote_pin_t pin, bool initial_high)
{
  nrf_drv_gpiote_out_config_t config = GPIOTE_CONFIG_OUT_TASK_TOGGLE(initial_high);
  APP_ERROR_CHECK(nrf_drv_gpiote_out_init(pin, &config));
  nrf_drv_gpiote_out_task_enable(pin);
}

static nrf_ppi_channel_t ppi_connect(uint32_t event, uint32_t task, uint32_t fork)
{
  nrf_ppi_channel_t channel;
  APP_ERROR_CHECK(nrfx_ppi_channel_alloc(&channel));
  APP_ERROR_CHECK(nrfx_ppi_channel_assign(channel, event, task));
  if (fork) {
    APP_ERROR_CHECK(nrfx_ppi_channel_fork_assign(channel, fork));
  }
  return channel;
}

static nrf_ppi_channel_group_t ppi_group(void)
{
  nrf_ppi_channel_group_t group;
  APP_ERROR_CHECK(nrfx_ppi_group_alloc(&group));
  return group;
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

  // Chip select (looped back to the SPIS CSN pin by a jumper) and XSLEEP are GPIOTE task
  // pins, so PPI can drive them at exact sensor events. XSLEEP is unconnected on HM01B0.
  gpiote_out_init(CAM_SPI_CS_OUT, true);
  gpiote_out_init(CAM_XSLEEP, true);

  nrf_drv_gpiote_in_config_t fvld_config = NRFX_GPIOTE_CONFIG_IN_SENSE_TOGGLE(true);
  fvld_config.pull = NRF_GPIO_PIN_NOPULL;
  APP_ERROR_CHECK(nrf_drv_gpiote_in_init(CAM_FRAME_VALID, &fvld_config, frame_valid_handler));
  nrf_drv_gpiote_in_config_t lvld_config = NRFX_GPIOTE_CONFIG_IN_SENSE_HITOLO(true);
  lvld_config.pull = NRF_GPIO_PIN_NOPULL;
  APP_ERROR_CHECK(nrf_drv_gpiote_in_init(CAM_LINE_VALID, &lvld_config, NULL));

  nrf_drv_timer_config_t counter_cfg = NRF_DRV_TIMER_DEFAULT_CONFIG;
  counter_cfg.mode = NRF_TIMER_MODE_COUNTER;
  counter_cfg.bit_width = NRF_TIMER_BIT_WIDTH_16;
  APP_ERROR_CHECK(nrf_drv_timer_init(&m_line_counter, &counter_cfg, line_counter_handler));
  // LVLD edges count from line 0, so the last line ends at transport_height.
  nrf_drv_timer_compare(&m_line_counter, NRF_TIMER_CC_CHANNEL0, m_info.transport_height, true);
  nrf_drv_timer_compare(&m_line_counter, NRF_TIMER_CC_CHANNEL1,
                        m_info.first_line ? m_info.first_line : COUNTER_NEVER, false);
  nrf_drv_timer_compare(&m_line_counter, NRF_TIMER_CC_CHANNEL2, COUNTER_NEVER, true);
  nrf_drv_timer_compare(&m_line_counter, NRF_TIMER_CC_CHANNEL3, m_info.first_line + 1, true);

  nrf_drv_timer_config_t pulse_cfg = NRF_DRV_TIMER_DEFAULT_CONFIG;
  pulse_cfg.frequency = NRF_TIMER_FREQ_16MHz;
  APP_ERROR_CHECK(nrf_drv_timer_init(&m_pulse_timer, &pulse_cfg, pulse_timer_handler));
  nrf_drv_timer_extended_compare(&m_pulse_timer, NRF_TIMER_CC_CHANNEL0, CS_PULSE_TICKS,
                                 NRF_TIMER_SHORT_COMPARE0_STOP_MASK | NRF_TIMER_SHORT_COMPARE0_CLEAR_MASK, false);

  uint32_t lvld = nrf_drv_gpiote_in_event_addr_get(CAM_LINE_VALID);
  uint32_t fvld = nrf_drv_gpiote_in_event_addr_get(CAM_FRAME_VALID);
  uint32_t cs_high = nrfx_gpiote_set_task_addr_get(CAM_SPI_CS_OUT);
  uint32_t cs_low = nrfx_gpiote_clr_task_addr_get(CAM_SPI_CS_OUT);
  m_group_arm = ppi_group();
  m_group_run = ppi_group();
  m_group_sleep = ppi_group();

  // Always on: line counter.
  nrfx_ppi_channel_enable(ppi_connect(lvld, nrf_drv_timer_task_address_get(&m_line_counter, NRF_TIMER_TASK_COUNT), 0));
  nrfx_ppi_channel_enable(ppi_connect(fvld, nrf_drv_timer_task_address_get(&m_line_counter, NRF_TIMER_TASK_CLEAR), 0));
  // Always on: end of the 2 us pulse.
  nrfx_ppi_channel_enable(ppi_connect(nrf_drv_timer_compare_event_address_get(&m_pulse_timer, NRF_TIMER_CC_CHANNEL0),
                                      cs_low, 0));

  // Start trigger. HM01B0 (first_line 1): right after line 0, skipping it as VueBuds did
  // with a 312 us delay. HM0360 (first_line 0): the FVLD edge; line 0 starts 750 us later.
  uint32_t start = m_info.first_line ? nrf_drv_timer_compare_event_address_get(&m_line_counter, NRF_TIMER_CC_CHANNEL1)
                                     : fvld;
  nrfx_ppi_channel_include_in_group(ppi_connect(start, cs_low, nrfx_ppi_task_addr_group_enable_get(m_group_run)),
                                    m_group_arm);
  nrfx_ppi_channel_include_in_group(ppi_connect(start, nrfx_ppi_task_addr_group_disable_get(m_group_arm), 0),
                                    m_group_arm);

  // Segment boundary: CS high and start the pulse timer. End of frame: CS high, run off.
  nrfx_ppi_channel_include_in_group(
      ppi_connect(nrf_drv_timer_compare_event_address_get(&m_line_counter, NRF_TIMER_CC_CHANNEL2), cs_high,
                  nrf_drv_timer_task_address_get(&m_pulse_timer, NRF_TIMER_TASK_START)),
      m_group_run);
  nrfx_ppi_channel_include_in_group(
      ppi_connect(nrf_drv_timer_compare_event_address_get(&m_line_counter, NRF_TIMER_CC_CHANNEL0), cs_high,
                  nrfx_ppi_task_addr_group_disable_get(m_group_run)),
      m_group_run);

  // HM0360 S2 on a frame boundary: XSLEEP low on the FVLD fall.
  m_ppi_sleep = ppi_connect(fvld, nrfx_gpiote_clr_task_addr_get(CAM_XSLEEP),
                            nrfx_ppi_task_addr_group_disable_get(m_group_sleep));
  nrfx_ppi_channel_include_in_group(m_ppi_sleep, m_group_sleep);

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
  // The SPIS keeps the semaphore from one segment to the next; segment ends are polled.
  nrf_spis_shorts_disable(m_spis.p_reg, NRF_SPIS_SHORT_END_ACQUIRE);
  nrf_spis_int_disable(m_spis.p_reg, NRF_SPIS_INT_END_MASK);

  nrf_drv_timer_enable(&m_line_counter);
  nrf_drv_gpiote_in_event_enable(CAM_LINE_VALID, false);
  nrf_drv_gpiote_in_event_enable(CAM_FRAME_VALID, true);
}

void capture_uninit(void)
{
  nrfx_ppi_group_disable(m_group_arm);
  nrfx_ppi_group_disable(m_group_run);
  nrfx_ppi_group_disable(m_group_sleep);
  nrfx_spis_uninit(&m_spis);
  nrf_drv_timer_disable(&m_line_counter);
  capture_mclk_shutdown();
}

void capture_arm(void)
{
  capture_arm_slot(0, 0);
}

void capture_arm_slot(uint32_t slot, uint8_t skip_frames)
{
  nrfx_ppi_group_disable(m_group_arm);
  nrfx_ppi_group_disable(m_group_run);
  nrfx_gpiote_set_task_trigger(CAM_SPI_CS_OUT);
  m_state = CAPTURE_IDLE;

  m_slot = frame_pool_slot(slot);
  APP_ERROR_CHECK_BOOL(m_slot != NULL);
  m_seen_fvld = false;
  m_armed_us = (uint32_t)systemTimeGetUs();
  m_stats.ok = false;
  m_stats.overflow = false;
  memset(m_stats.segment_bytes, 0, sizeof(m_stats.segment_bytes));
  // Zero the slot so lines DMA never wrote (line 0, or a short frame) are black, not stale.
  memset(m_slot, 0, (uint32_t)m_info.transport_width * m_info.transport_height);

  m_seg = 0;
  m_next_boundary = (m_seg_count > 1) ? m_info.first_line + m_seg_rows : COUNTER_NEVER;
  nrf_drv_timer_compare(&m_line_counter, NRF_TIMER_CC_CHANNEL2, m_next_boundary, true);
  m_spis.p_reg->STATUS = SPIS_STATUS_OVERFLOW_Msk | SPIS_STATUS_OVERREAD_Msk;
  m_spis.p_reg->EVENTS_END = 0;

  // Hand segment 0 to the SPIS (CS is high, so no transfer is running).
  m_buffer_ready = false;
  APP_ERROR_CHECK(nrfx_spis_buffers_set(&m_spis, m_tx_dummy, 0, segment_start(0),
                                        segment_lines(0) * m_info.transport_width));
  uint32_t t0 = (uint32_t)systemTimeGetUs();
  while (!m_buffer_ready && (uint32_t)systemTimeGetUs() - t0 < 1000) {
  }

  m_skip = skip_frames;
  m_state = CAPTURE_ARMED;
  if (skip_frames == 0) {
    nrfx_ppi_group_enable(m_group_arm);
  }
}

void capture_xsleep(bool high)
{
  // Cancels a pending capture_xsleep_on_frame_boundary().
  m_sleep_wanted = false;
  m_sleep_armed = false;
  nrfx_ppi_group_disable(m_group_sleep);
  if (high) {
    nrfx_gpiote_set_task_trigger(CAM_XSLEEP);
  } else {
    nrfx_gpiote_clr_task_trigger(CAM_XSLEEP);
  }
}

bool capture_xsleep_on_frame_boundary(void)
{
  m_sleep_wanted = true;
  // Right after a frame's last line FVLD is still high (HM0360: 36 blank lines, ~13 ms),
  // so the next FVLD edge is the fall: arm now. Otherwise the next FVLD rise arms it.
  if (m_after_last_line) {
    arm_sleep();
    return true;
  }
  return false;
}

uint8_t *capture_frame(void)
{
  return m_slot;
}

const capture_stats_t *capture_stats(void)
{
  return &m_stats;
}
