/*
 * nRF5340 port of fw/capture/capture.c (PR #32 design: every CS edge in hardware).
 *
 * DPPI differs from PPI in two ways that shape the channel map: an event publishes to one
 * channel only, and a task subscribes to one channel only. So
 *   - FVLD (one GPIOTE IN event) fans out through EGU0: CLEAR the line counter directly,
 *     and EGU0 TRIGGER[0] / [1] whose events feed the gated start / sleep channels;
 *   - CS has three task inputs (SET, CLR, OUT with polarity HiToLo = low): CLR for the
 *     start, OUT for the end of the 2 us pulse, SET for every CS-high edge;
 *   - the line counter's end-of-frame and boundary compares are doubled (CC4 = CC0,
 *     CC5 = CC2) so that one compare raises CS and the other disables the run group /
 *     starts the pulse timer.
 *
 *   channel         publisher(s)                        subscribers                     group
 *   LVLD            GPIOTE IN (LVLD fall)               TIMER1 COUNT                    always
 *   FVLD            GPIOTE IN (FVLD edge)               TIMER1 CLEAR, EGU0 TRIGGER0/1   always
 *   PULSE           TIMER2 COMPARE0 (2 us)              CS OUT (low)                    always
 *   CS_HIGH         TIMER1 COMPARE0 (last line),        CS SET                          always
 *                   TIMER1 COMPARE5 (boundary)
 *   START           TIMER1 COMPARE1 (HM01B0, line 0     CS CLR, CHG[RUN] EN,            ARM
 *                   done) or EGU0 TRIGGERED0 (HM0360)   CHG[ARM] DIS
 *   BOUNDARY        TIMER1 COMPARE2                     TIMER2 START                    RUN
 *   END             TIMER1 COMPARE4 (last line)         CHG[RUN] DIS                    RUN
 *   SLEEP           EGU0 TRIGGERED1                     XSLEEP CLR, CHG[SLEEP] DIS      SLEEP
 *
 * CS_HIGH is always on (fw/ gated it with the run group): outside a capture CS is already
 * high, so the extra SET edges change nothing.
 *
 * The SPIS is driven at register level (no driver, no interrupt): the CPU acquires the
 * semaphore once per frame to hand over segment 0, and the SPIS keeps it from then on
 * (no END -> ACQUIRE short), reading RXD.PTR at the start of each segment like on the
 * nRF52840.
 */
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_nrf.h>
#include <nrfx_gpiote.h>

#include "nrf_log.h"
#include "pins.h"
#include "event.h"
#include "timers.h"
#include "capture.h"

#define SPIS NRF_SPIS2
#define LINE_COUNTER NRF_TIMER1   // counts LVLD falls, cleared on every FVLD edge
#define PULSE_TIMER NRF_TIMER2    // one-shot 2 us CS pulse between DMA segments
#define FANOUT NRF_EGU0
#define LINE_COUNTER_IRQ_PRIORITY 1

#define DMA_MAX_BYTES (SPIS_RXD_MAXCNT_MAXCNT_Msk >> SPIS_RXD_MAXCNT_MAXCNT_Pos)  // 16 bits
#define CS_PULSE_TICKS 32u      // 2 us at 16 MHz: CS high between two DMA segments
#define COUNTER_NEVER 0xFFFFu   // a line count the counter does not reach within a frame
#define SPIS_WAIT_US 50u        // upper bound for END / CSN to follow a hardware CS edge
#define DPPI_ENABLE (1UL << 31)

// Line counter compares (fw/: CC0-CC3; CC4 / CC5 are the doubles DPPI needs).
#define CC_LAST_LINE  0   // transport_height: last line done  -> CS high (+ IRQ)
#define CC_START      1   // first_line: lines to skip done    -> start (HM01B0)
#define CC_BOUNDARY   2   // next boundary                     -> pulse timer (+ IRQ)
#define CC_PRELOAD    3   // first_line + 1                    -> IRQ: preload segment 1
#define CC_END        4   // = CC_LAST_LINE                    -> run group off
#define CC_CS_HIGH    5   // = CC_BOUNDARY                     -> CS high

