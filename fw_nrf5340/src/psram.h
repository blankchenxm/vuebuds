/*
 * PSRAM: MIKROE-5337 DRAM Click (AP Memory APS6404L, 8 MB) on the QSPI peripheral, the
 * nRF5340's fixed QSPI pins (2026-10-14; was SPIM4 on P1.12-P1.15, 8 MHz single line):
 *   CE# P0.18, SCLK P0.17, SIO0 P0.13, SIO1 P0.14, SIO2 P0.15, SIO3 P0.16
 * On the DK the onboard MX25R64 flash is cut off these pins (SB10-SB15 open, SB20-SB25
 * shorted). Quad (0xEB read 1-4-4 with 6 wait cycles, 0x38 write 1-4-4) and single line
 * (0x0B / 0x02) both work, but on the DK's jumper wires quad reads break off when all four
 * data lines go high at once (2026-10-14, src/hw_test.c), so the default is single line at
 * 48 MHz in 32 B bursts: CE# low ~6.5 us, under tCEM (8 us for the 3 V part, measured on the
 * pin). Main thread only; polled, no interrupt. Boot check: psramCheckStart() below.
 */
#ifndef PSRAM_H_
#define PSRAM_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PSRAM_SIZE  (8u * 1024 * 1024)
#define PSRAM_PAGE  1024u  // bursts never cross a page
#define PSRAM_DEFAULT_QUAD  false
#define PSRAM_DEFAULT_MHZ   48
#define PSRAM_DEFAULT_BURST 32u  // bytes per CE# low (power of two <= PSRAM_PAGE)

typedef struct {
  bool quad;         // false: 0x0B fast read / 0x02 write on one line
  uint8_t sck_mhz;   // 96 / n: 96, 48, 32, 24, 19, 16, 13, 12, ... 6
  uint16_t burst;    // bytes per CE# low (power of two, 4..PSRAM_PAGE)
} psram_config_t;

bool psramInit(void);  // default config; true if the chip answers with the APS6404L ID (0x0D 0x5D)
bool psramPresent(void);

/*
 * Boot check (2026-11-20, hub #39): the HM0360B board's chip lost bits 1-3 s after they were
 * written while reads right after the write were fine, so the check writes a few small blocks
 * across the chip, reads them at once and again PSRAM_CHECK_WAIT_MS later. Not passed: the
 * PSRAM is not used (psramPresent() false, users fall back to on-chip RAM).
 *   psramCheckStart(): boot, main thread; init + write + first read, returns at once.
 *   psramCheckFinish(): second read, once; waits until PSRAM_CHECK_WAIT_MS after the start
 *     (main.c posts EVENT_PSRAM_CHECK then; image.c calls it before its first PSRAM use).
 */
#define PSRAM_CHECK_WAIT_MS 2000
typedef enum {
  PSRAM_STATE_UNCHECKED = 0,  // psramCheckStart() not called yet
  PSRAM_STATE_WAITING,        // written, second read not done yet
  PSRAM_STATE_OK,
  PSRAM_STATE_NO_ID,          // no APS6404L ID: not fitted or not wired
  PSRAM_STATE_BAD_AT_ONCE,    // wrong right after the write (wires, clock)
  PSRAM_STATE_BAD_LATER,      // right at once, wrong PSRAM_CHECK_WAIT_MS later (retention)
} psram_state_t;
void psramCheckStart(void);
bool psramCheckFinish(void);  // true: PSRAM OK to use
psram_state_t psramState(void);
const char *psramStateName(psram_state_t state);
/* Any address, length and buffer (RAM or flash); unaligned edges go through a bounce buffer. */
bool psramWrite(uint32_t address, const void *data, size_t len);
bool psramRead(uint32_t address, void *data, size_t len);

/* For src/hw_test.c. */
bool psramConfigure(const psram_config_t *config);  // re-init QSPI, reset the chip, check the ID
const psram_config_t *psramConfig(void);
void psramReadId(uint8_t id[8]);  // MF, KGD, EID[6] (single line, custom instruction)
/* One QSPI transaction: address, length and buffer word aligned, buffer in RAM. */
bool psramXfer(bool write, uint32_t address, void *buf, uint32_t len);

#endif
