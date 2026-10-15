/*
 * Hardware check (build with -Cflags 'HW_TEST'; the camera / BLE application does not start).
 * Prints PASS / FAIL per item on RTT:
 *   [1] HM0360 answers I2C (camera init as usual)
 *   [2] FSIN    (P1.11): FSYNC on (0x3010[1]); frames must follow our pulses
 *   [3] TRIGGER (P0.25): MODE_SELECT = Streaming 4; frames only while the pin is high
 *   [4] INT     (P1.12, 2026-10-14): driven low by the sensor; Early VSYNC interrupt in pulse
 *       mode gives one INT pulse per frame; level mode holds INT until INT_CLEAR
 *   [5] PSRAM on QSPI (2026-10-14, P0.13-P0.18, src/psram.c): wires, ID, single line and quad
 *       at 8-96 MHz, CE# low time per burst measured on the pin (tCEM), throughput per burst
 *       size, 8 MB write / read twice, unaligned accesses through the psram.c API
 * Frames are counted by polling FVLD for rising edges (it is high for the whole frame
 * period and low only ~60 us between frames) and cross-checked with FRAME_COUNT.
 */
#ifdef HW_TEST

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <cmsis_core.h>
#include <hal/nrf_gpio.h>
#include <hal/nrf_qspi.h>

#include "camera.h"
#include "i2c.h"
#include "pins.h"
#include "psram.h"


#define REG_MODE_SELECT   0x0100
#define REG_FRAME_COUNT_H 0x0005
#define REG_FRAME_COUNT_L 0x0006
#define REG_FSYNC         0x3010  // [1] FSYNC enable
#define REG_COMMAND_UPDATE 0x0104
#define MODE_SLEEP        0x00
#define MODE_STREAMING    0x01
#define MODE_TRIGGER_PIN  0x04    // Streaming 4: streaming while TRIGGER is high

static int failures;

static void result(const char *item, bool pass, const char *detail)
{
  printk("[hwtest] %s: %s%s%s\n", item, pass ? "PASS" : "FAIL", detail[0] ? " - " : "", detail);
  if (!pass) {
    failures++;
  }
}

static uint16_t frameCount(void)
{
  return ((uint16_t)i2cRead16(CAMERA_I2C_ADDR, REG_FRAME_COUNT_H) << 8) |
         i2cRead16(CAMERA_I2C_ADDR, REG_FRAME_COUNT_L);
}

static uint32_t ticksToUs(int64_t ticks)
{
  return (uint32_t)k_ticks_to_us_floor64(ticks);
}

typedef struct {
  uint32_t frames;       // FVLD rising edges
  uint16_t counter;      // FRAME_COUNT difference
  uint32_t pulses;
  uint32_t latency_min_us, latency_max_us;  // pulse rise -> next FVLD rise
} window_t;

/*
 * Watch FVLD for ms. With pulse_pin, a 1 ms high pulse is sent every period_ms (first one
 * 50 ms in). Polling, so this thread owns the CPU for the window.
 */
static window_t watch(uint32_t ms, int pulse_pin, uint32_t period_ms)
{
  window_t w = {.latency_min_us = UINT32_MAX};
  uint16_t c0 = frameCount();
  int64_t start = k_uptime_ticks();
  int64_t end = start + k_ms_to_ticks_ceil64(ms);
  int64_t next_pulse = start + k_ms_to_ticks_ceil64(50);
  int64_t pulse_end = 0, last_pulse = -1;
  bool high = nrf_gpio_pin_read(CAM_FRAME_VALID);

  for (int64_t now = start; now < end; now = k_uptime_ticks()) {
    if (pulse_pin >= 0) {
      if (now >= next_pulse) {
        nrf_gpio_pin_set(pulse_pin);
        last_pulse = now;
        pulse_end = now + k_ms_to_ticks_ceil64(1);
        next_pulse += k_ms_to_ticks_ceil64(period_ms);
        w.pulses++;
      } else if (pulse_end && now >= pulse_end) {
        nrf_gpio_pin_clear(pulse_pin);
        pulse_end = 0;
      }
    }
    bool level = nrf_gpio_pin_read(CAM_FRAME_VALID);
    if (level && !high) {
      w.frames++;
      if (last_pulse >= 0) {
        uint32_t us = ticksToUs(now - last_pulse);
        w.latency_min_us = MIN(w.latency_min_us, us);
        w.latency_max_us = MAX(w.latency_max_us, us);
        last_pulse = -1;
      }
    }
    high = level;
  }
  if (pulse_pin >= 0) {
    nrf_gpio_pin_clear(pulse_pin);
  }
  w.counter = (uint16_t)(frameCount() - c0);
  return w;
}

static void printWindow(const char *name, const window_t *w)
{
  printk("[hwtest]   %-28s FVLD rises %3u, FRAME_COUNT +%3u, pulses %2u", name, w->frames, w->counter, w->pulses);
  if (w->latency_max_us) {
    printk(", pulse->FVLD %u..%u us", w->latency_min_us, w->latency_max_us);
  }
  printk("\n");
}

static void setMode(uint8_t mode)
{
  i2cWrite16(CAMERA_I2C_ADDR, REG_MODE_SELECT, mode);
  k_msleep(250);  // let a frame in progress finish
}

