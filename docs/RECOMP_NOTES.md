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

## Boots 2-6: memory card, sound, title

- Boot 2: with the any-slot libmc mode the card is found in both ports; the 3D intro (castle), the logo and the title
  screen (the desk and the open book, `TitleLoop` in `TITLE.BIN`) draw right.
- Every voice transfer of EzMIDI then stalled: `sceSdVoiceTransStatus(ch, WAIT)` (LIBSD export 19, 0x47020) spins on
  its done flag (0x4A1F4), which LIBSD's DMA handler (0x47674, irq 36/40) sets. A/B with the SPU2 off the wall clock
  and with the IOP interpreted: the irq is dispatched, but intrman drops it, because `EnableIntr(36)` never ran:
  LIBSD's `sceSdInit` (export 4, 0x46E28) never runs. SDRDRV calls it when the EE's libsdr asks (`set_spu` ->
  `sceSdRemoteInit` / `sceSdRemote`), and SDRDRV does register its server (0x80000701), but no RPC ever reached it.
- Two recompiler behaviours that only an unstripped ELF triggers sent those calls to the runtime's own libsdr HLE:
  1. every relocated `jal` was bound by symbol name to the runtime handler of that name (`.relmain` kept the
     relocations);
  2. every function named after a runtime handler was stubbed by name, whatever the config said.
  Fix (fork, `dc/native` 350dee4): `[general] native = ["name@0xaddr", ...]` keeps such functions as guest code, and a
  relocated call binds to a handler only when the config stubs the target. DC lists its libsdr client
  (`sceSdRemoteInit`, `sceSdTransToIOP`, `sceSdCallBack`, `sceSdRemote`). Boot 5: `sceHSyn_VoisTrans Ok` for each
  transfer, no IOP call given up (12 before).
- Boot 6: START once the title shows (`TitleLoop` reads START 0x800, Up/Down, X/START to confirm, Circle), then X:
  the game goes on to the in-game 3D scenes: a dungeon fight, the Fairy King in the forest, Toan's house.

## Boots 7-8: attract demo, native frame loop, New game

- Boot 7 (5 min, X every 4 s): the in-game 3D scenes of boot 6 are the attract demo (Toan, Xiao, Goro in their
  dungeons); the title loops logo -> demo -> logo until START is pressed at the logo.
- Native frame loop (`runtime/dc_overrides.cpp`, `DC_NATIVE=1`, default in `scripts/run.sh`): `WaitVSync__Fi`
  (0x12D070), the spin at the end of `MGEndFrame` until the vblank counter (0x2A2400) moves, becomes a wait on the PC
  clock at the FramePacer cap (`EeScheduler::waitHostDeadline`, as JC2_Recomp); EE time is charged with the real
  time and the frame is presented at the deadline. Boot 8: 60.0 fps with 59.8-60.2 vblanks/s.
- Boot 8 (START every 4 s, X 2 s after): the desk title stays from 28 s to 80 s, then New game: "DARK SHRINE" and the
  field HUD (the compass); START every 4 s then toggles the pause. The 3D field behind the HUD stays nearly black
  (mean brightness 3 of 255): to look at next.

## Boots 9-14: scripted input, New game, the opening

- Pad scripts never reached the game in boots 9-11. Two causes, both on the host side: a gamepad plugged into the PC
  (boot tests now set `PS2X_PAD_NO_HOST=1`), and environment variables put in front of a command from Git's bash,
  which do not reach MSYS2's bash when `/c/msys64/usr/bin` comes first in `PATH`: they are exported inside
  `bash -c '...'` now, and each boot saves what it got in `build/boots/<name>/env.txt`.
- Title flow: START skips the intro to the logo, START at the logo opens the desk (the menu answers from about 80 s
  in), X starts New game, then the name entry (X types letters; START jumps to DECIDE, X accepts).
- Boot 14: the opening in the Dark Shrine plays: Colonel Flag and his soldiers, the urn, the Dark Genie, with the
  dialogue text; 56-60 fps on the native frame loop. The dark field of boot 8 was the pause (START every 4 s).
- Open: while a dialogue line shows, the whole screen behind the text window is a striped beige instead of the scene.

## The picture was cut in half (display height)

