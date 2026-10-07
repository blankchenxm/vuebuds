/*
 * System time and delays. fw/ used TIMER1 (HFCLK); here the Zephyr kernel clock (RTC1,
 * 32.768 kHz crystal) is used, so time has a 30.5 us resolution and no 0.5 % drift.
 */
#ifndef TIMERS_H_
#define TIMERS_H_

#include <stdint.h>

uint32_t systemTimeGetMs(void);
uint64_t systemTimeGetUs(void);
void delayMs(uint32_t ms);  // sleeps the calling thread

#endif
