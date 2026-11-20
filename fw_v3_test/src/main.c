/*
 * balsamic3 (V3) bring-up test. Prints PASS / FAIL per item on RTT, then keeps reporting the
 * POWER button (S1, PMIC nEN) as seen by the PMIC.
 *
 * Pins and parts follow kicad-agent-flow/boards/balsamic3/board.yaml:
 *   I2C (TWIM1)  P0.26 SDA, P0.27 SCL: MAX77650 at 0x48, HM0360 at 0x24
 *   PSRAM        APS6404L on SPIM4 (1-bit SPI, 8 MHz): CS P0.18, SCK P0.17, SIO0 P0.13 (MOSI),
 *                SIO1 P0.14 (MISO), SIO2 P0.15 / SIO3 P0.16 held low (as on the DK test)
 *   Camera       XSHUTDN P1.05 (10 k pull-down), MCLK P1.08 (8 MHz: TIMER1 + DPPI + GPIOTE)
 *   LEDs         D1 (green) on PMIC LED0, D2 (red) on PMIC LED1
 *
 * Register values come from the MAX77650/51 programmer's guide: STAT_GLBL 0x05 (DIDM [7:6],
 * STAT_EN [2] = nEN low, STAT_PWR_HLD [1]), CID 0x11 [3:0], CNFG_SBB2_A 0x2D (IP [7:6],
 * TV_SBB2 [5:0] = 0.80 V + 50 mV * code on the MAX77650: 0x28 = 2.80 V, 0x32 = 3.30 V),
 * CNFG_LEDx_A (FS [7:6], INV [5], BRT [4:0]), CNFG_LEDx_B (period [7:4], duty [3:0]),
 * CNFG_LED_TOP 0x46 (EN_LED_MSTR [0]).
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_gpio.h>
#include <hal/nrf_reset.h>
#include <hal/nrf_spim.h>

#define PMIC_ADDR 0x48
#define CAM_ADDR  0x24

#define PMIC_ERCFLAG      0x04
#define PMIC_STAT_GLBL    0x05
#define PMIC_CNFG_GLBL    0x10
#define PMIC_CID          0x11
#define PMIC_STAT_CHG_B   0x03
#define PMIC_CNFG_SBB0_A  0x29
#define PMIC_CNFG_SBB1_A  0x2B
#define PMIC_CNFG_SBB2_A  0x2D
#define PMIC_CNFG_LDO_A   0x38
#define PMIC_CNFG_LED0_A  0x40
#define PMIC_CNFG_LED1_A  0x41
#define PMIC_CNFG_LED0_B  0x43
#define PMIC_CNFG_LED1_B  0x44
#define PMIC_CNFG_LED_TOP 0x46

#define TV_SBB2_2V8 0x28

#define PSRAM_CS   NRF_GPIO_PIN_MAP(0, 18)
#define PSRAM_SCK  NRF_GPIO_PIN_MAP(0, 17)
#define PSRAM_SIO0 NRF_GPIO_PIN_MAP(0, 13)
#define PSRAM_SIO1 NRF_GPIO_PIN_MAP(0, 14)
#define PSRAM_SIO2 NRF_GPIO_PIN_MAP(0, 15)
#define PSRAM_SIO3 NRF_GPIO_PIN_MAP(0, 16)
#define PSRAM_SIZE (8u * 1024 * 1024)
#define PSRAM_PAGE 1024u

#define CAM_XSHUTDN NRF_GPIO_PIN_MAP(1, 5)
#define CAM_MCLK    NRF_GPIO_PIN_MAP(1, 8)

static const struct device *const i2c = DEVICE_DT_GET(DT_NODELABEL(i2c1));
static int failures;
static uint8_t sbb2AtBoot;
static int sbb2Err = -1;

static void result(const char *item, bool pass, const char *fmt, ...)
{
  va_list ap;
  printk("[test] %-8s %s  ", item, pass ? "PASS" : "FAIL");
  va_start(ap, fmt);
  vprintk(fmt, ap);
  va_end(ap);
  printk("\n");
  if (!pass) {
    failures++;
  }
}

static int pmicRead(uint8_t reg, uint8_t *val)
{
  return i2c_reg_read_byte(i2c, PMIC_ADDR, reg, val);
}

static int pmicWrite(uint8_t reg, uint8_t val)
{
  return i2c_reg_write_byte(i2c, PMIC_ADDR, reg, val);
}

/* ---- 1. reset reason (S2 RESET shows up as the reset pin) ---- */
static void testResetReason(void)
{
  uint32_t r = nrf_reset_resetreas_get(NRF_RESET);
  nrf_reset_resetreas_clear(NRF_RESET, r);
  printk("[test] reset reason 0x%08x%s%s\n", r, (r & NRF_RESET_RESETREAS_RESETPIN_MASK) ? " (RESET pin / S2)" : "",
         r == 0 ? " (power-on)" : "");
}