static void testFsin(void)
{
  char detail[96];
  nrf_gpio_cfg_output(CAM_FSIN);
  nrf_gpio_pin_clear(CAM_FSIN);

  setMode(MODE_STREAMING);
  window_t free = watch(1000, -1, 0);
  printWindow("free running, 1 s", &free);

  i2cWrite16(CAMERA_I2C_ADDR, REG_FSYNC, 0x02);
  i2cWrite16NoVerify(CAMERA_I2C_ADDR, REG_COMMAND_UPDATE, 0x01);
  k_msleep(300);
  window_t none = watch(1000, -1, 0);
  window_t p250 = watch(2000, CAM_FSIN, 250);
  window_t p500 = watch(2000, CAM_FSIN, 500);
  printWindow("FSYNC on, no pulse, 1 s", &none);
  printWindow("FSYNC on, pulse / 250 ms", &p250);
  printWindow("FSYNC on, pulse / 500 ms", &p500);

  i2cWrite16(CAMERA_I2C_ADDR, REG_FSYNC, 0x00);
  i2cWrite16NoVerify(CAMERA_I2C_ADDR, REG_COMMAND_UPDATE, 0x01);
  setMode(MODE_SLEEP);

  // Wiring check: no frame without pulses, frames with pulses, never more frames than pulses.
  // (Whether every pulse gives a frame is a sensor question for the time-sync work.)
  bool pass = free.frames >= 5 && none.frames == 0 &&
              p250.frames > 0 && p250.frames <= p250.pulses + 1 &&
              p500.frames > 0 && p500.frames <= p500.pulses + 1;
  snprintk(detail, sizeof(detail), "frames %u / %u pulses (250 ms), %u / %u (500 ms), free %u/s",
           p250.frames, p250.pulses, p500.frames, p500.pulses, free.frames);
  result("[2] FSIN P1.11", pass, detail);
}

static void testTrigger(void)
{
  char detail[96];
  nrf_gpio_cfg_output(CAM_TRIGGER);
  nrf_gpio_pin_clear(CAM_TRIGGER);

  setMode(MODE_TRIGGER_PIN);
  window_t low1 = watch(1000, -1, 0);
  nrf_gpio_pin_set(CAM_TRIGGER);
  window_t high = watch(1000, -1, 0);
  nrf_gpio_pin_clear(CAM_TRIGGER);
  k_msleep(250);
  window_t low2 = watch(1000, -1, 0);
  printWindow("Streaming 4, TRIGGER low", &low1);
  printWindow("Streaming 4, TRIGGER high", &high);
  printWindow("Streaming 4, TRIGGER low again", &low2);
  setMode(MODE_SLEEP);

  bool pass = low1.frames == 0 && high.frames >= 5 && low2.frames == 0;
  snprintk(detail, sizeof(detail), "frames low %u, high %u, low %u", low1.frames, high.frames, low2.frames);
  result("[3] TRIGGER P0.25", pass, detail);
}

/* ---- [4] INT (P1.12) ---- */

#define REG_AE_CTRL1       0x2001  // [1] AE non-converged INT, [0] AE statistics INT
#define REG_INT_PULSE_MODE 0x2061  // [0] 1: pulse, 0: level
#define REG_INT_INDIC      0x2064  // [5] Early VSYNC, [3] MD, [2] AENC, [1] Stat, [0] ALC
#define REG_INT_CLEAR      0x2065
#define REG_EARLY_INT_EN   0x309C  // [0] Early VSYNC interrupt

typedef struct {
  uint32_t fvld, rises;
  uint32_t high_min_us, high_max_us;  // INT pulse width
  uint32_t lead_min_us, lead_max_us;  // INT rise -> next FVLD rise
  bool high_at_end;
} int_window_t;

static int_window_t watchInt(uint32_t ms)
{
  int_window_t w = {.high_min_us = UINT32_MAX, .lead_min_us = UINT32_MAX};
  int64_t start = k_uptime_ticks(), end = start + k_ms_to_ticks_ceil64(ms);
  bool f = nrf_gpio_pin_read(CAM_FRAME_VALID), i = nrf_gpio_pin_read(CAM_INT);
  int64_t rise = -1, lead = -1;
  for (int64_t now = start; now < end; now = k_uptime_ticks()) {
    bool fl = nrf_gpio_pin_read(CAM_FRAME_VALID), il = nrf_gpio_pin_read(CAM_INT);
    if (il && !i) {
      w.rises++;
      rise = lead = now;
    } else if (!il && i && rise >= 0) {
      uint32_t us = ticksToUs(now - rise);
      w.high_min_us = MIN(w.high_min_us, us);
      w.high_max_us = MAX(w.high_max_us, us);
      rise = -1;
    }
    if (fl && !f) {
      w.fvld++;
      if (lead >= 0) {
        uint32_t us = ticksToUs(now - lead);
        w.lead_min_us = MIN(w.lead_min_us, us);
        w.lead_max_us = MAX(w.lead_max_us, us);
        lead = -1;
      }
    }
    f = fl;
    i = il;
  }
  w.high_at_end = i;
  if (w.high_min_us == UINT32_MAX) {
    w.high_min_us = 0;
  }
  if (w.lead_min_us == UINT32_MAX) {
    w.lead_min_us = 0;
  }
  return w;
}

static void printInt(const char *name, const int_window_t *w)
{
  printk("[hwtest]   %-30s FVLD rises %3u, INT rises %3u, INT high %u..%u us, INT->FVLD %u..%u us%s\n", name,
         w->fvld, w->rises, w->high_min_us, w->high_max_us, w->lead_min_us, w->lead_max_us,
         w->high_at_end ? ", INT high at the end" : "");
}

static void intClear(void)
{
  i2cWrite16NoVerify(CAMERA_I2C_ADDR, REG_INT_CLEAR, 0x3F);
  i2cWrite16NoVerify(CAMERA_I2C_ADDR, REG_INT_CLEAR, 0x00);
}

static void intUpdate(void)
{
  i2cWrite16NoVerify(CAMERA_I2C_ADDR, REG_COMMAND_UPDATE, 0x01);
  k_msleep(300);
}

