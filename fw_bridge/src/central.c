/*
 * BLE central for up to two camera boards. The bridge, not the PC's Bluetooth stack, sets the
 * link: connection interval (from the PC's configure command, the same for both links),
 * 2M PHY, data length 251, ATT MTU 498, and it rejects the boards' own parameter requests
 * (fw_nrf5340 asks for 7.5-15 ms after 5 s). Connection events are extended while there is
 * radio time (CONFIG_BT_CTLR_SDC_CONN_EVENT_EXTEND_DEFAULT), so one link alone gets the whole
 * interval and two links share it.
 *
 * Connects one board at a time (one pending LE create at a time), reconnects after a drop.
 * Every data notification goes to the PC as a FRAME_NOTIFY frame from the BT RX thread; when
 * USB is slower than the radio that thread blocks, the controller runs out of RX buffers and
 * BLE flow control holds the board back (no packet is dropped).
 */
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/sys/byteorder.h>
#include <bluetooth/hci_vs_sdc.h>

#include "central.h"
#include "usb.h"

#define UUID_BASE(x) BT_UUID_128_ENCODE(0x47ea0000 | (x), 0xa0e4, 0x554e, 0x5282, 0x0afcd3246970)
static const struct bt_uuid_128 dataUuid = BT_UUID_INIT_128(UUID_BASE(0x1402));
static const struct bt_uuid_128 controlUuid = BT_UUID_INIT_128(UUID_BASE(0x1403));

#define CONNECT_TIMEOUT_10MS 300  // give up one connect attempt after 3 s, try the next slot
#define SUPERVISION_TIMEOUT_10MS 400

typedef struct {
  bool used;
  bt_addr_le_t addr;
  struct bt_conn *conn;
  bool connected;
  bool ready;
  uint16_t dataHandle, controlHandle;
  struct bt_gatt_discover_params disc;
  struct bt_gatt_discover_params cccDisc;
  struct bt_gatt_subscribe_params sub;
  struct bt_gatt_exchange_params mtu;
  struct k_work setup;  // PHY, data length, MTU, discovery: off the BT RX thread
  uint32_t packets, bytes;  // notifications since the last stats line
  // Latest connection anchor point on the bridge's controller clock (time sync, plan E).
  bool anchorValid;
  uint64_t anchorUs;
  uint16_t anchorCounter;
} slot_t;

static slot_t slots[CENTRAL_SLOTS];
static uint16_t interval = 12;  // 15 ms until the PC configures it
static bool connecting;
static K_MUTEX_DEFINE(lock);
static struct k_spinlock anchorLock;

#define CMD_TIME_REFERENCE 0xB5  // fw_nrf5340/src/timesync.h
#define REFERENCE_PERIOD_S 5

static void connectNext(struct k_work *work);
static void scheduleReference(void);
static K_WORK_DELAYABLE_DEFINE(connectWork, connectNext);

