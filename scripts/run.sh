#!/usr/bin/env bash
# Launches the recompiled game. Extra args are passed to ps2EntryRunner.
# Settings that must reach the game go through the environment of this script: export them.
source "$(dirname "$0")/common.sh"
BIN="$GAME_BUILD/PS2Recomp/ps2xRuntime/ps2EntryRunner$EXE"
[ -x "$BIN" ] || { echo "missing $BIN, run scripts/build_runtime.sh first"; exit 1; }
# Full native path, as the other ports: native GS renderer (no GS emulation, no paraLLEl-GS), the game's IOP modules
# recompiled by scripts/iop_recomp.sh, SPU2 and IOP on the PC clock.
export PS2X_GS_RENDERER="${PS2X_GS_RENDERER:-native}"
export PS2X_IOP_NATIVE="${PS2X_IOP_NATIVE:-1}"
export PS2X_SPU2_WALLCLOCK="${PS2X_SPU2_WALLCLOCK:-1}"
export PS2X_IOP_WALLCLOCK="${PS2X_IOP_WALLCLOCK:-1}"
# The native frame loop (runtime/dc_overrides.cpp nativeFrameEnd): no vblank paces the game; each frame ends by
# blocking the main thread until its deadline on the PC clock (PS2X_FRAME_CAP, 60). DC_NATIVE=0 goes back to the
# vblank grid.
export DC_NATIVE="${DC_NATIVE:-1}"
exec "$BIN" "$ELF" "$@"
