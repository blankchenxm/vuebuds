/*
 * PSRAM: MIKROE-5337 DRAM Click (AP Memory APS6404L, 8 MB) on SPIM4, 8 MHz, SPI mode 0:
 * CS P1.12, SDI P1.13, SDO P1.14, SCK P1.15 (Arduino D10-D13), IO2 / IO3 tied low.
 * Pins with high drive: with standard drive the HM0360 board's jumper wires gave an extra
 * SCK edge at each SDO rise (2026-10-11, src/hw_test.c). Bursts stay inside 1 KB pages and
 * keep CS low ~1 ms, longer than the datasheet's tCEM; checked with 8 MB write / read
 * passes at room temperature (user's choice 10-10). Main thread only; polled, no interrupt.
 */
#ifndef PSRAM_H_
#define PSRAM_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PSRAM_SIZE (8u * 1024 * 1024)

bool psramInit(void);  // true if the chip answers with the APS6404L ID (0x0D 0x5D)
bool psramPresent(void);
/* data must be in RAM (EasyDMA). */
bool psramWrite(uint32_t address, const void *data, size_t len);
bool psramRead(uint32_t address, void *data, size_t len);

#endif
