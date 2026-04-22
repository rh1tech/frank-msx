#!/bin/bash
# Flash frank-msx to connected Pico 2 (RP2350)

FIRMWARE="${1:-./build/frank-msx.elf}"

if [ ! -f "$FIRMWARE" ]; then
    FIRMWARE="${FIRMWARE%.elf}.uf2"
    if [ ! -f "$FIRMWARE" ]; then
        echo "Error: Firmware file not found"
        echo "Usage: $0 [firmware.elf|firmware.uf2]"
        echo "Default: ./build/frank-msx.elf"
        exit 1
    fi
fi

echo "Flashing: $FIRMWARE"
picotool load -f "$FIRMWARE" && picotool reboot -f
