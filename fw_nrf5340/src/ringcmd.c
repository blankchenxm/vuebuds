#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>

#include "nrf_log.h"

#include "ble.h"
#include "camera.h"
#include "image.h"
#include "psram.h"
#include "ring.h"
#include "snapshot.h"
#include "timers.h"
#include "timesync.h"
#include "ringcmd.h"

#define TX_STACK 2048
#define TX_PRIORITY K_PRIO_PREEMPT(5)  // below the main thread (0): capture first

typedef enum { JOB_FETCH, JOB_SEND } job_type_t;

typedef struct {
  uint8_t type;   // job_type_t
  uint8_t mode;   // 0xBA mode
  uint8_t n;
  uint32_t seq;
  uint64_t t1, t2;
} job_t;

typedef struct {
  uint8_t type;  // R_STATUS: built when sent; R_NONE: reason
  uint8_t reason;
} reply_t;

K_MSGQ_DEFINE(ringcmdJobs, sizeof(job_t), 4, 4);
K_MSGQ_DEFINE(ringcmdReplies, sizeof(reply_t), 8, 4);
K_SEM_DEFINE(ringcmdWake, 0, 64);

static bool m_inited;
static uint32_t m_seq;               // ring frame numbers, from 1 (since boot)
static uint32_t m_period_ms;         // of the last ring start (for "nearest" at the ends)
static volatile bool m_fetch_busy;   // a 0xBA is queued or going out

static uint64_t le64(const uint8_t *p)
{
  return sys_get_le64(p);
}

static uint32_t le32(const uint8_t *p)
{
  return sys_get_le32(p);
}

void ringcmdInit(void)
{
  if (m_inited) {
    return;
  }
  if (psramCheckFinish()) {  // boot check (psram.h): waits until 2 s after boot
    ringInit(NULL, 0);
    m_inited = true;
    return;
  }
  // No PSRAM: the RAM past slot 0 and the encoders, once the camera has set up the frame pool.
  uint32_t ram_bytes = 0;
  uint8_t *ram = imageCodecReady() ? imageRam(&ram_bytes) : NULL;
  if (ram) {
    ringInit(ram, ram_bytes);
    m_inited = true;
  }
}

static void queueReply(uint8_t type, uint8_t reason)
{
  reply_t r = {type, reason};
  if (k_msgq_put(&ringcmdReplies, &r, K_NO_WAIT) != 0) {
    NRF_LOG_RAW_INFO("%08d [ringcmd] reply queue full, reply 0x%02x dropped\n", systemTimeGetMs(), type);
    return;
  }
  k_sem_give(&ringcmdWake);
}

static bool queueJob(const job_t *j)
{
  if (k_msgq_put(&ringcmdJobs, j, K_NO_WAIT) != 0) {
    return false;
  }
  k_sem_give(&ringcmdWake);
  return true;
}

static bool bridgeNow(uint64_t *now)
{
  uint64_t l = timesyncLocalUs();
  if (timesyncLocalToBridge(l, now)) {
    return true;
  }
  *now = l;
  return false;
}

static uint8_t psramCode(void)
{
  switch (psramState()) {
    case PSRAM_STATE_OK: return 0;
    case PSRAM_STATE_NO_ID: return 1;
    case PSRAM_STATE_BAD_AT_ONCE: return 2;
    case PSRAM_STATE_BAD_LATER: return 3;
    default: return 0xFF;  // still checking (sendStatus waits for the result)
  }
}

/* ---------------- main thread ---------------- */

bool ringcmdStore(const camera_frame_t *frame, bool send)
{
  ringcmdInit();
  if (!imageCodecReady()) {  // the encoders need the frame pool (camera started)
    return false;
  }
  uint64_t t;
  if (!imageCaptureTime(&t)) {
    NRF_LOG_RAW_INFO("%08d [ring] not stored: no time reference\n", systemTimeGetMs());
    return false;
  }
  const ble_frame_header_t *h = imageHeader();
  ring_meta_t meta = {.integration = h->integration, .digital_gain = h->digital_gain,
                      .analog_gain = h->analog_gain, .flags = h->flags | BLE_HEADER_FROM_RING};
  uint8_t format, quality;
  imageFormat(&format, &quality);
  uint32_t seq = m_seq + 1, us;
  bool jpeg = format == BLE_FORMAT_JPEG;
  bool ok = jpeg ? ringAppendJpeg(frame, quality, seq, t, &meta, &us) : ringAppend(frame, seq, t, &meta, &us);
  if (!ok) {
    NRF_LOG_RAW_INFO("%08d [ring] frame %u NOT stored (write errors %u, refused for the frame being sent %u)\n",
                     systemTimeGetMs(), seq, ringStats()->write_errors, ringStats()->pinned_fails);
    return false;
  }
  m_seq = seq;
  const ring_entry_t *e = ringNewest();
  // host/m2_test.py reads these lines (RTT) to check fetched frames against the stored CRC.
  NRF_LOG_RAW_INFO("%08d [ring] stored %u t %u.%03u ms: %s%u %u B crc %08x, %s %u us, %u frames %u KB\n",
                   systemTimeGetMs(), seq, (uint32_t)(t / 1000000), (uint32_t)(t / 1000 % 1000), jpeg ? "q" : "raw",
                   jpeg ? quality : 0, e->len, e->crc, jpeg ? "encode + write" : "write", us, ringCount(),
                   ringStats()->used_bytes / 1024);
  if (send) {
    job_t j = {.type = JOB_SEND, .seq = seq};
    if (!queueJob(&j)) {
      NRF_LOG_RAW_INFO("%08d [ringcmd] job queue full, frame %u not sent\n", systemTimeGetMs(), seq);
    }
  }
  return true;
}

