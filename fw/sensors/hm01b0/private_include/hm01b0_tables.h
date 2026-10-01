#ifndef HM01B0_TABLES_H
#define HM01B0_TABLES_H

#include <stddef.h>

#include "hm01b0_types.h"

extern const hm01b0_regval_t hm01b0_base_init[];
extern const size_t hm01b0_base_init_count;

extern const hm01b0_regval_t hm01b0_mode_qvga[];
extern const size_t hm01b0_mode_qvga_count;
extern const hm01b0_regval_t hm01b0_mode_qqvga[];
extern const size_t hm01b0_mode_qqvga_count;

extern const hm01b0_regval_t hm01b0_common_init[];
extern const size_t hm01b0_common_init_count;

extern const hm01b0_regval_t hm01b0_interface_1bit[];
extern const size_t hm01b0_interface_1bit_count;

#endif /* HM01B0_TABLES_H */
