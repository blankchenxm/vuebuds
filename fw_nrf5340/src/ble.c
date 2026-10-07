/*
 * BLE peripheral "mustard" on Zephyr, same protocol as fw/ble_manager.c + fw/ble_cus.c:
 *   service 47ea1400-a0e4-554e-5282-0afcd3246970
 *   data    ...1402  notify: [seq, flags, payload...], flags bit0 = start of frame; a
 *                   start-of-frame payload begins with width, height (uint16 LE each)
 *   control ...1403  write 0xB1: start the camera stream (read / notify, 1 byte)
 * Like fw/, turning data notifications off or disconnecting reboots the chip.
 */
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

#include "nrf_log.h"
#include "event.h"
#include "timers.h"
#include "ble.h"

#define ADV_INTERVAL 300  // 187.5 ms in 0.625 ms units, as fw/main.h APP_ADV_INTERVAL
#define PACKET_FLAG_START_OF_FRAME 0x01
#define CMD_STREAM_START 0xB1
#define CMD_STREAM_STOP 0xB0
#define NOTIFY_IN_FLIGHT CONFIG_BT_ATT_TX_COUNT
#define NOTIFY_WAIT_MS 2000  // no notification completed for this long: give up the frame

#define UUID_BASE(x) BT_UUID_128_ENCODE(0x47ea0000 | (x), 0xa0e4, 0x554e, 0x5282, 0x0afcd3246970)
static const struct bt_uuid_128 serviceUuid = BT_UUID_INIT_128(UUID_BASE(0x1400));
static const struct bt_uuid_128 dataUuid = BT_UUID_INIT_128(UUID_BASE(0x1402));
static const struct bt_uuid_128 controlUuid = BT_UUID_INIT_128(UUID_BASE(0x1403));

static struct bt_conn *currentConn;
static uint8_t controlValue;
static uint8_t sequenceNumber;
static uint8_t packet[CONFIG_BT_L2CAP_TX_MTU];
K_SEM_DEFINE(notifySlots, NOTIFY_IN_FLIGHT, NOTIFY_IN_FLIGHT);

static void dataCccChanged(const struct bt_gatt_attr *attr, uint16_t value)
{
  if (value & BT_GATT_CCC_NOTIFY) {
    eventQueuePush(EVENT_BLE_DATA_STREAM_START);
  } else {
    eventQueuePush(EVENT_BLE_DATA_STREAM_STOP);
  }
}

static ssize_t controlRead(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf, uint16_t len,
                           uint16_t offset)
{
  return bt_gatt_attr_read(conn, attr, buf, len, offset, &controlValue, sizeof(controlValue));
}

static ssize_t controlWrite(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf, uint16_t len,
                            uint16_t offset, uint8_t flags)
{
  if (offset != 0 || len != 1) {
    return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
  }
  uint8_t cmd = *(const uint8_t *)buf;
  NRF_LOG_INFO("writing to 1403 0x%02x", cmd);
  if (cmd == CMD_STREAM_START) {
    eventQueuePush(EVENT_CAMERA_STREAM_START);
  }
  // 0xB0 (stream stop) did nothing in fw/ either; the audio / timesync commands are not ported.
  return len;
}

static void controlCccChanged(const struct bt_gatt_attr *attr, uint16_t value) {}

