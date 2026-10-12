#include <string.h>

#include <zephyr/kernel.h>
#include <hal/nrf_gpio.h>
#include <hal/nrf_spim.h>

#include "nrf_log.h"
#include "psram.h"

#define PSRAM_CS   NRF_GPIO_PIN_MAP(1, 12)
#define PSRAM_MOSI NRF_GPIO_PIN_MAP(1, 13)
#define PSRAM_MISO NRF_GPIO_PIN_MAP(1, 14)
#define PSRAM_SCK  NRF_GPIO_PIN_MAP(1, 15)
#define PSRAM_PAGE 1024u

#define CMD_READ     0x03
#define CMD_WRITE    0x02
#define CMD_READ_ID  0x9F
#define CMD_RESET_EN 0x66
#define CMD_RESET    0x99

static bool m_present;

static void output(uint32_t pin, bool inputConnected)
{
  nrf_gpio_cfg(pin, NRF_GPIO_PIN_DIR_OUTPUT,
               inputConnected ? NRF_GPIO_PIN_INPUT_CONNECT : NRF_GPIO_PIN_INPUT_DISCONNECT,
               NRF_GPIO_PIN_NOPULL, NRF_GPIO_PIN_H0H1, NRF_GPIO_PIN_NOSENSE);
}

/* One SPIM transfer; CS is handled by the caller (a command and its data are two transfers). */
static void xfer(const uint8_t *tx, size_t txLen, uint8_t *rx, size_t rxLen)
{
  NRF_SPIM_Type *s = NRF_SPIM4;
  nrf_spim_tx_buffer_set(s, tx, txLen);
  nrf_spim_rx_buffer_set(s, rx, rxLen);
  nrf_spim_event_clear(s, NRF_SPIM_EVENT_END);
  nrf_spim_task_trigger(s, NRF_SPIM_TASK_START);
  while (!nrf_spim_event_check(s, NRF_SPIM_EVENT_END)) {
  }
}

static void command(uint8_t cmd, uint32_t address, uint8_t out[4])
{
  out[0] = cmd;
  out[1] = address >> 16;
  out[2] = address >> 8;
  out[3] = address;
}

bool psramInit(void)
{
  NRF_SPIM_Type *s = NRF_SPIM4;
  nrf_gpio_pin_set(PSRAM_CS);
  output(PSRAM_CS, false);
  nrf_gpio_pin_clear(PSRAM_SCK);
  output(PSRAM_SCK, true);
  output(PSRAM_MOSI, false);
  nrf_gpio_cfg_input(PSRAM_MISO, NRF_GPIO_PIN_PULLDOWN);  // reads 0 without a chip
  nrf_spim_pins_set(s, PSRAM_SCK, PSRAM_MOSI, PSRAM_MISO);
  nrf_spim_frequency_set(s, NRF_SPIM_FREQ_8M);
  nrf_spim_configure(s, NRF_SPIM_MODE_0, NRF_SPIM_BIT_ORDER_MSB_FIRST);
  nrf_spim_orc_set(s, 0x00);
  nrf_spim_enable(s);

  static uint8_t buf[12];
  buf[0] = CMD_RESET_EN;
  nrf_gpio_pin_clear(PSRAM_CS);
  xfer(buf, 1, NULL, 0);
  nrf_gpio_pin_set(PSRAM_CS);
  buf[0] = CMD_RESET;
  nrf_gpio_pin_clear(PSRAM_CS);
  xfer(buf, 1, NULL, 0);
  nrf_gpio_pin_set(PSRAM_CS);
  k_busy_wait(100);

  command(CMD_READ_ID, 0, buf);
  nrf_gpio_pin_clear(PSRAM_CS);
  xfer(buf, 4, buf + 4, 8);  // 4 bytes during the command, then MF, KGD, EID
  nrf_gpio_pin_set(PSRAM_CS);
  m_present = buf[8] == 0x0D && buf[9] == 0x5D;
  NRF_LOG_RAW_INFO("[psram] ID %02x %02x: %s\n", buf[8], buf[9], m_present ? "APS6404L 8 MB" : "not found");
  if (!m_present) {
    nrf_spim_disable(s);
  }
  return m_present;
}

bool psramPresent(void)
{
  return m_present;
}

bool psramWrite(uint32_t address, const void *data, size_t len)
{
  if (!m_present || address + len > PSRAM_SIZE) {
    return false;
  }
  static uint8_t cmd[4];
  const uint8_t *p = data;
  while (len > 0) {
    size_t n = MIN(len, PSRAM_PAGE - address % PSRAM_PAGE);  // a write burst stays inside one page
    command(CMD_WRITE, address, cmd);
    nrf_gpio_pin_clear(PSRAM_CS);
    xfer(cmd, 4, NULL, 0);
    xfer(p, n, NULL, 0);
    nrf_gpio_pin_set(PSRAM_CS);
    address += n;
    p += n;
    len -= n;
  }
  return true;
}

bool psramRead(uint32_t address, void *data, size_t len)
{
  if (!m_present || address + len > PSRAM_SIZE) {
    return false;
  }
  static uint8_t cmd[4];
  uint8_t *p = data;
  while (len > 0) {
    size_t n = MIN(len, PSRAM_PAGE - address % PSRAM_PAGE);
    command(CMD_READ, address, cmd);
    nrf_gpio_pin_clear(PSRAM_CS);
    xfer(cmd, 4, NULL, 0);
    xfer(NULL, 0, p, n);
    nrf_gpio_pin_set(PSRAM_CS);
    address += n;
    p += n;
    len -= n;
  }
  return true;
}
