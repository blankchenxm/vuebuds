#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/irq.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/hci_vs.h>
#include <bluetooth/hci_vs_sdc.h>
#include <hal/nrf_ipc.h>
#include <nrfx_gpiote.h>

#include "nrf_log.h"
#include "capture.h"
#include "pins.h"
#include "timers.h"
#include "timesync.h"

// stress_test.c (CAPTURE_STRESS_US) also uses RTC0: time sync is left out of those builds.
#ifndef CAPTURE_STRESS_US

#define TS_RTC NRF_RTC0
#define TS_IPC_CHANNEL 4             // sysbuild/ipc_radio: CONFIG_MPSL_TRIGGER_IPC_TASK_ON_RTC_START_CHANNEL
#define TS_DPPI_RTC_START 25         // IPC RECEIVE[4] -> RTC0 CLEAR
#define TS_DPPI_WAKE 26              // RTC0 COMPARE0 -> XSLEEP SET
#define TS_CC_WAKE 0
#define TS_CC_FVLD 2                 // RTC0 CAPTURE2 on every FVLD edge (CAPTURE_DPPI_FVLD)
#define TS_TICKS_PER_EPOCH (1u << 24)  // RTC0 is 24 bits: 512 s
#define DPPI_ENABLE (1UL << 31)
#define WAKE_MIN_LEAD_US 3000        // MCLK must run a little before XSLEEP goes high
#define MCLK_LEAD_US 2000

static volatile uint32_t m_epochs;  // RTC0 overflows

/* Latest local anchor point (BT RX thread) and the bridge's reference (main thread). */
static struct k_spinlock m_lock;
static struct {
  bool valid;
  uint64_t us;
  uint16_t counter;
  uint32_t interval_us;
} m_local;
static struct {
  bool valid;
  uint64_t us;
  uint16_t counter;
} m_ref;

static uint64_t ticksToUs(uint64_t ticks)
{
  return ticks * 15625u / 512u;  // 32768 Hz
}

static uint64_t usToTicks(uint64_t us)
{
  return (us * 512u + 15625u / 2) / 15625u;
}

static void rtcIsr(const void *arg)
{
  ARG_UNUSED(arg);
  if (TS_RTC->EVENTS_OVRFLW) {
    TS_RTC->EVENTS_OVRFLW = 0;
    m_epochs++;
  }
  if (TS_RTC->EVENTS_COMPARE[TS_CC_WAKE]) {
    // The wake fired (DPPI set XSLEEP): disarm, or it would fire again one epoch later.
    TS_RTC->EVENTS_COMPARE[TS_CC_WAKE] = 0;
    TS_RTC->EVTENCLR = RTC_EVTEN_COMPARE0_Msk;
    TS_RTC->INTENCLR = RTC_INTENSET_COMPARE0_Msk;
  }
}

static uint64_t nowTicks(void)
{
  unsigned int key = irq_lock();
  uint32_t counter = TS_RTC->COUNTER;
  uint32_t epochs = m_epochs;
  if (TS_RTC->EVENTS_OVRFLW) {
    // Overflow pending (its interrupt cannot run now): count it, read the counter after it.
    counter = TS_RTC->COUNTER;
    epochs++;
  }
  irq_unlock(key);
  return ((uint64_t)epochs << 24) | counter;
}

