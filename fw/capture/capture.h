/*
 * nRF capture path shared by all sensors: MCLK, frame pool, and 1-bit SPIS reception.
 *
 * Every chip-select edge comes from hardware (PPI), so no interrupt has to meet a
 * line-blanking deadline (SoftDevice can delay ours by hundreds of us). TIMER2 counts
 * LVLD falls (cleared on each FVLD edge) and the CS pin is a GPIOTE task pin:
 *   start      HM01B0: right after line 0 (skipped on purpose, as VueBuds' 312 us delay did);
 *              HM0360: the FVLD edge (line 0 starts 750 us later)      -> CS low, segment 0
 *   boundary   line count = boundary -> CS high, TIMER4 -> CS low 2 us later -> next segment
 *   last line  line count = transport_height -> CS high -> EVENT_CAMERA_CAPTURE_DONE
 * EasyDMA moves at most 65,535 bytes per transfer, so the received lines are split into
 * whole-line segments that land back to back in one slot of the frame pool. The SPIS keeps
 * its semaphore between segments and reads RXD.PTR at the start of each one, so the CPU
 * writes the next segment's pointer while the current one is being received.
 *
 * HM0360 S2 must start on a frame boundary: XSLEEP is a GPIOTE task pin too, pulled low by
 * the FVLD fall through PPI (capture_xsleep_on_frame_boundary).
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

/* Arm reception of the next frame into slot 0 (the next start trigger). */
void capture_arm(void);

/* Arm reception into a given pool slot, letting skip_frames whole frames pass first. */
void capture_arm_slot(uint32_t slot, uint8_t skip_frames);

/* The slot last armed, holding the last completed frame (transport_width x transport_height). */
uint8_t *capture_frame(void);

/* XSLEEP now, or low on the next FVLD fall (frame boundary) -> EVENT_CAMERA_ASLEEP. */
void capture_xsleep(bool high);
bool capture_xsleep_on_frame_boundary(void);  // true: right after a last line, boundary ~13 ms away
bool capture_mclk_running(void);
const capture_stats_t *capture_stats(void);

#endif /* CAPTURE_H_ */
