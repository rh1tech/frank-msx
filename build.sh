#!/bin/bash
#
# build.sh - build frank-msx for RP2350
#
# Usage: ./build.sh [M2] [CPU_MHZ] [PSRAM_MHZ] [FLASH_MHZ]
# Only M2 is supported right now. Defaults: 252 / 133 / 66.
#
# Env vars:
#   USB_HID     0 = PS/2 only + USB CDC stdio   (default for dev builds)
#               1 = USB HID host keyboard/mouse/gamepad; CDC stdio off
#   HDMI_HSTX   0 = legacy PIO HDMI driver (default)
#               1 = RP2350 HSTX HDMI driver (with HDMI audio over data islands)
#
set -e

rm -rf ./build
mkdir build
cd build

BOARD_VARIANT="${1:-M2}"
: "${CPU_SPEED:=${2:-252}}"
: "${PSRAM_SPEED:=${3:-133}}"
: "${FLASH_SPEED:=${4:-66}}"
: "${MSX_MODEL:=${5:-3}}"   # 1 = MSX1, 2 = MSX2, 3 = MSX2+ (default)
: "${USB_HID:=0}"           # 0 = off (dev), 1 = on (release)
: "${HDMI_HSTX:=0}"         # 0 = PIO HDMI (default), 1 = HSTX HDMI+audio

if [[ "$USB_HID" == "1" || "$USB_HID" == "ON" || "$USB_HID" == "on" ]]; then
    USB_HID_CMAKE=ON
else
    USB_HID_CMAKE=OFF
fi

if [[ "$HDMI_HSTX" == "1" || "$HDMI_HSTX" == "ON" || "$HDMI_HSTX" == "on" ]]; then
    HDMI_HSTX_CMAKE=ON
else
    HDMI_HSTX_CMAKE=OFF
fi

cmake \
    -DPICO_PLATFORM=rp2350 \
    -DPICO_BOARD=pico2 \
    -DBOARD_VARIANT=${BOARD_VARIANT} \
    -DCPU_SPEED=${CPU_SPEED} \
    -DPSRAM_SPEED=${PSRAM_SPEED} \
    -DFLASH_SPEED=${FLASH_SPEED} \
    -DMSX_MODEL=${MSX_MODEL} \
    -DUSB_HID_ENABLED=${USB_HID_CMAKE} \
    -DHDMI_HSTX=${HDMI_HSTX_CMAKE} \
    ..

make -j$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