static void fetchCommand(const uint8_t *d, uint8_t len)
{
  job_t j = {.type = JOB_FETCH, .mode = len >= 2 ? d[1] : 0xFF};
  bool ok;
  switch (j.mode) {
    case 0:
    case 1:
      ok = len >= 10;
      if (ok) {
        j.t1 = le64(d + 2);
      }
      break;
    case 2:
      ok = len >= 19;
      if (ok) {
        j.t1 = le64(d + 2);
        j.t2 = le64(d + 10);
        j.n = d[18];
        ok = j.t1 <= j.t2 && j.n >= 1 && j.n <= RINGCMD_RANGE_MAX;
      }
      break;
    case 3:
      ok = len >= 6;
      if (ok) {
        j.seq = le32(d + 2);
      }
      break;
    default:
      ok = false;
  }
  if (!ok) {
    NRF_LOG_RAW_INFO("%08d [ringcmd] bad fetch command (mode %u, %u bytes)\n", systemTimeGetMs(), j.mode, len);
    queueReply(R_NONE, R_NONE_BAD_ARGS);
    return;
  }
  if (!timesyncHasReference()) {
    queueReply(R_NONE, R_NONE_NOT_SYNCED);
    return;
  }
  if (m_fetch_busy) {
    queueReply(R_NONE, R_NONE_BUSY);
    return;
  }
  m_fetch_busy = true;
  if (!queueJob(&j)) {
    m_fetch_busy = false;
    queueReply(R_NONE, R_NONE_BUSY);
  }
}

void ringcmdCommand(const ble_command_t *cmd)
{
  const uint8_t *d = cmd->data;
  switch (d[0]) {
    case SNAPSHOT_CMD_TIMED: {
      uint8_t mode = cmd->len >= 2 ? d[1] : 0xFF;
      if (mode == 1) {
        snapshotTimedCommand(d, cmd->len);  // no reply (F5)
        break;
      }
      if (mode == 0) {
        snapshotStop();
        queueReply(R_STATUS, 0);
        break;
      }
      uint16_t period = cmd->len >= 4 ? sys_get_le16(d + 2) : 0;
      if (mode != 2 || period < SNAPSHOT_PERIOD_MIN_MS || period > SNAPSHOT_PERIOD_MAX_MS) {
        NRF_LOG_RAW_INFO("%08d [ringcmd] bad 0xB8: mode %u, period %u ms (%u bytes)\n", systemTimeGetMs(), mode, period,
                         cmd->len);
        queueReply(R_NONE, R_NONE_BAD_ARGS);
        break;
      }
      if (!timesyncHasReference()) {
        queueReply(R_NONE, R_NONE_NOT_SYNCED);
        break;
      }
      ringcmdInit();
      if (snapshotMode() != SNAPSHOT_MODE_RING) {
        ringReset();  // a new start empties the ring
      }
      m_period_ms = period;
      snapshotRingStart(period);
      queueReply(R_STATUS, 0);
      break;
    }
    case RINGCMD_STATUS:
      queueReply(R_STATUS, 0);
      break;
    case RINGCMD_FETCH:
      fetchCommand(d, cmd->len);
      break;
  }
}

/* ---------------- ring sender thread ---------------- */