/* ---- 2. I2C scan ---- */
static void testI2cScan(bool *camPresent)
{
  int found = 0;
  bool pmic = false;
  *camPresent = false;
  for (uint8_t a = 0x08; a < 0x78; a++) {
    uint8_t b;
    if (i2c_read(i2c, &b, 1, a) == 0) {
      printk("[test] i2c device at 0x%02x\n", a);
      found++;
      pmic |= a == PMIC_ADDR;
      *camPresent |= a == CAM_ADDR;
    }
  }
  result("i2c", pmic, "%d device(s); PMIC 0x48 %s", found, pmic ? "found" : "MISSING");
}

/* ---- 3. PMIC identity, status, rails; SBB2 -> 2.8 V ---- */
static void testPmic(void)
{
  uint8_t stat, cid, glbl, erc, chgB, sbb0, sbb1, sbb2, ldo;
  if (pmicRead(PMIC_STAT_GLBL, &stat) || pmicRead(PMIC_CID, &cid) || pmicRead(PMIC_CNFG_GLBL, &glbl) ||
      pmicRead(PMIC_ERCFLAG, &erc) || pmicRead(PMIC_STAT_CHG_B, &chgB) || pmicRead(PMIC_CNFG_SBB0_A, &sbb0) ||
      pmicRead(PMIC_CNFG_SBB1_A, &sbb1) || pmicRead(PMIC_CNFG_SBB2_A, &sbb2) || pmicRead(PMIC_CNFG_LDO_A, &ldo)) {
    result("pmic", false, "I2C read failed");
    return;
  }
  uint8_t didm = stat >> 6;
  printk("[test] pmic STAT_GLBL=0x%02x (DIDM=%u %s, nEN %s, PWR_HLD %s) CID=0x%x CNFG_GLBL=0x%02x ERCFLAG=0x%02x\n",
         stat, didm, didm == 0 ? "MAX77650" : "not MAX77650", (stat & 0x04) ? "LOW (pressed/held)" : "high",
         (stat & 0x02) ? "high" : "low", cid & 0x0f, glbl, erc);
  printk("[test] pmic STAT_CHG_B=0x%02x (CHGIN_DTLS=%u) raw TV: SBB0_A=0x%02x SBB1_A=0x%02x LDO_A=0x%02x\n", chgB,
         (chgB >> 2) & 3, sbb0, sbb1, ldo);
  printk("[test] pmic SBB2 at boot: 0x%02x = %u mV (set to 2.8 V at the start of main)\n", sbb2AtBoot,
         800 + 50 * (sbb2AtBoot & 0x3f));
  result("pmic", didm == 0, "MAX77650 answers (DIDM=%u)", didm);
  result("sbb2", sbb2Err == 0 && (sbb2 & 0x3f) == TV_SBB2_2V8,
         "+2V8 is %u mV (reg 0x%02x). Measure TP26: expect ~2.80 V", 800 + 50 * (sbb2 & 0x3f), sbb2);
}