static void text(int slot, const char *fmt, ...)
{
  char buf[120];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintk(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  n = MIN(n, (int)sizeof(buf) - 1);
  printk("[bridge] slot %d: %s\n", slot, buf);
  usbSendFrame(FRAME_TEXT, slot < 0 ? 0xFF : slot, buf, n);
}

static int slotOf(struct bt_conn *conn)
{
  for (int i = 0; i < CENTRAL_SLOTS; i++) {
    if (slots[i].conn == conn) {
      return i;
    }
  }
  return -1;
}

static void sendEvent(int i, uint8_t state, uint8_t reason)
{
  central_event_t ev = {.state = state, .addr_type = slots[i].addr.type, .reason = reason};
  memcpy(ev.addr, slots[i].addr.a.val, 6);
  struct bt_conn_info info;
  if (slots[i].conn && slots[i].connected && bt_conn_get_info(slots[i].conn, &info) == 0) {
    ev.interval_1250us = info.le.interval_us / 1250;
    ev.tx_phy = info.le.phy->tx_phy == BT_GAP_LE_PHY_2M ? 2 : 1;
    ev.rx_phy = info.le.phy->rx_phy == BT_GAP_LE_PHY_2M ? 2 : 1;
    ev.tx_len = info.le.data_len->tx_max_len;
    ev.rx_len = info.le.data_len->rx_max_len;
    ev.mtu = bt_gatt_get_mtu(slots[i].conn);
  }
  usbSendFrame(FRAME_EVENT, i, &ev, sizeof(ev));
}

/* ---- notifications, discovery ---- */

static uint8_t onNotify(struct bt_conn *conn, struct bt_gatt_subscribe_params *params, const void *data,
                        uint16_t length)
{
  slot_t *s = CONTAINER_OF(params, slot_t, sub);
  int i = s - slots;
  if (data == NULL) {
    text(i, "notifications off");
    params->value_handle = 0;
    return BT_GATT_ITER_STOP;
  }
  s->packets++;
  s->bytes += length;
  usbSendFrame(FRAME_NOTIFY, i, data, length);
  return BT_GATT_ITER_CONTINUE;
}

static void subscribe(int i)
{
  slot_t *s = &slots[i];
  if (!s->dataHandle || !s->controlHandle) {
    text(i, "service not found (data 0x%04x, control 0x%04x)", s->dataHandle, s->controlHandle);
    bt_conn_disconnect(s->conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    return;
  }
  memset(&s->sub, 0, sizeof(s->sub));
  s->sub.notify = onNotify;
  s->sub.value = BT_GATT_CCC_NOTIFY;
  s->sub.value_handle = s->dataHandle;
  s->sub.ccc_handle = BT_GATT_AUTO_DISCOVER_CCC_HANDLE;
  s->sub.end_handle = s->controlHandle;  // the data CCC sits between the two characteristics
  s->sub.disc_params = &s->cccDisc;
  int err = bt_gatt_subscribe(s->conn, &s->sub);
  if (err && err != -EALREADY) {
    text(i, "subscribe failed (%d)", err);
    return;
  }
  // Notifications are on once the CCC write is sent; the board starts nothing until a command.
  s->ready = true;
  text(i, "ready: data 0x%04x, control 0x%04x", s->dataHandle, s->controlHandle);
  scheduleReference();
  sendEvent(i, CENTRAL_READY, 0);
}

static uint8_t onDiscover(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                          struct bt_gatt_discover_params *params)
{
  slot_t *s = CONTAINER_OF(params, slot_t, disc);
  if (attr == NULL) {
    subscribe(s - slots);
    return BT_GATT_ITER_STOP;
  }
  const struct bt_gatt_chrc *chrc = attr->user_data;
  if (bt_uuid_cmp(chrc->uuid, &dataUuid.uuid) == 0) {
    s->dataHandle = chrc->value_handle;
  } else if (bt_uuid_cmp(chrc->uuid, &controlUuid.uuid) == 0) {
    s->controlHandle = chrc->value_handle;
  }
  return BT_GATT_ITER_CONTINUE;
}

static void onMtu(struct bt_conn *conn, uint8_t err, struct bt_gatt_exchange_params *params)
{
  int i = slotOf(conn);
  if (i < 0) {
    return;
  }
  text(i, "MTU %u%s", bt_gatt_get_mtu(conn), err ? " (exchange failed)" : "");
  slot_t *s = &slots[i];
  memset(&s->disc, 0, sizeof(s->disc));
  s->disc.func = onDiscover;
  s->disc.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
  s->disc.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
  s->disc.type = BT_GATT_DISCOVER_CHARACTERISTIC;
  int e = bt_gatt_discover(conn, &s->disc);
  if (e) {
    text(i, "discovery failed (%d)", e);
  }
}

/* ---- time sync (plan E, as nrf/samples/bluetooth/conn_time_sync) ---- */

/* Controller anchor point reports: one per connection event and link. */
static bool onVendorEvent(struct net_buf_simple *buf)
{
  uint8_t *code = net_buf_simple_pull_mem(buf, sizeof(*code));
  if (*code != SDC_HCI_SUBEVENT_VS_CONN_ANCHOR_POINT_UPDATE_REPORT) {
    return false;
  }
  const sdc_hci_subevent_vs_conn_anchor_point_update_report_t *evt = (const void *)buf->data;
  struct bt_conn *conn = bt_hci_conn_lookup_handle(evt->conn_handle);
  if (conn == NULL) {
    return true;
  }
  int i = slotOf(conn);
  bt_conn_unref(conn);
  if (i >= 0) {
    k_spinlock_key_t key = k_spin_lock(&anchorLock);
    slots[i].anchorValid = true;
    slots[i].anchorUs = evt->anchor_point_us;
    slots[i].anchorCounter = evt->event_counter;
    k_spin_unlock(&anchorLock, key);
  }
  return true;
}

/* [cmd][anchor us u64][event counter u16] (+ [T u64] for timed commands) */
static int writeTimed(int i, uint8_t cmd, const uint64_t *t_us)
{
  slot_t *s = &slots[i];
  uint8_t msg[19];
  k_spinlock_key_t key = k_spin_lock(&anchorLock);
  bool valid = s->anchorValid;
  msg[0] = cmd;
  sys_put_le64(s->anchorUs, &msg[1]);
  sys_put_le16(s->anchorCounter, &msg[9]);
  k_spin_unlock(&anchorLock, key);
  if (!valid || !s->ready || !s->conn) {
    return -EAGAIN;
  }
  size_t len = 11;
  if (t_us) {
    sys_put_le64(*t_us, &msg[11]);
    len = 19;
  }
  return bt_gatt_write_without_response(s->conn, s->controlHandle, msg, len, false);
}

/* References keep the boards' mapping fresh (event counters wrap after 65536 events). */
static void sendReferences(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(referenceWork, sendReferences);
static void sendReferences(struct k_work *work)
{
  for (int i = 0; i < CENTRAL_SLOTS; i++) {
    if (slots[i].ready) {
      writeTimed(i, CMD_TIME_REFERENCE, NULL);
    }
  }
  k_work_reschedule(&referenceWork, K_SECONDS(REFERENCE_PERIOD_S));
}

static void scheduleReference(void)
{
  k_work_reschedule(&referenceWork, K_MSEC(200));  // after a couple of anchor reports
}

/* ---- connection management ---- */

static void connectNext(struct k_work *work)
{
  k_mutex_lock(&lock, K_FOREVER);
  if (!connecting) {
    for (int i = 0; i < CENTRAL_SLOTS; i++) {
      slot_t *s = &slots[i];
      if (!s->used || s->conn) {
        continue;
      }
      struct bt_conn_le_create_param create =
          BT_CONN_LE_CREATE_PARAM_INIT(BT_CONN_LE_OPT_NONE, BT_GAP_SCAN_FAST_INTERVAL, BT_GAP_SCAN_FAST_INTERVAL);
      create.timeout = CONNECT_TIMEOUT_10MS;
      struct bt_le_conn_param param = BT_LE_CONN_PARAM_INIT(interval, interval, 0, SUPERVISION_TIMEOUT_10MS);
      int err = bt_conn_le_create(&s->addr, &create, &param, &s->conn);
      if (err) {
        s->conn = NULL;
        text(i, "connect failed to start (%d)", err);
        k_work_reschedule(&connectWork, K_MSEC(500));
      } else {
        connecting = true;
      }
      break;
    }
  }
  k_mutex_unlock(&lock);
}

static void connected(struct bt_conn *conn, uint8_t err)
{
  k_mutex_lock(&lock, K_FOREVER);
  int i = slotOf(conn);
  connecting = false;
  if (i < 0) {
    k_mutex_unlock(&lock);
    return;
  }
  slot_t *s = &slots[i];
  if (err) {
    // 0x02 (unknown connection id): the 3 s attempt timed out; the board is off or busy.
    bt_conn_unref(s->conn);
    s->conn = NULL;
    k_mutex_unlock(&lock);
    k_work_reschedule(&connectWork, K_MSEC(100));
    return;
  }
  s->connected = true;
  s->dataHandle = s->controlHandle = 0;
  k_mutex_unlock(&lock);

  char addr[BT_ADDR_LE_STR_LEN];
  bt_addr_le_to_str(&s->addr, addr, sizeof(addr));
  text(i, "connected %s", addr);
  sendEvent(i, CENTRAL_CONNECTED, 0);

  // These wait for HCI command completions, which the BT RX thread (running this callback)
  // delivers: calling them here deadlocks ("Controller unresponsive" assert).
  k_work_submit(&s->setup);
}

static void setupLink(struct k_work *work)
{
  slot_t *s = CONTAINER_OF(work, slot_t, setup);
  struct bt_conn *conn = s->conn;
  if (!conn || !s->connected) {
    return;
  }
  int i = s - slots;
  int err = bt_conn_le_phy_update(conn, BT_CONN_LE_PHY_PARAM_2M);
  if (err) {
    text(i, "PHY update failed (%d)", err);
  }
  err = bt_conn_le_data_len_update(conn, BT_LE_DATA_LEN_PARAM_MAX);
  if (err) {
    text(i, "data length update failed (%d)", err);
  }
  s->mtu.func = onMtu;
  if (bt_gatt_exchange_mtu(conn, &s->mtu) != 0) {
    onMtu(conn, 0, &s->mtu);  // the board already exchanged it
  }
  k_work_reschedule(&connectWork, K_NO_WAIT);  // next slot
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
  k_mutex_lock(&lock, K_FOREVER);
  int i = slotOf(conn);
  if (i < 0) {
    k_mutex_unlock(&lock);
    return;
  }
  slot_t *s = &slots[i];
  s->connected = false;
  s->ready = false;
  bt_conn_unref(s->conn);
  s->conn = NULL;
  k_mutex_unlock(&lock);
  text(i, "disconnected (0x%02x)", reason);
  sendEvent(i, CENTRAL_DISCONNECTED, reason);
  k_work_reschedule(&connectWork, K_MSEC(300));  // the board reboots on disconnect (~1 s)
}

static bool paramRequest(struct bt_conn *conn, struct bt_le_conn_param *param)
{
  int i = slotOf(conn);
  if (param->interval_min <= interval && interval <= param->interval_max) {
    param->interval_min = param->interval_max = interval;  // keep ours
    return true;
  }
  text(i, "rejected parameter request %u-%u (keeping %u)", param->interval_min, param->interval_max, interval);
  return false;
}

static void paramUpdated(struct bt_conn *conn, uint16_t itv, uint16_t latency, uint16_t timeout)
{
  int i = slotOf(conn);
  if (i >= 0) {
    text(i, "interval %u.%02u ms, latency %u", itv * 125 / 100, (itv * 125) % 100, latency);
    sendEvent(i, CENTRAL_UPDATED, 0);
  }
}

static void phyUpdated(struct bt_conn *conn, struct bt_conn_le_phy_info *phy)
{
  int i = slotOf(conn);
  if (i >= 0) {
    text(i, "PHY tx %u rx %u", phy->tx_phy, phy->rx_phy);
    sendEvent(i, CENTRAL_UPDATED, 0);
  }
}

static void dataLenUpdated(struct bt_conn *conn, struct bt_conn_le_data_len_info *info)
{
  int i = slotOf(conn);
  if (i >= 0) {
    text(i, "data length tx %u rx %u", info->tx_max_len, info->rx_max_len);
    sendEvent(i, CENTRAL_UPDATED, 0);
  }
}

BT_CONN_CB_DEFINE(connCallbacks) = {
  .connected = connected,
  .disconnected = disconnected,
  .le_param_req = paramRequest,
  .le_param_updated = paramUpdated,
  .le_phy_updated = phyUpdated,
  .le_data_len_updated = dataLenUpdated,
};

/* RTT only: notifications per second per slot; CPU load with -Dfw_bridge_CONFIG_SCHED_THREAD_USAGE_ALL=y
 * -Dfw_bridge_CONFIG_THREAD_RUNTIME_STATS=y (excludes the controller's zero-latency interrupts). */
static void stats(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(statsWork, stats);
static void stats(struct k_work *work)
{
#ifdef CONFIG_SCHED_THREAD_USAGE_ALL
  static uint64_t lastAll, lastIdle;
  k_thread_runtime_stats_t all;
  k_thread_runtime_stats_all_get(&all);
  uint64_t dAll = all.total_cycles - lastAll, dIdle = all.idle_cycles - lastIdle;
  lastAll = all.total_cycles;
  lastIdle = all.idle_cycles;
  if (dAll + dIdle) {
    printk("[bridge] CPU busy %u%%\n", (uint32_t)(100 * dAll / (dAll + dIdle)));
  }
#endif
  for (int i = 0; i < CENTRAL_SLOTS; i++) {
    if (slots[i].packets) {
      printk("[bridge] slot %d: %u packets/s, %u KB/s\n", i, slots[i].packets, slots[i].bytes / 1024);
      slots[i].packets = slots[i].bytes = 0;
    }
  }
  k_work_reschedule(&statsWork, K_SECONDS(1));
}

/* ---- API ---- */

void centralInit(void)
{
  for (int i = 0; i < CENTRAL_SLOTS; i++) {
    k_work_init(&slots[i].setup, setupLink);
  }
  int err = bt_enable(NULL);
  printk("[bridge] Bluetooth %s (%d)\n", err ? "failed" : "on", err);
  bt_hci_register_vnd_evt_cb(onVendorEvent);
  sdc_hci_cmd_vs_conn_anchor_point_update_event_report_enable_t enable = {.enable = true};
  err = hci_vs_sdc_conn_anchor_point_update_event_report_enable(&enable);
  printk("[bridge] anchor point reports %s (%d)\n", err ? "failed" : "on", err);
  k_work_reschedule(&statsWork, K_SECONDS(1));
}

void centralConfigure(const bt_addr_le_t *addrs, uint8_t count, uint16_t interval_1250us)
{
  k_mutex_lock(&lock, K_FOREVER);
  bool changed = interval_1250us && interval_1250us != interval;
  if (interval_1250us) {
    interval = interval_1250us;
  }
  for (int i = 0; i < CENTRAL_SLOTS; i++) {
    slot_t *s = &slots[i];
    bool want = i < count;
    if (want && s->used && bt_addr_le_eq(&s->addr, &addrs[i])) {
      if (changed && s->conn && s->connected) {
        struct bt_le_conn_param p = BT_LE_CONN_PARAM_INIT(interval, interval, 0, SUPERVISION_TIMEOUT_10MS);
        bt_conn_le_param_update(s->conn, &p);
      }
      // Kept link: tell the (possibly new) PC session where it stands.
      if (s->connected) {
        sendEvent(i, s->ready ? CENTRAL_READY : CENTRAL_CONNECTED, 0);
      }
      continue;
    }
    if (s->conn) {
      bt_conn_disconnect(s->conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);  // slot freed in disconnected()
    }
    s->used = want;
    if (want) {
      bt_addr_le_copy(&s->addr, &addrs[i]);
    }
  }
  k_mutex_unlock(&lock);
  k_work_reschedule(&connectWork, K_NO_WAIT);
}

void centralWrite(uint8_t mask, const uint8_t *data, uint16_t len)
{
  // Back to back: each write goes out at its own link's next connection event.
  for (int i = 0; i < CENTRAL_SLOTS; i++) {
    if (!(mask & BIT(i))) {
      continue;
    }
    slot_t *s = &slots[i];
    int8_t err = -ENOTCONN;
    if (s->conn && s->connected && s->controlHandle) {
      err = bt_gatt_write_without_response(s->conn, s->controlHandle, data, len, false);
    }
    usbSendFrame(FRAME_WRITE, i, &err, 1);
  }
}

void centralTimedWrite(uint8_t mask, uint16_t delay_ms, uint8_t cmd)
{
  // One bridge time T for every link: the newest anchor point plus the delay.
  uint64_t base = 0;
  k_spinlock_key_t key = k_spin_lock(&anchorLock);
  for (int i = 0; i < CENTRAL_SLOTS; i++) {
    if ((mask & BIT(i)) && slots[i].anchorValid) {
      base = MAX(base, slots[i].anchorUs);
    }
  }
  k_spin_unlock(&anchorLock, key);
  uint64_t t_us = base + (uint64_t)delay_ms * 1000;
  for (int i = 0; i < CENTRAL_SLOTS; i++) {
    if (mask & BIT(i)) {
      int8_t err = base ? writeTimed(i, cmd, &t_us) : -EAGAIN;
      usbSendFrame(FRAME_WRITE, i, &err, 1);
    }
  }
  text(-1, "timed 0x%02x at %u.%03u ms (bridge clock)", cmd, (uint32_t)(t_us / 1000), (uint32_t)(t_us % 1000));
}

void centralDisconnect(uint8_t mask)
{
  for (int i = 0; i < CENTRAL_SLOTS; i++) {
    if ((mask & BIT(i)) && slots[i].conn) {
      bt_conn_disconnect(slots[i].conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    }
  }
}
