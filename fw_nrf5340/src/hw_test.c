/*
 * Hardware check for the wires added on 2026-10-10 (build with -Cflags 'HW_TEST'; the
 * camera / BLE application does not start). Prints PASS / FAIL per item on RTT:
 *   [1] HM0360 answers I2C (camera init as usual)
 *   [2] FSIN    (P1.11): FSYNC on (0x3010[1]); frames must follow our pulses
 *   [3] TRIGGER (P0.25): MODE_SELECT = Streaming 4; frames only while the pin is high
 *   [4] PSRAM   (SPIM4 8 MHz, CS P1.12, MOSI P1.13, MISO P1.14, SCK P1.15): ID, then all
 *       8 MB written and read back in 1 KB bursts (tCEM ignored, as agreed on 10-10)
 * Frames are counted by polling FVLD for rising edges (it is high for the whole frame
 * period and low only ~60 us between frames) and cross-checked with FRAME_COUNT.
 */
#ifdef HW_TEST

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_gpio.h>
#include <hal/nrf_spim.h>

#include "camera.h"
#include "i2c.h"
#include "pins.h"

#define CAM_FSIN     NRF_GPIO_PIN_MAP(1, 11)  // Arduino D9
#define CAM_TRIGGER  NRF_GPIO_PIN_MAP(0, 25)  // Arduino A4
#define PSRAM_CS     NRF_GPIO_PIN_MAP(1, 12)  // Arduino D10
#define PSRAM_MOSI   NRF_GPIO_PIN_MAP(1, 13)  // Arduino D11
#define PSRAM_MISO   NRF_GPIO_PIN_MAP(1, 14)  // Arduino D12
#define PSRAM_SCK    NRF_GPIO_PIN_MAP(1, 15)  // Arduino D13

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

/* ---- PSRAM (APS6404L) over SPIM4, register level like the camera SPIS ---- */

#define PSRAM_SIZE  (8u * 1024 * 1024)
#define PSRAM_PAGE  1024u

static uint8_t spiTx[4 + PSRAM_PAGE];
static uint8_t spiRx[4 + PSRAM_PAGE];

static void spim(const uint8_t *tx, size_t txLen, uint8_t *rx, size_t rxLen)
{
  NRF_SPIM_Type *s = NRF_SPIM4;
  nrf_gpio_pin_clear(PSRAM_CS);
  nrf_spim_tx_buffer_set(s, tx, txLen);
  nrf_spim_rx_buffer_set(s, rx, rxLen);
  nrf_spim_event_clear(s, NRF_SPIM_EVENT_END);
  nrf_spim_task_trigger(s, NRF_SPIM_TASK_START);
  while (!nrf_spim_event_check(s, NRF_SPIM_EVENT_END)) {
  }
  nrf_gpio_pin_set(PSRAM_CS);
}

static void psramInit(void)
{
  NRF_SPIM_Type *s = NRF_SPIM4;
  nrf_gpio_pin_set(PSRAM_CS);
  nrf_gpio_cfg_output(PSRAM_CS);
  nrf_gpio_pin_clear(PSRAM_SCK);
  nrf_gpio_cfg(PSRAM_SCK, NRF_GPIO_PIN_DIR_OUTPUT, NRF_GPIO_PIN_INPUT_CONNECT, NRF_GPIO_PIN_NOPULL,
               NRF_GPIO_PIN_S0S1, NRF_GPIO_PIN_NOSENSE);
  nrf_gpio_cfg_output(PSRAM_MOSI);
  nrf_gpio_cfg_input(PSRAM_MISO, NRF_GPIO_PIN_PULLDOWN);  // reads 0 if nothing drives it
  nrf_spim_pins_set(s, PSRAM_SCK, PSRAM_MOSI, PSRAM_MISO);
  nrf_spim_frequency_set(s, NRF_SPIM_FREQ_8M);
  nrf_spim_configure(s, NRF_SPIM_MODE_0, NRF_SPIM_BIT_ORDER_MSB_FIRST);
  nrf_spim_orc_set(s, 0x00);
  nrf_spim_enable(s);

  static const uint8_t resetEnable = 0x66, reset = 0x99;
  spim(&resetEnable, 1, NULL, 0);
  spim(&reset, 1, NULL, 0);
  k_busy_wait(100);
}

static uint32_t xorshift(uint32_t x)
{
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  return x;
}

static void fillPage(uint8_t *dst, uint32_t page, uint32_t seed)
{
  uint32_t x = (page + 1) * 2654435761u ^ seed;
  for (uint32_t i = 0; i < PSRAM_PAGE; i += 4) {
    x = xorshift(x);
    memcpy(dst + i, &x, 4);
  }
}

