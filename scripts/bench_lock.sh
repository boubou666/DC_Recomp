#!/usr/bin/env bash
# The bench lock shared by the PS2Recomp ports on this machine, through the fork's PS2Recomp/tools/bench_lock.sh:
# a timed game run or benchmark only starts when the lock is free, and holds it meanwhile. Heavy builds take no lock,
# but should not start while someone is measuring.
#   scripts/bench_lock.sh wait                         # until the lock is free
#   scripts/bench_lock.sh status
# Runs go through scripts/bench_run.sh, which takes and releases the lock around one command.
# Takes and releases are announced in the c2c f2f lane when the local hook build/announce.sh exists (not committed:
# it holds the room connection).
source "$(dirname "$0")/common.sh"
source "$ROOT/PS2Recomp/tools/bench_lock.sh"
[ -x "$ROOT/build/announce.sh" ] && export PS2X_BENCH_LOCK_ANNOUNCE="$ROOT/build/announce.sh"

case "${1:-status}" in
    wait) bench_lock_wait ;;
    status) content=$(tr -d '\r' < "$(bench_lock_file)" 2>/dev/null); [ -n "$(echo "$content" | tr -d '[:space:]')" ] && echo "$content" || echo "free" ;;
    *) echo "usage: $0 wait|status"; exit 1 ;;
esac