static void testInt(void)
{
  char detail[128];
  uint8_t ae1 = i2cRead16(CAMERA_I2C_ADDR, REG_AE_CTRL1);
  printk("[hwtest]   defaults: AE_CTRL1 0x%02x, PULSE_MODE 0x%02x, EARLY_INT_EN 0x%02x, INT_INDIC 0x%02x\n", ae1,
         i2cRead16(CAMERA_I2C_ADDR, REG_INT_PULSE_MODE), i2cRead16(CAMERA_I2C_ADDR, REG_EARLY_INT_EN),
         i2cRead16(CAMERA_I2C_ADDR, REG_INT_INDIC));

  // a) Interrupts off: the sensor drives INT low, so even our pull-up reads 0 (a loose wire reads 1).
  setMode(MODE_STREAMING);
  i2cWrite16(CAMERA_I2C_ADDR, REG_EARLY_INT_EN, 0x00);
  i2cWrite16(CAMERA_I2C_ADDR, REG_AE_CTRL1, ae1 & ~0x03);
  intUpdate();
  intClear();
  k_msleep(50);
  nrf_gpio_cfg_input(CAM_INT, NRF_GPIO_PIN_PULLUP);
  k_busy_wait(500);
  int up = nrf_gpio_pin_read(CAM_INT);
  nrf_gpio_cfg_input(CAM_INT, NRF_GPIO_PIN_PULLDOWN);
  k_busy_wait(500);
  int down = nrf_gpio_pin_read(CAM_INT);
  int_window_t off = watchInt(1000);
  printInt("interrupts off, 1 s", &off);
  snprintk(detail, sizeof(detail), "pull-up reads %d, pull-down reads %d, %u INT rises with interrupts off", up, down,
           off.rises);
  result("[4a] INT P1.12 driven low by the sensor", up == 0 && down == 0 && off.rises == 0, detail);

  // b) Pulse mode, Early VSYNC interrupt: one pulse per frame, a little before FVLD rises.
  i2cWrite16(CAMERA_I2C_ADDR, REG_INT_PULSE_MODE, 0x01);
  i2cWrite16(CAMERA_I2C_ADDR, REG_EARLY_INT_EN, 0x01);
  intUpdate();
  int_window_t early = watchInt(2000);
  printInt("Early VSYNC, pulse mode, 2 s", &early);
  bool perFrame = early.fvld >= 10 && early.rises + 1 >= early.fvld && early.rises <= early.fvld + 1;
  const char *source = "Early VSYNC";
  int_window_t got = early;
  if (!perFrame) {  // fall back to the AE statistics interrupt
    i2cWrite16(CAMERA_I2C_ADDR, REG_EARLY_INT_EN, 0x00);
    i2cWrite16(CAMERA_I2C_ADDR, REG_AE_CTRL1, (ae1 & ~0x03) | 0x01);
    intUpdate();
    int_window_t stat = watchInt(2000);
    printInt("AE statistics, pulse mode, 2 s", &stat);
    if (stat.rises > early.rises) {
      got = stat;
      source = "AE statistics";
      perFrame = stat.fvld >= 10 && stat.rises + 1 >= stat.fvld && stat.rises <= stat.fvld + 1;
    }
  }
  snprintk(detail, sizeof(detail), "%s: %u INT pulses / %u frames, pulse %u..%u us", source, got.rises, got.fvld,
           got.high_min_us, got.high_max_us);
  result("[4b] INT pulses once per frame", perFrame, detail);

  // c) Level mode: INT stays high until INT_CLEAR, then comes back with the next frame.
  i2cWrite16(CAMERA_I2C_ADDR, REG_INT_PULSE_MODE, 0x00);
  intUpdate();
  int before = nrf_gpio_pin_read(CAM_INT);
  uint8_t indic = i2cRead16(CAMERA_I2C_ADDR, REG_INT_INDIC);
  intClear();
  int cleared = nrf_gpio_pin_read(CAM_INT);
  k_msleep(300);
  int again = nrf_gpio_pin_read(CAM_INT);
  snprintk(detail, sizeof(detail), "INT %d (INT_INDIC 0x%02x), after INT_CLEAR %d, 300 ms later %d", before, indic,
           cleared, again);
  result("[4c] INT level mode + INT_CLEAR", before == 1 && cleared == 0 && again == 1, detail);

  // d) Everything off again.
  i2cWrite16(CAMERA_I2C_ADDR, REG_EARLY_INT_EN, 0x00);
  i2cWrite16(CAMERA_I2C_ADDR, REG_AE_CTRL1, ae1);
  intUpdate();
  intClear();
  int_window_t end = watchInt(1000);
  printInt("interrupts off again, 1 s", &end);
  setMode(MODE_SLEEP);
}

/* ---- [5] PSRAM (APS6404L) on QSPI ---- */

#define QSPI_CSN NRF_GPIO_PIN_MAP(0, 18)
#define TEST_BURST 64u  // bursts for the sweeps and diagnosis
#define TCEM_NS  8000u  // APS6404L-3SQR (3 V, DRAM Click), up to 85 C

