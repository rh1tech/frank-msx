#!/bin/bash
#
# build.sh - build frank-msx for RP2350
#
# Usage: ./build.sh [M2] [CPU_MHZ] [PSRAM_MHZ] [FLASH_MHZ]
# Only M2 is supported right now. Defaults: 252 / 133 / 66.
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

cmake \
    -DPICO_PLATFORM=rp2350 \
    -DPICO_BOARD=pico2 \
    -DBOARD_VARIANT=${BOARD_VARIANT} \
    -DCPU_SPEED=${CPU_SPEED} \
    -DPSRAM_SPEED=${PSRAM_SPEED} \
    -DFLASH_SPEED=${FLASH_SPEED} \
    -DMSX_MODEL=${MSX_MODEL} \
    -DUSB_HID_ENABLED=OFF \
    ..

make -j$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