/* Write then read back the whole chip; returns bad bytes. */
static uint32_t psramPass(uint32_t seed, uint32_t *writeMs, uint32_t *readMs, uint32_t *firstBad)
{
  static uint8_t expect[PSRAM_PAGE];
  uint32_t bad = 0;
  *firstBad = UINT32_MAX;

  int64_t t0 = k_uptime_get();
  for (uint32_t page = 0; page < PSRAM_SIZE / PSRAM_PAGE; page++) {
    uint32_t a = page * PSRAM_PAGE;
    spiTx[0] = 0x02;
    spiTx[1] = a >> 16;
    spiTx[2] = a >> 8;
    spiTx[3] = a;
    fillPage(spiTx + 4, page, seed);
    spim(spiTx, sizeof(spiTx), NULL, 0);
  }
  int64_t t1 = k_uptime_get();
  for (uint32_t page = 0; page < PSRAM_SIZE / PSRAM_PAGE; page++) {
    uint32_t a = page * PSRAM_PAGE;
    uint8_t cmd[4] = {0x03, a >> 16, a >> 8, a};
    spim(cmd, 4, spiRx, sizeof(spiRx));
    fillPage(expect, page, seed);
    for (uint32_t i = 0; i < PSRAM_PAGE; i++) {
      if (spiRx[4 + i] != expect[i]) {
        if (bad == 0) {
          *firstBad = a + i;
        }
        bad++;
      }
    }
  }
  *writeMs = (uint32_t)(t1 - t0);
  *readMs = (uint32_t)(k_uptime_get() - t1);
  return bad;
}

static void psramReadId(uint8_t id[8])
{
  uint8_t cmd[4] = {0x9F, 0, 0, 0};
  uint8_t rx[12];
  spim(cmd, 4, rx, sizeof(rx));
  memcpy(id, rx + 4, 8);
}

/* Quick write / read check of the first n bytes; returns bad bytes. */
static uint32_t psramQuick(uint32_t bytes)
{
  static uint8_t expect[PSRAM_PAGE];
  uint32_t bad = 0;
  for (uint32_t page = 0; page < bytes / PSRAM_PAGE; page++) {
    uint32_t a = page * PSRAM_PAGE;
    spiTx[0] = 0x02;
    spiTx[1] = a >> 16;
    spiTx[2] = a >> 8;
    spiTx[3] = a;
    fillPage(spiTx + 4, page, 0x5EED);
    spim(spiTx, sizeof(spiTx), NULL, 0);
  }
  for (uint32_t page = 0; page < bytes / PSRAM_PAGE; page++) {
    uint32_t a = page * PSRAM_PAGE;
    uint8_t cmd[4] = {0x03, a >> 16, a >> 8, a};
    spim(cmd, 4, spiRx, sizeof(spiRx));
    fillPage(expect, page, 0x5EED);
    for (uint32_t i = 0; i < PSRAM_PAGE; i++) {
      bad += spiRx[4 + i] != expect[i];
    }
  }
  return bad;
}

static void printBytes(const char *label, const uint8_t *p, int n)
{
  printk("[hwtest]   %-26s", label);
  for (int i = 0; i < n; i++) {
    printk(" %02x", p[i]);
  }
  printk("\n");
}

/* Pin levels with the internal pull-down, then pull-up (nothing driving from our side). */
static void psramPins(void)
{
  static const struct {
    uint32_t pin;
    const char *name;
  } pins[] = {{PSRAM_CS, "CS P1.12"}, {PSRAM_MOSI, "SDI P1.13"}, {PSRAM_MISO, "SDO P1.14"}, {PSRAM_SCK, "SCK P1.15"}};
  for (int i = 0; i < ARRAY_SIZE(pins); i++) {
    nrf_gpio_cfg_input(pins[i].pin, NRF_GPIO_PIN_PULLDOWN);
    k_busy_wait(100);
    int down = nrf_gpio_pin_read(pins[i].pin);
    nrf_gpio_cfg_input(pins[i].pin, NRF_GPIO_PIN_PULLUP);
    k_busy_wait(100);
    int up = nrf_gpio_pin_read(pins[i].pin);
    printk("[hwtest]   pin %-10s pull-down reads %d, pull-up reads %d\n", pins[i].name, down, up);
  }
}