/* ---- 4. LEDs: D1 green and D2 red blink in turn, 1 s period, 3.2 mA full scale ---- */
static void testLeds(void)
{
  int err = 0;
  err |= pmicWrite(PMIC_CNFG_LED0_A, 0x40 | 0x1f);         // 3.2 mA FS, in phase, full brightness
  err |= pmicWrite(PMIC_CNFG_LED1_A, 0x40 | 0x20 | 0x1f);  // 3.2 mA FS, inverted phase
  err |= pmicWrite(PMIC_CNFG_LED0_B, 0x17);                // 1.0 s period, 50 % duty
  err |= pmicWrite(PMIC_CNFG_LED1_B, 0x17);
  err |= pmicWrite(PMIC_CNFG_LED_TOP, 0x01);               // EN_LED_MSTR
  result("leds", err == 0, "D1 green and D2 red should blink in turn (check by eye)");
}

/* ---- 5. PSRAM over SPIM4 ---- */
static void spimXfer(const uint8_t *tx, size_t txLen, uint8_t *rx, size_t rxLen)
{
  NRF_SPIM_Type *s = NRF_SPIM4;
  nrf_spim_tx_buffer_set(s, tx, txLen);
  nrf_spim_rx_buffer_set(s, rx, rxLen);
  nrf_spim_event_clear(s, NRF_SPIM_EVENT_END);
  nrf_spim_task_trigger(s, NRF_SPIM_TASK_START);
  while (!nrf_spim_event_check(s, NRF_SPIM_EVENT_END)) {
  }
}

static void psramCmd(uint8_t cmd, uint32_t addr, uint8_t *out)
{
  out[0] = cmd;
  out[1] = addr >> 16;
  out[2] = addr >> 8;
  out[3] = addr;
}

static uint8_t pattern(uint32_t addr)
{
  uint32_t x = addr * 2654435761u;
  return (uint8_t)((x >> 24) ^ addr);
}

static void testPsram(void)
{
  static uint8_t cmd[4];
  static uint8_t buf[PSRAM_PAGE];
  static uint8_t id[12];
  NRF_SPIM_Type *s = NRF_SPIM4;

  nrf_gpio_pin_set(PSRAM_CS);
  nrf_gpio_cfg(PSRAM_CS, NRF_GPIO_PIN_DIR_OUTPUT, NRF_GPIO_PIN_INPUT_DISCONNECT, NRF_GPIO_PIN_NOPULL,
               NRF_GPIO_PIN_H0H1, NRF_GPIO_PIN_NOSENSE);
  nrf_gpio_pin_clear(PSRAM_SCK);
  nrf_gpio_cfg(PSRAM_SCK, NRF_GPIO_PIN_DIR_OUTPUT, NRF_GPIO_PIN_INPUT_CONNECT, NRF_GPIO_PIN_NOPULL,
               NRF_GPIO_PIN_H0H1, NRF_GPIO_PIN_NOSENSE);
  nrf_gpio_cfg(PSRAM_SIO0, NRF_GPIO_PIN_DIR_OUTPUT, NRF_GPIO_PIN_INPUT_DISCONNECT, NRF_GPIO_PIN_NOPULL,
               NRF_GPIO_PIN_H0H1, NRF_GPIO_PIN_NOSENSE);
  nrf_gpio_cfg_input(PSRAM_SIO1, NRF_GPIO_PIN_PULLDOWN);  // reads 0 if nothing drives it
  nrf_gpio_pin_clear(PSRAM_SIO2);
  nrf_gpio_cfg_output(PSRAM_SIO2);
  nrf_gpio_pin_clear(PSRAM_SIO3);
  nrf_gpio_cfg_output(PSRAM_SIO3);
  nrf_spim_pins_set(s, PSRAM_SCK, PSRAM_SIO0, PSRAM_SIO1);
  nrf_spim_frequency_set(s, NRF_SPIM_FREQ_8M);
  nrf_spim_configure(s, NRF_SPIM_MODE_0, NRF_SPIM_BIT_ORDER_MSB_FIRST);
  nrf_spim_orc_set(s, 0x00);
  nrf_spim_enable(s);

  cmd[0] = 0x66;  // reset enable, reset
  nrf_gpio_pin_clear(PSRAM_CS);
  spimXfer(cmd, 1, NULL, 0);
  nrf_gpio_pin_set(PSRAM_CS);
  cmd[0] = 0x99;
  nrf_gpio_pin_clear(PSRAM_CS);
  spimXfer(cmd, 1, NULL, 0);
  nrf_gpio_pin_set(PSRAM_CS);
  k_busy_wait(100);

  psramCmd(0x9f, 0, cmd);  // read ID: MF 0x0D, KGD 0x5D
  nrf_gpio_pin_clear(PSRAM_CS);
  spimXfer(cmd, 4, id, 12);
  nrf_gpio_pin_set(PSRAM_CS);
  bool idOk = id[4] == 0x0d && id[5] == 0x5d;
  result("psram-id", idOk, "MF=0x%02x KGD=0x%02x (expect 0x0d 0x5d)", id[4], id[5]);
  if (!idOk) {
    return;
  }

  printk("[test] psram writing 8 MB...\n");
  int64_t t0 = k_uptime_get();
  for (uint32_t a = 0; a < PSRAM_SIZE; a += PSRAM_PAGE) {
    for (uint32_t i = 0; i < PSRAM_PAGE; i++) {
      buf[i] = pattern(a + i);
    }
    psramCmd(0x02, a, cmd);
    nrf_gpio_pin_clear(PSRAM_CS);
    spimXfer(cmd, 4, NULL, 0);
    spimXfer(buf, PSRAM_PAGE, NULL, 0);
    nrf_gpio_pin_set(PSRAM_CS);
  }
  printk("[test] psram reading back...\n");
  uint32_t errors = 0;
  for (uint32_t a = 0; a < PSRAM_SIZE; a += PSRAM_PAGE) {
    psramCmd(0x03, a, cmd);
    nrf_gpio_pin_clear(PSRAM_CS);
    spimXfer(cmd, 4, NULL, 0);
    spimXfer(NULL, 0, buf, PSRAM_PAGE);
    nrf_gpio_pin_set(PSRAM_CS);
    for (uint32_t i = 0; i < PSRAM_PAGE; i++) {
      if (buf[i] != pattern(a + i)) {
        if (errors < 5) {
          printk("[test] psram mismatch at 0x%06x: 0x%02x != 0x%02x\n", a + i, buf[i], pattern(a + i));
        }
        errors++;
      }
    }
  }
  result("psram", errors == 0, "8 MB write/read, %u byte errors, %lld ms", errors, k_uptime_get() - t0);
}

