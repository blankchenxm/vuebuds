#ifndef HM0360_PRIVATE_H
#define HM0360_PRIVATE_H

#include "hm0360.h"

/* Single sensor per board, so the driver keeps one static device instead of a handle. */
typedef struct {
  hm0360_state_t state;
  camera_mode_t mode;
  hm0360_interface_t interface;
  camera_test_pattern_t test_pattern;
} hm0360_dev_t;

extern hm0360_dev_t hm0360_dev;

#endif /* HM0360_PRIVATE_H */
