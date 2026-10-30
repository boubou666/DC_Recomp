# Dark Cloud recompilation notes

How the port is brought up: every step, every fix and how it was found, and what remains open.

## The disc (SCUS-97111, version 1.02)

- `SYSTEM.CNF`: `BOOT2 = cdrom0:\SCUS_971.11;1`, `VER = 1.02`, `VMODE = NTSC`.
- `SCUS_971.11` is a CodeWarrior link that kept its symbol table (12,644 symbols, `.mwcats` sections): 3,942 FUNC
  symbols with sizes in the main program, 177 for the title overlay and 62 for the dungeon overlay.
- Sections: `main` at `0x100000` (file image `0x1A2380`, memory up to `0x1DABD00` with the BSS), then `title` and `dun`,
  both at `0x01DABD00` and both empty in the file (their code is in `TITLE.BIN` and `DUN.BIN`), then `heap` at
  `0x01F06B00`. `.reltitle` and `.reldun` keep the link-time relocations of the overlays; the overlay files are already
  linked at `0x01DABD00`, so the runtime does not need them.
- Inside `main`: EE code `0x100000-0x2452B8`, VU microcode (`.vutext`) `0x245300-0x24FB00`, data from `0x24FB00`, the
  CodeWarrior static initializers (`__sinit_*`) at `0x29FE80-0x2A1010` after the data, BSS from `0x2A2380` (`_fbss`),
  `_gp = 0x2A97F0`, stack `0x1F80000` (`_stack`, 512 KB).
- Overlays: `MWo3` header (+8 load `0x01DABD00`, +0xC text size, +0x10 data size, +0x14 BSS size, +0x18/+0x1C the
  constructor table, +0x20 the file name), loaded with the header, code from +0x80.

  | | text | data | BSS | code | static initializers | constructor table |
  |---|---|---|---|---|---|---|
  | `TITLE.BIN` | `0x29640` | `0xC77C` | `0x7C484` | `0x1DABD80-0x1DD5314` | `0x1DE0F80-0x1DE1AD8` | `0x1DE1AE0-0x1DE1AFC` |
  | `DUN.BIN` | `0x15DC0` | `0x2914` | `0x1426EC` | `0x1DABD80-0x1DC1A80` | `0x1DC3580-0x1DC440C` | `0x1DC4410-0x1DC4414` |

  No other `MWo3` module is on the disc (`DATA.DAT` holds none).
- IOP: `MODULES/IOPRP211.IMG` then SIO2MAN, PADMAN, MCMAN, MCSERV, LIBSD, SDRDRV, MODMIDI, MODHSYN, EZMIDI (the paths
  in the ELF). The network modules (INET, AN986, LIBNET, NETCNF, MSIFRPC, USBD) are on the disc but not in that list.

## Function maps

`tools/symtab_map.py` writes the CSV ps2_recomp reads from the FUNC symbols. The symbols cover all of the EE code: in
the main program only crt0's `_start` (`0x100008`, a NOTYPE label) is added from the uncovered-code pass; the overlays
need none. ps2_recomp also reads the ELF's own symbols for the main program; it skips the overlay symbols, whose
sections are empty.

Template arguments put commas in some C++ names (`Alloc64__21CDataAlloc<1,1690000>Fi`); ps2_recomp splits the map
on every comma, so `tools/symtab_map.py` writes them with `_` instead (5 names). The functions were recompiled anyway,
from the ELF's own symbols.

## Build

- `scripts/build_tools.sh`, `scripts/analyze.sh`, `scripts/recomp.sh`: 3,948 generated files; `ps2_vu_aot` finds 8 VU
  microcode fragments (5,312 instruction pairs, 2 also run as VU0 microcode). ps2_recomp warns about unresolved
  `jr` in some functions (CodeWarrior jump tables, `__sinit_*`), with fallback entries promoted.
- `scripts/overlays.sh`: `ovl_title` and `ovl_dun`, with their `.init` and `.ctors`.
- The runner builds in about 3 minutes with 0 errors. paraLLEl-GS is not built in (`PS2X_ENABLE_PARALLEL_GS OFF`).
- On Windows a shell started without `TMP`/`TEMP` made GCC write to `C:\Windows` (refused, every compile failed):
  `scripts/common.sh` sets them to `build/tmp` when they are missing.

## Boot 1 (2026-10-30): to the memory card prompt

- First run with the IOP interpreter only to dump the modules (`scripts/iop_recomp.sh`), as the other ports do: the
  game loads `IOPRP211.IMG` and the nine modules. `tools/iop_recomp.py` recompiles all nine with nothing left to the
  interpreter (SDRDRV 12 functions, PADMAN 129, SIO2MAN 47, ...).
- Native boot (native GS, native IOP, wall-clock SPU2 and IOP): `SDR driver version 2.0.0` and `EzMIDI driver version
  1.2.0` start, the game prints `Memory Card Initialized Successed!!`, and the screen shows "Memory Card (PS2) (8MB)
  not inserted. To save the game data, at least 400KB is required. Start anyway?" over the blue boot background, drawn
  right by the native renderer.
- Why "not inserted": every libmc call of the game passes slot 1 (`CMemoryCardAccess::SearchMcType` 0x213E7C:
  `sceMcGetInfo(port, 1, ...)`; `LoadSysConfig` 0x214384: `sceMcOpen(port, 1, ...)`), and the runtime's libmc HLE
  only accepted slot 0. On the console MCSERV answers for the card in the port whatever the slot when no multitap is
  plugged. Fix (fork, `dc/native`): `ps2_stubs::setMcAnySlot(true)` (or `PS2X_MC_ANY_SLOT=1`), off by default so the
  other ports keep their behaviour; `runtime/dc_overrides.cpp` turns it on.
