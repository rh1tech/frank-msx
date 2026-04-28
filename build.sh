#!/bin/bash
#
# build.sh - build frank-msx for RP2350
#
# Usage: ./build.sh [PLATFORM] [CPU_MHZ] [PSRAM_MHZ] [FLASH_MHZ] [MSX_MODEL]
#   PLATFORM: m1, m2 (default), dv, pc, z0
#
# Env vars:
#   PLATFORM         m1 | m2 (default) | dv | pc | z0
#   USB_HID          0 = PS/2 only + USB CDC stdio   (default for dev builds)
#                    1 = USB HID host keyboard/mouse/gamepad; CDC stdio off
#   HDMI_HSTX        0 = legacy PIO HDMI driver (default)
#                    1 = RP2350 HSTX HDMI driver (with HDMI audio) — M2 only
#   VIDEO_COMPOSITE  0 = HDMI / VGA output (default)
#                    1 = Software composite-TV output (PAL/NTSC) — M1/M2 only
#
set -e

rm -rf ./build
mkdir build
cd build

PLATFORM_IN="${PLATFORM:-${1:-m2}}"
# Accept uppercase aliases (legacy BOARD_VARIANT inputs like "M2").
PLATFORM=$(echo "$PLATFORM_IN" | tr '[:upper:]' '[:lower:]')

case "$PLATFORM" in
    m1|m2|dv|pc|z0) ;;
    *)
        echo "ERROR: Unknown PLATFORM='$PLATFORM_IN'. Expected one of: m1, m2, dv, pc, z0." >&2
        exit 1
        ;;
esac

: "${PSRAM_SPEED:=${3:-133}}"
: "${FLASH_SPEED:=${4:-66}}"
: "${MSX_MODEL:=${5:-3}}"      # 1 = MSX1, 2 = MSX2, 3 = MSX2+ (default)
: "${USB_HID:=0}"              # 0 = off (dev), 1 = on (release)
: "${HDMI_HSTX:=0}"            # 0 = PIO HDMI (default), 1 = HSTX HDMI+audio (M2 only)
: "${VIDEO_COMPOSITE:=0}"      # 0 = HDMI/VGA (default), 1 = composite TV (M1/M2 only)

to_onoff() {
    case "$1" in
        1|ON|on|YES|yes|TRUE|true) echo "ON" ;;
        *) echo "OFF" ;;
    esac
}

USB_HID_CMAKE=$(to_onoff "$USB_HID")
HDMI_HSTX_CMAKE=$(to_onoff "$HDMI_HSTX")
VIDEO_COMPOSITE_CMAKE=$(to_onoff "$VIDEO_COMPOSITE")

if [[ "$HDMI_HSTX_CMAKE" == "ON" && "$VIDEO_COMPOSITE_CMAKE" == "ON" ]]; then
    echo "ERROR: HDMI_HSTX=1 and VIDEO_COMPOSITE=1 are mutually exclusive." >&2
    exit 1
fi

if [[ "$HDMI_HSTX_CMAKE" == "ON" && "$PLATFORM" != "m2" ]]; then
    echo "ERROR: HDMI_HSTX=1 is only supported on PLATFORM=m2 (got '$PLATFORM')." >&2
    exit 1
fi

if [[ "$VIDEO_COMPOSITE_CMAKE" == "ON" && "$PLATFORM" != "m2" && "$PLATFORM" != "m1" ]]; then
    echo "ERROR: VIDEO_COMPOSITE=1 is only supported on PLATFORM=m1 or m2 (got '$PLATFORM')." >&2
    exit 1
fi

# CPU speed: composite-TV defaults to 378 MHz (the scanline alarm path
# needs the extra headroom), everything else runs fine at 252 MHz.
# Override with CPU_SPEED env var or positional arg $2.
if [[ -z "${CPU_SPEED:-}" && -z "${2:-}" ]]; then
    if [[ "$VIDEO_COMPOSITE_CMAKE" == "ON" ]]; then
        CPU_SPEED=378
    else
        CPU_SPEED=252
    fi
else
    : "${CPU_SPEED:=${2:-252}}"
fi

cmake \
    -DPICO_PLATFORM=rp2350 \
    -DPLATFORM=${PLATFORM} \
    -DCPU_SPEED=${CPU_SPEED} \
    -DPSRAM_SPEED=${PSRAM_SPEED} \
    -DFLASH_SPEED=${FLASH_SPEED} \
    -DMSX_MODEL=${MSX_MODEL} \
    -DUSB_HID_ENABLED=${USB_HID_CMAKE} \
    -DHDMI_HSTX=${HDMI_HSTX_CMAKE} \
    -DVIDEO_COMPOSITE=${VIDEO_COMPOSITE_CMAKE} \
    ..

make -j$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
