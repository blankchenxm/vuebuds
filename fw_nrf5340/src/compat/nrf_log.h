/* nRF5 SDK logging on Zephyr: printk to the RTT console, like the RTT backend of fw/. */
#ifndef NRF_LOG_H_
#define NRF_LOG_H_

#include <zephyr/sys/printk.h>

#define NRF_LOG_RAW_INFO(...) printk(__VA_ARGS__)
#define NRF_LOG_INFO(fmt, ...) printk(fmt "\n", ##__VA_ARGS__)
#define NRF_LOG_ERROR(fmt, ...) printk("<error> " fmt "\n", ##__VA_ARGS__)

#endif
