#ifndef HM0360_TYPES_H
#define HM0360_TYPES_H

#include <stdint.h>

#include "camera_sensor.h"

typedef enum {
  HM0360_STATE_UNINITIALIZED = 0,
  HM0360_STATE_STANDBY,     // MODE_SELECT = Sleep1 (software standby)
  HM0360_STATE_STREAMING,
} hm0360_state_t;

typedef enum {
  HM0360_DATA_INTERFACE_8_BIT = 0,
  HM0360_DATA_INTERFACE_4_BIT,
  HM0360_DATA_INTERFACE_1_BIT,
} hm0360_interface_t;

/* One register table entry. mask 0xFF writes value directly; other masks read-modify-write. */
typedef struct {
  uint16_t addr;
  uint8_t value;
  uint8_t mask;
  uint16_t delay_ms;
} hm0360_regval_t;

#endif /* HM0360_TYPES_H */
