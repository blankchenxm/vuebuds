#ifndef HM0360_TABLES_H
#define HM0360_TABLES_H

#include <stddef.h>

#include "hm0360_types.h"

extern const hm0360_regval_t hm0360_base_init[];
extern const size_t hm0360_base_init_count;

extern const hm0360_regval_t hm0360_mode_qvga[];
extern const size_t hm0360_mode_qvga_count;
extern const hm0360_regval_t hm0360_mode_qqvga[];
extern const size_t hm0360_mode_qqvga_count;

extern const hm0360_regval_t hm0360_interface_1bit[];
extern const size_t hm0360_interface_1bit_count;

#endif /* HM0360_TABLES_H */
