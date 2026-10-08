#!/bin/bash
# Read the glide-pico flash log. Do this on the personal Mac, not on the work Mac.
#
#   1. Hold BOOTSEL, connect the Pico, release BOOTSEL (the RPI-RP2 drive appears).
#   2. tools/pico-log.sh            save the log to logs/, print it, restart the firmware
#      tools/pico-log.sh --clear    erase the log area: all logs AND the learned edges
#      tools/pico-log.sh --stay     do not restart (stay in BOOTSEL mode)
#
# Needs picotool with USB support (tools/setup-pico-toolchain.sh builds it).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
PICO_HOME="${PICO_HOME:-$HOME/pico}"
PICOTOOL="${PICOTOOL:-$PICO_HOME/picotool/picotool/picotool}"
[[ -x "$PICOTOOL" ]] || { echo "picotool not found at $PICOTOOL: run tools/setup-pico-toolchain.sh"; exit 1; }

# Log area: flash offset 0x080000..0x200000 (flashlog.h), XIP address 0x10000000 + offset.
FROM=0x10080000
TO=0x10200000

case "${1:-}" in
  --clear)
    "$PICOTOOL" erase -r "$FROM" "$TO"
    echo "log area erased: the next boot starts with no learned edges"
    "$PICOTOOL" reboot -a
    exit 0
    ;;
esac

mkdir -p "$root/logs"
stamp="$(date +%Y%m%d-%H%M%S)"
bin="$root/logs/pico-$stamp.bin"
txt="$root/logs/pico-$stamp.txt"
"$PICOTOOL" save -r "$FROM" "$TO" "$bin" -t bin
python3 -I "$here/pico-log-decode.py" "$bin" > "$txt"
cat "$txt"
echo
echo "saved: $txt"
[[ "${1:-}" == "--stay" ]] || "$PICOTOOL" reboot -a
