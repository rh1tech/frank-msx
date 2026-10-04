#!/bin/bash
#
# frank-msx — fMSX for RP2350
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-msx
# SPDX-License-Identifier: GPL-3.0-or-later
#

# Flash frank-msx to connected Pico 2 (RP2350)

# Default: the newest ELF that CMake wrote into bin/<build type>/
DEFAULT_FW=$(ls -t ./bin/*frank-msx*.elf ./bin/*/*frank-msx*.elf 2>/dev/null | head -1)
FIRMWARE="${1:-${DEFAULT_FW:-./bin/frank-msx.elf}}"

if [ ! -f "$FIRMWARE" ]; then
    FIRMWARE="${FIRMWARE%.elf}.uf2"
    if [ ! -f "$FIRMWARE" ]; then
        echo "Error: Firmware file not found"
        echo "Usage: $0 [firmware.elf|firmware.uf2]"
        echo "Default: the newest bin/*/*frank-msx*.elf"
        exit 1
    fi
fi

echo "Flashing: $FIRMWARE"
picotool load -f "$FIRMWARE" && picotool reboot -f
