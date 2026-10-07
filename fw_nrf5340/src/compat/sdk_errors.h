/* nRF5 SDK error codes used by the shared sensor drivers (fw/sensors). */
#ifndef SDK_ERRORS_H_
#define SDK_ERRORS_H_

#include <stdint.h>

typedef uint32_t ret_code_t;

#define NRF_SUCCESS                0
#define NRF_ERROR_INVALID_STATE    8
#define NRF_ERROR_NOT_FOUND        5
#define NRF_ERROR_NOT_SUPPORTED    6
#define NRF_ERROR_INVALID_PARAM    7

#endif
