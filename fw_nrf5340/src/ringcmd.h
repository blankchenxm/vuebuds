/*
 * M2 commands (hub INTERFACES C2 / D, Xiaomeng 11-21; hub #68): ring buffer, status, fetch.
 * Control characteristic, little endian; times are the bridge's clock (u64 us, timesync.h).
 *   B8 02 <period u16 ms>        ring buffer: a picture at every multiple of the period, stored,
 *                                not sent (snapshot.h); again: new period            -> R_STATUS
 *   B8 00                        stop (timed capture or ring); frames stay           -> R_STATUS
 *   B8 01 <period u16 ms>        timed capture (snapshot.h), no reply
 *   B9                           status                                              -> R_STATUS
 *   BA 00 <t u64>                the frame nearest to t                 -> R_FRAME + frame / R_NONE
 *   BA 01 <t u64>                the newest frame at or before t        -> R_FRAME + frame / R_NONE
 *   BA 02 <t1 u64> <t2 u64> <n u8>  frames in [t1, t2], oldest first, at most n (<= 32)
 *                                -> (R_FRAME + frame) x k, R_DONE k / R_NONE
 *   BA 03 <frame number u32>     by frame number                        -> R_FRAME + frame / R_NONE
 * 0xBB (time query fallback) is not implemented; the number stays reserved.
 *
 * Replies: one notification each on the data characteristic, [seq, 0x04, type, ...] (ble.h),
 * sent between frames by the ring sender thread, which also sends the fetched frames (lower
 * priority than the main thread, so the ring keeps capturing while a fetch goes out).
 *   01 R_STATUS  state u8 (0 stopped / 1 timed / 2 ring) . period u16 . format u8 . quality u8 .
 *                frames u16 . oldest number u32 . oldest time u64 . newest number u32 . newest
 *                time u64 . now u64 . used KB u16 . total KB u16 . skipped u16 . overwritten u32 .
 *                PSRAM state u8 (0 ok, 1 no ID, 2 bad at once, 3 bad after 2 s; within 2 s of
 *                boot the reply waits for the check) . synced u8
 *   02 R_FRAME   number u32 . capture time u64 . length u32; the frame follows (header v2:
 *                frame_id = number low 16 bits, capture_us = time low 32 bits, flags + 0x08)
 *   03 R_NONE    reason u8 . oldest time u64 . newest time u64; reasons: 0 overwritten (before the
 *                oldest), 1 not taken yet (after the newest), 2 ring empty, 3 busy (previous fetch
 *                still going out), 4 bad arguments, 5 not synced
 *   04 R_DONE    count u8
 */
#ifndef RINGCMD_H_
#define RINGCMD_H_

#include <stdbool.h>
#include <stdint.h>

#include "ble.h"
#include "camera.h"

#define RINGCMD_STATUS 0xB9
#define RINGCMD_FETCH  0xBA

#define R_STATUS 0x01
#define R_FRAME  0x02
#define R_NONE   0x03
#define R_DONE   0x04

#define R_NONE_OVERWRITTEN 0
#define R_NONE_NOT_YET     1
#define R_NONE_EMPTY       2
#define R_NONE_BUSY        3
#define R_NONE_BAD_ARGS    4
#define R_NONE_NOT_SYNCED  5

#define RINGCMD_RANGE_MAX 32

void ringcmdInit(void);  // main thread, after the PSRAM boot check (or on the first use)
/* 0xB8 / 0xB9 / 0xBA (main thread). */
void ringcmdCommand(const ble_command_t *cmd);
/* The prepared picture (imagePrepare) into the ring in the 0xB3 format (PNG: stored raw);
 * send: then R_FRAME + the frame (0xB2 while the ring runs). False if not stored. */
bool ringcmdStore(const camera_frame_t *frame, bool send);

#endif
