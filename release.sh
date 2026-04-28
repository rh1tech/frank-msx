#!/bin/bash
#
# release.sh - build release UF2s for frank-msx
#
# Only M2 is supported right now; the script keeps the iteration loop so
# additional variants can be added later without rewriting it.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

VERSION_FILE="version.txt"

if [[ -f "$VERSION_FILE" ]]; then
    read -r LAST_MAJOR LAST_MINOR < "$VERSION_FILE"
else
    LAST_MAJOR=0
    LAST_MINOR=0
fi

NEXT_MINOR=$((LAST_MINOR + 1))
NEXT_MAJOR=$LAST_MAJOR
if [[ $NEXT_MINOR -ge 100 ]]; then
    NEXT_MAJOR=$((NEXT_MAJOR + 1))
    NEXT_MINOR=0
fi

echo ""
echo -e "${CYAN}┌─────────────────────────────────────────────────────────────────┐${NC}"
echo -e "${CYAN}│                    FRANK MSX Release Builder                   │${NC}"
echo -e "${CYAN}└─────────────────────────────────────────────────────────────────┘${NC}"
echo ""
echo -e "Last version: ${YELLOW}${LAST_MAJOR}.$(printf '%02d' $LAST_MINOR)${NC}"
echo ""

DEFAULT_VERSION="${NEXT_MAJOR}.$(printf '%02d' $NEXT_MINOR)"
if [[ -n "$1" ]]; then
    INPUT_VERSION="$1"
else
    read -p "Enter version [default: $DEFAULT_VERSION]: " INPUT_VERSION
    INPUT_VERSION=${INPUT_VERSION:-$DEFAULT_VERSION}
fi

if [[ "$INPUT_VERSION" == *"."* ]]; then
    MAJOR="${INPUT_VERSION%%.*}"
    MINOR="${INPUT_VERSION##*.}"
else
    read -r MAJOR MINOR <<< "$INPUT_VERSION"
fi

MINOR=$((10#$MINOR))
MAJOR=$((10#$MAJOR))

if [[ $MAJOR -lt 0 ]]; then
    echo -e "${RED}Error: Major version must be >= 0${NC}"
    exit 1
fi
if [[ $MINOR -lt 0 || $MINOR -ge 100 ]]; then
    echo -e "${RED}Error: Minor version must be 0-99${NC}"
    exit 1
fi

VERSION="${MAJOR}_$(printf '%02d' $MINOR)"
echo ""
echo -e "${GREEN}Building release version: ${MAJOR}.$(printf '%02d' $MINOR)${NC}"

printf '%d %02d\n' "$MAJOR" "$MINOR" > "$VERSION_FILE"

RELEASE_DIR="$SCRIPT_DIR/release"
mkdir -p "$RELEASE_DIR"

# Release matrix — one UF2 per (platform, video) combination that the
# platform supports. Entries are: "<platform>:<label>:<cmake args>".
RELEASE_TARGETS=(
    "m2:m2-hdmi-hstx:-DHDMI_HSTX=ON"
    "m2:m2-hdmi-pio:"
    "m2:m2-tv:-DVIDEO_COMPOSITE=ON"
    "m1:m1-hdmi-pio:"
    "m1:m1-tv:-DVIDEO_COMPOSITE=ON"
    "dv:dv-hdmi-pio:"
    "pc:pc-hdmi-pio:"
    "z0:z0-hdmi-pio:"
)
FAIL=0

# Release defaults: USB HID host enabled (USB keyboard/mouse/gamepad).
# Override with USB_HID=0 to produce a release UF2 with CDC stdio instead.
: "${USB_HID:=1}"
if [[ "$USB_HID" == "1" || "$USB_HID" == "ON" || "$USB_HID" == "on" ]]; then
    USB_HID_CMAKE=ON
else
    USB_HID_CMAKE=OFF
fi
echo -e "USB HID host: ${CYAN}${USB_HID_CMAKE}${NC}"

for ENTRY in "${RELEASE_TARGETS[@]}"; do
    IFS=':' read -r TGT_PLATFORM TGT_LABEL TGT_EXTRA <<< "$ENTRY"
    OUTPUT_NAME="frank-msx_${TGT_LABEL}_${VERSION}.uf2"

    echo ""
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo -e "${CYAN}Building: $OUTPUT_NAME${NC}"
    echo ""

    rm -rf build
    mkdir build
    cd build

    # shellcheck disable=SC2086  # intentional word-splitting of TGT_EXTRA
    cmake .. \
        -DPICO_PLATFORM=rp2350 \
        -DPLATFORM=${TGT_PLATFORM} \
        -DUSB_HID_ENABLED=${USB_HID_CMAKE} \
        ${TGT_EXTRA} > /dev/null 2>&1

    if make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4) > /dev/null 2>&1; then
        if [[ -f "frank-msx.uf2" ]]; then
            cp "frank-msx.uf2" "$RELEASE_DIR/$OUTPUT_NAME"
            echo -e "  ${GREEN}✓ ${TGT_LABEL}${NC} → release/$OUTPUT_NAME"
        else
            echo -e "  ${RED}✗ ${TGT_LABEL} UF2 not found${NC}"
            FAIL=1
        fi
    else
        echo -e "  ${RED}✗ ${TGT_LABEL} build failed${NC}"
        FAIL=1
    fi

    cd "$SCRIPT_DIR"
done

rm -rf build

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
if [[ $FAIL -eq 0 ]]; then
    echo -e "${GREEN}Release build complete!${NC}"
else
    echo -e "${RED}Some builds failed!${NC}"
fi
echo ""
ls -la "$RELEASE_DIR"/frank-msx_*_${VERSION}.uf2 2>/dev/null | awk '{print "  " $9 " (" $5 " bytes)"}'
echo ""
echo -e "Version: ${CYAN}${MAJOR}.$(printf '%02d' $MINOR)${NC}"
echo ""