- Every capture and the window showed only the top half of DC's picture (the logo against the bottom edge, no menu
  under it). DC sets its double buffer with libgraph's `sceGsSetDefDBuff` (0x112C08), which the runtime stubbed. That
  stub wrote DISPLAY as DW = w - 1, DH = h - 1, MAGH 0, DX 636, while libgraph's own `sceGsSetDefDBuff` calls
  `sceGsSetDefDispEnv` for both display environments (0x112C84, 0x112CA0), whose stub encodes the 2560-clock line
  (MAGH 3) and, in interlaced frame mode (SMODE2 INT+FFMD, DC's 224-line fields), DH = 2h - 1. The GS native present
  takes DH as frame lines in that mode and showed DH / 2 = 112 of the 224 lines, doubled.
- Fix (DC only, `recomp/dc_scus97111.toml`): `sceGsSetDefDBuff` in the `native` list, so the game's libgraph runs and
  calls the `sceGsSetDefDispEnv` stub. DISPLAY is now DW 2560, MAGH 3, DH 448, and the whole picture shows: the title
  menu (START, CONTINUE, OPTION), the desk narration's subtitles, the full scenes. The stub itself is left as it is
  (The Third Age stubs it too; reported on f2f).
- Still open: the window shows the picture with a black band on the left and its right edge cut (presentation
  offset); the dialogue backdrop (PSMT8 read of a CT32 target).

## Boot 15: past the opening, Norune (2026-10-30, paused here)

- 10 min scripted run (X every 2 s after the name entry): the Dark Shrine scene ends (the Genie, Colonel Flag), then
  Norune's scenes: the village at night, Toan eating at home, the festival around the shrine fire; 52-58 fps.
- Open, in order:
  1. Black checkerboard blocks over parts of some Norune scenes (texture pages missing or stale in the GS native back
     end). This run used a local, unpushed merge of `native/gs-backend` acd11f5 into `dc/native` (the 5 partial-FBMSK
     hunks resolved to the gs-backend side, as `jc2/native` af3cc76); to compare with `native/renderer` alone and with
     VP2S's `native/gs-backend-on-renderer` once it is up.
  2. The dialogue backdrop: DC reads its CT32 target back as PSMT8 with a CLUT (channel shuffle, likely a sepia copy
     of the scene); striped on the native back end. VP2S has a native-path GS recording of it (dc_dialogue2.gsr).
  3. Control of Toan in Norune not reached yet in the scripted runs.
- Tooling: window at 4:3 (`PS2X_WINDOW_SIZE=960x720` in `scripts/run.sh`), boot tests ignore the PC's gamepads and
  save their settings in `env.txt`.

## Native 3D and the cutscene stripes (2026-10-31)

- Native 3D (`DC_NATIVE3D=1`, `runtime/dc_native.cpp`): the model's material colour (its first quadword, unpacked
  to VU 0x25) now scales the lights and ambient, as the model microprogram does (f256fcac190325a3 at 0xAD0 / 0xBF8).
- Counted over a 280 s boot (intro, title, name entry, Dark Shrine, cutscenes) and the Norune runs: every MSCAL VU1
  was asked about was drawn natively (`gate: N MSCALs ... N drawn here`), and no DrawVu1 call was left to VU1 (0
  shadows, 0 program-0xF). VU1 runs no microcode in those scenes; shadows (RenderInfo + 0x320) and the screen-space
  program will need translating once a scene uses them.
- The stripes over the cutscene dialogues (whole screen, and inside the message box) were not the GS: the CPU GS
  reference showed them too. The speech balloon (`ClsMes::MakeFukidashi`) is drawn into the `fukidashibase` texture
  (CT32, fbp 227), then that texture is drawn over the screen. The balloon's tail points (ClsMes + 0x74..0x80, from
  `RollPos` with the angle `atan2(speaker - centre)`) came out at -830..1514, so the balloon landed at y ~935 in its
  target, outside the scissor, and the screen got the texture's old contents (glyph rows) with full alpha.
  `atan2` returned its own argument: DC's libm is the double-precision one (soft-float, CodeWarrior: arguments in
  a0 / a1 and the result in v0, 64-bit; separate `sinf` / `atan2f` exist), and the recompiler mapped `sin`, `cos`,
  `atan`, `atan2`, `sqrt`, `floor`, `fabs` to the runtime's handlers, which take floats in f12 / f14 and return f0.
  Removing them from `stubs` is not enough (the recompiler maps known names by itself): they are in the config's
  `native` list now, so the game's own libm runs. Balloons, tails and text are right; any other double maths DC does
  (cameras, menus) is fixed with it.
- Ruled out on the way, with switches kept for diagnosis: `DC_NO_DOF=1` (skips DepthOfField 0x1652C0),
  `DC_SKIP_TBP=<block>` (skips DC's 2D sprites of one texture), `DC_PROBE_TEX=1` (texture manager list, full-screen
  sprites, MGMoveImage, the balloon's fields, RollPos).
- Note for probes: `sceVif1PkAddGsAD` is a runtime stub called directly (not overridable through the function table);
  game functions called with JAL go through the dispatcher and can be hooked with `registerFunction`.
