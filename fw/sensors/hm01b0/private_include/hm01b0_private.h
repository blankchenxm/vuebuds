#ifndef HM01B0_PRIVATE_H
#define HM01B0_PRIVATE_H

#include "hm01b0.h"

/* Single sensor per board, so the driver keeps one static device instead of a handle. */
typedef struct {
  hm01b0_state_t state;
  camera_mode_t mode;
  hm01b0_interface_t interface;
  camera_test_pattern_t test_pattern;
} hm01b0_dev_t;

extern hm01b0_dev_t hm01b0_dev;

#endif /* HM01B0_PRIVATE_H */
