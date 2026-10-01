#ifndef HM01B0_TYPES_H
#define HM01B0_TYPES_H

#include <stdint.h>

#include "camera_sensor.h"

typedef enum {
  HM01B0_STATE_UNINITIALIZED = 0,
  HM01B0_STATE_STANDBY,
  HM01B0_STATE_STREAMING,
} hm01b0_state_t;

typedef enum {
  HM01B0_DATA_INTERFACE_8_BIT = 0,
  HM01B0_DATA_INTERFACE_4_BIT,
  HM01B0_DATA_INTERFACE_1_BIT,
} hm01b0_interface_t;

/* One register table entry. mask 0xFF writes value directly; other masks read-modify-write. */
typedef struct {
  uint16_t addr;
  uint8_t value;
  uint8_t mask;
  uint16_t delay_ms;
} hm01b0_regval_t;

#endif /* HM01B0_TYPES_H */
