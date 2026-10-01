/*
 * HM0360 wiring / state check (diagnostic tool, not part of the normal firmware).
 *
 * Build:  .\tools\dk.ps1 run -WiringTest   (or CFLAGS=-DWIRING_TEST make banji_dev ...)
 * Runs once at boot, before BLE, after an RTT host has attached (so the first run after a
 * power cycle can be seen), then the firmware boots normally. Prints a pass/fail line per
 * check. Uses the pins from gpio.h; RST/XSLEEP checks assume the HM0360 module (UC-806).
 */
#ifdef WIRING_TEST

#include "nrf.h"
#include "nrf_gpio.h"
#include "nrf_delay.h"
#include "SEGGER_RTT.h"
#include "nrf_log.h"
#include "nrf_log_ctrl.h"
#include "i2c.h"
#include "gpio.h"
#include "capture.h"

#define ADDR      0x24
#define MODEL_ID  0x0360
#define TEST_REG  0x2034  // AE_TARGET_MEAN: read/write, otherwise untouched here
#define TEST_VAL  0x55

// Busy-wait delays throughout: delayMs() sleeps in __WFE and, before BLE runs, only wakes on
// the 1 s system timer interrupt, which stretches short delays to about a second.
// The 20 ms pause lets the RTT logger drain the 512 B up buffer so lines are not dropped.
#define LOG(...) do { NRF_LOG_RAW_INFO(__VA_ARGS__); NRF_LOG_FLUSH(); nrf_delay_ms(20); } while (0)
#define PASS(ok) ((ok) ? "PASS" : "FAIL")

static uint16_t read_id(void)
{
  return ((uint16_t)i2cRead16(ADDR, 0x0000) << 8) | i2cRead16(ADDR, 0x0001);
}

// Poll the model ID; returns ms until it reads MODEL_ID, or -1 on timeout.
static int poll_id(uint32_t timeout_ms)
{
  for (uint32_t ms = 0; ms <= timeout_ms; ms += 5) {
    if (read_id() == MODEL_ID) {
      return (int)ms;
    }
    nrf_delay_ms(5);
  }
  return -1;
}

static void pin_drive(uint32_t pin, bool high)
{
  nrf_gpio_cfg_output(pin);
  nrf_gpio_pin_write(pin, high);
}

static void rst_pulse(void)
{
  pin_drive(CAM_XSHUTDOWN, 0);
  nrf_delay_ms(10);
  pin_drive(CAM_XSHUTDOWN, 1);
}

// Drive low, release to a floating input: the module's pull-up brings it back high if wired.
static bool pullup_present(uint32_t pin)
{
  pin_drive(pin, 0);
  nrf_delay_ms(10);
  nrf_gpio_cfg_input(pin, NRF_GPIO_PIN_NOPULL);
  nrf_delay_ms(20);
  return nrf_gpio_pin_read(pin);
}

// Sample a port-1 output pin with its input buffer connected; returns level changes in 10000 reads.
static uint32_t pin_changes(uint32_t pin)
{
  NRF_P1->PIN_CNF[pin & 31] &= ~GPIO_PIN_CNF_INPUT_Msk;
  uint32_t changes = 0, prev = (NRF_P1->IN >> (pin & 31)) & 1;
  for (int i = 0; i < 10000; i++) {
    uint32_t v = (NRF_P1->IN >> (pin & 31)) & 1;
    changes += (v != prev);
    prev = v;
  }
  return changes;
}

