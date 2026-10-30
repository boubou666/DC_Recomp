#!/usr/bin/env bash
# Runs one command under the shared bench lock (the fork's PS2Recomp/tools/bench_lock.sh, two slots since main
# c3524b7): waits for it, takes it, runs, and releases it on any exit (end, error, Ctrl-C, kill), so a stopped run
# never leaves the lock behind. A game run takes one slot; builds and timed runs take the bench alone (--alone).
#   scripts/bench_run.sh [--alone] <minutes> "<purpose>" <command> [args...]
source "$(dirname "$0")/common.sh"
# The lock tool is not written for common.sh's set -euo pipefail: its checks return false while it waits (two-slot
# version, main c3524b7), which ended this script silently before the command ran.
set +euo pipefail
source "$ROOT/PS2Recomp/tools/bench_lock.sh"
[ -x "$ROOT/build/announce.sh" ] && export PS2X_BENCH_LOCK_ANNOUNCE="$ROOT/build/announce.sh"
alone=""
[ "${1:-}" = "--alone" ] && { alone="alone"; shift; }
minutes="${1:?minutes}"; purpose="${2:?purpose}"; shift 2
[ $# -gt 0 ] || { echo "usage: $0 [--alone] <minutes> <purpose> <command> [args...]"; exit 1; }
if [ -n "$alone" ] && ! grep -q 'alone' "$ROOT/PS2Recomp/tools/bench_lock.sh"; then
    echo "bench_run: PS2Recomp/tools/bench_lock.sh predates the two-slot lock (main c3524b7); update the fork first"
    exit 1
fi

bench_lock_take "DC-Recomp" "$minutes" "$purpose" $alone
trap bench_lock_release EXIT
trap 'exit 130' INT TERM HUP
"$@"