BT_GATT_SERVICE_DEFINE(mustardService,
  BT_GATT_PRIMARY_SERVICE(&serviceUuid),
  BT_GATT_CHARACTERISTIC(&dataUuid.uuid, BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_NONE, NULL, NULL, NULL),
  BT_GATT_CCC(dataCccChanged, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
  BT_GATT_CHARACTERISTIC(&controlUuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE_WITHOUT_RESP | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ | BT_GATT_PERM_WRITE, controlRead, controlWrite, NULL),
  BT_GATT_CCC(controlCccChanged, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

#define DATA_ATTR (&mustardService.attrs[2])
#define CONTROL_ATTR (&mustardService.attrs[5])

static const struct bt_data advData[] = {
  BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
  BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
  BT_DATA_BYTES(BT_DATA_UUID128_ALL, UUID_BASE(0x1400)),
};

static void mtuExchanged(struct bt_conn *conn, uint8_t err, struct bt_gatt_exchange_params *params)
{
  NRF_LOG_INFO("ATT MTU exchange %s, MTU %u", err ? "failed" : "done", bt_gatt_get_mtu(conn));
}
static struct bt_gatt_exchange_params mtuParams = {.func = mtuExchanged};

static void connected(struct bt_conn *conn, uint8_t err)
{
  if (err) {
    NRF_LOG_RAW_INFO("%08d [ble] connection failed (0x%02x)\n", systemTimeGetMs(), err);
    return;
  }
  currentConn = bt_conn_ref(conn);
  NRF_LOG_RAW_INFO("%08d [ble] connected\n", systemTimeGetMs());
  bt_gatt_exchange_mtu(conn, &mtuParams);  // like nrf_ble_gatt: ask for MTU 247
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
  NRF_LOG_RAW_INFO("%08d [ble] disconnected (0x%02x)\n", systemTimeGetMs(), reason);
  if (currentConn) {
    bt_conn_unref(currentConn);
    currentConn = NULL;
  }
  eventQueuePush(EVENT_BLE_DISCONNECTED);
}

static void controlNotify(struct k_work *work)
{
  struct bt_conn *conn = currentConn;
  if (conn && bt_gatt_is_subscribed(conn, CONTROL_ATTR, BT_GATT_CCC_NOTIFY)) {
    bt_gatt_notify(conn, CONTROL_ATTR, &controlValue, sizeof(controlValue));
  }
}
static K_WORK_DEFINE(controlNotifyWork, controlNotify);

static void paramsUpdated(struct bt_conn *conn, uint16_t interval, uint16_t latency, uint16_t timeout)
{
  // Same text as fw/ (min, max in ms; host/ble_bench.py parses it). Zephyr reports the one in use.
  NRF_LOG_INFO("Connection interval updated: %d, %d", (5 * interval) / 4, (5 * interval) / 4);
  // fw/ writes 0xFF to the control characteristic here (notified if enabled). Notified from
  // the system work queue: this callback runs on the BT RX thread, which must not block on a
  // TX buffer while a frame fills the queue.
  controlValue = 0xFF;
  k_work_submit(&controlNotifyWork);
}

static void dataLenUpdated(struct bt_conn *conn, struct bt_conn_le_data_len_info *info)
{
  NRF_LOG_INFO("Data length updated to %u bytes.", info->tx_max_len);
}

static void phyUpdated(struct bt_conn *conn, struct bt_conn_le_phy_info *info)
{
  NRF_LOG_INFO("PHY updated: tx %u, rx %u", info->tx_phy, info->rx_phy);
}

BT_CONN_CB_DEFINE(connCallbacks) = {
  .connected = connected,
  .disconnected = disconnected,
  .le_param_updated = paramsUpdated,
  .le_data_len_updated = dataLenUpdated,
  .le_phy_updated = phyUpdated,
};

void bleInit(void)
{
  int err = bt_enable(NULL);
  if (err) {
    NRF_LOG_RAW_INFO("[ble] bt_enable failed (%d)\n", err);
    return;
  }
  err = bt_le_adv_start(BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN, ADV_INTERVAL, ADV_INTERVAL, NULL), advData,
                        ARRAY_SIZE(advData), NULL, 0);
  if (err) {
    NRF_LOG_RAW_INFO("[ble] advertising failed (%d)\n", err);
    return;
  }
  bt_addr_le_t addr;
  size_t count = 1;
  bt_id_get(&addr, &count);
  char str[BT_ADDR_LE_STR_LEN];
  bt_addr_le_to_str(&addr, str, sizeof(str));
  NRF_LOG_RAW_INFO("%08d [ble] address -> %s\n", systemTimeGetMs(), str);
  NRF_LOG_RAW_INFO("%08d [ble] advertising\n", systemTimeGetMs());
}

static void notifyDone(struct bt_conn *conn, void *user_data)
{
  k_sem_give(&notifySlots);
}

// Queue one packet; waits while NOTIFY_IN_FLIGHT notifications are in the stack.
static bool sendPacket(uint16_t length)
{
  if (k_sem_take(&notifySlots, K_MSEC(NOTIFY_WAIT_MS)) != 0) {
    NRF_LOG_RAW_INFO("%08d [ble] no notification completed in %d ms, frame dropped\n", systemTimeGetMs(),
                     NOTIFY_WAIT_MS);
    return false;
  }
  if (currentConn == NULL) {
    k_sem_give(&notifySlots);
    return false;
  }
  struct bt_gatt_notify_params params = {
    .attr = DATA_ATTR,
    .data = packet,
    .len = length,
    .func = notifyDone,
  };
  int err = bt_gatt_notify_cb(currentConn, &params);
  if (err) {
    k_sem_give(&notifySlots);
    NRF_LOG_ERROR("ble transmit failed: %d", err);
    return false;
  }
  return true;
}

void bleSendFrame(const uint8_t *pixels, uint16_t stride, uint16_t width, uint16_t height)
{
  if (currentConn == NULL) {
    return;
  }
  uint16_t maxPayload = MIN(bt_gatt_get_mtu(currentConn) - 3, sizeof(packet));
  bool startOfFrame = true;
  uint16_t row = 0, col = 0;

  while (row < height) {
    uint16_t length = 0;
    packet[length++] = sequenceNumber;
    packet[length++] = startOfFrame ? PACKET_FLAG_START_OF_FRAME : 0;
    if (startOfFrame) {
      packet[length++] = width & 0xFF;
      packet[length++] = width >> 8;
      packet[length++] = height & 0xFF;
      packet[length++] = height >> 8;
    }
    while (length < maxPayload && row < height) {
      uint16_t n = MIN(maxPayload - length, width - col);
      memcpy(&packet[length], pixels + (uint32_t)row * stride + col, n);
      length += n;
      col += n;
      if (col == width) {
        col = 0;
        row++;
      }
    }
    if (!sendPacket(length)) {
      return;  // disconnected (EVENT_BLE_DISCONNECTED follows) or stalled
    }
    startOfFrame = false;
    sequenceNumber++;
  }
}
