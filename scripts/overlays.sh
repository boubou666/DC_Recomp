#!/usr/bin/env bash
# Recompiles Dark Cloud's two code overlays as PS2Recomp overlay modules, into generated/overlays/<module>/.
# TITLE.BIN (title screen, opening, menus) and DUN.BIN (dungeons) are CodeWarrior 'MWo3' modules that both load at
# 0x01DABD00, header included; at run time the module whose first 64 bytes (the header, with its file name) match
# guest memory at that address is the one called.
# Stages per module: tools/wrap_elf.py (ELF at the load address, from VP2S_Recomp) -> tools/symtab_map.py (function
# map from the boot ELF's symbol table, which keeps the overlays' symbols) -> ps2_recomp in module mode.
#   scripts/overlays.sh
source "$(dirname "$0")/common.sh"
native() { command -v cygpath > /dev/null && cygpath -m "$1" || echo "$1"; }
WORK="$ROOT/build/overlay_work"
OUT="$GENERATED/overlays"
mkdir -p "$WORK" "$OUT/include"
[ -x "$TOOLS_BUILD/ps2xRecomp/ps2_recomp$EXE" ] || "$ROOT/scripts/build_tools.sh"
for module in title dun; do
    bin="$GAME_DIR/${module^^}.BIN"
    name="ovl_$module"
    python -I "$(native "$ROOT/tools/wrap_elf.py")" "$(native "$bin")" "$(native "$WORK/$name.elf")" \
        --flags-from "$(native "$ELF")" > "$WORK/$name.log" 2>&1 || { echo "wrap failed for $name (see $WORK/$name.log)"; exit 1; }
    python -I "$(native "$ROOT/tools/symtab_map.py")" "$(native "$ELF")" "$module" "$(native "$WORK/$name.csv")" \
        "$(native "$bin")" >> "$WORK/$name.log" 2>&1 || { echo "function map failed for $name (see $WORK/$name.log)"; exit 1; }
    cat > "$WORK/$name.toml" <<TOML
[general]
input = "$(native "$WORK/$name.elf")"
ghidra_output = "$(native "$WORK/$name.csv")"
output = "$(native "$OUT/$name")/"
single_file_output = false
patch_syscalls = false
patch_cop0 = true
patch_cache = true
module_name = "$name"
module_load = "0x01DABD00"
module_guard_size = 64
stubs = []
skip = []
TOML
    rm -rf "$OUT/$name"
    mkdir -p "$OUT/$name"
    "$TOOLS_BUILD/ps2xRecomp/ps2_recomp$EXE" "$(native "$WORK/$name.toml")" >> "$WORK/$name.log" 2>&1 ||
        { echo "ps2_recomp failed for $name (see $WORK/$name.log)"; exit 1; }
    # One include folder for every module's headers (names carry the module prefix), as in JC2_Recomp.
    cp "$OUT/$name/${name}_stubs.h" "$OUT/$name/${name}_functions.h" "$OUT/include/" 2>/dev/null || true
    echo "$name: $(ls "$OUT/$name" | wc -l) files ($(tail -1 "$WORK/$name.log"))"
done
