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
- Faces in native 3D: some come out speckled (the Mayor) or pale (the Colonel). Feeding the native texture cache the
  GS-to-GS copies DC's face animation makes (TexAnime -> MGMoveImage, TRXDIR 2) changed nothing, so it was not kept.
  Next suspects: the face texture's format / CLUT as the adapter passes it (TEX0 from the VU data's DIRECT, CBP / CSA
  of 4-bit textures), and TEX1 / mipmaps.
- Near and far: triangles are clipped to the microprogram's clip box (|z| <= w) before drawing. Characters still show
  through a pillar right at the camera in one cutscene shot; it may be DC fading occluders, not confirmed.
- Faces, narrowed (2026-10-31): DC uploads each character's texture just before its draw (ReloadTexture in the VIF1
  packet), into overlapping places (256x256 PSMT8 at blocks 6720, 6816, 7008, 7104, CLUT right after the texels).
  A same-moment A/B (`DC_NATIVE3D_VU1=1` with `PS2X_NATIVE_CAPTURE_DIR`: the GS frame drawn by VU1 next to the native
  frame decoded at draw time) shows nearly every character texture wrong when decoded at draw time; with stream
  draws (decoded at the group marker) most are right and some faces still are not. So it is when the native texture
  cache decodes against the uploads, not the geometry. VP2S's `native/gs-backend` texture store (uploads in global
  order, a texture made of the uploads still showing in its blocks) addresses this; it conflicts with
  `native/renderer` in native_renderer / native_texture_cache, asked on f2f how to bring it in.
  `DC_PROBE_FACES=1` logs characters' GS registers and writes their textures as the cache decodes them.
- Textures from the game's own assets (2026-10-31): the adapter hooks ReloadTexture (0x133070) and notes, per TEX0,
  which CTextureManager record (pixels, palette, size, sent-as-32-bit flag) the game just sent there, in the EE's
  order; each model draw decodes its texture from that asset in main memory and hands it to the renderer
  (`Renderer::draw` with a texture, PS2Recomp dc/native c66bb03). No GS packet tap, no GS memory. The Mayor's face is
  right now, and frames are faster (decoded textures reused). Still open: textures DC sends another way (no
  ReloadTexture record) fall back to the native texture cache, which is fed from GS uploads (GS-side: to replace by
  hooking those routines too); one character in front of a pillar mostly missing (depth or alpha, not texture).
  VP2S agreed on f2f that their GS-side texture store is not compliant and moved VP2 to the same EE-side approach.
- No packets read (2026-10-31, BaldHuman's rule as TMB applies it: no reading of GIF / VIF packets, not even those
  the game just built in EE RAM). `runtime/dc_native.cpp` rewritten: each DrawVu1 draws from the visual's MDT
  (noted by hooking CreateVUdataFromMDT / ...Remake; CVisualMDTVu1 keeps it at + 0x20), with clip = RenderInfo
  0x230 x 0x10 x the local-to-world matrix and RenderInfo 0x270 to the screen, RenderInfo's lights (+ 0x110..+ 0x1A0),
  the object's TEST / ZBUF / ALPHA from SetGsReg3's arguments, each material's texture from the texture record its
  name finds (as CreateVUdataFromMDT finds it), decoded from main memory. The VIF1 observer is gone: VU1 skips every
  program. Over 300 frames: ~278k primitives drawn, ~278.7k programs skipped; the few hundred skipped programs that
  are not DrawVu1 models (other VU1 users) are not drawn yet. Still present and still to remove: the game builds its
  packets anyway (nothing reads them; the renderer's groups take their place in the GS stream from VIF1's count of
  skipped programs), and the GS front end draws the 2D and composites.
- The fps overlay (`PS2X_SHOW_FPS=1`, F3) showed 0.0 in scenes where DC's display buffer does not flip: the native
  frame end now reports DC's frames (`noteGameFrame`).
