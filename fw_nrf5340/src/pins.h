/*
 * nRF5340 DK wiring (Obsidian note "nrf5340迁移" §2). Same as the nRF52840 DK (fw/gpio.h,
 * FF=0) except FVLD, LVLD and the CS loopback, whose 52840 pins are taken on this board
 * (P0.29 LED2, P0.11 VCOM1, P0.14 QSPI flash). I2C (P0.26 SDA / P0.27 SCL) is in the
 * devicetree overlay. Added 2026-10-10 / 10-14: FSIN P1.11, TRIGGER P0.25, INT P1.12; the
 * PSRAM is on the QSPI pins P0.13-P0.18 (src/psram.h).
 */
#ifndef PINS_H_
#define PINS_H_

#include <hal/nrf_gpio.h>

#define CAM_PCLK_OUT_TO_MCU   NRF_GPIO_PIN_MAP(1, 2)   // SPIS SCK         Arduino D14
#define CAM_D0                NRF_GPIO_PIN_MAP(1, 3)   // SPIS MOSI        Arduino D15
#define CAM_XSLEEP            NRF_GPIO_PIN_MAP(1, 4)   // HM0360 only      Arduino D2
#define CAM_XSHUTDOWN         NRF_GPIO_PIN_MAP(1, 5)   // HM0360 only      Arduino D3
#define CAM_SPI_CS_OUT        NRF_GPIO_PIN_MAP(1, 6)   // jumper to CS_IN  Arduino D4
#define CAM_SPI_CS_IN         NRF_GPIO_PIN_MAP(1, 7)   // SPIS CSN         Arduino D5
#define CAM_MCLK_IN_FROM_MCU  NRF_GPIO_PIN_MAP(1, 8)   // 8 MHz            Arduino D6
#define CAM_FRAME_VALID       NRF_GPIO_PIN_MAP(1, 9)   // FVLD / VSYNC     Arduino D7
#define CAM_LINE_VALID        NRF_GPIO_PIN_MAP(1, 10)  // LVLD / HREF      Arduino D8
#define CAM_FSIN              NRF_GPIO_PIN_MAP(1, 11)  // HM0360 FSIN      Arduino D9
#define CAM_TRIGGER           NRF_GPIO_PIN_MAP(0, 25)  // HM0360 TRIGGER   Arduino A4
#define CAM_INT               NRF_GPIO_PIN_MAP(1, 12)  // HM0360 INT (active high)  Arduino D10

#endif
