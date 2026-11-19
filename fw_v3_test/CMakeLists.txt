cmake_minimum_required(VERSION 3.20.0)

# Board bring-up test for balsamic3 (V3): ISP2053 (nRF5340) + MAX77650 + APS6404L + HM0360.
# Built for nrf5340dk/nrf5340/cpuapp with boards/nrf5340dk_nrf5340_cpuapp.overlay, which
# adapts the DK devicetree to the ISP2053 module. Results are printed on RTT.
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(balsamic3_hw_test)

target_sources(app PRIVATE src/main.c)
