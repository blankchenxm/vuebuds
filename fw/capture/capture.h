/*
 * nRF capture path shared by all sensors: MCLK, frame pool, and 1-bit SPIS
 * reception with a software chip select driven from FVLD/LVLD.
 *
 *   FVLD rise  -> TIMER4, after fvld_to_cs_us CS low -> DMA segment 0 from first_line
 *   LVLD fall  -> at each segment boundary, CS high/low -> next DMA segment
 *   last line  -> TIMER2 counts LVLD falls via PPI; at transport_height lines CS high
 *                 -> last segment done -> EVENT_CAMERA_CAPTURE_DONE
 *   FVLD fall  -> fallback end (VueBuds ended here; HM0360 FVLD falls 13 ms after the
 *                 last line and only 54-65 us before the next frame)
 *
 * This is the VueBuds timing; on HM01B0 the CS delay skips line 0 on purpose.
 * EasyDMA moves at most 65,535 bytes per transfer, so the received lines are split
 * into whole-line segments that land back to back in one slot of the frame pool.
 */
#ifndef CAPTURE_H_
#define CAPTURE_H_

#include <stdbool.h>
#include <stdint.h>

#include "camera_sensor.h"

#define CAPTURE_MAX_SEGMENTS 4

typedef struct {
  uint32_t frame;          // frames completed since capture_init
  uint8_t segments;
  uint32_t segment_bytes[CAPTURE_MAX_SEGMENTS];  // bytes DMA received per segment
  bool overflow;           // SPIS dropped bytes because a segment buffer was full
  bool ok;                 // every segment got exactly its lines and nothing overflowed
  bool ended_by_line_count; // ended after the last line (false: by the FVLD fall fallback)
  uint32_t duration_us;    // FVLD rise to last segment done
  uint32_t arm_to_fvld_us; // arming to the first FVLD rise (incl. skipped frames)
} capture_stats_t;

/* MCLK (TIMER3 + PPI + GPIOTE). init also starts the clock. */
void capture_mclk_init(void);
void capture_mclk_enable(bool enable);
void capture_mclk_shutdown(void);

/* Frame pool. init returns the number of slots of frame_bytes that fit. */
uint32_t frame_pool_init(uint32_t frame_bytes);
uint32_t frame_pool_size(void);
uint32_t frame_pool_slot_count(void);
uint8_t *frame_pool_slot(uint32_t index);

/* Set up frame sync GPIOs, CS, SPIS and the pool for the given mode. */
void capture_init(const camera_mode_info_t *info);
void capture_uninit(void);

/* Arm reception of the next frame into slot 0; the frame starts at the next FVLD rise. */
void capture_arm(void);

/* Arm reception into a given pool slot, letting skip_frames whole frames pass first. */
void capture_arm_slot(uint32_t slot, uint8_t skip_frames);

/* The slot last armed, holding the last completed frame (transport_width x transport_height). */
uint8_t *capture_frame(void);
bool capture_mclk_running(void);
const capture_stats_t *capture_stats(void);

#endif /* CAPTURE_H_ */
