#!/usr/bin/env bash
# Launches the recompiled game. Extra args are passed to ps2EntryRunner.
# Settings that must reach the game go through the environment of this script: export them.
source "$(dirname "$0")/common.sh"
BIN="$GAME_BUILD/PS2Recomp/ps2xRuntime/ps2EntryRunner$EXE"
[ -x "$BIN" ] || { echo "missing $BIN, run scripts/build_runtime.sh first"; exit 1; }
# Full native path, as the other ports: no GS at all (PS2X_GS_RENDERER=none: GIF packets neither drawn nor kept, the
# picture is the native renderer's alone, from the game's own data; PS2X_GS_RENDERER=native only to compare with the
# old GS path), the game's IOP modules recompiled by scripts/iop_recomp.sh, SPU2 and IOP on the PC clock.
export PS2X_GS_RENDERER="${PS2X_GS_RENDERER:-none}"
export PS2X_IOP_NATIVE="${PS2X_IOP_NATIVE:-1}"
export PS2X_SPU2_WALLCLOCK="${PS2X_SPU2_WALLCLOCK:-1}"
export PS2X_IOP_WALLCLOCK="${PS2X_IOP_WALLCLOCK:-1}"
# The native frame loop (runtime/dc_overrides.cpp nativeFrameEnd): no vblank paces the game; each frame ends by
# blocking the main thread until its deadline on the PC clock (PS2X_FRAME_CAP, 60). DC_NATIVE=0 goes back to the
# vblank grid.
export DC_NATIVE="${DC_NATIVE:-1}"
# The picture: the game's 3D and 2D drawn natively from its own data (runtime/dc_native.cpp); with no GS it is the
# only picture there is.
export DC_NATIVE3D="${DC_NATIVE3D:-1}"
# The runtime shows the picture at 4:3, as the PS2's TV did; its default 640x448 window put a black band on each side.
export PS2X_WINDOW_SIZE="${PS2X_WINDOW_SIZE:-960x720}"
exec "$BIN" "$ELF" "$@"
