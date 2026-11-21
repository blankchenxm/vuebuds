#include <string.h>

#include <zephyr/kernel.h>
#include <hal/nrf_clock.h>
#include <hal/nrf_gpio.h>
#include <hal/nrf_qspi.h>
#include <nrfx_qspi.h>
#include <mdk/nrf_erratas.h>

#include "nrf_log.h"
#include "psram.h"

#define PSRAM_CSN NRF_GPIO_PIN_MAP(0, 18)
#define PSRAM_SCK NRF_GPIO_PIN_MAP(0, 17)
#define PSRAM_IO0 NRF_GPIO_PIN_MAP(0, 13)
#define PSRAM_IO1 NRF_GPIO_PIN_MAP(0, 14)
#define PSRAM_IO2 NRF_GPIO_PIN_MAP(0, 15)
#define PSRAM_IO3 NRF_GPIO_PIN_MAP(0, 16)

#define CMD_READ_ID  0x9F
#define CMD_RESET_EN 0x66
#define CMD_RESET    0x99

#define XFER_TIMEOUT_LOOPS 100000  // a 1 KB burst at 6 MHz single line is ~1.4 ms

static bool m_present, m_inited, m_reset;
static psram_config_t m_config = {PSRAM_DEFAULT_QUAD, PSRAM_DEFAULT_MHZ, PSRAM_DEFAULT_BURST};
static uint32_t m_bounce[(PSRAM_PAGE + 8) / 4];

/*
 * The QSPI writes in "page programs": WREN (0x06), the write, then RDSR (0x05) polled until
 * WIP (bit 0) is 0. The PSRAM ignores 0x06 / 0x05 and leaves SIO1 floating, so it gets a
 * pull-down: the status then reads 0 and the write ends at once.
 */
static void io1PullDown(void)
{
  NRF_P0->PIN_CNF[14] = (NRF_P0->PIN_CNF[14] & ~GPIO_PIN_CNF_PULL_Msk) |
                        (GPIO_PIN_CNF_PULL_Pulldown << GPIO_PIN_CNF_PULL_Pos);
}

/* Errata 159: QSPI transfers need HFCLK / 2 (64 MHz CPU); CPU_128MHZ builds drop to it meanwhile. */
static nrf_clock_hfclk_div_t clockEnter(void)
{
  nrf_clock_hfclk_div_t div = nrf_clock_hfclk_div_get(NRF_CLOCK);
  if (nrf53_errata_159() && div != NRF_CLOCK_HFCLK_DIV_2) {
    nrf_clock_hfclk_div_set(NRF_CLOCK, NRF_CLOCK_HFCLK_DIV_2);
  }
  return div;
}

static void clockLeave(nrf_clock_hfclk_div_t div)
{
  if (nrf_clock_hfclk_div_get(NRF_CLOCK) != div) {
    nrf_clock_hfclk_div_set(NRF_CLOCK, div);
  }
}

static int cinstr(uint8_t opcode, nrf_qspi_cinstr_len_t length, const void *tx, void *rx)
{
  nrf_qspi_cinstr_conf_t c = NRFX_QSPI_DEFAULT_CINSTR(opcode, length);
  nrf_clock_hfclk_div_t div = clockEnter();
  int err = nrfx_qspi_cinstr_xfer(&c, tx, rx);
  clockLeave(div);
  return err;
}

void psramReadId(uint8_t id[8])
{
  // 0x9F, 24-bit address, then MF, KGD, EID[6]: 1 + 8 bytes, the first 3 received during the address.
  uint8_t tx[8] = {0}, rx[8] = {0};
  cinstr(CMD_READ_ID, NRF_QSPI_CINSTR_LEN_9B, tx, rx);
  memset(id, 0, 8);
  memcpy(id, rx + 3, 5);
}

/*
 * Recovery when the ID does not read back (seen after quad tests that went wrong): the chip may
 * sit in QPI mode, where commands are 4 bits wide, which the QSPI cannot send (its opcode is
 * always on one line). Bit-bang on GPIO: QPI exit (0xF5) and reset in QPI, then reset in SPI.
 */