static nrfx_gpiote_t *m_gpiote;
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

// Shared with the GPIOTE / timer interrupts
static volatile capture_state_t m_state;
static volatile uint8_t m_skip;            // frames still to let pass before capturing
static volatile bool m_seen_fvld;          // an FVLD rise happened since arming
static volatile uint32_t m_armed_us;
static volatile uint32_t m_last_rise_us;
static volatile uint32_t m_frame_start_us;
static volatile uint8_t m_seg;             // segment being received
static volatile uint16_t m_next_boundary;
static volatile bool m_sleep_wanted;       // XSLEEP low wanted on the next FVLD fall
static volatile bool m_sleep_armed;        // ... and the DPPI channel for it is enabled
static volatile bool m_after_last_line;    // a frame's last line has passed, FVLD not yet changed
static capture_stats_t m_stats;

void *capture_gpiote(void)
{
  return gpio_nrf_gpiote_by_port_get(DEVICE_DT_GET(DT_NODELABEL(gpio1)));
}

void capture_dppi_publish(uint32_t event_address, uint8_t channel)
{
  *(volatile uint32_t *)(event_address + 0x80u) = channel | DPPI_ENABLE;  // PUBLISH_x
}

void capture_dppi_subscribe(uint32_t task_address, uint8_t channel)
{
  *(volatile uint32_t *)(task_address + 0x80u) = channel | DPPI_ENABLE;   // SUBSCRIBE_x
}

static void group_enable(uint8_t group)
{
  NRF_DPPIC->TASKS_CHG[group].EN = 1;
}

static void group_disable(uint8_t group)
{
  NRF_DPPIC->TASKS_CHG[group].DIS = 1;
}

static void set_boundary(uint16_t line)
{
  LINE_COUNTER->CC[CC_BOUNDARY] = line;
  LINE_COUNTER->CC[CC_CS_HIGH] = line;
}

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
 * the start of each transfer, so the switch at the boundary needs no CPU.
 */
static void preload_segment(uint8_t seg)
{
  SPIS->RXD.PTR = (uint32_t)segment_start(seg);
  SPIS->RXD.MAXCNT = segment_lines(seg) * m_info.transport_width;
}

static bool wait_us(bool (*done)(void), uint32_t limit_us)
{
  // k_busy_wait steps instead of the 30 us kernel clock: the limits are tens of us.
  for (uint32_t t = 0; !done(); t++) {
    if (t >= limit_us) {
      return false;
    }
    k_busy_wait(1);
  }
  return true;
}

static bool spis_end_seen(void)
{
  return SPIS->EVENTS_END != 0;
}

static bool csn_low(void)
{
  return nrf_gpio_pin_read(CAM_SPI_CS_IN) == 0;
}