// Count rising edges on pin for ms using GPIOTE ch7 -> PPI ch15 -> TIMER4 (counter).
// GPIOTE cannot follow MHz clocks, so for PCLKO / D0 a non-zero count only proves activity.
static uint32_t count_edges(uint32_t pin, uint32_t ms)
{
  NRF_TIMER4->TASKS_STOP = 1;
  NRF_TIMER4->MODE = TIMER_MODE_MODE_LowPowerCounter;
  NRF_TIMER4->BITMODE = TIMER_BITMODE_BITMODE_32Bit;
  NRF_TIMER4->TASKS_CLEAR = 1;

  nrf_gpio_cfg_input(pin, NRF_GPIO_PIN_NOPULL);
  NRF_GPIOTE->CONFIG[7] = (GPIOTE_CONFIG_MODE_Event << GPIOTE_CONFIG_MODE_Pos) |
                          ((pin & 31) << GPIOTE_CONFIG_PSEL_Pos) |
                          ((pin >> 5) << GPIOTE_CONFIG_PORT_Pos) |
                          (GPIOTE_CONFIG_POLARITY_LoToHi << GPIOTE_CONFIG_POLARITY_Pos);
  NRF_PPI->CH[15].EEP = (uint32_t)&NRF_GPIOTE->EVENTS_IN[7];
  NRF_PPI->CH[15].TEP = (uint32_t)&NRF_TIMER4->TASKS_COUNT;
  NRF_PPI->CHENSET = 1u << 15;

  NRF_TIMER4->TASKS_START = 1;
  nrf_delay_ms(ms);
  NRF_TIMER4->TASKS_CAPTURE[0] = 1;
  uint32_t n = NRF_TIMER4->CC[0];

  NRF_TIMER4->TASKS_STOP = 1;
  NRF_PPI->CHENCLR = 1u << 15;
  NRF_GPIOTE->CONFIG[7] = 0;
  return n;
}

static void wait_for_rtt_host(void)
{
  // The host catches the up buffer's read offset up to the write offset once it is reading.
  NRF_LOG_RAW_INFO("wiring test: waiting for RTT host...\n");
  NRF_LOG_FLUSH();
  volatile unsigned *rd = (volatile unsigned *)&_SEGGER_RTT.aUp[0].RdOff;
  volatile unsigned *wr = (volatile unsigned *)&_SEGGER_RTT.aUp[0].WrOff;
  for (int i = 0; i < 1200 && *rd != *wr; i++) {
    nrf_delay_ms(100);
  }
  nrf_delay_ms(500);
}