- Other VU1 programs (2026-10-31): a 7-minute run (intro, title, name entry, Dark Shrine, cutscenes, Norune start)
  with `DC_PROBE_VU1=1` draws 15,172,342 model primitives natively and VU1 skips 15,172,008 programs (the difference
  is the last frame still in flight), every one at pc 0 or 8, the model programs' entries. So the "few hundred
  non-model programs" counted earlier were a frame-boundary lag, and in the reachable part of the game every VU1
  program is a DrawVu1 model. The other VU1 users in the code never ran there: CVisualShadow (shadows),
  CCloth::DrawVu1 (0x13C470), CWater::DrawVu1 (0x160F20); CEffect::Draw runs in Norune (900-30,000 calls per 300
  frames) without starting a VU1 program. They need a scripted run into gameplay (Toan's control, a dungeon) first.
- Into gameplay, cloth and shadows (2026-10-31): `build/boot16.pad` (the opening, then X every 0.5 s) reaches
  Norune's opening scenes, where CCloth::DrawVu1 (about 3 per frame) and CVisualShadow::DrawVu1 (about 9 per frame)
  run. Their programs were skipped without native batches numbered for them, so every later batch landed at the wrong
  place in the GS stream (black frames). Both are taken over now: no VU data, packet or VU1 program for them. Cloth is
  drawn natively from its own simulation grid (CCloth + 0x1110 positions, + 0x5110 normals, + 0x6110 texture
  coordinates, CreateVUData's strip order). Shadows are not drawn yet: their colour comes from the shadow program
  (images 755a / 69a6, VU 22 = RenderInfo + 0x328..), still to read. Water (CWater::DrawVu1) has not run yet.
- BaldHuman's texture rule for DC (via VP2S, 2026-10-31): plain TIM2 pixels and palettes from the game's records are
  decoded at runtime; texels stored in GS memory order (records with + 0x4C set) only through an offline conversion of
  the disc's files, no GS layout function in the game. The runtime un-swizzle is removed. Those textures are about 97%
  of the model draws: the 3D is untextured until the offline converter (disc IMG files -> native images in the build
  tree, loaded by the record's name and size at the record hook) exists.
- Offline texture conversion (2026-10-31): DC's pictures are TIM2 inside IMG archives (1,227 IMG blocks in DATA.DAT:
  `.img` files and blocks embedded in `.chr`, `.pak`, `.pac`, `.pim`, `.mpd`), plain on the disc; EnterTexture
  (0x1313B0) is what puts the 8-bit ones in GS order (Conv8to32). `tools/dc_texconv.py game
  build/textures/dc_textures.bin` converts the 2,243 8-bit pictures (921 distinct, 80 MB, never committed) to a store
  of RGBA palettes (CSM1 order resolved) and linear indices. At runtime an 8-bit record's texture comes from the store
  by the record's name, its palette's hash (the palette EnterTexture copies into the record unchanged) and its size;
  no GS layout function runs in the game. Every GS-order texture in the Norune run was found; the Colonel's face is
  right for the first time in the native path.
- Native shadows (2026-10-31): DC's shadow pass is MGBeginDrawShadow (0x1308A0: the GS drawing into a work texture
  cleared to black), the shadow-mode draws (RenderInfo + 0x320 set: CVisualShadow's shadow models and models drawn in
  the pass), whose program (images 69a6 / a750 at 0x1AF8) marks the work texture with RGBA (1, 1, 1, 1) through
  RenderInfo's shadow matrix (+ 0x2D0) with no depth test, then MGEndDrawShadow (0x130B30, strength in a0) laying the
  work texture over the frame as CT24 with TEXA AEM (marked texels: alpha = the strength) and ALPHA 0x46, Cd x (1 -
  As). Natively: begin/end hooked; each shadow-mode draw's MDT triangles noted with clip = RenderInfo 0x230 x 0x10 x
  0x2D0 x the local-to-world matrix; at the end they are drawn black with alpha = the strength, alpha blend, no depth
  test. Difference left: where triangles overlap, the game's mask darkens once and this twice (needs a stencil in the
  renderer). The dancers in the Dark Shrine cast their shadows again.