static void bbNibble(uint8_t v)
{
  nrf_gpio_pin_write(PSRAM_IO0, v & 1);
  nrf_gpio_pin_write(PSRAM_IO1, (v >> 1) & 1);
  nrf_gpio_pin_write(PSRAM_IO2, (v >> 2) & 1);
  nrf_gpio_pin_write(PSRAM_IO3, (v >> 3) & 1);
  k_busy_wait(1);
  nrf_gpio_pin_set(PSRAM_SCK);
  k_busy_wait(1);
  nrf_gpio_pin_clear(PSRAM_SCK);
}

static void bbCommand(uint8_t cmd, bool qpi)
{
  nrf_gpio_pin_clear(PSRAM_CSN);
  k_busy_wait(1);
  if (qpi) {
    bbNibble(cmd >> 4);
    bbNibble(cmd & 0xF);
  } else {
    for (int b = 7; b >= 0; b--) {
      bbNibble((cmd >> b) & 1);  // SPI: SI = SIO0, the other lines low
    }
  }
  k_busy_wait(1);
  nrf_gpio_pin_set(PSRAM_CSN);
  k_busy_wait(1);
}

static void bbRecover(void)
{
  static const uint32_t pins[] = {PSRAM_CSN, PSRAM_SCK, PSRAM_IO0, PSRAM_IO1, PSRAM_IO2, PSRAM_IO3};
  nrf_gpio_pin_set(PSRAM_CSN);
  nrf_gpio_pin_clear(PSRAM_SCK);
  for (int i = 0; i < ARRAY_SIZE(pins); i++) {
    nrf_gpio_cfg(pins[i], NRF_GPIO_PIN_DIR_OUTPUT, NRF_GPIO_PIN_INPUT_DISCONNECT, NRF_GPIO_PIN_NOPULL,
                 NRF_GPIO_PIN_H0H1, NRF_GPIO_PIN_NOSENSE);
  }
  bbCommand(0xF5, true);
  bbCommand(CMD_RESET_EN, true);
  bbCommand(CMD_RESET, true);
  k_busy_wait(100);
  bbCommand(CMD_RESET_EN, false);
  bbCommand(CMD_RESET, false);
  k_busy_wait(100);
}

static bool qspiStart(const psram_config_t *config)
{
  if (m_inited) {
    nrfx_qspi_uninit();
    m_inited = false;
  }
  // QSPI base clock: HFCLK192M / 1 = 192 MHz, / 2 inside the QSPI = 96 MHz, SCK = 96 / (SCKFREQ + 1).
  nrf_clock_task_trigger(NRF_CLOCK, NRF_CLOCK_TASK_HFCLK192MSTART);
  nrf_clock_hfclk192m_div_set(NRF_CLOCK, NRF_CLOCK_HFCLK_DIV_1);
  k_busy_wait(10);
  uint32_t k = DIV_ROUND_UP(96u, MAX(config->sck_mhz, 6u)) - 1;

  nrfx_qspi_config_t c = NRFX_QSPI_DEFAULT_CONFIG(PSRAM_SCK, PSRAM_CSN, PSRAM_IO0, PSRAM_IO1, PSRAM_IO2, PSRAM_IO3);
  c.prot_if.readoc = config->quad ? NRF_QSPI_READOC_READ4IO : NRF_QSPI_READOC_FASTREAD;
  c.prot_if.writeoc = config->quad ? NRF_QSPI_WRITEOC_PP4IO : NRF_QSPI_WRITEOC_PP;
  c.phy_if.sck_delay = 1;  // CE# high between commands >= 62.5 ns (tCPH 18 ns)
  c.phy_if.sck_freq = (nrf_qspi_frequency_t)MIN(k, 15u);
  c.timeout = 10;  // ms
  if (nrfx_qspi_init(&c, NULL, NULL) != 0) {
    NRF_LOG_RAW_INFO("[psram] QSPI init failed\n");
    return false;
  }
  m_inited = true;
  io1PullDown();
  nrf_clock_hfclk_div_t div = clockEnter();
  int err = nrfx_qspi_activate(true);
  clockLeave(div);
  if (err != 0) {
    NRF_LOG_RAW_INFO("[psram] QSPI activate failed (%d)\n", err);
    return false;
  }
  return true;
}

static bool idOk(void)
{
  uint8_t id[8];
  psramReadId(id);
  return id[0] == 0x0D && id[1] == 0x5D;
}

