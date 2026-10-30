#!/usr/bin/env bash
# Unattended boot: runs the game for <seconds> through scripts/run.sh, saves the presented frame every
# <interval> seconds to build/boots/<name>/frame_*.png and the output to build/boots/<name>/run.log.
#   scripts/boot_test.sh <name> [seconds, default 60] [interval, default 5]   (PS2X_PAD_SCRIPT etc. pass through)
source "$(dirname "$0")/common.sh"
NAME="${1:?name}"; SECS="${2:-60}"; INTERVAL="${3:-5}"
OUT="$ROOT/build/boots/$NAME"
rm -rf "$OUT" && mkdir -p "$OUT"
native() { command -v cygpath > /dev/null && cygpath -m "$1" || echo "$1"; }
# Unattended: the pad scripts only (PS2X_PAD_SCRIPT, PS2X_PAD_INPUT), whatever is plugged into the PC; a connected
# gamepad kept the live input file from reaching the game (boot 9). PS2X_PAD_NO_HOST=0 to play along.
export PS2X_PAD_NO_HOST="${PS2X_PAD_NO_HOST:-1}"
export PS2X_CAPTURE_DIR="$(native "$OUT")" PS2X_CAPTURE_INTERVAL="$INTERVAL"
timeout "$SECS" "$ROOT/scripts/run.sh" > "$OUT/run.log" 2>&1
code=$?
echo "boot $NAME: exit $code after <= ${SECS}s, $(ls "$OUT"/*.png 2>/dev/null | wc -l) frames, $(wc -l < "$OUT/run.log") log lines"