/* ---- 6. camera: only if a module is plugged in; +2V8 must already be 2.8 V ---- */
/* 8 MHz MCLK as in fw_nrf5340/src/capture/capture_mclk.c (verified on the DK): TIMER1 at
 * 16 MHz with CC[0] = 1 and COMPARE0_CLEAR fires every tick, DPPI toggles P1.08 through a
 * GPIOTE task. (PWM cannot do this: COUNTERTOP must be >= 3.) */
#define MCLK_GPIOTE_CH 7
#define MCLK_DPPI_CH   0

static void mclkStart(void)
{
  nrf_gpio_pin_clear(CAM_MCLK);
  nrf_gpio_cfg(CAM_MCLK, NRF_GPIO_PIN_DIR_OUTPUT, NRF_GPIO_PIN_INPUT_DISCONNECT, NRF_GPIO_PIN_NOPULL,
               NRF_GPIO_PIN_H0H1, NRF_GPIO_PIN_NOSENSE);
  NRF_TIMER1->TASKS_STOP = 1;
  NRF_TIMER1->MODE = TIMER_MODE_MODE_Timer;
  NRF_TIMER1->BITMODE = TIMER_BITMODE_BITMODE_16Bit;
  NRF_TIMER1->PRESCALER = 0;
  NRF_TIMER1->CC[0] = 1;
  NRF_TIMER1->SHORTS = TIMER_SHORTS_COMPARE0_CLEAR_Msk;
  NRF_TIMER1->TASKS_CLEAR = 1;
  NRF_GPIOTE0->CONFIG[MCLK_GPIOTE_CH] =
      (GPIOTE_CONFIG_MODE_Task << GPIOTE_CONFIG_MODE_Pos) | (8 << GPIOTE_CONFIG_PSEL_Pos) |
      (1 << GPIOTE_CONFIG_PORT_Pos) | (GPIOTE_CONFIG_POLARITY_Toggle << GPIOTE_CONFIG_POLARITY_Pos) |
      (GPIOTE_CONFIG_OUTINIT_Low << GPIOTE_CONFIG_OUTINIT_Pos);
  NRF_TIMER1->PUBLISH_COMPARE[0] = MCLK_DPPI_CH | TIMER_PUBLISH_COMPARE_EN_Msk;
  NRF_GPIOTE0->SUBSCRIBE_OUT[MCLK_GPIOTE_CH] = MCLK_DPPI_CH | GPIOTE_SUBSCRIBE_OUT_EN_Msk;
  NRF_DPPIC->CHENSET = BIT(MCLK_DPPI_CH);
  NRF_TIMER1->TASKS_START = 1;
}