- Model data kept (2026-10-31): for most static pieces (the town's map parts) DC reuses the MDT's memory once it has
  built the visual's VU data from it, so a draw that reads the MDT then reads other data. That garbage drew stray
  triangles. It also gave a wrong primitive count, so groups lost step with the VU1 programs skipped (black
  gameplay). CreateVUdataFromMDT (0x135AA0) and its Remake (0x135E50) are hooked: the MDT ("MDT\0", size at + 8) is
  copied as the game builds from it, keyed by the VU data. A draw reads the live MDT while it is still there
  (animated models keep theirs at + 0x20), the copy otherwise. In Norune, the groups numbered now equal the programs
  skipped (91,500 per 300 frames).
- Water (2026-10-31): CWater (Norune's river; CreateVUData 0x1607E0, DrawVu1 0x160F20) is taken over: no VU data.
  Its height field (rows at + 0, columns at + 4, heights at + 8, corners at + 0x20 / + 0x30 / + 0x40, height scale
  + 0x9C) is drawn natively as strips in the water colour (+ 0x90), alpha blended, no Z write. The game's refraction
  (a copy of the frame drawn through the water) is not drawn yet.
- Depth of field natively (2026-10-31): the town (EdDrawDOF), the dungeons and the cutscenes call DepthOfField
  (0x1652C0). It stretches the 640 x 224 frame to half its width (and that to half again) in a work texture through
  the GS, then draws that texture back in strips, Z test GEQUAL at the focus depths. The GS copy has none of the
  native 3D, so the strips would lay its stale pages over everything behind the focus. It is not what spoils
  Norune (64 x 32 blocks of stale GS pages over the town): those stay with DepthOfField skipped
  (`DC_NO_DOF=1`). DepthOfField is taken over. The renderer gets
  Material::fromFrame: a texture that is the frame as drawn so far, copied at the draw's group with the width halved
  1 or 2 times by bilinear blits, as the game stretches it. The adapter draws the game's strips as they are (8 x 41
  then 14 x 21, depths from its table 0x266930 through the matrix at 0x1C75570, the haze's random walk at 0x1D18A00)
  sampling that copy. `DC_NO_DOF=1` still skips it for diagnosis.
- Back faces (2026-10-31): the models' program (f256...) culls. Its flag word at VU 0 is DrawVu1's qword (x the
  program x 2, y RenderInfo + 0x33C, z RenderInfo + 0x310). With z bit 2 set, its list loop (0x1300), strip loop
  (0x1510, the winding taken back every vertex) and clipper (0x2368) all take the screen-space (v2 - v0) x (v1 - v0).
  A negative z sets ADC on the triangle's last vertex. The adapter drew every face: characters drawn without Z write
  (ZBUF ZMSK) showed their insides over their fronts (Seth's cape, Flag's head in the Dark Shrine). The same test now
  runs in submit for models with the flag (`DC_NO_CULL=1` turns it off for diagnosis).
- Texture binding (2026-10-31): CreateVUdataFromMDT binds each primitive's texture once, when it builds the VU data.
  GetTextureHandle -> SearchTextureName gives the newest record of the material's name; a missing name gives handle
  -1, which GetTexture takes as record 0. The record's TEX0 goes into the VU data, and Remake keeps it. The adapter
  looked the name up at draw time, so a later model with a texture of the same name took over: the Colonel's face
  was blank and the Genie's belly plain once both were loaded. The create hook now notes the record per primitive
  (with the MDT copy, keyed by the VU data), the same way and at the same moment; draws use it.
- Shadow pass on the GS side: dropped (2026-10-31). MGBeginDrawShadow / MGEndDrawShadow are taken over entirely
  (the shadows are native). Their GS work cleared 432 x 432 of the work texture (fbp 210, where Norune's water also
  copies the frame) and laid all 640 x 224 of it over the frame. No program marks it any more, so the stale rest
  blacked out Norune, with page-sized pieces of old frames left (`PS2X_GS_NATIVE_TARGETLOG` showed target 210
  reloaded from GS memory every frame).
- Vertex colours and fog (2026-10-31): two parts of the model program the adapter skipped, which changed every
  scene's look against the VU1 reference run. (1) Colour streams: an MDT with + 0x20 has a fourth index per tuple.
  SetVuData copies that colour (four floats) into the VU data and sets 0x100 in the primitive header. The program's
  lighting at 0xBF8 multiplies the lit colour by it per component before the clamp (0xAD0 is the same without it).
  Map pieces are prelit this way. (2) Fog: DrawVu1 sets PRMODE FGE when RenderInfo + 0x324 is set (outside the
  shadow pass) and FOGCOL from + 0x338. It puts (+ 0x328, + 0x334, + 0x330, + 0x32C) at VU 22; the program gives
  each vertex F = max(min(VU22.x + VU22.w / w, VU22.y), VU22.z). Both are now in drawModel (Vertex::fog = F / 255,
  Material::fog with that colour). `DC_LOG_VCOLOUR=1` logs the first colours read.
- Near geometry and runaway vertices (2026-10-31). (1) Z: the models' GIF tag writes ST, RGBAQ, XYZF2 (REGS 0x412)
  PACKED, where the GS takes Z from bits 4..27 of the FTOI4 word: Z is z / w itself, 24 bits. The adapter used 16 x
  z / w (an XYZ2's Z). Near geometry went past 2^24 and wrapped to the far end: models behind drawn in front, and
  triangles near the camera missing. A clamp tried in between flattened near characters (inside-out faces). The
  scale is now 1 and the renderer's 24-bit wrap stays, as XYZF2 itself does.
  (2) The guard band: without RenderInfo + 0x310 bit 1, the program does not clip. Its loops keep a triangle only if
  every vertex's screen x / w, y / w and w lie strictly inside VU 20 / 21 (RenderInfo + 0x210 / + 0x220: FMAND of the
  signs of VU21 - v and v - VU20, mask 0xD0). In Norune those are x, y in (0, 4095.9) and w in (6, 65535). Otherwise
  ADC drops it whole. The adapter now does the same (`DC_NO_GUARD=1` turns it off). Cloth grids hold runaway
  vertices (about 1e37) that the PS2 never shows for that reason; they were the streaks across Norune.
- IM2 archives (2026-10-31): 2,546 of DC's texture directories use "IM2\0" instead of "IMG\0", with the same layout
  (the map pieces' textures, s140101...). `tools/dc_texconv.py` reads both: 3,615 distinct 8-bit textures (was 921).
  A store miss falls back to the converted indices of that name and size, coloured with the record's current
  palette (plain CT32 in main memory), and every miss is logged once.
- IM2 means pre-swizzled (2026-10-31): EnterIMGFile (0x132BA0) compares the archive's magic with "IM2" and passes the
  result to EnterTexture as its last argument; with it set, EnterTexture copies the 8-bit texels unchanged instead of
  running Conv8to32, i.e. IM2 pictures are already in GS memory order on the disc. The converter took them as linear
  (the intro's book, the "Norune Village" title and the map pieces' textures came out scrambled). They are now
  resolved offline (`tools/gs_layout.py`): the data laid out as the PSMCT32 image of half the width and height the
  game uploads, read back as PSMT8. No GS layout function runs in the game.
- No GS (2026-10-31): `PS2X_GS_RENDERER=none` is DC's default (scripts/run.sh). The native renderer's present()
  draws the frame in the game's call order. The 2D routines are taken over one by one from their own arguments and
  the game's records: clears, fills, the set2DSprite family, the message windows' glyphs (set2DSprite_Core) and
  text colour (SetClut), sprite tables, the effects' 3D sprites, texture animation (MGMoveImage / MoveImageTest
  applied to the decoded textures), the balloons (drawn into an offscreen target), DoF (a copy of the frame).
  present() now honours Material::depthTest (the 2D was depth-tested against the 3D and hidden).
