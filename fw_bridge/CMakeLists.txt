cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(vuebuds_bridge)

# -Dfw_bridge_USB_TEST=1: USB link test build (src/usb_test.c, host/usb_test.py) instead of the bridge.
zephyr_get(USB_TEST SYSBUILD LOCAL)
if(USB_TEST)
  target_compile_definitions(app PRIVATE USB_TEST)
endif()

target_sources(app PRIVATE src/main.c src/usb.c src/usb_test.c src/central.c)