bool psramConfigure(const psram_config_t *config)
{
  m_present = false;
  m_config = *config;
  if (!qspiStart(config)) {
    return false;
  }
  if (!m_reset) {  // once: a reset may lose the contents (hw_test switches modes over the same data)
    cinstr(CMD_RESET_EN, NRF_QSPI_CINSTR_LEN_1B, NULL, NULL);
    cinstr(CMD_RESET, NRF_QSPI_CINSTR_LEN_1B, NULL, NULL);
    k_busy_wait(100);
    m_reset = true;
  }
  m_present = idOk();
  if (!m_present) {
    nrfx_qspi_uninit();
    m_inited = false;
    bbRecover();
    m_present = qspiStart(config) && idOk();
    NRF_LOG_RAW_INFO("[psram] ID did not read back; QPI exit + reset: %s\n", m_present ? "recovered" : "still no ID");
  }
  return m_present;
}

bool psramInit(void)
{
  psram_config_t c = {PSRAM_DEFAULT_QUAD, PSRAM_DEFAULT_MHZ, PSRAM_DEFAULT_BURST};
  bool ok = psramConfigure(&c);
  uint8_t id[8];
  psramReadId(id);
  NRF_LOG_RAW_INFO("[psram] QSPI %s %u MHz, %u B bursts, ID %02x %02x: %s\n", c.quad ? "quad" : "single line", 96 / (DIV_ROUND_UP(96u, c.sck_mhz)), c.burst,
                   id[0], id[1], ok ? "APS6404L 8 MB" : "not found");
  return ok;
}

bool psramPresent(void)
{
  return m_present;
}

const psram_config_t *psramConfig(void)
{
  return &m_config;
}

bool psramXfer(bool write, uint32_t address, void *buf, uint32_t len)
{
  NRF_QSPI_Type *q = NRF_QSPI;
  if (write) {
    nrf_qspi_write_buffer_set(q, buf, len, address);
  } else {
    nrf_qspi_read_buffer_set(q, buf, len, address);
  }
  nrf_clock_hfclk_div_t div = clockEnter();
  nrf_qspi_event_clear(q, NRF_QSPI_EVENT_READY);
  nrf_qspi_task_trigger(q, write ? NRF_QSPI_TASK_WRITESTART : NRF_QSPI_TASK_READSTART);
  bool ok = false;
  for (int i = 0; i < XFER_TIMEOUT_LOOPS; i++) {
    if (nrf_qspi_event_check(q, NRF_QSPI_EVENT_READY)) {
      ok = true;
      break;
    }
  }
  clockLeave(div);
  return ok;
}

static bool inRam(const void *p)
{
  return ((uintptr_t)p & 0xE0000000u) == 0x20000000u;
}

/*
 * Bursts on a grid of m_config.burst bytes (so never across a 1 KB page). Inside a burst the
 * QSPI needs a word-aligned address and length: an aligned RAM buffer goes straight to EasyDMA,
 * anything else through the bounce buffer (writes with unaligned edges read those words first).
 */
static bool access(bool write, uint32_t address, uint8_t *p, size_t len)
{
  if (!m_present || address + len > PSRAM_SIZE) {
    return false;
  }
  uint32_t burst = m_config.burst;
  while (len > 0) {
    uint32_t end = MIN(address + len, (address & ~(burst - 1)) + burst);
    uint32_t n = end - address;
    uint32_t lo = address & ~3u, hi = (end + 3) & ~3u;
    if (lo == address && hi == end && ((uintptr_t)p & 3) == 0 && inRam(p)) {
      if (!psramXfer(write, address, p, n)) {
        return false;
      }
    } else {
      uint8_t *b = (uint8_t *)m_bounce;
      if (write) {
        if ((lo != address || hi != end) && !psramXfer(false, lo, b, hi - lo)) {
          return false;
        }
        memcpy(b + (address - lo), p, n);
        if (!psramXfer(true, lo, b, hi - lo)) {
          return false;
        }
      } else {
        if (!psramXfer(false, lo, b, hi - lo)) {
          return false;
        }
        memcpy(p, b + (address - lo), n);
      }
    }
    address += n;
    p += n;
    len -= n;
  }
  return true;
}

bool psramWrite(uint32_t address, const void *data, size_t len)
{
  return access(true, address, (uint8_t *)data, len);
}

bool psramRead(uint32_t address, void *data, size_t len)
{
  return access(false, address, data, len);
}

/* ---------------- boot check ---------------- */