static void psramRaw(void)
{
  uint8_t rx[4 + 16];
  uint8_t cmd[4] = {0x03, 0, 0, 0};
  spim(cmd, 4, rx, sizeof(rx));
  printBytes("read @0 before write:", rx + 4, 16);

  uint8_t wr[4 + 16] = {0x02, 0, 0, 0};
  for (int i = 0; i < 16; i++) {
    wr[4 + i] = i;
  }
  spim(wr, sizeof(wr), NULL, 0);
  spim(cmd, 4, rx, sizeof(rx));
  printBytes("read @0 after 00..0f:", rx + 4, 16);
  spim(cmd, 4, rx, sizeof(rx));
  printBytes("read @0 again:", rx + 4, 16);

  uint8_t fast[5] = {0x0B, 0, 0, 0, 0};  // fast read, 8 wait cycles
  uint8_t frx[5 + 16];
  spim(fast, 5, frx, sizeof(frx));
  printBytes("fast read @0:", frx + 5, 16);

  uint8_t idcmd[4] = {0x9F, 0, 0, 0};
  uint8_t idrx[20];
  spim(idcmd, 4, idrx, sizeof(idrx));
  printBytes("read ID, all bytes:", idrx, 20);

  // Same byte everywhere: few transitions (00 / ff) vs. many (aa / 55), plus single bits.
  static const uint8_t fills[] = {0x00, 0xFF, 0xAA, 0x55, 0x80, 0x01, 0xF0, 0x0F};
  for (int f = 0; f < ARRAY_SIZE(fills); f++) {
    memset(wr + 4, fills[f], 16);
    spim(wr, sizeof(wr), NULL, 0);
    spim(cmd, 4, rx, sizeof(rx));
    char label[32];
    snprintk(label, sizeof(label), "wrote 16 x %02x, read:", fills[f]);
    printBytes(label, rx + 4, 16);
  }
}

/* Bit-banged SPI mode 0, ~100 kHz: same transactions without the SPIM peripheral. */
static void bbXfer(const uint8_t *tx, size_t txLen, uint8_t *rx, size_t rxLen)
{
  size_t n = MAX(txLen, rxLen);
  nrf_gpio_pin_clear(PSRAM_CS);
  k_busy_wait(5);
  for (size_t i = 0; i < n; i++) {
    uint8_t out = i < txLen ? tx[i] : 0x00, in = 0;
    for (int b = 7; b >= 0; b--) {
      nrf_gpio_pin_write(PSRAM_MOSI, (out >> b) & 1);
      k_busy_wait(5);
      nrf_gpio_pin_set(PSRAM_SCK);  // chip samples SI, we sample SO on the rising edge
      in = (in << 1) | nrf_gpio_pin_read(PSRAM_MISO);
      k_busy_wait(5);
      nrf_gpio_pin_clear(PSRAM_SCK);
    }
    if (i < rxLen) {
      rx[i] = in;
    }
  }
  k_busy_wait(5);
  nrf_gpio_pin_set(PSRAM_CS);
  k_busy_wait(5);
}

static void psramBitBang(void)
{
  nrf_gpio_pin_set(PSRAM_CS);
  nrf_gpio_cfg_output(PSRAM_CS);
  nrf_gpio_pin_clear(PSRAM_SCK);
  nrf_gpio_cfg_output(PSRAM_SCK);
  nrf_gpio_pin_clear(PSRAM_MOSI);
  nrf_gpio_cfg_output(PSRAM_MOSI);
  nrf_gpio_cfg_input(PSRAM_MISO, NRF_GPIO_PIN_PULLDOWN);

  static const uint8_t re = 0x66, rst = 0x99;
  bbXfer(&re, 1, NULL, 0);
  bbXfer(&rst, 1, NULL, 0);
  k_busy_wait(100);

  uint8_t idcmd[4] = {0x9F, 0, 0, 0}, idrx[12];
  bbXfer(idcmd, 4, idrx, sizeof(idrx));
  printBytes("bit-bang read ID:", idrx, 12);

  uint8_t wr[4 + 16] = {0x02, 0, 0, 0};
  for (int i = 0; i < 16; i++) {
    wr[4 + i] = 0x10 + i;
  }
  bbXfer(wr, sizeof(wr), NULL, 0);
  uint8_t cmd[4] = {0x03, 0, 0, 0}, rx[4 + 16];
  bbXfer(cmd, 4, rx, sizeof(rx));
  printBytes("bit-bang read @0 (10..1f):", rx + 4, 16);
}

/*
 * Shorts between the PSRAM wires (and FSIN next to them): drive one pin high / low, the others
 * are inputs pulled the other way. CS stays high from the Click's pull-up (chip SO is high-Z).
 */
