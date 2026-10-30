#!/usr/bin/env bash
# Regenerates recomp/dc_scus97111.toml from the ELF with ps2_analyzer. The retail ELF keeps its symbol table, so the
# functions come from it (no Ghidra map) and the SDK functions are named by their own symbols.
# WARNING: this overwrites hand edits in the TOML; diff before committing.
source "$(dirname "$0")/common.sh"
[ -f "$ELF" ] || { echo "missing $ELF, run scripts/extract_iso.sh first"; exit 1; }
cd "$ROOT"
"$TOOLS_BUILD/ps2xAnalyzer/ps2_analyzer$EXE" "game/SCUS_971.11" "recomp/dc_scus97111.toml"
sed -i -e 's|^output = .*|output = "generated/"|' "$CONFIG"