/* The wires before the QSPI takes the pins: levels with our pull-down / pull-up, then shorts. */
static void psramWires(void)
{
  struct {
    uint32_t pin;
    const char *name;
    bool drive;  // INT is the sensor's output: only listened to
    int held;    // reads the same with either pull: held by something else (-1: free)
  } p[] = {{QSPI_CSN, "CE# P0.18", true},
           {NRF_GPIO_PIN_MAP(0, 17), "SCK P0.17", true},
           {NRF_GPIO_PIN_MAP(0, 13), "SIO0 P0.13", true},
           {NRF_GPIO_PIN_MAP(0, 14), "SIO1 P0.14", true},
           {NRF_GPIO_PIN_MAP(0, 15), "SIO2 P0.15", true},
           {NRF_GPIO_PIN_MAP(0, 16), "SIO3 P0.16", true},
           {CAM_FSIN, "FSIN P1.11", true},
           {CAM_INT, "INT P1.12", false}};
  char detail[96] = "";
  for (int i = 0; i < ARRAY_SIZE(p); i++) {
    nrf_gpio_cfg_input(p[i].pin, NRF_GPIO_PIN_PULLDOWN);
    k_busy_wait(200);
    int down = nrf_gpio_pin_read(p[i].pin);
    nrf_gpio_cfg_input(p[i].pin, NRF_GPIO_PIN_PULLUP);
    k_busy_wait(200);
    int up = nrf_gpio_pin_read(p[i].pin);
    p[i].held = down == up ? down : -1;
    printk("[hwtest]   pin %-11s pull-down reads %d, pull-up reads %d%s\n", p[i].name, down, up,
           p[i].held < 0 ? "" : (p[i].held ? "  (held high)" : "  (held low)"));
    if (p[i].held >= 0) {
      snprintk(detail + strlen(detail), sizeof(detail) - strlen(detail), "%s%s held %s", detail[0] ? ", " : "",
               p[i].name, p[i].held ? "high" : "low");
    }
  }
  int shorts = 0;
  for (int a = 0; a < ARRAY_SIZE(p); a++) {
    if (!p[a].drive) {
      continue;
    }
    for (int level = 1; level >= 0; level--) {
      for (int b = 0; b < ARRAY_SIZE(p); b++) {
        if (b != a) {
          nrf_gpio_cfg_input(p[b].pin, level ? NRF_GPIO_PIN_PULLDOWN : NRF_GPIO_PIN_PULLUP);
        }
      }
      nrf_gpio_pin_write(p[a].pin, level);
      nrf_gpio_cfg_output(p[a].pin);
      k_busy_wait(200);
      for (int b = 0; b < ARRAY_SIZE(p); b++) {
        if (b != a && p[b].held < 0 && nrf_gpio_pin_read(p[b].pin) == level) {
          printk("[hwtest]   driving %s %s pulls %s %s\n", p[a].name, level ? "high" : "low", p[b].name,
                 level ? "high" : "low");
          shorts++;
        }
      }
      nrf_gpio_cfg_input(p[a].pin, NRF_GPIO_PIN_PULLDOWN);
    }
  }
  nrf_gpio_cfg_output(CAM_FSIN);
  nrf_gpio_pin_clear(CAM_FSIN);
  nrf_gpio_cfg_input(CAM_INT, NRF_GPIO_PIN_PULLDOWN);
  result("[5a] PSRAM / FSIN / INT wires: no shorts", shorts == 0, detail);
}

