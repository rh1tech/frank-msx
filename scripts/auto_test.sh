#!/bin/bash
# Flash, capture USB serial, and grab a VGA frame — used by the autonomous
# test loop. Runs entirely headless so Claude Code can iterate on the VGA
# driver without manual flashing.
#
# Usage:  ./scripts/auto_test.sh [seconds]
#
# Output:
#   scripts/out/serial.log   — captured USB CDC output
#   scripts/out/vga.jpg      — single frame from the USB VGA capture card
set -u

SECONDS_TO_CAPTURE="${1:-20}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/scripts/out"
mkdir -p "$OUT"

SERIAL_DEV="/dev/cu.usbmodem21301"
UF2="$ROOT/build/frank-msx.uf2"

# Video capture device index reported by ffmpeg -f avfoundation -list_devices.
# We autodetect by matching "USB Video" (YGTek-family HDMI/VGA capture).
# Override via VGA_DEVICE=n env var.
VGA_DEVICE_NAME="${VGA_DEVICE_NAME:-USB Video}"
VGA_DEVICE="${VGA_DEVICE:-$(ffmpeg -f avfoundation -list_devices true -i "" 2>&1 \
    | awk -v name="$VGA_DEVICE_NAME" '
        /AVFoundation video devices:/ {in_video = 1; next}
        /AVFoundation audio devices:/ {in_video = 0; next}
        in_video && index($0, name) > 0 {
            # Skip the leading [AVFoundation indev @ 0x...] prefix and match
            # the device-index bracket that comes after it.
            if (match($0, /\] \[[0-9]+\]/)) {
                s = substr($0, RSTART+3, RLENGTH-4)
                print s; exit
            }
        }')}"
# Fall back to 0 if nothing matched. AVFoundation renumbers devices when
# things are re-plugged, so the name match above is the primary path.
VGA_DEVICE="${VGA_DEVICE:-0}"
echo "== capture card at avfoundation index [$VGA_DEVICE] =="

if [ ! -f "$UF2" ]; then
    echo "No firmware at $UF2 — run ./build.sh first"
    exit 1
fi

# 1. Flash.
#
# picotool's `-f` uses the USB CDC reset-channel to force the running
# firmware into BOOTSEL. On frank-msx this fails: when fMSX halts after
# "Emulation ended. Halting", the reset interface stops handling control
# transfers and the board sits in a half-dead state (CDC alive, reset
# channel unresponsive). But in that state `picotool info`/`load` still
# work through the vendor interface — so we plain-load without `-f` and
# then ask it to reboot.
#
# When the firmware IS still running, the `picotool load` call fails; we
# then fall back to `picotool load -F` which does hard-force BOOTSEL.
echo "== flashing =="
if ! picotool load "$UF2" 2>/dev/null; then
    echo "  plain load failed, forcing BOOTSEL..."
    picotool load -F "$UF2"
fi
picotool reboot 2>/dev/null || picotool reboot -f

# 2. Wait for USB CDC to come back up.
echo "== waiting for $SERIAL_DEV =="
for i in $(seq 1 30); do
    if [ -e "$SERIAL_DEV" ]; then break; fi
    sleep 0.5
done
if [ ! -e "$SERIAL_DEV" ]; then
    echo "  $SERIAL_DEV never appeared after flash"
    exit 1
fi
sleep 1

rm -f "$OUT/serial.log" "$OUT/vga.jpg"

# 3. Capture serial in background. Use `script` to keep the line open even
# when the firmware is quiet between prints.
echo "== capturing ${SECONDS_TO_CAPTURE}s of serial =="
( timeout "$SECONDS_TO_CAPTURE" cat "$SERIAL_DEV" > "$OUT/serial.log" 2>/dev/null ) &
SERIAL_PID=$!

# Firmware prints boot banner + ROM attempts over ~6 s. Grab a VGA frame
# once the firmware has settled into its post-init state.
# Override with VGA_DELAY env var to capture later in boot (after welcome
# screen clears / emulation is drawing MSX frames).
sleep "${VGA_DELAY:-7}"

# 4. One JPEG from the USB VGA capture card. Request 640x480 to match the
# driver's native VGA mode.
ffmpeg -nostdin -loglevel error \
    -f avfoundation -framerate 30 -video_size 640x480 -i "${VGA_DEVICE}:none" \
    -frames:v 1 -y "$OUT/vga.jpg" 2> "$OUT/vga_ffmpeg.log" || true

wait $SERIAL_PID 2>/dev/null || true

echo "== done =="
echo "serial log: $OUT/serial.log ($(wc -c < "$OUT/serial.log" 2>/dev/null || echo 0) bytes)"
echo "vga frame:  $OUT/vga.jpg   ($(wc -c < "$OUT/vga.jpg" 2>/dev/null || echo 0) bytes)"
