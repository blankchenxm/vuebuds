/*
 * nRF5340 port of fw/capture/capture.h: MCLK, frame pool, and 1-bit SPIS reception.
 * Same API and the same hardware chip-select scheme as fw/ (PR #32), built on DPPI
 * instead of PPI (channel map in capture.c).
 *
 * Every chip-select edge comes from hardware (DPPI), so no interrupt has to meet a
 * line-blanking deadline. TIMER1 counts LVLD falls (cleared on each FVLD edge) and the
 * CS pin is a GPIOTE task pin:
 *   start      HM01B0: right after line 0 (skipped on purpose, as VueBuds' 312 us delay did);
 *              HM0360: the FVLD edge (line 0 starts 750 us later)      -> CS low, segment 0
 *   boundary   line count = boundary -> CS high, TIMER2 -> CS low 2 us later -> next segment
 *   last line  line count = transport_height -> CS high -> EVENT_CAMERA_CAPTURE_DONE
 * EasyDMA moves at most 65,535 bytes per transfer, so the received lines are split into
 * whole-line segments that land back to back in one slot of the frame pool. The SPIS keeps
 * its semaphore between segments and reads RXD.PTR at the start of each one, so the CPU
 * writes the next segment's pointer while the current one is being received.
 *
 * HM0360 S2 must start on a frame boundary: XSLEEP is a GPIOTE task pin too, pulled low by
 * the FVLD fall through DPPI (capture_xsleep_on_frame_boundary).
 */
#ifndef CAPTURE_H_
#define CAPTURE_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "camera_sensor.h"

#define CAPTURE_MAX_SEGMENTS 8  // VGA: 480 lines x 640 B = 5 segments of 96 lines

typedef struct {
  uint32_t frame;          // frames completed since capture_init
  uint8_t segments;
  uint32_t segment_bytes[CAPTURE_MAX_SEGMENTS];  // bytes DMA received per segment
  bool overflow;           // SPIS dropped bytes because a segment buffer was full
  bool ok;                 // every segment got exactly its lines and nothing overflowed
  uint32_t duration_us;    // FVLD rise to last segment done
  uint32_t arm_to_fvld_us; // arming to the first FVLD rise (incl. skipped frames)
} capture_stats_t;

/* MCLK (TIMER0 + DPPI + GPIOTE). init also starts the clock. */
void capture_mclk_init(void);
void capture_mclk_enable(bool enable);
void capture_mclk_shutdown(void);

/* Frame pool. init returns the number of slots of frame_bytes that fit; slot + dma_offset
 * (where the SPIS DMA starts writing) is word aligned, as the nRF5340 SPIS needs. */
uint32_t frame_pool_init(uint32_t frame_bytes, uint32_t dma_offset);
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

/* Fatal check for setup calls (kept when asserts are compiled out). */
#define CAPTURE_CHECK(expr)                                                     \
  do {                                                                          \
    if (!(expr)) {                                                              \
      printk("[cap] check failed: %s (%s:%d)\n", #expr, __FILE__, __LINE__);    \
      k_panic();                                                                \
    }                                                                           \
  } while (0)

/* Shared with capture_mclk.c: the GPIOTE instance of the Zephyr GPIO driver, and DPPI helpers. */
void *capture_gpiote(void);
void capture_dppi_publish(uint32_t event_address, uint8_t channel);
void capture_dppi_subscribe(uint32_t task_address, uint8_t channel);

/* DPPI channels (fixed; nothing else on the application core uses DPPI). */
#define CAPTURE_DPPI_MCLK      16
#define CAPTURE_DPPI_LVLD      17
#define CAPTURE_DPPI_FVLD      18
#define CAPTURE_DPPI_PULSE     19
#define CAPTURE_DPPI_START     20
#define CAPTURE_DPPI_BOUNDARY  21
#define CAPTURE_DPPI_CS_HIGH   22
#define CAPTURE_DPPI_END       23
#define CAPTURE_DPPI_SLEEP     24
/* DPPI channel groups */
#define CAPTURE_GROUP_ARM      3
#define CAPTURE_GROUP_RUN      4
#define CAPTURE_GROUP_SLEEP    5

#endif /* CAPTURE_H_ */