static void cycleCounterOn(void)
{
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static uint32_t cyclesToNs(uint32_t c)
{
  return (uint32_t)((uint64_t)c * 1000000000ull / SystemCoreClock);
}

/* After every psramConfigure: CE# input buffer on, so the CPU can watch the pin the QSPI drives. */
static bool configure(bool quad, uint8_t mhz, uint16_t burst)
{
  psram_config_t c = {.quad = quad, .sck_mhz = mhz, .burst = burst};
  bool ok = psramConfigure(&c);
  NRF_P0->PIN_CNF[18] &= ~GPIO_PIN_CNF_INPUT_Msk;  // INPUT = Connect
  return ok;
}

typedef struct {
  uint32_t frames;    // CE# low periods (a write is WREN, write, RDSR)
  uint32_t max_low_ns;
  uint32_t total_ns;  // task start -> READY
} burst_t;

/* One QSPI transaction with CE# polled on the pin (~10 cycles a loop, ~0.15 us at 64 MHz). */
static burst_t measureBurst(bool write, uint32_t address, void *buf, uint32_t len)
{
  NRF_QSPI_Type *q = NRF_QSPI;
  const volatile uint32_t *in = &NRF_P0->IN;
  const uint32_t mask = 1u << 18;
  burst_t b = {0};
  if (write) {
    nrf_qspi_write_buffer_set(q, buf, len, address);
  } else {
    nrf_qspi_read_buffer_set(q, buf, len, address);
  }
  unsigned key = irq_lock();
  nrf_qspi_event_clear(q, NRF_QSPI_EVENT_READY);
  uint32_t t0 = DWT->CYCCNT, start = 0, max = 0;
  bool low = false;
  nrf_qspi_task_trigger(q, write ? NRF_QSPI_TASK_WRITESTART : NRF_QSPI_TASK_READSTART);
  for (uint32_t guard = 0; guard < 2000000; guard++) {
    uint32_t now = DWT->CYCCNT;
    bool l = !(*in & mask);
    if (l && !low) {
      start = now;
      b.frames++;
    } else if (!l && low) {
      max = MAX(max, now - start);
    }
    low = l;
    if (!l && q->EVENTS_READY) {
      break;
    }
  }
  uint32_t total = DWT->CYCCNT - t0;
  irq_unlock(key);
  b.max_low_ns = cyclesToNs(max);
  b.total_ns = cyclesToNs(total);
  return b;
}

/* Worst CE# low over n bursts of the current burst size, read and write. */
static void worstBursts(int n, burst_t *rd, burst_t *wr)
{
  static uint32_t buf[PSRAM_PAGE / 4];
  uint32_t len = psramConfig()->burst;
  *rd = *wr = (burst_t){0};
  for (int i = 0; i < n; i++) {
    uint32_t a = 0x100000 + i * len;
    burst_t w = measureBurst(true, a, buf, len), r = measureBurst(false, a, buf, len);
    if (w.max_low_ns > wr->max_low_ns) {
      *wr = w;
    }
    if (r.max_low_ns > rd->max_low_ns) {
      *rd = r;
    }
  }
}

static uint32_t hash32(uint32_t x)
{
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

static uint32_t m_buf[PSRAM_PAGE / 4];

static void pattern(uint32_t *dst, uint32_t address, uint32_t seed)
{
  for (uint32_t i = 0; i < PSRAM_PAGE / 4; i++) {
    dst[i] = hash32((address / 4 + i) ^ seed);
  }
}

static bool writeRegion(uint32_t base, uint32_t bytes, uint32_t seed)
{
  for (uint32_t a = base; a < base + bytes; a += PSRAM_PAGE) {
    pattern(m_buf, a, seed);
    if (!psramWrite(a, m_buf, PSRAM_PAGE)) {
      return false;
    }
  }
  return true;
}

/* Bad bytes in [base, base + bytes); UINT32_MAX if a transfer timed out. */
static uint32_t verifyRegion(uint32_t base, uint32_t bytes, uint32_t seed, uint32_t *firstBad)
{
  static uint32_t expect[PSRAM_PAGE / 4];
  uint32_t bad = 0;
  *firstBad = UINT32_MAX;
  for (uint32_t a = base; a < base + bytes; a += PSRAM_PAGE) {
    if (!psramRead(a, m_buf, PSRAM_PAGE)) {
      return UINT32_MAX;
    }
    pattern(expect, a, seed);
    const uint8_t *g = (const uint8_t *)m_buf, *e = (const uint8_t *)expect;
    for (uint32_t i = 0; i < PSRAM_PAGE; i++) {
      if (g[i] != e[i]) {
        if (bad == 0) {
          *firstBad = a + i;
        }
        bad++;
      }
    }
  }
  return bad;
}

static uint32_t check(uint32_t bytes, uint32_t seed)
{
  uint32_t firstBad;
  if (!writeRegion(0, bytes, seed)) {
    return UINT32_MAX;
  }
  return verifyRegion(0, bytes, seed, &firstBad);
}

/* KB/s over bytes, same 1 KB RAM buffer each time (no pattern work in the timing). */
static void throughput(uint32_t bytes, uint32_t *writeKBs, uint32_t *readKBs)
{
  int64_t t0 = k_uptime_ticks();
  for (uint32_t a = 0; a < bytes; a += PSRAM_PAGE) {
    psramWrite(a, m_buf, PSRAM_PAGE);
  }
  int64_t t1 = k_uptime_ticks();
  for (uint32_t a = 0; a < bytes; a += PSRAM_PAGE) {
    psramRead(a, m_buf, PSRAM_PAGE);
  }
  int64_t t2 = k_uptime_ticks();
  uint32_t wUs = MAX(1u, ticksToUs(t1 - t0)), rUs = MAX(1u, ticksToUs(t2 - t1));
  *writeKBs = (uint32_t)((uint64_t)bytes * 1000000 / 1024 / wUs);
  *readKBs = (uint32_t)((uint64_t)bytes * 1000000 / 1024 / rUs);
}

static const uint8_t m_mhz[] = {8, 16, 24, 32, 48, 96};

/* Unaligned writes / reads of 1-300 bytes through psram.c, from RAM and from flash, vs. a RAM copy. */
static bool apiCheck(char *detail, size_t size)
{
  static uint8_t shadow[4096], src[304], dst[304];
  static const uint8_t flashData[300] = {[0] = 0x5A, [1] = 0xC3, [17] = 0x81, [299] = 0x7E};
  const uint32_t base = 0x200001;  // odd on purpose
  uint32_t x = 0x9E3779B9u, mismatches = 0;
  for (uint32_t i = 0; i < sizeof(shadow); i++) {
    shadow[i] = (uint8_t)hash32(i);
  }
  bool ok = psramWrite(base, shadow, sizeof(shadow));
  for (int n = 0; n < 400 && ok; n++) {
    x = hash32(x + n);
    uint32_t off = x % (sizeof(shadow) - 300), len = 1 + (x >> 12) % 300, skew = (x >> 24) & 3;
    const uint8_t *from;
    if (n % 10 == 9) {
      from = flashData;
    } else {
      for (uint32_t i = 0; i < len; i++) {
        src[skew + i] = (uint8_t)hash32(x ^ i);
      }
      from = src + skew;
    }
    ok = psramWrite(base + off, from, len);
    memcpy(shadow + off, from, len);
    x = hash32(x);
    off = x % (sizeof(shadow) - 300);
    len = 1 + (x >> 12) % 300;
    skew = (x >> 24) & 3;
    ok = ok && psramRead(base + off, dst + skew, len);
    mismatches += memcmp(dst + skew, shadow + off, len) != 0;
  }
  static uint8_t all[4096];
  ok = ok && psramRead(base, all, sizeof(all));
  bool same = memcmp(all, shadow, sizeof(all)) == 0;
  snprintk(detail, size, "400 random writes + reads (1-300 B, odd addresses, RAM and flash sources): %u bad reads, final 4 KB %s",
           mismatches, same ? "matches" : "differs");
  return ok && mismatches == 0 && same;
}

/* ---- quad diagnosis: known contents written on one line, read on four (and the reverse) ---- */

#define FILL_ADDRESS 0x0BADADD5u  // every word holds its own address

static void fillAll(uint32_t value)
{
  configure(false, 48, TEST_BURST);
  for (uint32_t a = 0; a < PSRAM_SIZE; a += PSRAM_PAGE) {
    for (uint32_t i = 0; i < PSRAM_PAGE / 4; i++) {
      m_buf[i] = value == FILL_ADDRESS ? a + 4 * i : value;
    }
    psramWrite(a, m_buf, PSRAM_PAGE);
  }
}

static void io23Pull(uint32_t pull)
{
  for (int pin = 15; pin <= 16; pin++) {
    NRF_P0->PIN_CNF[pin] = (NRF_P0->PIN_CNF[pin] & ~GPIO_PIN_CNF_PULL_Msk) | (pull << GPIO_PIN_CNF_PULL_Pos);
  }
}

static void dump(const char *label, uint32_t address)
{
  uint32_t w[4] = {0};
  bool ok = psramXfer(false, address, w, sizeof(w));
  printk("[hwtest]   %-44s @%06x: %08x %08x %08x %08x%s\n", label, address, w[0], w[1], w[2], w[3], ok ? "" : " (timeout)");
}

/*
 * Which toggling breaks quad reads: each test byte repeated over 4 KB (written on one line), read
 * back on four. In quad a byte is two nibbles, high first: byte 0x01 toggles SIO0 every clock,
 * 0x0F all four lines, 0x00 / 0xFF / 0x33 / 0xCC none.
 */
static void quadToggles(uint8_t mhz)
{
  static const uint8_t bytes[] = {0x00, 0xFF, 0x33, 0xCC, 0x01, 0x02, 0x04, 0x08, 0x03, 0x0C, 0x05, 0x0A, 0x0F, 0xF0, 0x5A};
  const uint32_t base = 0x10000, len = 4096;
  static uint8_t got[4096];
  char line[160] = "";
  for (int i = 0; i < ARRAY_SIZE(bytes); i++) {
    configure(false, 48, TEST_BURST);
    memset(m_buf, bytes[i], PSRAM_PAGE);
    for (uint32_t a = base; a < base + len; a += PSRAM_PAGE) {
      psramWrite(a, m_buf, PSRAM_PAGE);
    }
    configure(true, mhz, TEST_BURST);
    psramRead(base, got, len);
    uint32_t bad = 0;
    for (uint32_t k = 0; k < len; k++) {
      bad += got[k] != bytes[i];
    }
    snprintk(line + strlen(line), sizeof(line) - strlen(line), " %02x:%u", bytes[i], bad);
  }
  printk("[hwtest]   quad %2u MHz, bad bytes / 4 KB per repeated byte:%s\n", mhz, line);
}

/* Random bytes: expected vs. quad-read, and whether the error looks like one extra / missing SCK edge. */
static void quadShift(uint8_t mhz)
{
  static uint8_t exp[64] __aligned(4), got[64] __aligned(4);
  for (int i = 0; i < 64; i++) {
    exp[i] = (uint8_t)hash32(i * 77 + mhz);
  }
  configure(false, 48, TEST_BURST);
  psramWrite(0x20000, exp, sizeof(exp));
  configure(true, mhz, TEST_BURST);
  psramXfer(false, 0x20000, got, sizeof(got));
  printk("[hwtest]   quad %u MHz random  exp:", mhz);
  for (int i = 0; i < 24; i++) {
    printk(" %02x", exp[i]);
  }
  printk("\n[hwtest]   quad %u MHz random  got:", mhz);
  for (int i = 0; i < 24; i++) {
    printk(" %02x", got[i]);
  }
  printk("\n");
  // Nibble streams: got shifted by s nibbles (s < 0: got is late) vs. exp, longest match from the start.
  int first = -1;
  for (int i = 0; i < 128 && first < 0; i++) {
    uint8_t ne = (exp[i / 2] >> (i % 2 ? 0 : 4)) & 0xF, ng = (got[i / 2] >> (i % 2 ? 0 : 4)) & 0xF;
    if (ne != ng) {
      first = i;
    }
  }
  for (int s = -2; s <= 2; s++) {
    int match = 0;
    for (int i = MAX(first, 0); i < 128; i++) {
      int j = i + s;
      if (j < 0 || j >= 128) {
        continue;
      }
      uint8_t ne = (exp[j / 2] >> (j % 2 ? 0 : 4)) & 0xF, ng = (got[i / 2] >> (i % 2 ? 0 : 4)) & 0xF;
      match += ne == ng;
    }
    printk("[hwtest]   quad %u MHz: from the first wrong nibble (%d), got[i] == exp[i%+d] for %d of %d nibbles\n", mhz,
           first, s, match, 128 - MAX(first, 0));
  }
}

/* Random 16 KB written on one line, read on four: bad bytes by position inside the 64 B burst. */
static void quadPositions(uint8_t mhz)
{
  uint32_t hist[16] = {0}, fb;
  configure(false, 48, TEST_BURST);
  writeRegion(0x30000, 16 * 1024, 0x777);
  configure(true, mhz, TEST_BURST);
  static uint32_t expect[PSRAM_PAGE / 4];
  for (uint32_t a = 0x30000; a < 0x30000 + 16 * 1024; a += PSRAM_PAGE) {
    psramRead(a, m_buf, PSRAM_PAGE);
    pattern(expect, a, 0x777);
    const uint8_t *g = (const uint8_t *)m_buf, *e = (const uint8_t *)expect;
    for (uint32_t i = 0; i < PSRAM_PAGE; i++) {
      hist[(i % 64) / 4] += g[i] != e[i];
    }
  }
  (void)fb;
  printk("[hwtest]   quad %2u MHz, bad bytes by burst offset (4 B bins, 256 each):", mhz);
  for (int i = 0; i < 16; i++) {
    printk(" %u", hist[i]);
  }
  printk("\n");
}

/*
 * 0xFF read on four lines comes back as f1 11 11 ..: does the chip stop driving after the first
 * nibble (all four outputs going high at once)? All four lines pulled the same way: a chip that
 * still drives gives ff either way, a silent one gives the pull. Then: is the ID still there?
 */
static void quadAllHigh(uint8_t mhz)
{
  static const struct {
    uint32_t pull;
    const char *name;
  } pulls[] = {{GPIO_PIN_CNF_PULL_Pulldown, "pull-down"}, {GPIO_PIN_CNF_PULL_Pullup, "pull-up"}};
  for (int p = 0; p < 2; p++) {
    for (int fill = 0; fill < 2; fill++) {
      uint8_t v = fill ? 0xFF : 0x77;  // 0x77: three lines high, never all four
      configure(false, 48, TEST_BURST);
      memset(m_buf, v, 64);
      psramXfer(true, 0x40000, m_buf, 64);
      configure(true, mhz, TEST_BURST);
      for (int pin = 13; pin <= 16; pin++) {
        NRF_P0->PIN_CNF[pin] = (NRF_P0->PIN_CNF[pin] & ~GPIO_PIN_CNF_PULL_Msk) | (pulls[p].pull << GPIO_PIN_CNF_PULL_Pos);
      }
      uint8_t r1[16] __aligned(4), r2[16] __aligned(4), id[8];
      psramXfer(false, 0x40000, r1, 16);
      psramXfer(false, 0x40000, r2, 16);
      psramReadId(id);  // no reset in between
      printk("[hwtest]   all %02x, quad %u MHz, IO0-IO3 %-9s read: %02x %02x %02x %02x %02x %02x %02x %02x | again %02x %02x %02x %02x | ID %02x %02x\n",
             v, mhz, pulls[p].name, r1[0], r1[1], r1[2], r1[3], r1[4], r1[5], r1[6], r1[7], r2[0], r2[1], r2[2], r2[3],
             id[0], id[1]);
    }
  }
}

static void quadDiag(void)
{
  quadAllHigh(8);
  quadAllHigh(32);
  quadToggles(8);
  quadToggles(32);
  quadPositions(8);
  quadPositions(32);
  quadShift(8);
  quadShift(32);
  printk("[hwtest]   --- quad diagnosis (single line 48 MHz writes / reads, quad 8 MHz) ---\n");
  static const struct {
    uint32_t fill, pull;
    const char *label;
  } cases[] = {{0xFFFFFFFFu, GPIO_PIN_CNF_PULL_Pulldown, "all ff, IO2/IO3 pull-down, quad read"},
               {0x00000000u, GPIO_PIN_CNF_PULL_Pullup, "all 00, IO2/IO3 pull-up, quad read"},
               {0x33333333u, GPIO_PIN_CNF_PULL_Pullup, "all 33 (IO0/IO1 bits), pull-up, quad read"},
               {0xCCCCCCCCu, GPIO_PIN_CNF_PULL_Pulldown, "all cc (IO2/IO3 bits), pull-down, quad read"}};
  for (int i = 0; i < ARRAY_SIZE(cases); i++) {
    fillAll(cases[i].fill);
    configure(true, 8, TEST_BURST);
    io23Pull(cases[i].pull);
    dump(cases[i].label, 0x000000);
    dump(cases[i].label, 0x123450);
    io23Pull(GPIO_PIN_CNF_PULL_Disabled);
  }
  fillAll(FILL_ADDRESS);
  configure(true, 8, TEST_BURST);
  dump("word = own address, quad read", 0x000100);
  dump("word = own address, quad read", 0x5A5A00);
  configure(false, 8, TEST_BURST);
  dump("word = own address, single read", 0x5A5A00);

  // Quad write of a marker into an all-zero chip; single-line read back, then where did it land?
  fillAll(0);
  configure(true, 8, TEST_BURST);
  static uint32_t marker[4] = {0x76543210u, 0xFEDCBA98u, 0x11223344u, 0xA5C35A3Cu};
  psramXfer(true, 0x000100, marker, sizeof(marker));
  configure(false, 48, TEST_BURST);
  dump("marker quad-written, single read", 0x000100);
  int hits = 0;
  for (uint32_t a = 0; a < PSRAM_SIZE && hits < 8; a += PSRAM_PAGE) {
    psramRead(a, m_buf, PSRAM_PAGE);
    for (uint32_t i = 0; i < PSRAM_PAGE / 4 && hits < 8; i++) {
      if (m_buf[i] != 0) {
        printk("[hwtest]   non-zero word after the quad write: @%06x = %08x\n", a + 4 * i, m_buf[i]);
        hits++;
      }
    }
  }
  if (hits == 0) {
    printk("[hwtest]   no non-zero word anywhere: the quad write did not reach the chip\n");
  }
}

static void testPsram(void)
{
  char detail[160];
  cycleCounterOn();
  psramWires();

  // [5b] ID over the QSPI custom instruction, slowest clock first.
  bool idOk = configure(false, 8, TEST_BURST);
  uint8_t id[8];
  psramReadId(id);
  snprintk(detail, sizeof(detail), "MF 0x%02x KGD 0x%02x (expect 0x0d 0x5d)", id[0], id[1]);
  result("[5b] PSRAM ID on QSPI (8 MHz)", idOk, detail);
  if (!idOk) {
    return;
  }

  // [5c] / [5d] Single line (0x0B / 0x02) and quad (0xEB / 0x38) at each clock: 64 KB check,
  // CE# low per 64 B burst on the pin, throughput over 256 KB.
  bool ok[2][ARRAY_SIZE(m_mhz)];
  for (int quad = 0; quad < 2; quad++) {
    for (int i = 0; i < ARRAY_SIZE(m_mhz); i++) {
      bool idGood = configure(quad, m_mhz[i], TEST_BURST);
      uint32_t bad = check(64 * 1024, 0x1000 * m_mhz[i] + quad);
      burst_t rd, wr;
      worstBursts(64, &rd, &wr);
      uint32_t wKBs, rKBs;
      throughput(256 * 1024, &wKBs, &rKBs);
      ok[quad][i] = idGood && bad == 0;
      printk("[hwtest]   %-6s %2u MHz: ID %s, 64 KB %u bad, CE# low per 64 B read %u ns / write %u ns (%u CE# periods), "
             "write %u KB/s, read %u KB/s\n",
             quad ? "quad" : "single", m_mhz[i], idGood ? "ok" : "BAD", bad, rd.max_low_ns, wr.max_low_ns, wr.frames,
             wKBs, rKBs);
    }
  }
  for (int quad = 0; quad < 2; quad++) {
    char *d = detail;
    d[0] = 0;
    bool any = false, at32 = false;
    for (int i = 0; i < ARRAY_SIZE(m_mhz); i++) {
      snprintk(d + strlen(d), sizeof(detail) - strlen(d), "%s%u MHz %s", i ? ", " : "", m_mhz[i], ok[quad][i] ? "ok" : "BAD");
      any |= ok[quad][i];
      at32 |= m_mhz[i] == 32 && ok[quad][i];
    }
    result(quad ? "[5d] PSRAM quad 0xEB / 0x38 (32 MHz needed)" : "[5c] PSRAM single line 0x0B / 0x02",
           quad ? at32 : any, detail);
  }

  quadDiag();

  // [5e] Cross check at 32 MHz: written on one line, read on four, and the other way round.
  configure(false, 32, TEST_BURST);
  writeRegion(0, 64 * 1024, 0xC0FFEE);
  configure(true, 32, TEST_BURST);
  uint32_t fb, bad1 = verifyRegion(0, 64 * 1024, 0xC0FFEE, &fb);
  writeRegion(0, 64 * 1024, 0xBEEF);
  configure(false, 32, TEST_BURST);
  uint32_t bad2 = verifyRegion(0, 64 * 1024, 0xBEEF, &fb);
  snprintk(detail, sizeof(detail), "single write -> quad read %u bad, quad write -> single read %u bad (64 KB)", bad1, bad2);
  result("[5e] PSRAM single / quad cross check", bad1 == 0 && bad2 == 0, detail);

  // Burst size: CE# low on the pin vs. throughput (quad, 32 MHz and 48 MHz).
  static const uint16_t bursts[] = {16, 32, 64, 128, 256, 512, 1024};
  for (int f = 0; f < 2; f++) {
    uint8_t mhz = f ? 48 : 32;
    if (!ok[1][f ? 4 : 3]) {
      continue;
    }
    for (int i = 0; i < ARRAY_SIZE(bursts); i++) {
      configure(true, mhz, bursts[i]);
      uint32_t bad = check(64 * 1024, 0xB000 + bursts[i]);
      burst_t rd, wr;
      worstBursts(200, &rd, &wr);
      uint32_t wKBs, rKBs;
      throughput(1024 * 1024, &wKBs, &rKBs);
      printk("[hwtest]   quad %u MHz burst %4u B: CE# low read %5u ns, write %5u ns (task %u / %u ns), %s tCEM 8 us, "
             "write %5u KB/s, read %5u KB/s, 64 KB %u bad\n",
             mhz, bursts[i], rd.max_low_ns, wr.max_low_ns, rd.total_ns, wr.total_ns,
             MAX(rd.max_low_ns, wr.max_low_ns) <= TCEM_NS ? "within" : "OVER  ", wKBs, rKBs, bad);
    }
  }

  // [5f] - [5j] The default (psramInit, src/psram.h).
  configure(PSRAM_DEFAULT_QUAD, PSRAM_DEFAULT_MHZ, PSRAM_DEFAULT_BURST);
  burst_t rd, wr;
  worstBursts(1000, &rd, &wr);
  uint32_t worst = MAX(rd.max_low_ns, wr.max_low_ns);
  snprintk(detail, sizeof(detail), "%s %u MHz, %u B bursts: worst CE# low read %u ns, write %u ns over 1000 bursts (limit %u ns)",
           PSRAM_DEFAULT_QUAD ? "quad" : "single", PSRAM_DEFAULT_MHZ, PSRAM_DEFAULT_BURST, rd.max_low_ns, wr.max_low_ns, TCEM_NS);
  result("[5f] PSRAM default keeps tCEM", worst > 0 && worst <= TCEM_NS, detail);

  uint32_t wKBs, rKBs;
  throughput(1024 * 1024, &wKBs, &rKBs);
  snprintk(detail, sizeof(detail), "write %u KB/s, read %u KB/s (need >= 300, goal >= 2048: a raw VGA frame in 150 ms)",
           wKBs, rKBs);
  result("[5g] PSRAM default throughput >= 2 MB/s", wKBs >= 2048 && rKBs >= 2048, detail);

  // [5h] / [5i] Whole chip, twice.
  static const uint32_t seeds[] = {0x12345678u, 0xA5A5A5A5u};
  for (int i = 0; i < 2; i++) {
    int64_t t0 = k_uptime_get();
    bool wOk = writeRegion(0, PSRAM_SIZE, seeds[i]);
    int64_t t1 = k_uptime_get();
    uint32_t firstBad, bad = wOk ? verifyRegion(0, PSRAM_SIZE, seeds[i], &firstBad) : UINT32_MAX;
    int64_t t2 = k_uptime_get();
    snprintk(detail, sizeof(detail), "8 MB, default config, %u bad bytes, write %u ms, read + compare %u ms", bad,
             (uint32_t)(t1 - t0), (uint32_t)(t2 - t1));
    if (bad && bad != UINT32_MAX) {
      snprintk(detail + strlen(detail), sizeof(detail) - strlen(detail), ", first bad 0x%06x", firstBad);
    }
    result(i == 0 ? "[5h] PSRAM 8 MB pass 1" : "[5i] PSRAM 8 MB pass 2", bad == 0, detail);
  }

  // [5j] The API the encoders use (src/image.c), with odd sizes and addresses.
  bool apiOk = apiCheck(detail, sizeof(detail));
  result("[5j] PSRAM psramWrite / psramRead unaligned", apiOk, detail);

  printk("[hwtest]   (theory, quad: read CE# low = (8 + 6 + 6 + 2 x bytes) / SCK, write (8 + 6 + 2 x bytes) / SCK)\n");
}

void hwTestRun(void)
{
  k_msleep(1500);  // time for the RTT logger to attach
  printk("[hwtest] ===== hardware check =====\n");

  cameraInit();
  uint16_t id = cameraModelId();
  char detail[48];
  snprintk(detail, sizeof(detail), "model 0x%04x", id);
  result("[1] HM0360 I2C", id == 0x0360, detail);

#ifdef HW_TEST_PSRAM_ONLY  // -Cflags 'HW_TEST;HW_TEST_PSRAM_ONLY': skip [2]-[4]
  id = 0;
#endif
  if (id == 0x0360) {
    cameraSensorStream();  // MCLK on; the tests set MODE_SELECT themselves
    setMode(MODE_SLEEP);
    testFsin();
    testTrigger();
    testInt();
  }
  testPsram();

  printk("[hwtest] ===== done: %s (%d failed) =====\n", failures ? "FAIL" : "ALL PASS", failures);
}

#endif /* HW_TEST */