typedef struct __packed {
  uint8_t state;
  uint16_t period;
  uint8_t format;
  uint8_t quality;
  uint16_t frames;
  uint32_t oldest_seq;
  uint64_t oldest_us;
  uint32_t newest_seq;
  uint64_t newest_us;
  uint64_t now_us;
  uint16_t used_kb;
  uint16_t total_kb;
  uint16_t skipped;
  uint32_t overwritten;
  uint8_t psram;
  uint8_t synced;
  uint8_t camera;  // CAMERA_STATE_* (hub #92, added at the end: old hosts read the first 51 bytes)
} status_t;
BUILD_ASSERT(sizeof(status_t) == 52, "R_STATUS content is 52 bytes (INTERFACES C2)");

typedef struct __packed {
  uint8_t reason;
  uint64_t oldest_us;
  uint64_t newest_us;
} none_t;

typedef struct __packed {
  uint32_t seq;
  uint64_t capture_us;
  uint32_t length;
} frame_reply_t;

static bool psramSettled(void)
{
  psram_state_t st = psramState();
  return st != PSRAM_STATE_UNCHECKED && st != PSRAM_STATE_WAITING;
}

static void sendStatus(void)
{
  // Within 2 s of boot the PSRAM check is still running: wait for it (and the ring set up after
  // it, main thread), so the PSRAM state is one of INTERFACES C2's values (at most ~2.5 s).
  for (int i = 0; i < 50 && (!psramSettled() || (psramState() == PSRAM_STATE_OK && ringCapacity() == 0)); i++) {
    k_sleep(K_MSEC(50));
  }
  status_t s = {0};
  s.state = snapshotMode();
  s.period = snapshotPeriodMs();
  uint8_t format, quality;
  imageFormat(&format, &quality);
  s.format = format;
  s.quality = quality;
  ringLock();
  s.frames = (uint16_t)ringCount();
  const ring_entry_t *o = ringAt(0), *n = ringNewest();
  if (o) {
    s.oldest_seq = o->seq;
    s.oldest_us = o->capture_us;
    s.newest_seq = n->seq;
    s.newest_us = n->capture_us;
  }
  s.used_kb = (uint16_t)(ringStats()->used_bytes / 1024);
  s.total_kb = (uint16_t)(ringCapacity() / 1024);
  s.overwritten = ringStats()->evicted;
  ringUnlock();
  uint32_t skips = snapshotRingSkips();
  s.skipped = skips > UINT16_MAX ? UINT16_MAX : (uint16_t)skips;
  s.psram = psramCode();
  uint64_t now;
  s.synced = bridgeNow(&now);
  s.now_us = now;
  s.camera = cameraState();
  bleSendReply(R_STATUS, &s, sizeof(s));
  NRF_LOG_RAW_INFO("%08d [ringcmd] R_STATUS: state %u, period %u, %u frames (%u..%u), %u/%u KB, skipped %u, "
                   "overwritten %u, psram %u, synced %u, camera %u\n", systemTimeGetMs(), s.state, s.period, s.frames, s.oldest_seq,
                   s.newest_seq, s.used_kb, s.total_kb, s.skipped, s.overwritten, s.psram, s.synced, s.camera);
}

static void sendNone(uint8_t reason)
{
  none_t r = {.reason = reason};
  ringLock();
  if (ringCount()) {
    r.oldest_us = ringAt(0)->capture_us;
    r.newest_us = ringNewest()->capture_us;
  }
  ringUnlock();
  bleSendReply(R_NONE, &r, sizeof(r));
  NRF_LOG_RAW_INFO("%08d [ringcmd] R_NONE %u\n", systemTimeGetMs(), reason);
}

static void drainReplies(void)
{
  reply_t r;
  while (k_msgq_get(&ringcmdReplies, &r, K_NO_WAIT) == 0) {
    if (r.type == R_STATUS) {
      sendStatus();
    } else {
      sendNone(r.reason);
    }
  }
}

typedef struct {
  ring_entry_t e;
  uint32_t crc;
} send_ctx_t;

static size_t readEntry(void *ctx, uint32_t offset, uint8_t *dst, size_t n)
{
  send_ctx_t *c = ctx;
  if (!ringRead(&c->e, offset, dst, n)) {
    return 0;
  }
  c->crc = crc32_ieee_update(c->crc, dst, n);
  return n;
}

