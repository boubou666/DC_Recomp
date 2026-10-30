#!/usr/bin/env bash
# Builds and runs the PS2Recomp fork's own tests (ps2x_tests) from PS2Recomp/, in build/forktests: run it before
# posting a runtime fix for the shared native/* branches. A build: take the bench alone (scripts/bench_run.sh --alone).
#   scripts/fork_tests.sh [ps2x_tests arguments]
# FORK_SRC=<a fork checkout, e.g. a worktree of another branch> FORK_OUT=<its build folder> test another tree.
source "$(dirname "$0")/common.sh"
export PATH="/c/msys64/ucrt64/bin:$PATH"
SRC="${FORK_SRC:-$ROOT/PS2Recomp}"
OUT="${FORK_OUT:-$ROOT/build/forktests}"
cmake -S "$SRC" -B "$OUT" -G Ninja -DCMAKE_BUILD_TYPE=Release -DPS2X_BUILD_TEST=ON -DPS2X_BUILD_STUDIO=OFF \
    > "$OUT.configure.log" 2>&1 || { tail -20 "$OUT.configure.log"; exit 1; }
cmake --build "$OUT" --target ps2x_tests -j "$JOBS" > "$OUT.build.log" 2>&1 || { grep -m10 -E "error|FAILED" "$OUT.build.log"; exit 1; }
test="$(find "$OUT" -name "ps2x_tests$EXE" -type f | head -1)"
[ -n "$test" ] || { echo "ps2x_tests not found in $OUT"; exit 1; }
# From the fork's root: some tests read its sources (ps2xRecomp/include/ps2recomp/instructions.h).
cd "$SRC" && "$test" "$@"
