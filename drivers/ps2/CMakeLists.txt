#
# frank-msx — fMSX for RP2350
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-msx
# SPDX-License-Identifier: GPL-3.0-or-later
#

# PS/2 Unified Driver
# PIO-based keyboard + mouse (separate state machines).
# ps2.c is the low-level driver (tracks murmsnes); the
# ps2kbd_wrapper.* pair adapts it to the keyboard-event API that
# the MSX platform consumes.

add_library(ps2_driver STATIC
    ${CMAKE_CURRENT_LIST_DIR}/ps2.c
    ${CMAKE_CURRENT_LIST_DIR}/ps2kbd_wrapper.c
)

pico_generate_pio_header(ps2_driver ${CMAKE_CURRENT_LIST_DIR}/ps2.pio)

target_include_directories(ps2_driver PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../../src
)

target_link_libraries(ps2_driver PUBLIC
    pico_stdlib
    hardware_pio
    hardware_gpio
    hardware_clocks
    hardware_irq
    hardware_sync
)

if(USB_HID_ENABLED)
    target_compile_definitions(ps2_driver PRIVATE USB_HID_ENABLED=1)
endif()
