cmake_minimum_required(VERSION 3.17)

set(PICO_BOARD pico)
include(pico_sdk_import.cmake)

project(glide_pico C CXX ASM)
pico_sdk_init()

# Pico-PIO-USB host driver from the TinyUSB tree (fetched with tools/get_deps.py rp2040).
include(${PICO_TINYUSB_PATH}/hw/bsp/rp2040/family.cmake OPTIONAL RESULT_VARIABLE _fam)
if (NOT TARGET tinyusb_pico_pio_usb)
  set(TOP ${PICO_TINYUSB_PATH})
  check_and_add_pico_pio_usb_support()
endif()

# glide_pico:      development build (adds a CDC serial log and 1200-baud reboot to UF2)
# glide_pico_work: work build (keyboard + mouse + pointer only, re-flash needs BOOTSEL)
foreach(target glide_pico glide_pico_work)
  add_executable(${target}
    src/main.c
    src/usb_descriptors.c
    src/flashlog.c
  )
  target_include_directories(${target} PRIVATE src)
  target_link_libraries(${target} PRIVATE
    pico_stdlib
    hardware_flash
    tinyusb_device
    tinyusb_host
    tinyusb_pico_pio_usb
  )
  # Pico-PIO-USB has many warnings with -Wconversion and similar.
  target_compile_options(${target} PRIVATE -Wall -Wno-unused-function)
  pico_add_extra_outputs(${target})
endforeach()
target_compile_definitions(glide_pico_work PRIVATE GLIDE_NO_CDC=1)
