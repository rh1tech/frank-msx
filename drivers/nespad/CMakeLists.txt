#
# frank-msx — fMSX for RP2350
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-msx
# SPDX-License-Identifier: GPL-3.0-or-later
#

add_library(nespad INTERFACE)

target_sources(nespad INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/nespad.c
)

target_include_directories(nespad INTERFACE ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(nespad INTERFACE
    hardware_pio
    hardware_gpio
)