static void psramShorts(void)
{
  static const struct {
    uint32_t pin;
    const char *name;
  } pins[] = {{CAM_FSIN, "FSIN P1.11"}, {PSRAM_CS, "CS P1.12"}, {PSRAM_MOSI, "SDI P1.13"},
              {PSRAM_MISO, "SDO P1.14"}, {PSRAM_SCK, "SCK P1.15"}};
  int shorts = 0;
  for (int a = 0; a < ARRAY_SIZE(pins); a++) {
    for (int level = 1; level >= 0; level--) {
      for (int b = 0; b < ARRAY_SIZE(pins); b++) {
        if (b != a) {
          nrf_gpio_cfg_input(pins[b].pin, level ? NRF_GPIO_PIN_PULLDOWN : NRF_GPIO_PIN_PULLUP);
        }
      }
      nrf_gpio_pin_write(pins[a].pin, level);
      nrf_gpio_cfg_output(pins[a].pin);
      k_busy_wait(200);
      for (int b = 0; b < ARRAY_SIZE(pins); b++) {
        if (b != a && nrf_gpio_pin_read(pins[b].pin) == level && !(pins[b].pin == PSRAM_CS && level)) {
          printk("[hwtest]   driving %s %s pulls %s %s\n", pins[a].name, level ? "high" : "low",
                 pins[b].name, level ? "high" : "low");
          shorts++;
        }
      }
      nrf_gpio_cfg_input(pins[a].pin, NRF_GPIO_PIN_PULLDOWN);
    }
  }
  nrf_gpio_cfg_output(CAM_FSIN);
  nrf_gpio_pin_clear(CAM_FSIN);
  result("[4-] PSRAM wires: no shorts", shorts == 0, "");
}

static void testPsram(void)
{
  char detail[128];
  psramShorts();
  psramPins();
  psramBitBang();
  psramInit();
  psramRaw();

  static const struct {
    nrf_spim_frequency_t f;
    const char *name;
  } freqs[] = {{NRF_SPIM_FREQ_1M, "1 MHz"}, {NRF_SPIM_FREQ_2M, "2 MHz"},
               {NRF_SPIM_FREQ_4M, "4 MHz"}, {NRF_SPIM_FREQ_8M, "8 MHz"}};
  for (int i = 0; i < ARRAY_SIZE(freqs); i++) {
    nrf_spim_frequency_set(NRF_SPIM4, freqs[i].f);
    uint8_t id[8];
    psramReadId(id);
    uint32_t bad = psramQuick(64 * 1024);
    printk("[hwtest]   PSRAM %s: ID %02x %02x | EID %02x %02x %02x %02x %02x %02x | 64 KB check %u bad bytes\n",
           freqs[i].name, id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7], bad);
  }
  nrf_spim_frequency_set(NRF_SPIM4, NRF_SPIM_FREQ_8M);

  uint8_t id[8];
  psramReadId(id);
  bool idOk = id[0] == 0x0D && id[1] == 0x5D;
  snprintk(detail, sizeof(detail), "MF 0x%02x KGD 0x%02x (expect 0x0d 0x5d)", id[0], id[1]);
  result("[4a] PSRAM ID (8 MHz)", idOk, detail);

  static const uint32_t seeds[] = {0x12345678u, 0xA5A5A5A5u};
  for (int i = 0; i < 2; i++) {
    uint32_t wMs, rMs, firstBad;
    uint32_t bad = psramPass(seeds[i], &wMs, &rMs, &firstBad);
    snprintk(detail, sizeof(detail), "8 MB at 8 MHz, %u bad bytes, write %u ms (%u KB/s), read %u ms (%u KB/s)",
             bad, wMs, 8192u * 1000 / wMs, rMs, 8192u * 1000 / rMs);
    if (bad) {
      snprintk(detail + strlen(detail), sizeof(detail) - strlen(detail), ", first bad 0x%06x", firstBad);
    }
    result(i == 0 ? "[4b] PSRAM 8 MB pass 1" : "[4c] PSRAM 8 MB pass 2", bad == 0, detail);
  }
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

  if (id == 0x0360) {
    cameraSensorStream();  // MCLK on; the tests set MODE_SELECT themselves
    setMode(MODE_SLEEP);
    testFsin();
    testTrigger();
  }
  testPsram();

  printk("[hwtest] ===== done: %s (%d failed) =====\n", failures ? "FAIL" : "ALL PASS", failures);
}

#endif /* HW_TEST */