void wiringTest(void)
{
  wait_for_rtt_host();
  LOG("\n===== HM0360 wiring test: MCLK P1.%02u, RST P1.%02u, XSLEEP P1.%02u =====\n",
      CAM_MCLK_IN_FROM_MCU & 31, CAM_XSHUTDOWN & 31, CAM_XSLEEP & 31);

  // 1. Module powered and control lines connected (10 k pull-ups to 2.8 V on the module).
  bool sleep_pu = pullup_present(CAM_XSLEEP);
  bool rst_pu = pullup_present(CAM_XSHUTDOWN);
  LOG("[1] module pull-ups: XSLEEP %s, RST %s\n", PASS(sleep_pu), PASS(rst_pu));

  // 2. CS loopback jumper P0.12 -> P0.14.
  nrf_gpio_cfg_input(CAM_SPI_CS_IN, NRF_GPIO_PIN_NOPULL);
  pin_drive(CAM_SPI_CS_OUT, 0);
  nrf_delay_ms(1);
  uint32_t lo = nrf_gpio_pin_read(CAM_SPI_CS_IN);
  pin_drive(CAM_SPI_CS_OUT, 1);
  nrf_delay_ms(1);
  uint32_t hi = nrf_gpio_pin_read(CAM_SPI_CS_IN);
  LOG("[2] CS jumper P0.12 -> P0.14: %s (read %u, %u)\n", PASS(lo == 0 && hi == 1), lo, hi);

  // 3. The sensor needs MCLK even for I2C, so no answer without it.
  pin_drive(CAM_XSLEEP, 1);
  rst_pulse();
  nrf_delay_ms(10);
  uint16_t id = read_id();
  LOG("[3] MCLK off: id 0x%04X, %s (expect no answer)\n", id, PASS(id != MODEL_ID));

  // 4. MCLK toggles on the nRF pin.
  capture_mclk_init();
  nrf_delay_ms(1);
  uint32_t on = pin_changes(CAM_MCLK_IN_FROM_MCU);
  capture_mclk_enable(false);
  uint32_t off = pin_changes(CAM_MCLK_IN_FROM_MCU);
  capture_mclk_enable(true);
  LOG("[4] MCLK pin: %u level changes running, %u stopped: %s\n", on, off, PASS(on > 1000 && off == 0));

  // 5. Reset with MCLK running, then the ID answers (repeat to catch flaky contacts).
  int ok = 0, worst = 0;
  for (int i = 0; i < 5; i++) {
    rst_pulse();
    int t = poll_id(1000);
    ok += (t >= 0);
    worst = (t > worst) ? t : worst;
  }
  LOG("[5] RST pulse with MCLK on: %d / 5 read id 0x%04X (slowest %d ms): %s\n", ok, MODEL_ID, worst, PASS(ok == 5));

  // 6. RST really resets the sensor: I2C gone while low, written register back to default after.
  i2cWrite16(ADDR, TEST_REG, TEST_VAL);
  pin_drive(CAM_XSHUTDOWN, 0);
  nrf_delay_ms(10);
  bool gone = (read_id() != MODEL_ID);
  pin_drive(CAM_XSHUTDOWN, 1);
  bool back = (poll_id(1000) >= 0);
  uint8_t reg = i2cRead16(ADDR, TEST_REG);
  LOG("[6] RST: I2C off while low %s, register reset 0x%02X %s\n", PASS(gone), reg, PASS(back && reg != TEST_VAL));

  // 7. XSLEEP: I2C gone while low, back when high.
  pin_drive(CAM_XSLEEP, 0);
  nrf_delay_ms(10);
  gone = (read_id() != MODEL_ID);
  pin_drive(CAM_XSLEEP, 1);
  int t = poll_id(1000);
  LOG("[7] XSLEEP: I2C off while low %s, back after %d ms %s\n", PASS(gone), t, PASS(t >= 0));

  // 8. MCLK stopped and restarted (VueBuds clock gating): registers kept.
  i2cWrite16(ADDR, TEST_REG, TEST_VAL);
  capture_mclk_enable(false);
  nrf_delay_ms(200);
  capture_mclk_enable(true);
  t = poll_id(1000);
  reg = i2cRead16(ADDR, TEST_REG);
  LOG("[8] MCLK off 200 ms -> on: back after %d ms, register 0x%02X %s\n", t, reg, PASS(t >= 0 && reg == TEST_VAL));

  // 9. Monitor S2 wake-up: XSLEEP low, MCLK off, MCLK on, XSLEEP high; registers kept.
  i2cWrite16(ADDR, TEST_REG, TEST_VAL);
  pin_drive(CAM_XSLEEP, 0);
  nrf_delay_ms(1);
  capture_mclk_enable(false);
  nrf_delay_ms(500);
  capture_mclk_enable(true);
  nrf_delay_ms(1);
  pin_drive(CAM_XSLEEP, 1);
  t = poll_id(1000);
  reg = i2cRead16(ADDR, TEST_REG);
  LOG("[9] S2 sleep/wake: back after %d ms, register 0x%02X %s\n", t, reg, PASS(t >= 0 && reg == TEST_VAL));

  // 10. Sync and data lines: stream with power-on defaults (8-bit, full frame) and count edges.
  rst_pulse();
  poll_id(1000);
  i2cWrite16(ADDR, 0x0100, 0x01);
  nrf_delay_ms(300);
  uint32_t fvld = count_edges(CAM_FRAME_VALID, 1000);
  uint32_t hvld = count_edges(CAM_LINE_VALID, 1000);
  uint32_t pclk = count_edges(CAM_PCLK_OUT_TO_MCU, 1000);
  uint32_t d0 = count_edges(CAM_D0, 1000);
  i2cWrite16(ADDR, 0x0100, 0x00);
  // NRF_LOG takes at most 6 arguments.
  LOG("[10] edges in 1 s: FVLD %u %s, HVLD %u %s\n", fvld, PASS(fvld > 0), hvld, PASS(hvld > fvld));
  LOG("     PCLKO %u %s, D0 %u %s\n", pclk, PASS(pclk > 0), d0, PASS(d0 > 0));

  // Leave the sensor in reset and MCLK off; cameraInit() resets it with MCLK running later.
  pin_drive(CAM_XSHUTDOWN, 0);
  capture_mclk_enable(false);
  LOG("===== wiring test done =====\n");
}

#endif
