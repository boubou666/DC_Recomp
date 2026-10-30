#!/usr/bin/env bash
# Recompiles every IOP module Dark Cloud loads (SIO2MAN, PADMAN, MCMAN, MCSERV, LIBSD, SDRDRV, MODMIDI, MODHSYN,
# EZMIDI) to native C++ in generated/iop/ (run with PS2X_IOP_NATIVE=1, scripts/run.sh's default). The recompiler works
# from each module as the IOP loaded it (relocated at its base), so this first boots the game long enough for it to
# load its modules, with PS2X_IOP_DUMP_MODULES and the IOP interpreter, then runs the fork's tools/iop_recomp.py on
# the dumps (as TMB_Recomp's scripts/iop_recomp.sh). Like the rest of the generated code, never committed.
#   scripts/iop_recomp.sh [seconds of boot, default 40]
source "$(dirname "$0")/common.sh"
cd "$ROOT"
native() { command -v cygpath > /dev/null && cygpath -m "$1" || echo "$1"; }
SECONDS_TO_BOOT=${1:-40}
BIN="$GAME_BUILD/PS2Recomp/ps2xRuntime/ps2EntryRunner$EXE"
[ -x "$BIN" ] || { echo "missing $BIN, run scripts/build_runtime.sh first"; exit 1; }
DUMPS="$ROOT/build/iop_modules"
rm -rf "$DUMPS" && mkdir -p "$DUMPS" "$GENERATED/iop"
PS2X_IOP_NATIVE=0 PS2X_IOP_DUMP_MODULES="$(native "$DUMPS")" PS2X_GS_RENDERER=native \
    timeout "$SECONDS_TO_BOOT" "$BIN" "$ELF" > build/iop_dump.log 2>&1 || true
ls "$DUMPS" | grep -qi '^SDRDRV.IRX.*\.json$' || { echo "no dump of SDRDRV (see build/iop_dump.log)"; exit 1; }
for json in "$DUMPS"/*.json; do
    python "$(native "$ROOT/PS2Recomp/tools/iop_recomp.py")" "$(native "$json")" --out "$(native "$GENERATED/iop")" |
        grep -E '^[^ ]|functions,' || exit 1
done
