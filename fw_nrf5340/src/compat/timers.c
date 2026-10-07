#include <zephyr/kernel.h>

#include "timers.h"

uint32_t systemTimeGetMs(void)
{
  return k_uptime_get_32();
}

uint64_t systemTimeGetUs(void)
{
  return k_ticks_to_us_floor64(k_uptime_ticks());
}

void delayMs(uint32_t ms)
{
  k_msleep(ms);
}
