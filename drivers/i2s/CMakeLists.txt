#
# frank-msx — fMSX for RP2350
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-msx
# SPDX-License-Identifier: GPL-3.0-or-later
#

# I2S audio driver (copied verbatim from murmnes drivers/i2s/).
# murmnes compiles the .c + .pio directly into its executable; here we
# wrap them into an OBJECT-style library so the top-level CMakeLists can
# link a single `i2s_audio` target.

add_library(i2s_audio STATIC
    ${CMAKE_CURRENT_LIST_DIR}/i2s_audio.c
)

pico_generate_pio_header(i2s_audio ${CMAKE_CURRENT_LIST_DIR}/audio_i2s.pio)

target_include_directories(i2s_audio PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(i2s_audio
    pico_stdlib
    hardware_pio
    hardware_dma
    hardware_gpio
    hardware_clocks
    hardware_irq
)