/* Register address and read as two transfers with a STOP in between, like fw/'s i2cRead16. */
static int camRead(uint16_t reg, uint8_t *val)
{
  uint8_t r[2] = {reg >> 8, reg & 0xff};
  int err = i2c_write(i2c, r, 2, CAM_ADDR);
  return err ? err : i2c_read(i2c, val, 1, CAM_ADDR);
}

/* Order from fw_nrf5340/src/camera.c: MCLK first (the HM0360 does not answer I2C without it),
 * 100 ms, then XSHUTDOWN low 10 ms and high; XSLEEP is pulled up on the board (awake). */
static void testCamera(void)
{
  nrf_gpio_pin_clear(CAM_XSHUTDN);
  nrf_gpio_cfg_output(CAM_XSHUTDN);
  mclkStart();
  k_msleep(100);
  nrf_gpio_pin_clear(CAM_XSHUTDN);
  k_msleep(10);
  nrf_gpio_pin_set(CAM_XSHUTDN);  // released only after +2V8 is at 2.8 V
  k_msleep(5);
  uint8_t hi = 0, lo = 0;
  int err = camRead(0x0000, &hi) | camRead(0x0001, &lo);
  if (err) {
    printk("[test] camera  SKIP  no answer at 0x24 (module not plugged in?)\n");
    return;
  }
  result("camera", hi == 0x03 && lo == 0x60, "HM0360 model ID 0x%02x%02x (expect 0x0360)", hi, lo);
}

/* After a PMIC power-up SBB2 (+2V8, camera AVDD) is 3.3 V, above the HM0360 AVDD range
 * (2.6-3.0 V). Lower it first, before anything else; XSHUTDN stays low (10 k pull-down). */
static void sbb2To2v8First(void)
{
  if (!device_is_ready(i2c)) {
    return;
  }
  sbb2Err = pmicRead(PMIC_CNFG_SBB2_A, &sbb2AtBoot);
  if (sbb2Err == 0) {
    sbb2Err = pmicWrite(PMIC_CNFG_SBB2_A, (sbb2AtBoot & 0xc0) | TV_SBB2_2V8);
  }
}

int main(void)
{
  sbb2To2v8First();
  k_msleep(200);  // let the RTT host attach
  printk("\n===== balsamic3 hardware test =====\n");
  testResetReason();
  if (!device_is_ready(i2c)) {
    printk("[test] i2c1 not ready\n");
    return 0;
  }
  bool camPresent;
  testI2cScan(&camPresent);
  testPmic();
  testLeds();
  testPsram();
  testCamera();
  printk("===== %s: %d failure(s) =====\n", failures ? "FAILED" : "ALL PASS", failures);
  printk("[test] Press S1 (POWER) briefly: the PMIC's debounced nEN state is printed on change.\n");
  printk("[test] Do not hold S1 for ~16 s: the PMIC then turns the board off.\n");

  uint8_t last = 0xff;
  int64_t lastBeat = 0;
  while (1) {
    uint8_t stat;
    if (pmicRead(PMIC_STAT_GLBL, &stat) == 0) {
      uint8_t en = (stat >> 2) & 1;
      if (en != last) {
        printk("[test] S1/nEN: %s (STAT_GLBL=0x%02x)\n", en ? "LOW = pressed" : "high = released", stat);
        last = en;
      }
    }
    if (k_uptime_get() - lastBeat > 10000) {
      lastBeat = k_uptime_get();
      printk("[test] alive %lld s\n", lastBeat / 1000);
    }
    k_msleep(50);
  }
  return 0;
}
