#
# frank-msx — fMSX for RP2350
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-msx
# SPDX-License-Identifier: GPL-3.0-or-later
#

add_library(tv_driver STATIC
    ${CMAKE_CURRENT_LIST_DIR}/tv-software.c
    ${CMAKE_CURRENT_LIST_DIR}/graphics.c
)

target_include_directories(tv_driver PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
)

target_compile_options(tv_driver PRIVATE
    -Wno-error
    -Wno-incompatible-pointer-types
    -Wno-all
)

target_link_libraries(tv_driver
    pico_stdlib
    hardware_dma
    hardware_pio
    hardware_clocks
)