// A segment has ended (CS went high in hardware): record what the DMA received.
static void segment_done(void)
{
  wait_us(spis_end_seen, SPIS_WAIT_US);
  SPIS->EVENTS_END = 0;
  if (m_seg < CAPTURE_MAX_SEGMENTS) {
    m_stats.segment_bytes[m_seg] = SPIS->RXD.AMOUNT;
  }
  if (SPIS->STATUS & SPIS_STATUS_OVERFLOW_Msk) {
    m_stats.overflow = true;
    SPIS->STATUS = SPIS_STATUS_OVERFLOW_Msk;
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

static void line_counter_isr(const void *arg)
{
  ARG_UNUSED(arg);

  if (LINE_COUNTER->EVENTS_COMPARE[CC_PRELOAD]) {
    LINE_COUNTER->EVENTS_COMPARE[CC_PRELOAD] = 0;
    // One line after the start trigger. If CS went low, the frame is being received.
    if (m_state == CAPTURE_ARMED && csn_low()) {
      m_state = CAPTURE_RUNNING;
      m_frame_start_us = m_last_rise_us;
      if (m_seg_count > 1) {
        preload_segment(1);
      }
    }
  }

  if (LINE_COUNTER->EVENTS_COMPARE[CC_BOUNDARY]) {
    LINE_COUNTER->EVENTS_COMPARE[CC_BOUNDARY] = 0;
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
      set_boundary(m_next_boundary);
    }
  }

  if (LINE_COUNTER->EVENTS_COMPARE[CC_LAST_LINE]) {
    LINE_COUNTER->EVENTS_COMPARE[CC_LAST_LINE] = 0;
    // Last line of a frame. Hardware has raised CS if this frame was being received.
    m_after_last_line = true;
    if (m_state == CAPTURE_RUNNING) {
      segment_done();
      frame_done();
    } else if (m_state == CAPTURE_ARMED && m_skip > 0 && --m_skip == 0) {
      // The skipped frames are over: the next start trigger begins the capture.
      group_enable(CAPTURE_GROUP_ARM);
    }
  }
}

static void arm_sleep(void)
{
  m_sleep_armed = true;
  group_enable(CAPTURE_GROUP_SLEEP);
}

static void frame_valid_handler(nrfx_gpiote_pin_t pin, nrfx_gpiote_trigger_t trigger, void *context)
{
  m_after_last_line = false;
  if (m_sleep_armed && !(NRF_DPPIC->CHEN & BIT(CAPTURE_DPPI_SLEEP))) {
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

static void gpiote_out_init(nrfx_gpiote_pin_t pin, nrf_gpiote_polarity_t polarity)
{
  uint8_t ch;
  CAPTURE_CHECK(nrfx_gpiote_channel_alloc(m_gpiote, &ch) == 0);
  nrfx_gpiote_output_config_t out = NRFX_GPIOTE_DEFAULT_OUTPUT_CONFIG;
  // Input buffer on, so the level can be read back (wiring check, CS loopback).
  out.input_connect = NRF_GPIO_PIN_INPUT_CONNECT;
  nrfx_gpiote_task_config_t task = {
    .task_ch = ch,
    .polarity = polarity,
    .init_val = NRF_GPIOTE_INITIAL_VALUE_HIGH,
  };
  CAPTURE_CHECK(nrfx_gpiote_output_configure(m_gpiote, pin, &out, &task) == 0);
  nrfx_gpiote_out_task_enable(m_gpiote, pin);
}

static void gpiote_in_init(nrfx_gpiote_pin_t pin, nrfx_gpiote_trigger_t trigger,
                           nrfx_gpiote_interrupt_handler_t handler)
{
  uint8_t ch;
  CAPTURE_CHECK(nrfx_gpiote_channel_alloc(m_gpiote, &ch) == 0);
  static const nrf_gpio_pin_pull_t pull = NRF_GPIO_PIN_NOPULL;
  nrfx_gpiote_trigger_config_t trigger_config = {
    .trigger = trigger,
    .p_in_channel = &ch,
  };
  nrfx_gpiote_handler_config_t handler_config = {
    .handler = handler,
    .p_context = NULL,
  };
  nrfx_gpiote_input_pin_config_t config = {
    .p_pull_config = &pull,
    .p_trigger_config = &trigger_config,
    .p_handler_config = handler ? &handler_config : NULL,
  };
  CAPTURE_CHECK(nrfx_gpiote_input_configure(m_gpiote, pin, &config) == 0);
}

static void spis_init(void)
{
  nrf_gpio_cfg_input(CAM_PCLK_OUT_TO_MCU, NRF_GPIO_PIN_NOPULL);
  nrf_gpio_cfg_input(CAM_D0, NRF_GPIO_PIN_NOPULL);
  nrf_gpio_cfg_input(CAM_SPI_CS_IN, NRF_GPIO_PIN_NOPULL);
  SPIS->ENABLE = SPIS_ENABLE_ENABLE_Disabled << SPIS_ENABLE_ENABLE_Pos;
  SPIS->PSEL.SCK = CAM_PCLK_OUT_TO_MCU;
  SPIS->PSEL.MOSI = CAM_D0;
  SPIS->PSEL.MISO = SPIS_PSEL_MISO_CONNECT_Disconnected << SPIS_PSEL_MISO_CONNECT_Pos;
  SPIS->PSEL.CSN = CAM_SPI_CS_IN;
  // Mode 1 (sample D0 on the falling PCLK edge), MSB first. fw/ uses mode 0 (rising edge),
  // but on the nRF5340 the rising edge lands where the HM0360 changes D0: its images came
  // out as random speckle (only the low-transition color bar looked clean). Mode 1 samples
  // half a clock from that change; both sensors measured clean in both modes (user: 10-06).
  SPIS->CONFIG = (SPIS_CONFIG_ORDER_MsbFirst << SPIS_CONFIG_ORDER_Pos) |
                 (SPIS_CONFIG_CPHA_Trailing << SPIS_CONFIG_CPHA_Pos) |
                 (SPIS_CONFIG_CPOL_ActiveHigh << SPIS_CONFIG_CPOL_Pos);
  SPIS->DEF = 0xFF;
  SPIS->ORC = 0xFF;
  // The SPIS keeps the semaphore from one segment to the next; segment ends are polled.
  SPIS->SHORTS = 0;
  SPIS->INTENCLR = 0xFFFFFFFFu;
  SPIS->TXD.PTR = (uint32_t)m_tx_dummy;
  SPIS->TXD.MAXCNT = 0;
  SPIS->ENABLE = SPIS_ENABLE_ENABLE_Enabled << SPIS_ENABLE_ENABLE_Pos;
}

// Hand a buffer to the SPIS: take the semaphore (granted while CS is high), set it, release.
static bool spis_buffer_set(uint8_t *rx, uint32_t length)
{
  SPIS->EVENTS_ACQUIRED = 0;
  SPIS->TASKS_ACQUIRE = 1;
  uint32_t t;
  for (t = 0; !SPIS->EVENTS_ACQUIRED && t < 1000; t++) {
    k_busy_wait(1);
  }
  if (!SPIS->EVENTS_ACQUIRED) {
    return false;
  }
  SPIS->EVENTS_ACQUIRED = 0;
  SPIS->RXD.PTR = (uint32_t)rx;
  SPIS->RXD.MAXCNT = length;
  SPIS->TXD.PTR = (uint32_t)m_tx_dummy;
  SPIS->TXD.MAXCNT = 0;
  SPIS->TASKS_RELEASE = 1;
  return true;
}

void capture_init(const camera_mode_info_t *info)
{
  m_info = *info;
  m_gpiote = capture_gpiote();
  uint32_t frame_bytes = (uint32_t)m_info.transport_width * m_info.transport_height;
  m_rx_lines = m_info.transport_height - m_info.first_line;
  uint32_t rx_bytes = (uint32_t)m_rx_lines * m_info.transport_width;
  m_seg_count = (rx_bytes + DMA_MAX_BYTES - 1) / DMA_MAX_BYTES;
  m_seg_rows = (m_rx_lines + m_seg_count - 1) / m_seg_count;
  CAPTURE_CHECK(m_seg_count <= CAPTURE_MAX_SEGMENTS);
  CAPTURE_CHECK((uint32_t)m_seg_rows * m_info.transport_width <= DMA_MAX_BYTES);
  CAPTURE_CHECK(m_info.standard.y >= m_info.first_line);

  // The SPIS DMA needs a word-aligned RXD.PTR: the pool aligns segment 0, and every later
  // segment starts a whole number of words after it.
  uint32_t slots = frame_pool_init(frame_bytes, (uint32_t)m_info.first_line * m_info.transport_width);
  CAPTURE_CHECK(slots >= 1);
  CAPTURE_CHECK(((uint32_t)m_seg_rows * m_info.transport_width) % 4u == 0 || m_seg_count == 1);
  m_slot = frame_pool_slot(0);
  memset(&m_stats, 0, sizeof(m_stats));
  m_stats.segments = m_seg_count;

  NRF_LOG_RAW_INFO("[cap] transport %ux%u, DMA lines %u-%u in %u segment(s) of %u lines\n",
                   m_info.transport_width, m_info.transport_height, m_info.first_line,
                   m_info.transport_height - 1, m_seg_count, m_seg_rows);
  NRF_LOG_RAW_INFO("[cap] frame pool 0x%08X, %u B = %u slot(s) of %u B\n",
                   (uint32_t)frame_pool_slot(0), frame_pool_size(), slots, frame_bytes);

  // Chip select (looped back to the SPIS CSN pin by a jumper) and XSLEEP are GPIOTE task
  // pins, so DPPI can drive them at exact sensor events. XSLEEP is unconnected on HM01B0.
  // Polarity HiToLo: the OUT task drives the pin low (CS: end of the 2 us pulse).
  gpiote_out_init(CAM_SPI_CS_OUT, NRF_GPIOTE_POLARITY_HITOLO);
  gpiote_out_init(CAM_XSLEEP, NRF_GPIOTE_POLARITY_HITOLO);
  gpiote_in_init(CAM_FRAME_VALID, NRFX_GPIOTE_TRIGGER_TOGGLE, frame_valid_handler);
  gpiote_in_init(CAM_LINE_VALID, NRFX_GPIOTE_TRIGGER_HITOLO, NULL);

  LINE_COUNTER->TASKS_STOP = 1;
  LINE_COUNTER->MODE = TIMER_MODE_MODE_Counter;
  LINE_COUNTER->BITMODE = TIMER_BITMODE_BITMODE_16Bit;
  LINE_COUNTER->SHORTS = 0;
  // LVLD edges count from line 0, so the last line ends at transport_height.
  LINE_COUNTER->CC[CC_LAST_LINE] = m_info.transport_height;
  LINE_COUNTER->CC[CC_END] = m_info.transport_height;
  LINE_COUNTER->CC[CC_START] = m_info.first_line ? m_info.first_line : COUNTER_NEVER;
  set_boundary(COUNTER_NEVER);
  LINE_COUNTER->CC[CC_PRELOAD] = m_info.first_line + 1;
  LINE_COUNTER->INTENSET = (TIMER_INTENSET_COMPARE0_Msk << CC_LAST_LINE) |
                           (TIMER_INTENSET_COMPARE0_Msk << CC_BOUNDARY) |
                           (TIMER_INTENSET_COMPARE0_Msk << CC_PRELOAD);
  IRQ_CONNECT(TIMER1_IRQn, LINE_COUNTER_IRQ_PRIORITY, line_counter_isr, NULL, 0);
  irq_enable(TIMER1_IRQn);

  PULSE_TIMER->TASKS_STOP = 1;
  PULSE_TIMER->MODE = TIMER_MODE_MODE_Timer;
  PULSE_TIMER->BITMODE = TIMER_BITMODE_BITMODE_16Bit;
  PULSE_TIMER->PRESCALER = 0;  // 16 MHz
  PULSE_TIMER->CC[0] = CS_PULSE_TICKS;
  PULSE_TIMER->SHORTS = TIMER_SHORTS_COMPARE0_STOP_Msk | TIMER_SHORTS_COMPARE0_CLEAR_Msk;
  PULSE_TIMER->TASKS_CLEAR = 1;

  uint32_t lvld = nrfx_gpiote_in_event_address_get(m_gpiote, CAM_LINE_VALID);
  uint32_t fvld = nrfx_gpiote_in_event_address_get(m_gpiote, CAM_FRAME_VALID);
  uint32_t cs_high = nrfx_gpiote_set_task_address_get(m_gpiote, CAM_SPI_CS_OUT);
  uint32_t cs_low = nrfx_gpiote_clr_task_address_get(m_gpiote, CAM_SPI_CS_OUT);
  uint32_t cs_pulse_low = nrfx_gpiote_out_task_address_get(m_gpiote, CAM_SPI_CS_OUT);
  NRF_DPPIC->CHENCLR = BIT(CAPTURE_DPPI_LVLD) | BIT(CAPTURE_DPPI_FVLD) | BIT(CAPTURE_DPPI_PULSE) |
                       BIT(CAPTURE_DPPI_START) | BIT(CAPTURE_DPPI_BOUNDARY) | BIT(CAPTURE_DPPI_CS_HIGH) |
                       BIT(CAPTURE_DPPI_END) | BIT(CAPTURE_DPPI_SLEEP);
  // Group membership first: CHG[n] ignores writes once its EN or DIS task is subscribed.
  NRF_DPPIC->CHG[CAPTURE_GROUP_ARM] = BIT(CAPTURE_DPPI_START);
  NRF_DPPIC->CHG[CAPTURE_GROUP_RUN] = BIT(CAPTURE_DPPI_BOUNDARY) | BIT(CAPTURE_DPPI_END);
  NRF_DPPIC->CHG[CAPTURE_GROUP_SLEEP] = BIT(CAPTURE_DPPI_SLEEP);

  // Always on: line counter, FVLD fan-out, end of the 2 us pulse, CS high.
  capture_dppi_publish(lvld, CAPTURE_DPPI_LVLD);
  capture_dppi_subscribe((uint32_t)&LINE_COUNTER->TASKS_COUNT, CAPTURE_DPPI_LVLD);

  capture_dppi_publish(fvld, CAPTURE_DPPI_FVLD);
  capture_dppi_subscribe((uint32_t)&LINE_COUNTER->TASKS_CLEAR, CAPTURE_DPPI_FVLD);
  capture_dppi_subscribe((uint32_t)&FANOUT->TASKS_TRIGGER[0], CAPTURE_DPPI_FVLD);
  capture_dppi_subscribe((uint32_t)&FANOUT->TASKS_TRIGGER[1], CAPTURE_DPPI_FVLD);

  capture_dppi_publish((uint32_t)&PULSE_TIMER->EVENTS_COMPARE[0], CAPTURE_DPPI_PULSE);
  capture_dppi_subscribe(cs_pulse_low, CAPTURE_DPPI_PULSE);

  capture_dppi_publish((uint32_t)&LINE_COUNTER->EVENTS_COMPARE[CC_LAST_LINE], CAPTURE_DPPI_CS_HIGH);
  capture_dppi_publish((uint32_t)&LINE_COUNTER->EVENTS_COMPARE[CC_CS_HIGH], CAPTURE_DPPI_CS_HIGH);
  capture_dppi_subscribe(cs_high, CAPTURE_DPPI_CS_HIGH);

  // Start trigger. HM01B0 (first_line 1): right after line 0, skipping it as VueBuds did
  // with a 312 us delay. HM0360 (first_line 0): the FVLD edge; line 0 starts 750 us later.
  uint32_t start = m_info.first_line ? (uint32_t)&LINE_COUNTER->EVENTS_COMPARE[CC_START]
                                     : (uint32_t)&FANOUT->EVENTS_TRIGGERED[0];
  capture_dppi_publish(start, CAPTURE_DPPI_START);
  capture_dppi_subscribe(cs_low, CAPTURE_DPPI_START);
  capture_dppi_subscribe((uint32_t)&NRF_DPPIC->TASKS_CHG[CAPTURE_GROUP_RUN].EN, CAPTURE_DPPI_START);
  capture_dppi_subscribe((uint32_t)&NRF_DPPIC->TASKS_CHG[CAPTURE_GROUP_ARM].DIS, CAPTURE_DPPI_START);

  // Segment boundary: the pulse timer (CS high comes from CS_HIGH). End of frame: run off.
  capture_dppi_publish((uint32_t)&LINE_COUNTER->EVENTS_COMPARE[CC_BOUNDARY], CAPTURE_DPPI_BOUNDARY);
  capture_dppi_subscribe((uint32_t)&PULSE_TIMER->TASKS_START, CAPTURE_DPPI_BOUNDARY);
  capture_dppi_publish((uint32_t)&LINE_COUNTER->EVENTS_COMPARE[CC_END], CAPTURE_DPPI_END);
  capture_dppi_subscribe((uint32_t)&NRF_DPPIC->TASKS_CHG[CAPTURE_GROUP_RUN].DIS, CAPTURE_DPPI_END);

  // HM0360 S2 on a frame boundary: XSLEEP low on the FVLD fall.
  capture_dppi_publish((uint32_t)&FANOUT->EVENTS_TRIGGERED[1], CAPTURE_DPPI_SLEEP);
  capture_dppi_subscribe(nrfx_gpiote_clr_task_address_get(m_gpiote, CAM_XSLEEP), CAPTURE_DPPI_SLEEP);
  capture_dppi_subscribe((uint32_t)&NRF_DPPIC->TASKS_CHG[CAPTURE_GROUP_SLEEP].DIS, CAPTURE_DPPI_SLEEP);

  NRF_DPPIC->CHENSET = BIT(CAPTURE_DPPI_LVLD) | BIT(CAPTURE_DPPI_FVLD) | BIT(CAPTURE_DPPI_PULSE) |
                       BIT(CAPTURE_DPPI_CS_HIGH);

  spis_init();

  LINE_COUNTER->TASKS_CLEAR = 1;
  LINE_COUNTER->TASKS_START = 1;
  nrfx_gpiote_trigger_enable(m_gpiote, CAM_LINE_VALID, false);
  nrfx_gpiote_trigger_enable(m_gpiote, CAM_FRAME_VALID, true);
}

void capture_uninit(void)
{
  group_disable(CAPTURE_GROUP_ARM);
  group_disable(CAPTURE_GROUP_RUN);
  group_disable(CAPTURE_GROUP_SLEEP);
  SPIS->ENABLE = SPIS_ENABLE_ENABLE_Disabled << SPIS_ENABLE_ENABLE_Pos;
  LINE_COUNTER->TASKS_STOP = 1;
  capture_mclk_shutdown();
}

void capture_arm(void)
{
  capture_arm_slot(0, 0);
}

void capture_arm_slot(uint32_t slot, uint8_t skip_frames)
{
  group_disable(CAPTURE_GROUP_ARM);
  group_disable(CAPTURE_GROUP_RUN);
  nrfx_gpiote_set_task_trigger(m_gpiote, CAM_SPI_CS_OUT);
  m_state = CAPTURE_IDLE;

  m_slot = frame_pool_slot(slot);
  CAPTURE_CHECK(m_slot != NULL);
  m_seen_fvld = false;
  m_armed_us = (uint32_t)systemTimeGetUs();
  m_stats.ok = false;
  m_stats.overflow = false;
  memset(m_stats.segment_bytes, 0, sizeof(m_stats.segment_bytes));
  // Zero the slot so lines DMA never wrote (line 0, or a short frame) are black, not stale.
  memset(m_slot, 0, (uint32_t)m_info.transport_width * m_info.transport_height);

  m_seg = 0;
  m_next_boundary = (m_seg_count > 1) ? m_info.first_line + m_seg_rows : COUNTER_NEVER;
  set_boundary(m_next_boundary);
  SPIS->STATUS = SPIS_STATUS_OVERFLOW_Msk | SPIS_STATUS_OVERREAD_Msk;
  SPIS->EVENTS_END = 0;

  // Hand segment 0 to the SPIS (CS is high, so no transfer is running).
  if (!spis_buffer_set(segment_start(0), segment_lines(0) * m_info.transport_width)) {
    NRF_LOG_RAW_INFO("[cap] SPIS semaphore not granted\n");
  }

  m_skip = skip_frames;
  m_state = CAPTURE_ARMED;
  if (skip_frames == 0) {
    group_enable(CAPTURE_GROUP_ARM);
  }
}

void capture_xsleep(bool high)
{
  // Cancels a pending capture_xsleep_on_frame_boundary().
  m_sleep_wanted = false;
  m_sleep_armed = false;
  group_disable(CAPTURE_GROUP_SLEEP);
  if (high) {
    nrfx_gpiote_set_task_trigger(m_gpiote, CAM_XSLEEP);
  } else {
    nrfx_gpiote_clr_task_trigger(m_gpiote, CAM_XSLEEP);
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
