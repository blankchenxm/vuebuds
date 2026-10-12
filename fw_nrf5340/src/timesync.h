/*
 * Time sync with the BLE-USB bridge (plan E, after nrf/samples/bluetooth/conn_time_sync):
 *   - local clock: RTC0 of the application core, cleared by the network core when its
 *     controller RTC starts (IPC channel 4, CONFIG_MPSL_TRIGGER_IPC_TASK_ON_RTC_START), so it
 *     counts the controller's microseconds (30.5 us steps);
 *   - the controller reports every connection event's anchor point (time, event counter);
 *     the bridge sends its own (time, counter) of an event: 0xB5 reference. Both sides time
 *     the same air event, so bridge time <-> local time follows from the latest local anchor;
 *   - RTC0 COMPARE0 -> DPPI -> XSLEEP SET wakes the sensor at a bridge time (0xB7);
 *   - every FVLD edge -> DPPI -> RTC0 CAPTURE2: the hardware time of the frame's FVLD rise.
 * Commands (control characteristic, little endian):
 *   B5 <bridge anchor us u64> <event counter u16>                      reference only
 *   B7 <bridge anchor us u64> <event counter u16> <bridge time T u64>  take a picture at T
 * Uses RTC0 (so not with CAPTURE_STRESS_US) and DPPI channels 25 / 26.
 */
#ifndef TIMESYNC_H_
#define TIMESYNC_H_

#include <stdbool.h>
#include <stdint.h>

#include "ble.h"

#define TIMESYNC_CMD_REFERENCE 0xB5
#define TIMESYNC_CMD_SNAPSHOT_AT 0xB7

void timesyncBleReady(void);       // after bt_enable(): anchor point reports on
uint64_t timesyncLocalUs(void);    // controller clock (RTC0), microseconds
bool timesyncHasReference(void);
bool timesyncBridgeToLocal(uint64_t bridge_us, uint64_t *local_us);
bool timesyncLocalToBridge(uint64_t local_us, uint64_t *bridge_us);
/* RTC0 time of the last FVLD edge (hardware capture), microseconds; read before the next edge. */
uint64_t timesyncLastFvldUs(void);
/* XSLEEP high at this local time (hardware); false if too close or too far away. */
bool timesyncWakeAt(uint64_t local_us);
void timesyncCancelWake(void);
/* 0xB5 / 0xB7; for 0xB7 returns true and the local time of T in *local_us. */
bool timesyncCommand(const ble_command_t *cmd, uint64_t *local_us);

#endif