static int timesyncSysInit(void)
{
  TS_RTC->PRESCALER = 0;
  TS_RTC->EVTENSET = RTC_EVTEN_OVRFLW_Msk;
  TS_RTC->INTENSET = RTC_INTENSET_OVRFLW_Msk;
  IRQ_CONNECT(RTC0_IRQn, 5, rtcIsr, NULL, 0);
  irq_enable(RTC0_IRQn);
  // Started now, cleared again when the network core starts its controller RTC (as the sample).
  nrf_ipc_receive_config_set(NRF_IPC, TS_IPC_CHANNEL, BIT(TS_IPC_CHANNEL));
  NRF_IPC->PUBLISH_RECEIVE[TS_IPC_CHANNEL] = TS_DPPI_RTC_START | DPPI_ENABLE;
  TS_RTC->SUBSCRIBE_CLEAR = TS_DPPI_RTC_START | DPPI_ENABLE;
  // Hardware time of every FVLD edge (the capture layer publishes them on CAPTURE_DPPI_FVLD).
  TS_RTC->SUBSCRIBE_CAPTURE[TS_CC_FVLD] = CAPTURE_DPPI_FVLD | DPPI_ENABLE;
  NRF_DPPIC->CHENSET = BIT(TS_DPPI_RTC_START);
  TS_RTC->TASKS_START = 1;
  return 0;
}
SYS_INIT(timesyncSysInit, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

uint64_t timesyncLocalUs(void)
{
  return ticksToUs(nowTicks());
}

/* ---- anchor points ---- */

static bool onVendorEvent(struct net_buf_simple *buf)
{
  uint8_t *code = net_buf_simple_pull_mem(buf, sizeof(*code));
  if (*code != SDC_HCI_SUBEVENT_VS_CONN_ANCHOR_POINT_UPDATE_REPORT) {
    return false;
  }
  const sdc_hci_subevent_vs_conn_anchor_point_update_report_t *evt = (const void *)buf->data;
  struct bt_conn *conn = bt_hci_conn_lookup_handle(evt->conn_handle);
  uint32_t interval_us = 0;
  if (conn) {
    struct bt_conn_info info;
    if (bt_conn_get_info(conn, &info) == 0) {
      interval_us = info.le.interval_us;
    }
    bt_conn_unref(conn);
  }
  k_spinlock_key_t key = k_spin_lock(&m_lock);
  m_local.valid = interval_us != 0;
  m_local.us = evt->anchor_point_us;
  m_local.counter = evt->event_counter;
  m_local.interval_us = interval_us;
  k_spin_unlock(&m_lock, key);
  return true;
}

void timesyncBleReady(void)
{
  int err = bt_hci_register_vnd_evt_cb(onVendorEvent);
  sdc_hci_cmd_vs_conn_anchor_point_update_event_report_enable_t enable = {.enable = true};
  int err2 = hci_vs_sdc_conn_anchor_point_update_event_report_enable(&enable);
  NRF_LOG_RAW_INFO("[sync] anchor point reports %s (%d, %d), local clock %u ms\n", (err || err2) ? "FAILED" : "on",
                   err, err2, (uint32_t)(timesyncLocalUs() / 1000));
}

bool timesyncHasReference(void)
{
  return m_ref.valid && m_local.valid;
}

/* Bridge time of the local anchor's event: reference + whole intervals (the bridge's clock sets
 * the schedule). Valid while the counters are within +-32767 events (8 min at 15 ms). */
static bool bridgeAtLocalAnchor(uint64_t *bridge_us, uint64_t *local_us)
{
  k_spinlock_key_t key = k_spin_lock(&m_lock);
  bool ok = m_ref.valid && m_local.valid;
  if (ok) {
    int16_t events = (int16_t)(m_local.counter - m_ref.counter);
    *bridge_us = m_ref.us + (int64_t)events * m_local.interval_us;
    *local_us = m_local.us;
  }
  k_spin_unlock(&m_lock, key);
  return ok;
}

bool timesyncBridgeToLocal(uint64_t bridge_us, uint64_t *local_us)
{
  uint64_t b, l;
  if (!bridgeAtLocalAnchor(&b, &l)) {
    return false;
  }
  *local_us = l + (int64_t)(bridge_us - b);
  return true;
}

bool timesyncLocalToBridge(uint64_t local_us, uint64_t *bridge_us)
{
  uint64_t b, l;
  if (!bridgeAtLocalAnchor(&b, &l)) {
    return false;
  }
  *bridge_us = b + (int64_t)(local_us - l);
  return true;
}

uint64_t timesyncLastFvldUs(void)
{
  uint64_t now = nowTicks();
  uint32_t cc = TS_RTC->CC[TS_CC_FVLD];
  uint64_t epoch = now >> 24;
  if (cc > (now & (TS_TICKS_PER_EPOCH - 1))) {
    epoch--;  // captured before the last overflow
  }
  return ticksToUs((epoch << 24) | cc);
}

/* ---- timed wake ---- */

static void mclkOn(struct k_timer *timer)
{
  capture_mclk_enable(true);
}
K_TIMER_DEFINE(m_mclk_timer, mclkOn, NULL);

bool timesyncWakeAt(uint64_t local_us)
{
  uint64_t now = timesyncLocalUs();
  if (local_us < now + WAKE_MIN_LEAD_US || local_us > now + 400000000ull) {
    return false;
  }
  // XSLEEP is a GPIOTE task pin of the capture layer (its CLR is the frame-boundary sleep).
  capture_dppi_subscribe(nrfx_gpiote_set_task_address_get(capture_gpiote(), CAM_XSLEEP), TS_DPPI_WAKE);
  uint64_t ticks = usToTicks(local_us);
  TS_RTC->CC[TS_CC_WAKE] = (uint32_t)ticks & (TS_TICKS_PER_EPOCH - 1);
  TS_RTC->EVENTS_COMPARE[TS_CC_WAKE] = 0;
  TS_RTC->PUBLISH_COMPARE[TS_CC_WAKE] = TS_DPPI_WAKE | DPPI_ENABLE;
  NRF_DPPIC->CHENSET = BIT(TS_DPPI_WAKE);
  TS_RTC->EVTENSET = RTC_EVTEN_COMPARE0_Msk;
  TS_RTC->INTENSET = RTC_INTENSET_COMPARE0_Msk;
  // The sensor needs MCLK before XSLEEP: start it shortly before (kernel timer).
  k_timer_start(&m_mclk_timer, K_USEC(local_us - now - MCLK_LEAD_US), K_NO_WAIT);
  return true;
}

void timesyncCancelWake(void)
{
  k_timer_stop(&m_mclk_timer);
  TS_RTC->EVTENCLR = RTC_EVTEN_COMPARE0_Msk;
  TS_RTC->INTENCLR = RTC_INTENSET_COMPARE0_Msk;
}

/* ---- commands ---- */

bool timesyncCommand(const ble_command_t *cmd, uint64_t *local_us)
{
  const uint8_t *d = cmd->data;
  if ((d[0] != TIMESYNC_CMD_REFERENCE && d[0] != TIMESYNC_CMD_SNAPSHOT_AT) || cmd->len < 11) {
    return false;
  }
  m_ref.us = sys_get_le64(&d[1]);
  m_ref.counter = sys_get_le16(&d[9]);
  m_ref.valid = true;
  if (d[0] != TIMESYNC_CMD_SNAPSHOT_AT || cmd->len < 19) {
    return false;
  }
  uint64_t t_bridge = sys_get_le64(&d[11]);
  if (!timesyncBridgeToLocal(t_bridge, local_us)) {
    NRF_LOG_RAW_INFO("[sync] no local anchor yet\n");
    return false;
  }
  NRF_LOG_RAW_INFO("%08d [sync] picture at bridge %u ms = local %u ms (now %u ms)\n", systemTimeGetMs(),
                   (uint32_t)(t_bridge / 1000), (uint32_t)(*local_us / 1000), (uint32_t)(timesyncLocalUs() / 1000));
  return true;
}

#else  /* CAPTURE_STRESS_US: no time sync */
void timesyncBleReady(void) {}
uint64_t timesyncLocalUs(void) { return 0; }
bool timesyncHasReference(void) { return false; }
bool timesyncBridgeToLocal(uint64_t bridge_us, uint64_t *local_us) { return false; }
bool timesyncLocalToBridge(uint64_t local_us, uint64_t *bridge_us) { return false; }
uint64_t timesyncLastFvldUs(void) { return 0; }
bool timesyncWakeAt(uint64_t local_us) { return false; }
void timesyncCancelWake(void) {}
bool timesyncCommand(const ble_command_t *cmd, uint64_t *local_us) { return false; }
#endif