// R_FRAME + the frame with this number; false if it is gone or the link failed.
static bool sendSeq(uint32_t seq)
{
  send_ctx_t c = {0};
  ringLock();
  const ring_entry_t *e = ringFindSeq(seq);
  if (e) {
    c.e = *e;
    ringPin(seq);
  }
  ringUnlock();
  if (!e) {
    NRF_LOG_RAW_INFO("%08d [ringcmd] frame %u no longer in the ring\n", systemTimeGetMs(), seq);
    return false;
  }
  frame_reply_t fr = {.seq = c.e.seq, .capture_us = c.e.capture_us, .length = c.e.len};
  ble_frame_header_t h = {
    .width = c.e.width, .height = c.e.height, .format = c.e.format, .quality = c.e.quality, .length = c.e.len,
    .capture_us = (uint32_t)c.e.capture_us, .frame_id = (uint16_t)c.e.seq, .integration = c.e.integration,
    .analog_gain = c.e.analog_gain, .digital_gain = c.e.digital_gain, .flags = c.e.flags | BLE_HEADER_FROM_RING,
  };
  uint32_t t0 = systemTimeGetMs();
  bleLock();
  bool ok = bleSendReply(R_FRAME, &fr, sizeof(fr)) && bleSendImage(&h, readEntry, &c);
  bleUnlock();
  ringPin(0);
  NRF_LOG_RAW_INFO("%08d [ringcmd] sent %u (%u B) in %u ms: crc %08x %s\n", systemTimeGetMs(), seq, c.e.len,
                   systemTimeGetMs() - t0, c.crc, !ok ? "SEND FAILED" : (c.crc == c.e.crc ? "ok" : "CRC MISMATCH"));
  return ok;
}

static void fetch(const job_t *j)
{
  uint32_t seqs[RINGCMD_RANGE_MAX];
  uint32_t count = 0;
  int reason = -1;
  uint64_t now;
  bridgeNow(&now);
  bool running = snapshotMode() == SNAPSHOT_MODE_RING;
  uint64_t half = (uint64_t)m_period_ms * 500;  // half a period, us

  ringLock();
  uint32_t n = ringCount();
  const ring_entry_t *o = ringAt(0), *nw = ringNewest();
  if (n == 0) {
    reason = R_NONE_EMPTY;
  } else {
    switch (j->mode) {
      case 0:  // nearest; a frame before the oldest / after the newest may be (have been) closer
        if (j->t1 + half < o->capture_us) {
          reason = R_NONE_OVERWRITTEN;
        } else if (running && j->t1 > nw->capture_us + half) {
          reason = R_NONE_NOT_YET;
        } else {
          seqs[count++] = ringFindNearest(j->t1)->seq;
        }
        break;
      case 1:  // at or before t; while the ring runs a t still to come may get a later frame
        if (j->t1 < o->capture_us) {
          reason = R_NONE_OVERWRITTEN;
        } else if (running && j->t1 > now) {
          reason = R_NONE_NOT_YET;
        } else {
          seqs[count++] = ringFindAtOrBefore(j->t1)->seq;
        }
        break;
      case 2:  // [t1, t2], oldest first, at most n
        for (uint32_t i = ringFirstAtOrAfter(j->t1); i < n && count < j->n && ringAt(i)->capture_us <= j->t2; i++) {
          seqs[count++] = ringAt(i)->seq;
        }
        if (count == 0) {
          reason = j->t2 < o->capture_us ? R_NONE_OVERWRITTEN : R_NONE_NOT_YET;  // inside: none taken in it
        }
        break;
      case 3:
        if (j->seq < o->seq) {
          reason = R_NONE_OVERWRITTEN;
        } else if (j->seq > nw->seq) {
          reason = R_NONE_NOT_YET;
        } else if (ringFindSeq(j->seq)) {
          seqs[count++] = j->seq;
        } else {
          reason = R_NONE_OVERWRITTEN;  // a number that was never stored (failed write)
        }
        break;
    }
  }
  ringUnlock();

  if (reason >= 0) {
    sendNone((uint8_t)reason);
    return;
  }
  uint8_t sent = 0;
  for (uint32_t i = 0; i < count; i++) {
    if (i > 0) {
      drainReplies();  // other replies between two frames
    }
    if (sendSeq(seqs[i])) {
      sent++;
    }
  }
  if (j->mode == 2) {
    bleSendReply(R_DONE, &sent, 1);
    NRF_LOG_RAW_INFO("%08d [ringcmd] R_DONE %u\n", systemTimeGetMs(), sent);
  } else if (sent == 0) {
    sendNone(R_NONE_OVERWRITTEN);  // overwritten between the lookup and the send
  }
}

static void txThread(void *a, void *b, void *c)
{
  for (;;) {
    k_sem_take(&ringcmdWake, K_FOREVER);
    drainReplies();
    job_t j;
    if (k_msgq_get(&ringcmdJobs, &j, K_NO_WAIT) == 0) {
      if (j.type == JOB_SEND) {
        sendSeq(j.seq);
      } else {
        fetch(&j);
        m_fetch_busy = false;
      }
      drainReplies();
    }
  }
}
K_THREAD_DEFINE(ringcmdTx, TX_STACK, txThread, NULL, NULL, NULL, TX_PRIORITY, 0, 0);
