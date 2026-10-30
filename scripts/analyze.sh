#!/usr/bin/env bash
# Regenerates recomp/dc_scus97111.toml from the ELF with ps2_analyzer. The retail ELF keeps its symbol table, so the
# functions come from it (no Ghidra map) and the SDK functions are named by their own symbols.
# WARNING: this overwrites hand edits in the TOML; diff before committing.
source "$(dirname "$0")/common.sh"
[ -f "$ELF" ] || { echo "missing $ELF, run scripts/extract_iso.sh first"; exit 1; }
cd "$ROOT"
"$TOOLS_BUILD/ps2xAnalyzer/ps2_analyzer$EXE" "game/SCUS_971.11" "recomp/dc_scus97111.toml"
# The function map from the ELF's own symbols (tools/symtab_map.py) adds crt0's _start, a NOTYPE label.
python -I tools/symtab_map.py game/SCUS_971.11 main recomp/maps/dc_main.csv
sed -i -e '/^ghidra_output = /d' \
       -e 's|^# Path to Ghidra exported function map.*|# Function map from the ELF symbol table (tools/symtab_map.py), in the Ghidra CSV format\nghidra_output = "recomp/maps/dc_main.csv"|' \
       -e 's|^output = .*|output = "generated/"|' "$CONFIG"
echo "Re-apply the manual stub edits (git diff recomp/dc_scus97111.toml): sceSdRemoteInit, sceSdTransToIOP, sceSdRemote stay native"
