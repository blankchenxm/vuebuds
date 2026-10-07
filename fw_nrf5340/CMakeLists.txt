cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(vuebuds_nrf5340)

# Compile-time choices, same names and values as fw/Makefile. dk5340.ps1 passes them as
# -Dfw_nrf5340_<var>=... (sysbuild hands image-prefixed variables to this image only).
#   CAM_MODE          QVGA (default) or QQVGA
#   CAM_TEST_PATTERN  OFF (default), COLOR_BAR or WALKING_1
#   CAM_APP           STREAM (default) or MONITOR
#   EXTRA_DEFINES     extra defines, ';'-separated (like fw/'s CFLAGS=-DXXX)
foreach(var CAM_MODE CAM_TEST_PATTERN CAM_APP EXTRA_DEFINES)
  zephyr_get(${var} SYSBUILD LOCAL)
endforeach()
if(NOT CAM_MODE)
  set(CAM_MODE QVGA)
endif()
if(NOT CAM_TEST_PATTERN)
  set(CAM_TEST_PATTERN OFF)
endif()
if(NOT CAM_APP)
  set(CAM_APP STREAM)
endif()
message(STATUS "vuebuds: CAM_MODE=${CAM_MODE} CAM_TEST_PATTERN=${CAM_TEST_PATTERN} CAM_APP=${CAM_APP} EXTRA_DEFINES=${EXTRA_DEFINES}")

set(FW ${CMAKE_CURRENT_SOURCE_DIR}/../fw)

target_compile_definitions(app PRIVATE
  CAMERA_MODE=CAMERA_MODE_${CAM_MODE}
  CAMERA_TEST_PATTERN=CAMERA_TEST_PATTERN_${CAM_TEST_PATTERN}
  ${EXTRA_DEFINES}
)
if(CAM_APP STREQUAL "MONITOR")
  target_compile_definitions(app PRIVATE CAMERA_APP_MONITOR)
endif()

# Sensor drivers and register tables are shared with fw/ (not copied). They see the
# nRF5 SDK headers they include (i2c.h, timers.h, nrf_log.h, ...) through src/compat.
target_include_directories(app PRIVATE
  src
  src/compat
  src/capture
  ${FW}/sensors
  ${FW}/sensors/hm01b0/include
  ${FW}/sensors/hm0360/include
)
target_sources(app PRIVATE
  ${FW}/sensors/camera_sensor.c
  ${FW}/sensors/hm01b0/hm01b0_mode_info.c
  ${FW}/sensors/hm01b0/hm01b0_modes.c
  ${FW}/sensors/hm01b0/hm01b0_reg.c
  ${FW}/sensors/hm01b0/hm01b0_sensor.c
  ${FW}/sensors/hm0360/hm0360_mode_info.c
  ${FW}/sensors/hm0360/hm0360_modes.c
  ${FW}/sensors/hm0360/hm0360_reg.c
  ${FW}/sensors/hm0360/hm0360_sensor.c
)
file(GLOB APP_SOURCES src/*.c src/compat/*.c src/capture/*.c)
target_sources(app PRIVATE ${APP_SOURCES})