#define CHECK_BLOCK 1024u
static const uint32_t m_check_at[] = {0x000000, 0x2AB400, 0x555800, PSRAM_SIZE - CHECK_BLOCK};  // spread over A0-A22
static psram_state_t m_state;
static int64_t m_check_t0;
static uint32_t m_check_bad[2];  // bad bytes at once / after the wait

static uint8_t checkByte(uint32_t address)
{
  uint32_t x = address * 0x9E3779B1u + 0x5A17u;  // differs per address, so aliased addresses show up
  x ^= x >> 15;
  x *= 0x2C1B3C6Du;
  x ^= x >> 12;
  return (uint8_t)x;
}

static uint32_t checkRead(void)
{
  uint8_t *b = (uint8_t *)m_bounce;  // psramRead takes RAM buffers directly, so the bounce buffer is free here
  uint32_t bad = 0;
  for (int i = 0; i < ARRAY_SIZE(m_check_at); i++) {
    if (!psramRead(m_check_at[i], b, CHECK_BLOCK)) {
      return UINT32_MAX;
    }
    for (uint32_t j = 0; j < CHECK_BLOCK; j++) {
      bad += b[j] != checkByte(m_check_at[i] + j);
    }
  }
  return bad;
}

void psramCheckStart(void)
{
  if (m_state != PSRAM_STATE_UNCHECKED) {
    return;
  }
  if (!psramInit()) {
    m_state = PSRAM_STATE_NO_ID;
    NRF_LOG_RAW_INFO("[psram] boot check: %s, using on-chip RAM\n", psramStateName(m_state));
    return;
  }
  uint8_t *b = (uint8_t *)m_bounce;
  bool ok = true;
  for (int i = 0; i < ARRAY_SIZE(m_check_at) && ok; i++) {
    for (uint32_t j = 0; j < CHECK_BLOCK; j++) {
      b[j] = checkByte(m_check_at[i] + j);
    }
    ok = psramWrite(m_check_at[i], b, CHECK_BLOCK);
  }
  m_check_t0 = k_uptime_get();
  m_check_bad[0] = ok ? checkRead() : UINT32_MAX;
  m_state = PSRAM_STATE_WAITING;
  if (m_check_bad[0] != 0) {
    m_state = PSRAM_STATE_BAD_AT_ONCE;
    m_present = false;
    NRF_LOG_RAW_INFO("[psram] boot check: %u / %u bytes wrong right after the write: %s, using on-chip RAM\n",
                     m_check_bad[0], (unsigned)sizeof(m_check_at) / 4 * CHECK_BLOCK, psramStateName(m_state));
  }
}

bool psramCheckFinish(void)
{
  if (m_state == PSRAM_STATE_UNCHECKED) {
    psramCheckStart();
  }
  if (m_state == PSRAM_STATE_WAITING) {
    int64_t left = m_check_t0 + PSRAM_CHECK_WAIT_MS - k_uptime_get();
    if (left > 0) {
      k_sleep(K_MSEC(left));
    }
    m_check_bad[1] = checkRead();
    uint32_t waited = (uint32_t)(k_uptime_get() - m_check_t0);
    m_state = m_check_bad[1] == 0 ? PSRAM_STATE_OK : PSRAM_STATE_BAD_LATER;
    m_present = m_state == PSRAM_STATE_OK;
    NRF_LOG_RAW_INFO("[psram] boot check: %u x %u B written, 0 wrong at once, %u wrong %u ms later: %s%s\n",
                     (unsigned)ARRAY_SIZE(m_check_at), CHECK_BLOCK, m_check_bad[1], waited, psramStateName(m_state),
                     m_present ? "" : ", using on-chip RAM");
  }
  return m_state == PSRAM_STATE_OK;
}

psram_state_t psramState(void)
{
  return m_state;
}

const char *psramStateName(psram_state_t state)
{
  switch (state) {
    case PSRAM_STATE_UNCHECKED: return "not checked";
    case PSRAM_STATE_WAITING: return "waiting";
    case PSRAM_STATE_OK: return "OK";
    case PSRAM_STATE_NO_ID: return "no PSRAM (ID did not read back)";
    case PSRAM_STATE_BAD_AT_ONCE: return "FAILED (wrong at once: wires / clock)";
    case PSRAM_STATE_BAD_LATER: return "FAILED (lost bits after the wait: retention)";
  }
  return "?";
}
