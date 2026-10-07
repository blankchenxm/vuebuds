/* Busy-wait delays (nRF5 SDK nrf_delay.h). */
#ifndef NRF_DELAY_H_
#define NRF_DELAY_H_

#include <zephyr/kernel.h>

#define nrf_delay_us(us) k_busy_wait(us)
#define nrf_delay_ms(ms) k_busy_wait((uint32_t)(ms) * 1000u)

#endif
