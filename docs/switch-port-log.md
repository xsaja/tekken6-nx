# Switch port log

The Switch port in the order it was done: what was found, what changed, how it was checked and what the console
measured. It started as the notes of a stall in Ryujinx (2026-11-09), hence the first sections.

## The Switch NRO stalls at ~0 fps in Ryujinx (2026-11-09)

Read first: `docs/plan.md`, `docs/progress.md` (Phase 3), `docs/tekken6-notes.md`. Last commit when this was written: `9488268e`.

## Goal and state

Phase 3: the Switch build with reNut on deko3d, NRO, title screen in Ryujinx.
- Milestone 1 (build links) done, `app/out/switch/renut.nro` (56.5 MB). Milestone 2 (runs in Ryujinx) done.
- Milestone 3 (title screen drawn) was seen once: runs `emu4` and `emu6` drew the attract movie and PRESS START at ~23 fps.
- **Open problem:** later runs stop presenting frames (game 0 fps, no crash, ~22-35 % of 400 % CPU, every thread waiting in the
  kernel, no PNG captures after the stall). It happens with and without scripted input.

## Setup (all paths relative to the folder that holds the repository)

| What | Where |
|---|---|
| repo | `tekken6-nx/` |
| game files (disc extracted) | `game/` ; junction `ryu-data/sdcard/switch/tekken6-nx/game_root` -> `game/` |
| Ryujinx fork build | `ryu-build/Ryujinx.exe` (source `refs/ryujinx`, `dotnet build src/Ryujinx/Ryujinx.csproj -c Release -o ...`) |
| Ryujinx data root | `ryu-data/` (keys + firmware copied in; never commit; not in the repo tree) |
| NRO under test | `ryu-data/sdcard/switch/tekken6-nx/renut.nro` (copy of `tekken6-nx/app/out/switch/renut.nro`) |
| NRO caches on the SD | `.../tekken6-nx/deko3d_shaders.bin` (DKSH variant cache), `pipeline_list.bin` |
| NRO logs on the SD | `ryu-data/sdcard/switch/rex_profile.log` (profiler + stack samples), `rex_stderr.log` |
| Ryujinx log | `tekken6-nx/out/emu_runs/<name>/ryujinx_stdout.txt` |
| run script | `tools/run_ryujinx.ps1 -Seconds N -Name emuX -MinCommitGB 7 -Toml out/emu_tX.toml` (needs `-ExecutionPolicy Bypass`; copies the toml to `katamari.toml` beside the NRO; moves PNG captures into `out/emu_runs/<name>` only when the run ends by itself) |
| Switch build | `tools\build_switch.bat` (incremental, ~3 min; `-- -k 0` for ninja flags), toolchain devkitA64 |
| shader chain | `build-logs/switch_shaders_local.ps1` (a copy of `tools/switch_shaders.ps1` with this host's Python); output `app/out/renut_switch/`, `out/deko3d_shaders/{glsl,glsl_opt,dksh,report.txt}` |
| precompiled variant cache | `python tools/deko3d_variant_cache.py --list app/out/pc/pipeline_list.bin --list <Switch pipeline_list.bin> --merge <cache> --out out/deko3d_shaders/deko3d_shaders.bin` -> 4,104 variants, 13.9 MB; the old run-time cache is saved as `out/deko3d_shaders/switch_run_cache.bin` (483 KB) |

Ryujinx needs about 8 GB of free physical memory (the PC has 15.7 GB): run `dotnet build-server shutdown` first and check
`FreePhysicalMemory`. Test tomls (in `out/`, not committed): `emu_t3.toml` (captures every 10 s), `emu_t4.toml` / `emu_t5.toml`
(scripted input: `input_script = "25:START,32:A,37:DOWN,39:A,43:A,..."`, `headless = true`, `switch_profile_stacks = true`),
`emu_t6.toml` (captures every 2 s, no script, stacks on). Keys: `nfsmw_captura_cada_s`, `nfsmw_captura_max`.

## Run history (what each run did)

| Run | Setup | Result |
|---|---|---|
| emu1 | first NRO | guest crash at 38 s in the SaltyNX probe's `fprintf(stderr)` (fixed: `RexSwitchSaltyNxEnable(0)` in `app/src/renut_app.h`) |
| emu2 | after that | game exited at ~12 s: `game_root` not found, because Ryujinx starts homebrew with no argv and `REX_SWITCH_FALLBACK_EXE` still named Katamari (fixed in `app/CMakeLists.txt`) |
| emu3 | fixed | 240 s alive, 13-24 fps, no captures |
| emu4 | captures every 10 s | OK: 12 PNGs, PRESS START over feathers, attract movie (Nina), ~23 fps |
| emu5 | NRO rebuilt with the new shader package (126 uam failures fixed), 7.75 GB free | **0 fps from ~10 s, 0 PNGs** |
| emu6 | same NRO as emu5, 8.4 GB free | OK: 12 PNGs, 20-50 fps for 120+ s |
| emu7 | scripted input, small cache | title -> main menu (~35 s) -> "NOW LOADING" for a fight (~90 s); then ~0 fps; PNGs stop at 95 s |
| emu8 | scripted, 420 s, stack sampling | 0 fps from the 7th sample; the `plume::Deko3DDevice::compileWorker` thread (runs uam, the shader compiler) was busy; about 228 variants compiled in 7 min |
| emu9 | scripted + the 13.9 MB precompiled cache | stalled at ~30 s, PNGs stop at 20 s (two black ones); cache file unchanged (no run-time compile); all threads waiting |
| emu10 | **no script**, big cache, captures every 2 s | 3.9 fps at 10 s then 0 fps, **no PNG at all**; killed by hand at 2:15 |
| emu11 | no script, **small cache back**, 150 s | **also stalls**: 0.7 fps at 10 s then 0 fps, 1 PNG (2 s). So hypothesis 4 (the big cache) is ruled out |

So the stall is not tied to input nor to the cache size. Since emu5 the runs are mostly bad (emu6 and the early part of emu7 were fine);
emu5 vs emu6 are the same NRO and cache. Something about the state around ryu-data may have changed after emu6 (Ryujinx's own
caches under `ryu-data` such as the guest-code PPTC and its shader disk cache; `ryu-data/games` / `ryu-data/bis`): worth trying a run
with those folders removed (keep `system/` keys and `bis/system` firmware).

## SOLVED (2026-11-10): a stolen auto-reset SetEvent in the POSIX/Horizon threading layer

emu16 (fixed NRO, no script, 150 s): 20-23 fps to the end, 57 PNGs up to 128 s (attract movie, FREE PLAY). Before: 0 fps
from ~25 s.

emu17 (fixed NRO, scripted input, `out/emu_t7.toml` = emu_t5 with captures every 5 s, 240 s, small 483 KB variant cache):
**a live fight**. PRESS START 26 s, main menu 37 s, Offline Mode / Arcade Battle 47 s, character select (Lars) 52 s, VS
screen Lars vs Zafina stage 1 at 80 s, intros 115-153 s, ROUND 1 at 158 s, FIGHT at 174 s, fight running 180-206 s
(timer 59 -> 56; rooftop stage, rain and HUD drawn right). Profile: 30-57 fps in menus, 7-12 fps in the fight at
~230 % CPU (Ryujinx, not a console figure). Two slow stretches remain, at ~90-100 s (0.2 / 0.0 fps, no PNG between 80 and
115 s) and ~140 s (1.1 fps): **shader variant compiles, not a deadlock**. The `plume::Deko3DDevice::compileWorker`
thread is at 96-99 % in uam (`nv50_ir::Program::optimizePostRA` -> `PostRADualIssue::visit`, `Value::interfers`) and
Main XThread waits in `D3DDevice_Swap_hook -> render_thread::QueueFrame -> PosixConditionBase::Wait` behind it.

emu18 (same as emu17 with the precompiled cache): **no stall at all**. The cache was rebuilt first with the 12 variants
emu17 compiled that it lacked: `python tools/deko3d_variant_cache.py --list app/out/pc/pipeline_list.bin --list <SD
pipeline_list.bin> --merge out/deko3d_shaders/deko3d_shaders_4104.bin --out out/deko3d_shaders/deko3d_shaders.bin`
-> 4,116 variants, 13.4 MB, copied to the SD (backups: `deko3d_shaders_4104.bin`, emu17's run cache
`switch_run_cache_emu17.bin`). Per sample 38, 53, 33, 53, 30, 34, 43, 13, 39, 35, 27, 25, 25, 26, 23, 21, 21, 23, 24,
24, 23 fps; minimum 13 fps at ~80 s (emu17: 0.0). Loading/VS 67 s, Lars intro 84 s, READY (Lars vs Kazuya, dojo) at
~100 s (emu17: FIGHT at 174 s), fight to the end of the capture at 220 s (timer 13). The cache grew by 15 KB (a few
run-time compiles, none visible as a stall). In the fight the render thread spends its time in `PagesClean` /
`PmullHash` (guest-memory dirty-page hashing), `ConvertVertices`, `WholeGeometryTry`; fps drifts from ~39 to ~22.

**First console run (2026-11-10, package `builds/2026-11-10_0024_tekken6_consola`, logs in
`out/console_runs/console1/`)**: no freeze, no crash. 50-60 fps in the menus, 44-53 fps in the first fight, 34-38 fps in
a later stretch, ~42 at the end (one 13 fps sample at a load). CPU 1020 MHz, GPU 460.8 MHz, three cores (mask 0x7):
CPU total ~270 % of the 300 % the process has, so CPU-bound. `python tools/switch_profile_groups.py <rex_profile.log>
<renut.elf>` (samples 12+) gives, per thread group:
- 6 job-pool XThreads: 116 % CPU together; 37 % of it in `sub_82548758`, a VMX128 loop blending four matrices by
  weights per vertex (character skinning). The biggest single lever: a native NEON version (hot-function method).
- render thread (core 1): 53 %; `PagesClean` 27 % + `PmullHash` 5.5 % (guest-memory change checks), `ConvertVertices`
  13 %, `FastDrawLocked` 10 %.
- Main XThread: 59 %, spread thin (top function 3.8 %).
- XMA Decoder: 13 % (ffmpeg WMA decode).
The fourth core (README "Cores": the forwarder NPDM) would add a third more CPU without code.

How it was found. The guest main thread's stack *is* in `rex_profile.log`; symbolize with
`aarch64-none-elf-addr2line -f -C -e app/out/switch/renut <offset>` (the unstripped ELF; `image+X` is the ELF address):
`Main XThread` waits in `PosixConditionBase::Wait <- rex::thread::SignalAndWait <- NtSignalAndWaitForSingleObjectEx <-
sub_824BD200` (kernel32 `SignalObjectAndWait`); the guest frames above it are not in the walk. A temporary log of the
SignalAndWait calls blocked over 2 s (handles + object types, removed again) showed threads 0x4 and 0xC parked for ever
on one mutex + auto-reset event pair from ~26 s, while the frame clock (0x82C66098, mutex/event F8000060/64) kept ticking.

The pattern: the game's condition variable `sub_82528B70` (init: mutex + auto-reset event + 64-bit value),
`sub_82528C10` (lock, value = v, unlock, SetEvent) and `sub_82528C68` (lock; while value != t: SignalObjectAndWait(mutex,
event), relock). The object at +144 of `sub_82519F50` is a ping-pong between the main thread (`sub_82519918` sets 1,
`sub_82519928` waits for 0) and its worker (`sub_82519EC0`: waits for 1, works, sets 0). The main thread's SetEvent left
the event signalled until the woken worker took the event's host mutex; the main thread got there first in its own
wait for 0, consumed the event, saw value 1 and slept again. The worker then found the event clear: both asleep for
ever. Windows hands a SetEvent to the oldest thread already waiting, so the event is never left for a newcomer.
Timing-dependent (Ryujinx scheduling), hence emu4/emu6 fine; the PC uses native Win32 events and never hits it.

Fix in `sdk/src/core/threading_posix.cpp`: an auto-reset event keeps a FIFO of waiting threads and `Signal` / `Pulse`
grant the oldest one directly (`GrantOrSet`); the state is set only when nobody waits. `SignalAndWait` joins that
queue (`Arm`) before it signals, like Windows' atomic signal-and-wait. Also `PulseEvent`: it was `Set; sleep 10 us;
Reset`, which loses the wake-up the same way (the job-pool barrier sub_822E1B28 / sub_822E1AA0); a manual-reset pulse now
bumps a generation that every current waiter sees. Known limit (comment in the code): a WaitForMultipleObjects caller does
not see a manual-reset pulse and is not in the auto-reset queue (it still sees the state when nobody is queued).
`sdk/tests/pulse_event_test.cpp barrier|pingpong [s] [old]` replays both patterns on a PC: `old` deadlocks (ping-pong at
round 0, barrier in 2-43 rounds), the new code runs 3.6 M ping-pong rounds and 66 k barrier rounds without a loss.

The rest of this note is the history before the fix.

## What the profiler says during a stall (`rex_profile.log`, "Stack sampling: active (1 ms)")

- 94-95 % of samples "waiting in the kernel". The render thread (`renut::gpu::render_thread::Run`) is idle in a wait.
- The hot guest-side thread is `sub_82592C48` (XMedia-related), in `KeWaitForSingleObject`: an idle worker.
- A few samples are in `fsdev_read` -> `pread` (reading files through libnx's fs from the SD in Ryujinx).
- In emu8 the pc samples were inside uam (`nv50_ir::*`, `glsl_symbol_table`) on `compileWorker`: the compiler running.
- **Not known:** what the guest main thread (`Main XThread`) waits for (answered above). Its CPU is 0.7 %. The
  "10.0 s" label of every sample is the report interval, not the uptime. `vblank thread wakes 0` appears in the "system per
  second" line of the stalled samples (not checked against the reNut VBlank thread, see below).

## Hypotheses, none tested

1. File I/O: DATA3xx.BIN reads through Ryujinx's sdmc (a junction to a folder on S:) are very slow, and the game's loader
   (XCTD decode) waits. The `pread` samples support it weakly. Test: time a read of a big DATA file through the junction, or
   copy `game/` into the SD folder (6.7 GB), or add an `[io]` timing log.
2. A real wait/deadlock between guest threads or between the guest and the render thread (fence, `KeWaitForSingleObject`).
   Notes: on PC the first Swap stalled the frame loop until a 60 Hz guest "reNut VBlank" thread was added
   (`hooks/device.cpp StartVBlankThread`, `docs/tekken6-notes.md`); that thread shows 0.4 % CPU here. `threading_posix.cpp`
   (the wait/multi-wait rework of tekken6-recomp) was NOT ported ("Switch only, phase 3" in the notes). It is the first thing to
   try, since the Switch uses the POSIX/Horizon thread layer and the PC does not.
3. Ryujinx itself (memory pressure at 7.75-8.4 GB free, the PPTC / shader disk cache of the fork, `GPU processing thread is too
   slow` warning seen once).
4. ~~The 4,104-variant precompiled cache hurts~~: ruled out by emu11 (small cache, same stall).

## Fixes already made this phase (committed)

`tools/build_switch.bat` (VS Community path, Python 3.12, NVK off, ninja flags after `--`); `app/thirdparty/plume/CMakeLists.txt`
(NVK library check only when NVK is linked); `sdk/thirdparty/FFmpeg/config.h` (`__SWITCH__` branch); `tools/run_ryujinx.ps1`;
`app/CMakeLists.txt` (fallback exe path `/switch/tekken6-nx/renut.nro`); `app/src/renut_app.h` (SaltyNX off);
`app/thirdparty/reblue-XenosRecomp/XenosRecomp/shader_common.h` (`cubeMapDirections[9]`: the 126 shaders uam rejected now
compile, 6,872 / 6,872). Host tools installed: MSYS2 clang64 into `C:\devkitPro\msys2` (for `out/deko3d_tools/uam-build/uam.exe`;
`pacman.conf` got a `[clang64]` repo, backup `pacman.conf.bak-clang64`), `switch-libzstd` (devkitPro pacman, package name
`switch-libzstd`), SPIRV-Cross copied from the Vulkan SDK.

## Other open items

- The boot logos (Bandai Namco / Namco / Dolby, 4-14 s on PC) were black in the Switch captures at 10 s and 15 s; unverified
  whether the game draws them (on PC the automated captures of those screens were black too, and fine in a manual run).
- Lili's dress too shiny / a flickering wall on PC (phase 2). The cube-map fix may explain the dress: the PC shader cache was
  not rebuilt with the new header (`tools\build_pc.bat`).
- Never run on a console. Frame rate in Ryujinx on this PC is not a console figure.

## A good next step

Get a stack of the guest main thread during a stall (the sampler lists threads but its top stacks are dominated by idle
workers), e.g. dump every thread's top frames per name, or use Ryujinx's GDB stub (`GdbStubPort` 55555 is in its config).
Then compare against hypothesis 2 (port `threading_posix.cpp`) before touching shaders again.

## Open (2026-11-10): the 2D art does not show on the Switch (parked)

The boot logos, the Kazuya artwork behind "Select battle position preference", the character-select portraits (and
likely the stage thumbnails) are missing on the console and in Ryujinx; the PC (reNut on Vulkan) shows them. The intro
audio desync is another matter (below). What is established, all from console runs unless said:

- The draw is issued: `banjo_renut_diag_draw_log_s` on the dialog gives the same draw list as the PC
  (`out/pc_runs/dlg_ref2`): 1024x576 post passes, the scene copy to 1280x720, then the 2D layer with pixel shader
  38FFFB8FB02A2068. Kazuya is the 2D layer's first quad (prim 13, blend 00010706), texture header BD5E725C:
  1280x720, fmt 20 (8_8_8_8), linear, endian 1 (8in16), base 0DDFC000, identical on PC and Switch, data in guest
  memory (766 of 1024 words of its first page not zero on both). The draw log now prints each draw's textures.
- The texture reaches the GPU: `banjo_renut_diag_show_texture = "BD5E725C"` shows Kazuya on the console (through
  the slot the game samples). `banjo_renut_diag_show_alpha = true` (new) shows its alpha: 1 everywhere (opaque).
- Not the store cards (`banjo_renut_store_cards_verify` logged no "changed under clean cards"), not a missing shader
  or format (log at warn: none), not a setting: the PC with all 41 Switch-only values (`out/switch_defaults_args.txt`,
  backend left on Vulkan) shows Kazuya. Turned off on the console without effect: banjo_deko3d_texture_skip,
  alpha_spec, draw_resolves, txq_constants, early_z, zcull, vtx_dedupe, depth_handover, push_constants;
  banjo_renut_smaa; renut_post_process_precision High.
- So: an opaque full-screen quad with a good texture draws nothing visible on deko3d. Next ideas: where its vertices
  land (vertex shader D26545C3CD8DC897's constants / the viewport / the quad-list conversion of a full-screen quad),
  or a later draw covering it; a GPU capture would settle it (no RenderDoc on the console: dump the 1280x720 target
  right after that draw, e.g. a diagnostic that ends the frame at draw N).

Intro audio desync: the XMV (WMV9) video decoder thread (`XThread48A281C0`, functions 0x826CE1D8 and 0x826E....)
runs at 98 % of a core during the intro: the video cannot keep real time on the A57. Fix candidate: decode with
FFmpeg's VC-1 decoder (NEON) instead of the game's code; CONFIG_VC1_DECODER / WMV3 are 0 in the SDK's FFmpeg config.

### 2026-11-14 follow-up (parked again)

- Fable's plan (steps 0-2) in Ryujinx: banjo_deko3d_vs_outputs off, the uam passes + async variants off, docked 720:
  no change. `banjo_renut_diag_stop_after_draw` (new, draw.cpp) + the extended DrawLog showed the Kazuya draw
  (ordinal 12 on both; texture, vertices, geometry, viewport equal) gets vs c128/c129/c131 = (0.3585,1.1691) /
  (-0.2959,0.2868) / (0.00977,1.2789) on the Switch vs (2,0) / (0,2) / (0,0) on the PC: a small tilted quad (the grey
  tilted rectangle at the top of every Switch capture).
- Not the renderer: banjo_renut_snapshot_deltas_verify 108,113 draws 0 differ, deltas off same values. Not the port's
  enhancements nor the native skinning (all off: same values). So the guest writes them.
- `tekken_diag_sprite_callers` (new, backend/queued.cpp QueueDraw, stderr "[sprite]"): on the PC the full-screen
  draw (c128.x 0x40000000) comes from lr 8252ACB8 <- 82529060 / 82528D68 <- 82519E88 <- 82519F0C <- 825261C4 (the
  2D layer worker sub_82519EC0). A later Ryujinx run logged the SAME chain with the PC's values (0x40000000) and no
  0x3EB79000 draw, yet the art was still missing and the tilted patch still there: so the wrong-value quad may be a
  different draw than assumed, or the values vary per run. Next: log every prim-13 draw of that chain with its texture
  header and constants on both platforms (not only the two c128.x values), then compare the inputs of sub_8252ACB8's
  caller (sub_82529060 / sub_82528D68) on PC vs Switch.
- Ruled out (2026-11-14): katamari-nx 1.0.1's floating-point codegen keys (fctiwz_inline, fcti_inline, div_inline,
  fp_compares_modeless, fp_mode_lookahead(+_past_branch), fp_copy_pairs, fp_copy_forwarding, fp_moves_modeless) added
  to app/renut_config.toml, fresh codegen + post_codegen + Switch build: boot logo still black at 10 s, the dialog the
  same (tilted patch, no Kazuya) in Ryujinx (out/emu_runs/dlg_fpkeys). Reverted to the bring-up config and regenerated.
  A diff of katamari-nx-1.0.1 against this tree found no other float-related difference (same post-codegen passes,
  compile flags and plume_deko3d); its rc10 black screen was a deko3d centroid-varying parse, already in this tree.

## Where things stand (2026-11-14) and how to continue

Reference build: `builds/2026-11-14_0929_tekken6_pgo_lto` (PGO + LTO, profile in `pgo/` from one console session,
not in git; `app/out/switch-pgo-use`). Console, stock clocks: fights 42.5 fps average (34.9 before), 54 max, rain
stage ~22. Everything since `ce7a7593`: threading deadlock fixed; native blend jobs sub_82548758 / sub_825481F8
(`app/src/cpu/tekken_skin_native.cpp`, bit-exact); SaltyNX overlay works (`tekken_saltynx`); the 4,116-variant shader
cache on the SD. Any code change leaves part of the PGO profile stale (those functions just lose it); a fresh codegen
needs a new PGO session (instrumented build: `BUILD_DIR=app\out\switch-pgo-gen`, `CMAKE_EXTRA=-DBANJO_PGO=generate`;
merge: `python tools/pgo_merge.py <folder of .stream>`; final: `-DBANJO_PGO=use -DBANJO_SWITCH_LTO=ON`).

Next, in order of value:
1. Fourth CPU core: the game is CPU-bound on 3 cores (~270 %), Core 3 sits at 38-45 % with system work only. Needs a
   forwarder NSP on the console and its program id in `banjo_forwarder_program_id` (dist/switch/README.txt "Cores").
2. Rain stage (~22 fps): workers spend time in sub_822E05C0 / sub_822E10F0 (scalar float, many calls, likely
   particles); measure with `python tools/switch_profile_groups.py <rex_profile.log> <renut.elf>`.
3. Render thread: PagesClean + PmullHash (~a third of it), ConvertVertices.
4. Intro video desync: WMV9 decoder thread at 98 % of a core; FFmpeg VC-1 decoder not enabled in the SDK.
5. Parked: missing 2D art (sections above). Next diagnostic: log every prim-13 draw of the chain
   8252ACB8 <- 82529060/82528D68 <- 82519E88 with its texture header and c128..c131 on PC and Switch
   (`tekken_diag_sprite_callers` in backend/queued.cpp logs only two c128.x values today).

2026-11-15, fourth core: everything is already in the tree (sdk/src/core/threading_switch.cpp: banjo_system_core
0/1/2, banjo_system_core_xma, the mask wrap, RexSwitchEnsureForwarderNpdm) and runtime-only, so the PGO+LTO NRO is
reused unchanged. Tekken's own forwarder id: 05D6EA1333487200 (banjo ...7000, katamari ...7100). Test package
`builds/2026-11-15_tekken6_4nucleos` (LEEME.txt): the NPDM for that id (katamari_main.npdm with the id patched in its
three places) and tomls A = mode 1 + XMA (Audio Worker + XMA Decoder, ~14 % in the Nov 10 console profile, to core
3), B = mode 2 (game threads may use core 3 too). The Sphaira forwarder used for testing cannot pick its id: it runs as
05941069A073B000 (renut_001.log), so the package now carries the NPDM and the tomls for that id.
Console results (same PGO+LTO NRO, stages not identified): A (mode 1): core 3 42-60 % idle, fights in 10 s
windows 49-53 / ~40 / ~36.7 fps. B (mode 2, "game threads on 0xF"): core 3 down to 10-25 % idle, the light stages
54-59 fps, the heavy one still 35-38; audio gaps only around loads; it felt smoother. In the heavy stage the
main thread is the limit (67 % CPU, the ten workers 86-96 % waiting): more cores do not help there, the main thread's
hot functions do (item 2). Lip sync off at ~37 fps: game time is frame-locked, audio real time.
Mode B made the default (app/katamari_switch_defaults.toml: banjo_system_core 2, _xma true, the forwarder id); new
reference build `builds/2026-11-15_0813_tekken6_pgo_lto_4nucleos` (same PGO profile, no code change). The
forwarder's NPDM is under atmosphere/contents/05941069A073B000 in the package.
The next run still logged "banjo_system_core 1" (an old A katamari.toml on the SD overrides the built-in
value). Its findings: rain ~21.5 fps with four workers at ~35 % each and core 3 45 % idle (mode B should help there:
untested); the 0-15 fps dips between fights are uam compiles of pixel-shader variants missing from the cache (22
"BLOCKING (a draw waits)", ~1 s each; cache 4,750 variants at start, it grows, so each stage stalls only the first time).
First real run of the mode-B default build: rain (same signature: four workers at 33-46 %) 18.5 fps vs 21.5 in mode
A; core 3 only ~11 % idle, the XMA Decoder there at 17 % (10 % in A) plus a 10 % thread on core 3 (likely the uam
background compiles). Other stages 44-50 fps, not comparable (different stages each run). So mode B may hurt the
worker-bound rain stage: preempted workers on the shared core delay the frame. Needs a same-stage A/B (toml only).
Mode B stays the default.

Intro video (2026-11-15, console, intro left to play twice): the movie thread (XThread91C14040) is at 98 % of a core
for ~190 s per attract loop. The movies (game/DATA315..320.BIN) are ASF: WMV3 1280x720 29.97 fps at 5.5-8 Mbit/s,
WMA Pro 5.1 48 kHz audio, 40-77 s long (DATA320, 77 s, 8 Mbit/s, the helicopter/Jin opening, is the likely intro).
If one movie fills those 190 s, the decoder runs at ~40 % of real time: it needs ~2.5x, more than hand natives of the
recompiled decoder would give. Candidate: FFmpeg's wmv3/vc1 decoder (NEON) behind the XMV player
(sub_82591648 / sub_82593A18 copy Y/U/V into the game's textures); first a console benchmark of FFmpeg decoding
DATA320 to see whether the A57 reaches 30 fps.
Measured (console, 2026-11-15, `tekken_wmv3_bench`, app/src/cpu/tekken_wmv3_bench.cpp; FFmpeg's WMV3 turned on for
the Switch in sdk/thirdparty/FFmpeg/config_switch_aarch64.h; frames from tools/wmv3_bench_extract.py): 900 frames of
DATA320 in 13.93 s on one core = 64.6 fps, 15.5 ms average, worst 26.6 ms, 0 errors. FFmpeg is ~2.15x real time (the
recompiled decoder ~0.4x): replacing the XMV player's video decode with FFmpeg is viable. Next: find the player's
per-frame decode entry (sub_82591648 / sub_82593A18 and below) and feed FFmpeg the same compressed frames.
Done (untested on the console): app/src/cpu/tekken_video_wmv3.cpp, ported from refs/nfsmw-nx's native WMV3 (same XDK
library, other version). Its header documents Tekken's interface: DecodeData sub_825D2B30 (type [ctx+284]; I/P/B
slots [ctx+15804]/[15808]/[3080]), frame entry sub_825D6328, setup sub_825CF600 (data [ctx+3364]), data thunk
sub_825EA6F8, planes [ctx+3744..3764], loop filter [ctx+3916]. The SDK sniffs ASF headers in xboxkrnl_io.cpp
(TekkenLastAsfRead). Movies are I/P only, no loop filter. Cvars tekken_video_wmv3_native (on), _shadow, _diag.
Test package builds/2026-11-15_1014_tekken6_video_ffmpeg (log_level info: look for "[video] WMV3" lines).
Its console run: files/config/planes all recognised ("planes ok"), but every frame "decoded by the game: call outside
sub_825D6328": the post-codegen passes (direct_calls.py, inline_leaves.py) had made the calls to sub_825D6328,
sub_825CF600 and the thunk sub_825EA6F8 direct/inlined before the hooks existed. Lesson: after adding a REX_HOOK_RAW
of a directly-called function, re-run `python tools/post_codegen.py` (~10 min) before building. The game's own decoder
ran at ~27 fps there (33 ms/frame), not 40 % of real time. Rebuilt: builds/2026-11-15_1123_tekken6_video_ffmpeg2.

## Where things stand (2026-11-15) and how to resume

Session of 2026-11-15, in short (details in the dated notes above):
- Fourth core: done and the default. The test Sphaira forwarder runs as 05941069A073B000 (Sphaira cannot pick the
  id); its four-core NPDM lives at sdmc:/atmosphere/contents/05941069A073B000/exefs/main.npdm (the NRO also writes it).
  Built-in defaults: banjo_system_core = 2, banjo_system_core_xma = true, banjo_forwarder_program_id = that id. The game
  must be launched from the forwarder icon (from Sphaira / title takeover it gets three cores). Light stages ~54-59
  fps; the rain stage looked worse in mode 2 (18.5 vs 21.5 in mode 1) but stages are random in Arcade, so no clean
  A/B; no further tests were run. A katamari.toml on the SD overrides the built-in values (one stale toml
  already fooled a run).
- Intro video: FFmpeg's WMV3 decodes the intro at 64.6 fps on one console core (tekken_wmv3_bench). The native decode
  path (app/src/cpu/tekken_video_wmv3.cpp, ported from refs/nfsmw-nx) is written and built, but NOT yet seen working
  on the console: the first test fell back to the game's decoder because of the post-codegen passes (fixed by
  re-running tools/post_codegen.py; see the note above).

Latest build to test: `builds/2026-11-15_1123_tekken6_video_ffmpeg2` (PGO+LTO, mode-2 defaults, native WMV3 on,
katamari.toml with log_level = "info"). The SD only needs its renut.nro and katamari.toml in switch/tekken6-nx/.

How to resume:
1. Run that build from the forwarder and let the intro play to the end, then bring rex_profile.log
   and logs/renut_001.log. In the log: `[video] WMV3: context ... planes ok ... FFmpeg ready`, then every 5 s
   `[video] WMV3 '...': N frames/s | game G ... | FFmpeg F at X ms` -- success is F > 0 and G = 0 after the first I
   frame, no "decoded by the game" warnings, ~30 frames/s. Check whether lips/music match and whether the
   picture is clean (blocks/green = wrong planes: rerun with tekken_video_wmv3_native = false and
   tekken_video_wmv3_shadow = true to compare).
2. If it works: drop log_level from the shipped toml, measure what the movie thread still spends (WMA Pro audio,
   sub_8263E6B8 / sub_826E50F8), and take a new reference build.
3. Next items by value: the rain stage / main-thread hot functions (item 2 of the 2026-11-14 list; heavy stages are
   main-thread bound at ~37 fps), the render thread (item 3), the parked 2D art.
4. Any new REX_HOOK_RAW on a function that game code calls directly needs `python tools/post_codegen.py` (~10 min)
   before building, or the hook is bypassed. The generated tree (app/generated) is not in git: after a fresh codegen,
   post_codegen runs by itself, but the PGO profile goes stale (new PGO session needed, see the 2026-11-14 section).

Vulkan on the Switch (2026-11-15, idea: the missing 2D art was only ever tried with deko3d on the Switch):
Mesa SDK zip (mesa-26.2.4-switch-unified-horizon-sdk) is extracted to `../sdks/`
mesa-26.2.4-switch. Its libvulkan.a archive index lacked every Rust object (core, alloc...: ~5,200 symbols), so the link
failed with core::panicking undefined; `aarch64-none-elf-ranlib` on it fixed that (original kept as
libvulkan.a.orig). Build folder app/out/switch-nvk (plain -O3, no PGO/LTO; configured with -DREXGLUE_SWITCH_NVK=ON
-DREXGLUE_SWITCH_NVK_SDK=<that>/opt/devkitpro/portlibs/switch; the configure .bat was a local one-off).
Package builds/2026-11-15_2215_tekken6_vulkan_nvk (renut_gpu_backend = "vulkan"): untested. If the logos/Kazuya show
there, the bug is in the deko3d back end; if not, in what the guest computes (c128..c131).
Ryujinx runs emu_vk1 / emu_vk2 (out/emu_runs): NVK starts ("Vulkan on NVIDIA Tegra X1 (GM20B) (NVK GM20B)"), but
every depth target and two 512x288 resolve targets fail: vmaCreateImage -> 0xFFFFFFFE (VK_ERROR_OUT_OF_DEVICE_MEMORY,
11,782 times, in sdcard/switch/rex_stderr.log), msaa_level = 1 made no difference; every capture is black. Colour
targets are created. Likely the emulator does not give NVK the memory it wants for Z images (Maxwell Z "kinds"), so
inconclusive in Ryujinx; the console test is still the real one. The emulator SD keeps the deko3d NRO
(renut.nro.vulkan_20261115 next to it).
Native WMV3 CONFIRMED on the console (build 2026-11-15_1123_tekken6_video_ffmpeg2): DATA320 at 30 frames/s, FFmpeg
150-156 frames per 5 s at ~19 ms, game 0, no rejections; the crow and the audio now look in sync.
Console run of the Vulkan package (2026-11-15): NVK works there (rex_stderr.log has no vmaCreateImage
failure), but the logos are still missing. So the bug is NOT the deko3d back end. Another person's
rexglue Tekken 6 port show the Dolby logo and Kazuya on a Switch (at ~2 fps, i.e. the SDK's emulated Xenos GPU, not
reNut). Next decisive test: the same Vulkan NRO with `banjo_renderer = "emulated"`, `gpu_plugin = "xenos"`
(package's pruebas/katamari_gpu_emulada.toml) on the console. If the art shows, the culprit is reNut (its D3D hooks
feed the guest something that makes the 2D layer's c128..c131 wrong on the Switch); if not, the Switch codegen
(post-codegen ARM-only passes such as pch_neon_shuffles / vperm_one_source / vec_byteswap are suspects, PC is fine).
Ryujinx cannot run this: emu_xenos1 crashed Ryujinx after 30 s (host VK ErrorOutOfDeviceMemory).
Console result: a plain red screen at ~2.2 fps; the main thread at 100 %, 53-67 draws and 13 resolves a frame (the game
runs and draws). So the SDK's emulated GPU does not present correctly on this NVK build: inconclusive about reNut.
The other person's port presumably runs upstream rexglue's GPU path, not this tree's. Next, cheaper and decisive: the
planned guest-side trace (item 5 above): log inputs/outputs of the 2D layer chain (sub_8252ACB8 <- 82529060 /
82528D68) on the PC and in Ryujinx (deko3d build, which reproduces the tilted patch), find the first function whose
output differs, then read its generated code for Switch-only transformations.

Rain/snow work (2026-11-16): console profile out/console_runs/rain_1123 (build 1123 ELF there): rain/snow ~19.5 fps;
four workers 35-48 % (sub_822E05C0 24 % of worker samples, sub_822E10F0 7.5 %, the skin native sub_82548758 18 %),
main 37 %, render 26 % (PagesClean, ConvertVertices), XMA 18 % on core 3. First machine-copy batch: 984 functions in
app/src/cpu/hot/localized_bulk.txt (switch_hot_bulk.py --min-samples 1 --merge, plus 822E37A8 by hand: `.part.0`
names hide functions from the tool); post_codegen re-run. sub_822E05C0 / sub_822E10F0 cannot be copies (global lock,
fences, raw guest access): hand-native candidates if rain stays slow. sub_8230F128 skipped (REX_LOOP_OUT unknown to
localize_native). Instrumented PGO build with the copies: builds/2026-11-16_0009_tekken6_pgo_instrumentado_copias
(app/out/switch-pgo-gen). Next: play it (rain/snow if they come up), brings sdmc:/switch/tekken6-nx/pgo/
*.stream; then a NEW profile folder (keep the old pgo/ as pgo_20261114), `python tools/pgo_merge.py <streams>`, and
the pgo-use + LTO build.

PGO session with the copies done (2026-11-16, one 16 min stream, kept in out/console_runs/pgo_20261116; the old
profile is pgo_20261114/, the new one pgo/); the pgo-use + LTO build follows (build-logs/switch-pgo-use-copies-20261116.log).
Screenshots of that run (2D glitches): the tilted grey rectangle top-left on the loading screen, a huge
tilted white band across the button-settings menu, the "VS" splash stretched; the stage-select thumbnails DO draw. So
the 2D art is drawn but with a wrong transform (same family as Kazuya's c128..c131), not missing textures/shaders.
Tool idea agreed to analyse: frame capture on demand (hold MINUS ~1 s): reuse [drawlog] (NextDrawOrdinal in
render/draw.cpp is time-triggered by banjo_renut_diag_draw_log_s; add an atomic request set from
sdk/src/input/switch/switch_input_driver.cpp, which already reads HidNpadButton_Minus) plus one PNG
(nfsmw_nativo_captura), written to sdmc:/switch/tekken6-nx/captures/. Behind a cvar, off by default.

Built (2026-11-16): builds/2026-11-16_2230_tekken6_pgo_lto_copias = PGO (new profile) + LTO + the 983 copies + the
capture tool; no profile-mismatch warnings. UNTESTED on the console. Capture tool as implemented: cvar
input_capture_hold_minus (sdk/src/input/switch/switch_input_driver.cpp: MINUS held 0.7 s = one request, MINUS hidden
from the game), render/draw.cpp NextDrawOrdinal logs the two frames after a request as [drawlog] (needs
banjo_renut_lean_diagnostics = false: the hot draw functions were left untouched to keep their PGO profile, and
log_level info), native/nfsmw_nativo_captura.cpp saves capturas/captura_menos_NNN.png. The package's
captura_con_menos/katamari.toml turns it on. Take captures of the loading screen (tilted grey patch) and
the button-settings menu (white band), then compare the 2D draws' c128..c131 with the PC draw log.
Performance analysis (order): codegen register keys still at bring-up in app/renut_config.toml
(biggest lever for the flat main thread); XMA FFmpeg table cache from nfsmw-nx; hand natives for the particle
functions; skin native tuning (31 % of worker samples in normal fights); render thread PagesClean/ConvertVertices/
FlushVertexFills (58 % of it). Possible repeatable rain test: the title-screen background chooser (stage thumbnails).

Console result of 2026-11-16_2230 (out/console_runs/copias_2230, mode 2): rain/snow 21.8 fps average over 18 windows
(20.2 in rain_1123: +8 %), other loaded fight windows 45.4 (46.6 before; different random stages, so no real change
shown). The copies run (n_825234A0 7 % of main in rain, n_824F7CB0 5.8 % of workers). Rain workers now: skin native
sub_82548758 16.7 %, sub_822E05C0 14.3 % (24 % before), sub_822E10F0 6.4 %. Conclusion: copies + PGO gave little;
the next gains need the targeted items (particle natives, skin, render thread, codegen keys).

Capture tool works on the console (2026-11-16, out/console_runs/captura_2d_1: 9 PNGs, 105,738 [drawlog] lines in
drawlog.txt; requests made while loading log draws but save no PNG, "no game image"). Findings for the 2D quads
(vs D26545C3CD8DC897, ps 38FFFB8FB02A2068, prim 13; c128/c129 = 2x2 transform, c131 = translation, c132 = size):
- button-settings menu, frames 1957/2078/2159, draw 345 (size 468x300) = the white band: c128 (0.0487,-1.281),
  c129 (-27.18,-23.04), c131 (-1.95,-2.09), identical in all three frames; draws 346-350 beside it are sane
  (diagonal scales 0.09-0.59).
- practice VS screen, frame 4129: 736 quads; many with c128 = c129 = 0 (so nothing shows: the missing portraits),
  some with denormal garbage (-1.6e-37, 5e-38), some sane.
So some sprites get a transform that is zero or stale memory while their neighbours are right: the transform of
those sprites is not computed/written (or read before it is) on the Switch. The instruction-forms self-check passes
(248 of 248), all enhancements off did not change it (2026-11-14). Suspects: the 2D layer job (sub_82519EC0 worker)
versus its reader (ordering/wait semantics in threading_posix, the earlier note that values vary per run), or one
guest function on that path miscompiled for ARM. Next: the same menu's draw log on the PC (what draw 345 should be),
then trace who writes that sprite's matrix (chain 8252ACB8 <- 82529060/82528D68 <- 82519E88).

Trace started (2026-11-16, stopped by the usage limit): the sprite draw is sub_8252ABD0 (draw call at 8252ACB8:
bl 826B6828 with prim 13, count 4). Before it: vtable call [[r31+8]]+40 with r4 = [r31+12], then sub_8252B050(r31,
flags [r31+20], device), sub_8252AB08, sub_82532D30(r3 = [r31+8], r4 = 18, r5 = [r31+24], r6 = [r31+28]): one of these
sets c128..c133 from the sprite object at r31. Next: read sub_8252B050 / sub_8252AB08 / sub_82532D30 to find where the
2x2 matrix is loaded from, log that source struct on PC and Switch for the 468x300 sprite of the button-settings menu
(PC draw log of that menu first), and walk back to its writer.

2D transform trace (2026-11-17, Ryujinx out/emu_runs/emu_sprite1..3 and emu_watch1, PC out/pc_runs/sprite_pc1):
- tekken_diag_sprite_nodes (queued.cpp, stderr [sprite-node]): the 2D quad's c128..c131 come from the draw node's
  constant block at [node+12] (= node+0x80): size, color, then the transform at block+32. PC: every node ok. Switch:
  the same nodes often have the transform written as ZERO scale (with a computed -0 in c129.w) or left as the
  FFFF0000 fill; size and color are right. So the game writes the transform, with a zero scale (stuck animations?).
- Ruled out: the CAS barrier (banjo_cpu_guest_cas_no_barrier = false: logos still black in Ryujinx), physical
  windows A0/C0/E0 (the three aliases hold the same bytes), the display refresh rate (common code).
- TEKKEN_WATCH_STORES (renut_pch.h, compiled only with -DTEKKEN_WATCH_STORES; app/out/switch has it in
  CMAKE_CXX_FLAGS_RELEASE now): the functions storing into the blocks' transform words, all on one thread:
  sub_82542C20 (block+0x20 = the matrix), sub_82542AA8, sub_82542BB8, sub_82542CD8 (same family), sub_8252AD20,
  sub_8251A650, sub_8253CBF8, sub_8252CCC0, sub_82520F08, sub_82529A38, sub_82530900. Next: read sub_82542C20 and
  its family, find the scale input (likely an animation value), and compare it with the PC.
  The original pch is in out/renut_pch.h.before_watch.

2D art root cause narrowed (2026-11-18, Ryujinx, diagnostic build app/out/switch; hooks in app/src/cpu/tekken_diag_2d.cpp,
cvar tekken_diag_matrix; sub_82542AA8/82542C20/822E8E08/8256F458 removed from localized_bulk.txt for the hooks):
- The logos are one effect item (sub_822E8E08 draws it with the item matrix at [item+44], a stack temporary). That
  matrix = A x B in sub_8256FAC8 (lr 8256FB30 -> sub_8256F458): A = identity + (640,360), right; B = the animation's
  per-node matrix from a table (sub_8256D598 -> jump table 82802EC0 -> sub_8256D3D0: [obj+32]+8 + index*64), at
  A9A3A850 / A9A39810: ALL 16 floats ZERO. Other nodes' entries are filled (rotated/scaled values).
- A guest-store watch (TEKKEN_WATCH_STORES in renut_pch.h, range A9A39800-A9A3A900) saw NO guest function ever write
  those entries in 120 s: the data arrives through host code (file read / decompression / host copy).
- Ruled out today: the five ARM-only codegen paths together (REX_GUEST_CAS_LEGACY, REX_BSWAP128_TABLE,
  REX_SIMDE_SHUFFLES, REX_FLOAT_LOADS_INTEGER, REX_VSEL_OFF: still zero), native memcpy/memset (banjo_hand_memcpy/
  memset, banjo_cpu_native_memcpy/memset = false: still zero), the clock (guest ticks start at 0), different game
  data (Ryujinx's game_root is a link to the PC's game folder).
- Next (in progress when paused): sdk xboxkrnl_io.cpp NtReadFile got tekken_diag_read_watch (hex address): logs the
  read that fills it ([readwatch] file, offset, bytes). Set it to A9A3A850 in out/emu_matrix.toml and run 120 s. If
  no read covers it, look at the XCTD/XMemDecompress path (sdk/src/system/xfile.cpp) and other host writers, and
  compare the same table entry on the PC.
- State: app/out/switch CMAKE_CXX_FLAGS_RELEASE = -O3 -DNDEBUG -DTEKKEN_WATCH_STORES; renut_pch.h has the watch
  macro (inactive without the define); the emulator SD holds the diagnostic NRO (renut.nro.deko3d_20261114 beside it).

Build: `MSYS_NO_PATHCONV=1 cmd /c "set BUILD_DIR=<repo>\app\out\switch-pgo-use&&
tools\build_switch.bat"` from the repo root (Git Bash; ~7-9 min; the folder is configured with PGO use + LTO).


## 2D art: root cause found and fixed (2026-11-19)

Not the renderer, not the game data: a codegen bug in `tools/loop_locals.py`.

- The 2D library's matrix getter `sub_8256D598` dispatches through the table at 0x82802EC0; cases 0/1 go to
  `sub_8256D5B8` (a descriptor-resolving loop, a `switch` whose cases 0/1 jump back to the loop head and 2-5 call
  `sub_8256D3D0/D3E8/D400`, which read the index at `[r4+4/8/12]`).
- loop_locals.py took that loop as a region ending at `case 1: goto loc_8256D5B8;`, inside the switch. The later
  cases leave the region through their `case` labels (no goto, so no exit stub) and the `REX_LOOP_OUT` stores were
  emitted after case 1: dead code. On the PC the loop locals are references to ctx (no effect); on the Switch they
  are real locals, so cases 2-5 ran with the stale `ctx.r4` (the node, not the descriptor) and read the raw node id
  (0x921) as the matrix index: table + 0x921*64, zeros, zero scale, nothing drawn. Same bug in `sub_8256CBC8`.
- Fix: the stores moved before the `switch` in both functions in `app/generated/renut_recomp.4.cpp` / `.21.cpp`
  (hand edit, marked tekken6-nx 2026-11-19), and loop_locals.py now rejects a region whose lines leave a block open
  (counter `open_block`), so a fresh codegen does not bring it back. Three other functions (82363070, 82431828,
  8230F128) have the same dead stores but all their cases are gotos to stubs: harmless.
- Verified in Ryujinx (out/emu_runs/emu_loopfix, emu_loopfix_img): the getter picks table+0x100/0x140 like the PC,
  0 zero-scale results (187 before), the Bandai Namco / namco / Dolby logos and PRESS START text show.
  Console: not yet tested (PGO NRO app/out/switch-pgo-use/renut.nro, 2026-11-19 05:32).

## Optimisation: codegen phases A + B (2026-11-19)

- Phase A (no audit): cr/ctr/xer/reserved as locals, skip_msr, katamari-nx's fp / dform keys. Phase B (audited, see
  docs/tekken6-notes.md "Codegen options: the register diet"): non_volatile_as_local (v64-v127 kept),
  non_argument_as_local (80 functions kept), elide_lr_stores, setjmp 0x826C8510 / longjmp 0x826C81F0. Both checked in
  Ryujinx (out/emu_runs/emu_phaseA, emu_phaseB, emu_phaseB_movie: logos, intro movie, menus, a full fight, no fault).
  Diagnostics cleanup: tekken_diag_2d.cpp out of the build, its four functions back in localized_bulk.txt.
- New PGO session (instrumented, user's console at 1428 MHz OC, out/console_runs/pgo_20261119); profile pgo/ (old one
  pgo_20261116/). Build builds/2026-11-19_2223_tekken6_pgo_lto_dieta (app/out/switch-pgo-use, no profile mismatch).
- Console, stock 1020 MHz (out/console_runs/dieta_2223): mean 54.6 fps over 95 windows (43.6 in copias_2230), none
  below 40 (20 below 30 before), CPU 222 % (258 %). Rain / snow most likely did not come up (sub_822E05C0 1.9 % of the
  workers, 14-24 % in rain before): still to measure. Workers: skin sub_82548758 35 %; render thread: PagesClean 24 %,
  ConvertVertices 19 %, FlushVertexFillsLocked 7.6 %.

## Particle natives (2026-11-19 night)

- `app/src/cpu/tekken_particle_native.cpp` (in app/CMakeLists.txt): hand natives of the two rain / snow particle
  updates, `sub_822E05C0` (5-150 particles an emitter, called by sub_822E1AA0) and `sub_822E10F0` (103 an emitter,
  called by sub_822E2B40), hooked with REX_HOOK_RAW (`python tools/post_codegen.py` re-run: the two callers now call
  the hooks; neither function is in localized_bulk.txt). Technique: the generated code and its callees transliterated
  with host locals (same float expressions and operand order, the same simde calls for the VMX matrix-register
  helpers v92..v95, the CRT atan2 sub_826A1000 and the sine table inline); no ctx register traffic; flush-mode
  switches grouped (2-3 a particle instead of ~12; the 3-lane vector add/sub of the update step runs in the scalar
  mode unless an input or result is denormal). Rare / big callees stay generated calls with the original's registers:
  sub_822E00C0 (respawn, uses the game RNG at 0x829C4C90), sub_822B71B8 (force field). Effects reproduced: guest
  memory (records, vertex slots, slot table, counters, matrix stack, store cards), v92..v95 / v127 unchanged, r1, the
  final flush mode, r3 = 0 for sub_822E10F0.
- Cvars: `tekken_native_particles` (default ON), `tekken_native_particles_check` (tests: runs the original, restores
  the inputs, runs the native with the counter values the original took, compares every byte both write; keeps the
  original's result on a difference), `tekken_native_particles_check_control` (the check with the original on both
  sides: the harness's own noise).
- Verification (Switch build in Ryujinx; the PC build crashes in the intro with "Call to invalid or unregistered
  function at guest address 0x00011C00" even with these hooks compiled out, so it is a pre-existing PC problem, not
  looked into). Rain stage reached through Practice: `out/emu_parts_explore.toml` (6 DOWNs to Practice, Lars vs Lars,
  one RIGHT on the stage grid = "Lightning Storm", the rain helipad; the grid is row 1 left to right from the "?").
  - emu_parts_storm4 (check): 113,359 calls compared (13.9 M particles), 53 differ; 2,515 more calls had an RNG race
    (another thread stepped the unlocked RNG word between the two runs: it shows even for sub_822E10F0, which never
    reads it), 126 skipped (a slot counter moved by another thread).
  - emu_parts_control (control, original vs original): 110,823 calls, 46 differ, 2,571 RNG races. Same rate and same
    kinds (respawned positions, sub_822E10F0 slot counts / records of 103-particle emitters): other threads change the
    inputs during the call (spawner, camera / weather settings). So nothing in the residual is the native's.
  - Bug found and fixed on the way: the atan2 constant table is at 0x8280E2B8 (first attempt read 0x827EE2B8: every
    falling-particle angle came out 0).
  - emu_parts (natives on, no check, rain stage, 300 s): rain drawn, fight runs, no fault; 32,768 calls of
    sub_822E10F0 and >16,384 of sub_822E05C0 natively (~4 M particles).
- Build state: app/generated re-processed by post_codegen (23 min); app/out/switch rebuilt (plain NRO, copied to
  ryu-data); app/out/pc rebuilt (crashes as said). The PGO folder (switch-pgo-use) not rebuilt: its profile is stale
  for the two callers now. Not measured on the console yet: the gain on the rain stage (was ~22 fps, the two
  functions 14-24 % + 6-7 % of the workers).
- Open: console measurement on the rain stage (Practice -> Lightning Storm is repeatable); the PC crash; the snow
  stage not identified (the same two functions are probably its emitters too).

## Skinning native tuning (2026-11-20 night)

- Where the time went (`out/console_runs/dieta_2223`, PGO build `builds/2026-11-19_2223_tekken6_pgo_lto_dieta/renut.elf`,
  per-PC histogram of the worker threads): `sub_82548758` has 7,890 samples; 3,256 of them (41 %) sit on one
  instruction, the loop's first load (the record's matrix indices at +64, `ldr w3,[x19,w1,uxtw]`). The sampled PC is
  the oldest instruction not yet retired, so the loop waits on the record stream (96-byte records, each read once a
  frame) missing the caches; the A57's own prefetcher does not keep ahead of it. The rest is spread thinly: the bone
  matrix loads (20-80 samples each, so L1 / L2 hits), the arithmetic, 12 stack spills (GCC runs out of the 32 vector
  registers), the stores (~350). The half job `sub_825481F8` has the same pattern but much weaker (108 of 952 on its
  index load).
- Looked at and left alone: the flush mode is already switched once a call (not a record); the record / matrix loads
  are already 16-byte NEON loads with `rev32`; caching the matrices of consecutive records that share bones would save
  only L1 hits and needs registers the loop does not have. `-ffp-contract=off` is on the whole target
  (app/CMakeLists.txt), and the arithmetic was not touched.
- Change (`app/src/cpu/tekken_skin_native.cpp`): software prefetch of the input records `tekken_native_skin_prefetch`
  records ahead (default 8; two lines a 96-byte record, one a 64-byte record; 0 = off, GCC unswitches the loop so 0
  costs nothing). `tekken_native_skin_prefetch_out` (default 0, off: untested on the console) also prefetches the
  output lines for writing. `tekken_native_skin_check_control` added (the check with the original on both sides). The
  check path now runs the same Run with the same prefetches. A prefetch never faults and changes no value.
- Verification (Switch O3 build app/out/switch in Ryujinx, `out/emu_skin_check.toml` = emu_t7 + the check):
  `out/emu_runs/emu_skin_check`: 898,000 calls (158,183,464 records) compared, 0 differ; Arcade fight drawn (Lili).
  No control run was needed (0 differences). `out/emu_runs/emu_skin_native` (check off, prefetch 8, 240 s): Arcade fights drawn correctly (Lili, Bruce; captures 30 / 40), no fault. So `tekken_native_skin` stays ON with the prefetch at 8.
- Not measured: the speed-up. Ryujinx's JIT on x86 says nothing about A57 cache misses. Expected: most of the 41 %
  stall goes, so `sub_82548758` could drop from 35 % towards ~22-25 % of the workers; how much of that reaches the
  frame rate depends on whether the workers are on the critical path. To measure on the console: the same fight with
  `tekken_native_skin_prefetch = 0` vs `8` (and try 4 / 16, and `tekken_native_skin_prefetch_out = 4`) in the toml,
  then `python tools/switch_profile_groups.py rex_profile.log <renut.elf>`; the hook's loop-head share should fall.
- The PGO folder (switch-pgo-use) is not rebuilt; its profile is still fine for this file (the loop shape only gained
  the prefetches).
- PC build: still crashes ("Call to invalid or unregistered function at guest address 0x00011C00 from 0x00000000",
  `out/pc_runs/nohooks`, main thread, ~16 s into the intro). `animnode_pc3` (2026-11-19 05:04, before the phase A/B
  codegen commit) ran fine, so it most likely came with the register diet (elide_lr_stores explains the LR of 0;
  non_volatile_as_local / the setjmp-longjmp handling are the suspects on the PC side, where locals and ctx behave
  differently from the Switch). Not looked into further.

## Overnight summary (2026-11-20 morning)

What the night left, nothing committed:
- Particle natives (`app/src/cpu/tekken_particle_native.cpp`, cvar `tekken_native_particles`, default on): sub_822E05C0 / sub_822E10F0,
  checked in Ryujinx on the rain stage (113,359 calls: 53 differences against a control of 46 for the original against itself,
  the same thread-race noise). Details in "Particle natives (2026-11-19 night)".
- Skinning native: input-record prefetch (`tekken_native_skin_prefetch`, default 8): 898,000 calls, 0 differences. Details in
  "Skinning native tuning (2026-11-20 night)".
- Final NRO: `../builds/2026-11-20_0500_tekken6_pgo_lto_particulas_skin` (PGO+LTO, the 2026-11-19 profile, stale only for the
  two particle callers and the skin loop; GCC is told to tolerate the mismatch). Ryujinx: `out/emu_runs/final_pgo_t7` (logos,
  menus, an arcade fight to the end of the capture) and `out/emu_runs/final_pgo_storm` (Practice, Lightning Storm, rain,
  particles native): both render correctly, no fault.
- The snow stage: `out/emu_t7.toml`'s arcade fight in `final_pgo_t7` landed on a snowy forest stage (falling snow), so it is reachable.

To measure on the console (stock clocks, `banjo_show_fps` and the profile stacks on in the package's katamari.toml):
1. an ordinary fight, against the 54.6 fps of the 2026-11-19 build;
2. Practice -> Lars vs Lars -> one RIGHT on the stage grid (Lightning Storm, rain; about 22 fps before).

Open: the PC build crashes in the intro since the phase A/B codegen ("Call to invalid or unregistered function at guest
address 0x00011C00", caller 0 because of elide_lr_stores); the Switch is not affected.

## Console result of the overnight build (2026-11-20)

- Particles native ON (`../builds/2026-11-20_0500_tekken6_pgo_lto_particulas_skin` as packaged): rain and snow stages in
  arcade at 42-49 fps (rain was ~22 before), but it **froze mid-fight** after ~3 min: 14.8 fps, then 0 fps for over a
  minute, CPU ~40 %, no fault in rex_stderr. Main XThread and the six job workers all parked in `sub_824BD200`
  (SignalObjectAndWait) -> `rex::thread::SignalAndWait`; render thread idle; thread 2069 in KeWaitForSingleObject via
  n_824F0250. The same lost-wake-up family as the 2026-11-09 stall; the guest frames above sub_824BD200 are not in the walk.
  The job pool around the particles: sub_822E1AA0 runs sub_822E05C0 and then atomically counts jobs at 0x82941280; the
  last one SetEvents 0x82945CB8 (sub_824BD0F0). The native's slot counters use the same CAS as the generated lwarx/stwcx.
- Particles native OFF (`tekken_native_particles = false` in katamari.toml), same NRO: 46 windows (7.5 min), rain/snow
  included (262,144 particle calls), **no freeze**, 41-43 fps in the fights, mean 44.7.
- So the particle native buys nothing visible at the console's frame rate and is the prime suspect for the freeze (one
  run each, so not proven). Default changed to OFF in the source (not rebuilt). Open: why a faster particle job would
  expose a lost wake-up (candidates: the job pool's event / pulse handling in `sdk/src/core/threading_posix.cpp`).

## Semaphore wake-up: one waiter per count (2026-11-20)

The console profile with the particle native off (`out/console_runs/final_0500_noparts`, fights at ~42 fps) has no thread
at 100 %: main 54 %, render 65 %, workers 10-24 % each. Main's biggest wait (about a quarter of its samples) was inside
`NtReleaseSemaphore -> PosixSemaphore::Release -> pthread_mutex_lock`: every release `notify_all`ed the six job workers,
and every worker that took a count `notify_all`ed the rest again, so the herd fought for the semaphore's mutex while
main tried to release the next job.

Change (`sdk/src/core/threading_posix.cpp`, cvar `thread_semaphore_wake_one`, default true): Release(n) with more than
n blocked waiters does n `notify_one`; a taker that leaves counts behind wakes one more (post_execution); otherwise the
old behaviour. `sdk/tests/semaphore_wake_test.cpp` (six workers, two of them in 1 ms slices, releases of 1 or in
batches, every job waited for): 3 x 10 s, about 17 M jobs, none lost or taken twice. Ryujinx `out/emu_runs/sem_t7`: an
arcade fight renders, no fault. Package: `../builds/2026-11-20_0758_tekken6_pgo_lto_semaforo` (particle native off by
default). Console measurement pending.

## Semaphores on address arbitration; the console's language (2026-11-20)

Console run of the wake-one build (`out/console_runs/sem_0758`, three stages, two rain and one snow, ~41 / ~50 / ~44 fps
in the fights, no freeze in 19.5 min): main still spent ~23 % of the fight windows in `PosixSemaphore::Release ->
pthread_mutex_lock` (30 % before). Cause: Horizon gives a thread woken from a condition variable
(svcWaitProcessWideKeyAtomic) the mutex before it runs, so main's next ReleaseSemaphore waited until that worker got a core.

Change (`sdk/src/core/threading_posix.cpp`, cvar `thread_semaphore_address_wait`, default true, Switch only): a thread
blocked on a semaphore sleeps in `svcWaitForAddress(WaitIfEqual)` on a sequence word with the mutex released; a wake
bumps the word after the state change and `svcSignalToAddress`es it (n waiters for a release of n, one more from a taker
that leaves counts, all for an interrupt). Events, mutants and timers keep the condition variable.
`sdk/tests/semaphore_wake_test.cpp addr` (C++20 atomic wait standing in for the svcs): 3 x 10 s, ~21 M jobs, none lost or
taken twice. Not run in Ryujinx (a deliberate choice: console test directly).

Language (`app/src/tekken_language.cpp`, called first in OnPreSetup): with no `user_language` in katamari.toml, the
Switch's system language picks XGetLanguage's answer (Japanese 2, German 3, French / Canadian French 4, Spanish / Latin
American Spanish 5, Italian 6, Korean 7, Russian 12, English 1 for the rest); the player template no longer sets it.

Package: `../builds/2026-11-20_1158_tekken6_pgo_lto_semaforo_idioma`. Console test pending.

Console run of that package (`out/console_runs/addr_1158`, 21.7 min, three stages in practice, no freeze): fights at
~42 / ~51 / ~41 fps, the same as before. Main's wait in `PosixSemaphore::Release` fell from ~23 % to ~2 % of the fight
windows, but the time moved: main now waits for the jobs to finish (NtWaitForSingleObjectEx via n_824C7CD8, ~15 %),
in `PosixEvent::Pulse -> pthread_mutex_lock` (~8 %, the same condition-variable mutex hand-off, for events), in
`D3DDevice_Swap_hook` (~7 %) and in a critical section (~4 %). Main 52 % CPU, render 68 %, workers 10-24 % each:
nobody is saturated, so the frame is bound by a dependency chain (main -> jobs -> render), not by one thread's work.
The language log line goes to the main log, not rex_stderr.log, so it was not seen.
Stages of `addr_1158`, in order : Lightning Storm (rain) ~42 fps, Manji Valley (snow) ~51, Cemetery (rain) ~41.
The language detection works on the console (a Spanish console got the game in Spanish).

## Frame-chain diagnostic (2026-11-20)

Cvar `tekken_frame_chain` (default false, init only; set it in the test package's katamari.toml). Every 10 s two lines
go to stderr (`rex_stderr.log` on the console). Code: `sdk/include/rex/frame_chain.h`, `sdk/src/core/frame_chain.cpp`
(accounting and report), hooks in `sdk/src/kernel/xboxkrnl/xboxkrnl_threading.cpp`, `xboxkrnl_rtl.cpp`,
`sdk/src/system/xthread.cpp` (marks the main thread), `app/src/renut_engine/gpu/d3d/hooks/device.cpp` (Swap) and
`app/src/renut_engine/gpu/backend/render_thread.cpp` / `present.cpp` / `video.cpp` (render thread).

One frame = one `D3DDevice_Swap_hook` call on the guest main thread ("Main XThread", flagged by a thread_local set
once when that thread starts; every other thread pays one TLS load per kernel entry). All values are window totals
divided by the frames of the window (ms per frame); `/Nx` is the number of calls per frame.

Line 1, `[frame_chain] F fps | frame A ms (p90 P, max M) | main: ...`:
- `frame`: Swap end to Swap end (mean, 90th percentile, maximum).
- `swap`: time inside the Swap hook; `ahead`: of it, the wait in `render_thread::QueueFrame` for the render thread
  (more than `kMaxFramesAhead` frames queued).
- `waits:` main's time in kernel calls, measured around the call in the xboxkrnl entry:
  `obj` KeWaitForSingleObject / NtWaitForSingleObjectEx; `multi` KeWaitForMultipleObjects / NtWaitForMultipleObjectsEx;
  `sigwait` NtSignalAndWaitForSingleObjectEx; `pulse` KePulseEvent / NtPulseEvent; `set` KeSetEvent / NtSetEvent (also
  the critical-section leave's set); `sem` KeReleaseSemaphore / NtReleaseSemaphore; `cs` RtlEnterCriticalSection's full
  wait after the spin; `sleep` KeDelayExecutionThread / NtYieldExecution; `rt` the renderer's own waits for its
  render thread outside the Swap (`render_thread::Drain`, `WaitForFenceProgress`).
  Nested scopes count once, to the outermost (a wait inside the Swap hook counts as `swap`; the cs wait as `cs`).
- `busy`: frame - swap - all waits (main running guest code and hooks; includes State().mutex contention on the main
  side and anything blocking that is not one of the calls above, e.g. XAM waits).

Line 2, `[frame_chain] top obj: 0xKEY type T ms x N/frame, ... | render: ...`:
- top 3 objects of main's `obj` and `cs` waits by total time: the key is the handle (NtWaitForSingleObjectEx), the
  guest object address (KeWaitForSingleObject) or the critical section's guest address (`cs`); the type comes from the
  XObject (event / sem / mutant / thread / timer). Up to 64 distinct objects per window.
- `render:` (render thread, `render_thread.cpp` Run): `wait-main` time in the work condition variable (waiting for the
  game); `swapchain` acquire + present (`present.cpp` WaitScopes); `fence` GPU fence waits (`video.cpp`
  waitForCommandFence / Deko3DFenceCopyWait); `other-wait` the other WaitScopes (cleaner, record worker drain); `lock`
  blocked taking `State().mutex` for an item (try_lock first, timed only when contended); `busy` = batch run time minus
  those waits. `batches/frame`: how many wake-ups ran work.

Reading it: if main's `swap ahead` is large the render thread is the limit; if `obj` (job completion) dominates with
the render thread idle (`wait-main`), the jobs are; if `busy` alone is near the frame time, the main thread is.
Not measured: the six job workers' busy time (not cheap without per-thread wall accounting of every guest thread).

## Frame-chain results on the console (2026-11-20, `out/console_runs/chain_1311`)

Practice, handheld, stock clocks (CPU 1020, GPU 460.8). Per-frame averages of the steady fight windows (ms):

| stage | fps | frame | main busy | main waits (obj / cs / pulse / sem / swap-ahead) | render busy | render wait-main | swapchain | render other |
|---|---|---|---|---|---|---|---|---|
| Lightning Storm (rain) | ~44.8 | 22.3 | 15.6 | 3.4 / 1.3 / 0.8 / 0.5 / 0.5 | 12.8 | 2.5 | 5.5 | 1.4 |
| Manji Valley (snow) | ~56.7 | 17.6 | 11.4 | 1.9 / 0.7 / 2.4 / 1.0 / 0.1 | 9.3 | 3.5 | 4.6 | 0.3 |
| Cemetery (rain) | ~43.3 | 23.1 | 15.0 | 2.6 / 1.4 / 1.1 / 0.5 / 2.3 | 13.5 | 0.4 | 7.7 | 1.3 |

- Main's single biggest object wait: semaphore 0xF8000080, once per frame, 2-3 ms in the rain fights (0.01 ms in menus).
  Critical section 0x829CA098 (~5 per frame, 1.3-1.5 ms). Snow: mutant 0xF8000048 pulsed/waited 84 times a frame (1.3 ms).
- Cemetery: the render thread is the limit (it never waits for main, 0.4 ms; main waits for it, "ahead" 2.3 ms): render
  busy 13.5 + swapchain 7.7 (acquire / present waiting for the GPU to free an image) = the frame.
- Lightning Storm: both main (15.6 busy + ~6.5 waits) and render (12.8 + 5.5 + 1.4) fill the frame.
- Manji Valley: main is the limit (render waits 3.5 ms for it).
- swapchain 4.6-7.7 ms in fights vs ~3 ms in the 60 fps menus: the GPU (460.8 MHz handheld) is likely part of the chain.

## GPU clock test and the freeze again (2026-11-20, `out/console_runs/chain_gpu768`)

GPU at 768 MHz (sys-clk, confirmed by the profiler's clock line), CPU 1020: Cemetery ~40 fps (43 at 460.8), swapchain
8.4-9.4 ms per frame, so the GPU clock is not the limit; the swapchain time looks like vsync (FIFO, 3 images) waiting.
The run froze again in a fight with the particle native OFF: Main XThread and job workers in
`NtSignalAndWaitForSingleObjectEx` (sub_824BD200, PosixConditionBase::Wait), other workers in NtWaitForSingleObjectEx
(n_824C7CD8): the same signature as the particle-native freeze, so that native was not its cause. A latent lost wake-up.

Diagnostic (`thread_stall_report_s`, default 5): a wait with no timeout waits in slices and, blocked 5 s, prints to
stderr `[stall] guest thread ... blocked after 5 s: <kind> a=..(type) b=..(type) | object ... signaled handoff
cond_waiters armed queue pulse_gen addr_wait multi`; the kernel entries (Ke/NtWait, SignalAndWait, WaitMultiple, the
critical-section wait with its owner) record what is waited for (rex::thread::SetWaitNote). Package:
`../builds/2026-11-20_1402_tekken6_pgo_lto_diag_bloqueo` (frame chain on too).

## GPU time and the presentation wait (2026-11-21, `out/console_runs/gpuprof_docked`, docked, Cemetery)

- `[gpu_profile]` in the fights: the GPU takes 13.0-13.8 ms per frame (draws 1536x864 colour+depth ~6 ms with 310-370
  draws, resolves ~2.5 ms, depth resolves ~0.9 ms, 1920x1080 compositing ~0.7 ms, present ~0.6 ms). Frame 24.7 ms: the
  GPU is idle about half of it.
- `[frame_chain]` render thread: busy ~13.2, acquire ("swapchain") ~4.0, present + fence waits ("fence") ~4.0, cleaner
  and drain waits ~1.9, waiting for main ~1.5 ms.
- rex_profile.log, render thread in those windows: 16.6 % of its samples in `bqDequeueBuffer <- nwindowDequeueBuffer <-
  dk Swapchain::acquireImage`, 14.8 % in `bqQueueBuffer <- nwindowQueueBuffer <- presentImage`: binder IPC to the
  system's compositor (vi). In the same windows `system core 3: idle ~2 %` (banjo_system_core 2: every game thread
  allowed on core 3). The compositor shares core 3 with our threads and answers late: this, not the GPU (768 MHz test)
  nor vsync (renut_vsync off test), is the ~8 ms the render thread loses each frame.
- Next test (toml only): banjo_system_core = 1 (game threads on cores 0-2; only audio output / Audio Worker / XAudio
  mixer on core 3).

Console run with `banjo_system_core = 1` and `banjo_renut_render_height_handheld = 576` (handheld,
`out/console_runs/syscore1_576`): worse. Fights Cemetery ~38.6 fps (40-43 before), Lightning Storm ~36 (45),
Manji Valley ~44 (57). Game threads on cores 0-2 only: render thread busy 15-19 ms (13 before, CPU contention), main waits
longer for the jobs (obj 5-7 ms) and in PulseEvent (~3 ms). The render thread's acquire + present waits only fell to
~4.5-7 ms, and core 3 stayed 17-38 % idle with only audio on it: freeing core 3 is not the fix for the compositor IPC.
GPU at 576 lines handheld: 8-10.5 ms per frame, far from the limit, so 720 stays. Back to banjo_system_core = 2; the
compositor wait is for banjo_async_present (the binder calls on a helper thread).

## Async present and event address wait (2026-11-21)

Ported from [renut_nx_ultimate_base](https://github.com/ChanseyIsTheBest/renut_nx_ultimate_base) (ChanseyIsTheBest,
with permission; credited in THIRD_PARTY_NOTICES.md), its docs/ultimate-fixes.md rows 145 and 65. Both on by default.

- `banjo_async_present` (row 145; defined in app/src/cpu/banjo_enhancements.cpp, read by name in
  sdk/src/ui/switch_perf.cpp): `nwindowQueueBuffer` returns at once and a helper thread (priority 0x2C, cores 0/2)
  queues the buffer and immediately dequeues the next one; the next `nwindowDequeueBuffer` (deko3d's acquireImage,
  now wrapped: `--wrap=nwindowDequeueBuffer` in sdk/cmake/rexglue_switch.cmake) takes that buffer, waiting for the
  helper if needed. Same calls, same order. plume's `Deko3DSwapChain::destroy` calls `RexSwitchPerfPresentQuiesce`
  to cancel the buffer dequeued ahead before the swapchain releases the window (dock / undock resize). Aimed at the
  ~8 ms a frame the render thread spent in `bqDequeueBuffer` / `bqQueueBuffer` (section above). Console test pending:
  fps and the render thread's swapchain / fence waits in `[frame_chain]`; set it false in the toml to A/B.
- `thread_event_address_wait` (row 65; threading_posix.cpp): events use the semaphore's existing address wait
  (`address_wait_`, `addr_seq_`, `AddressWaitLocked`) instead of the condition variable, so a SetEvent no longer
  waits for a woken thread to get a core. Kept exactly: the auto-reset FIFO hand-off (Waiter / queue_ / GrantOrSet,
  Arm / Disarm for SignalAndWait) and PulseEvent's pulse_gen_. Every wake of an event (Set, Pulse, a grant passed on
  by Disarm, an alertable interrupt) bumps the address and wakes all blocked threads (-1; the FIFO grant decides who
  passes). New with it: a manual-reset Set with waiters also bumps pulse_gen_, so a Reset right after cannot strand
  them (Windows semantics; the upstream row does the same), and blocked event waiters re-check every 100 ms as a net.
  The stall report keeps working (it measures elapsed time, not slices).
- Stress test: sdk/tests/event_address_wait_test.cpp (C++20 atomic wait standing in for the svcs, no 100 ms net so a
  lost wake shows as a stall). 10 s per mode, address mode: handoff 2.25 M Sets, pulse 0.81 M pulses x 6 waiters,
  setreset 0.83 M, SignalAndWait ping-pong 12.5 M, Pulse barrier (Tekken's guest condition variable) 1.07 M rounds,
  alternating Set/Reset barrier 1.69 M: all ok. Condition-variable mode: all ok except setreset, which stalls at once
  (the old path's Set + immediate Reset strands woken waiters; only on a PC condvar here, but it is the case the new Set
  rule closes).

## Hang snapshot (2026-11-21)

Cvar `tekken_hang_snapshot` (default true, init only). Code: `sdk/src/system/hang_snapshot.cpp`,
`sdk/include/rex/system/hang_snapshot.h`. Diagnostic only: no wait or signal changes.

- **Wait record per guest thread.** `XObject::Wait` / `SignalAndWait` / `WaitMultiple` (so every Ke/Nt wait entry and
  the critical-section wait) and `KeDelayExecutionThread` / `NtYieldExecution` fill a record in a fixed pool of 96
  (registered on the thread's first wait or signal, released in `XThread::Exit`). It holds the kind, up to 4 objects
  (handle, guest address, type letter), the timeout, alertable, when the streak started (the first wait on the same
  objects since the last satisfied one), the streak's waits / timeouts / APC returns, the last result, a sequence
  number, and the last 8 waits.
- **Signal history per object.** `XObject` keeps its last 4 signal operations (guest thread id, steady-clock time):
  `Set`, `Pulse`, `Reset` (XEvent), `Release` (XSemaphore, with the count and the previous count), `RelMutant`,
  `SigWait` (the signal half of SignalAndWait). A host thread shows as `host`.
- **Trigger.** Every `D3DDevice_Swap_hook` stamps `frame_chain::LastSwapNs()`. A watchdog thread (started with the guest
  main thread, 250 ms poll) prints one snapshot when there has been no Swap for 4 s after the first Swap, then one per
  further 30 s while it lasts; a Swap re-arms it.
- **Object lifetime.** Object pointers are read only while the thread is inside the wait and while the watchdog holds a
  busy flag; a thread that leaves a wait during a snapshot waits for the flag (seq_cst on both sides). A thread caught
  between two waits (a poller) is retried up to 40 times, 2 ms apart, with the flag released in between.

Format (every line starts with `[hang] `; at most 200 lines):

```
[hang] ==== snapshot #1: no Swap for 4.1 s (51234 Swaps so far), 23 guest threads, 9 objects ====
[hang] t0001 Main XThread (F8000048)          pri=0 MAIN | WAIT sigwait wait=E:F80000A8 sig=E:F80000A4 inf alert | streak 4.1 s: 1 waits, 0 timeouts, 0 apc | last ok 4102 ms ago | in wait 4.1 s | seq 912345
[hang] t000C XThread000C (F80000E0)           pri=0 | poll obj E:F8000120@4A0034C0 to=1ms | streak 4.0 s: 3810 waits, 3810 timeouts, 0 apc | last timeout 0 ms ago | seq 77120
[hang] t0007 XThread0007 (F8000090)           pri=0 | WAIT cs 4A10F2C0 inf | streak 4.1 s: 1 waits, 0 timeouts, 0 apc | ...
[hang]   obj E:F80000A8 | sig=0 cw=0 armed=1 q=1 pgen=3 aw=1 multi=0 auto | waiters t0001(w) | ops(812): Set t0009 4102ms; SigWait t0003 4103ms; ...
[hang]   obj S:F8000140@4A0035A0 | sig=0 cw=6 armed=0 q=0 pgen=0 aw=1 multi=0 count=0/16 | waiters t0002 t0003 ... | ops(40211): Release(6,prev 0) t0001 4110ms; ...
[hang]   obj M:F8000150 | sig=0 ... count=1 owner=t0004 | waiters ... | ops(..): RelMutant t0004 ...
[hang]   cs 4A10F2C0 waited by t0007 | owner KTHREAD 4005A000 = t0004 | lock_count 1 recursion 1
[hang]   main t0001 wait -8: sigwait E:F80000A8 0.21 ms -> ok, ended 4180 ms ago
[hang] ==== end of snapshot #1 ====
```

- Thread line: `t<guest tid>` (the tid in the guest KTHREAD), the host thread name `Name (handle)`, the guest priority
  (`XThread::priority`), `MAIN` for the guest main thread; `WAIT` = inside the wait at the snapshot, `poll` = caught in a
  wait on a retry (it keeps returning), `run ` = not in a wait (the fields are its last wait). Kinds: `obj`, `sigwait`
  (`wait=` the object waited on, `sig=` the object signalled), `any(n)` / `all(n)` (first 4 objects), `cs <section>`,
  `sleep <ms>`, `yield`. Objects: `<type>:<handle>[@<guest address>]`, types E event, S semaphore, M mutant, T thread,
  R timer, O other. `inf` / `to=<ms>`, `alert`. `streak`: since the first wait on these objects after the last
  satisfied one (for sleeps: since the first of a run of equal sleeps).
- Object line: the host state (`PosixConditionBase::DebugState`: `sig` signalled, `cw` threads blocked in Wait, `armed`
  SignalAndWait threads armed, `q` hand-off queue, `pgen` pulse generation, `aw` address wait, `multi` parked
  WaitMultiple callers, then `manual`/`auto`, `count=n/max` or the mutant's `count`; `nolock` if its mutex could not be
  taken in 2 ms), the mutant's owner, the waiting threads (`(w)` / `(s)`: waited / signalled in a sigwait), and the last
  4 signal operations, newest first, with the total count `ops(n)`.
- The 17 % polling thread (KeWaitForSingleObject via sub_824F0250) shows as a `poll` / `run` line with many timeouts in
  its streak; its object's line says who last signalled it and when.
- The guest condition variable (sub_824BD200 = SignalObjectAndWait wrapper: r3 the handle signalled, r4 the handle
  waited on) shows as the `sigwait sig= wait=` pair.

## Rain freeze: cause and fix (2026-11-21)

**Cause: a lost wake-up in the game's own rain code.** Each rain batch runs two kinds of pool jobs:

- **Phase 1:** the last phase-1 job (`sub_822E1AA0`) calls PulseEvent on the manual-reset event `RainCondvar`
  (handle at guest 0x82945CB8) through `sub_824BD0F0` -> `__imp__NtPulseEvent`. It does this **without** holding
  `RainMutex` (handle at 0x82945CBC), and it writes no predicate.
- **Phase 2** (`sub_822E1B28`, on another pool worker): takes `RainMutex` (`822E1B54 bl 824BCF38`), waits with no
  predicate (`822E1B68 bl 824BD200` = SignalObjectAndWait(sig=RainMutex, wait=RainCondvar, INFINITE, alertable=0)),
  then relocks and processes.
- If the pulse lands before phase 2 is armed in SignalObjectAndWait (on the Switch a worker can be preempted for
  ~10 ms between taking the mutex and the wait), the pulse finds nobody and is lost. Phase 2 never finishes, main waits
  for the job group forever: 0 fps.
- **Proof** (console hang snapshot): `RainCondvar` had 522 pulses and `pulse_gen` 521, so exactly one pulse found
  nobody waiting. Batches never overlap (main waits for the whole group before the next one): one phase-1 "last"
  pulse and one phase-2 wait per batch.

**Fix 1: targeted pulse latch** (`thread_pulse_latch_names`, string, default `RainCondvar`; comma-separated guest
event names; empty = off).

- `XEvent::Pulse` matches the event's name against the list once, at its first pulse, and enables the latch on the
  host event (`rex::thread::Event::EnablePulseLatch`).
- `PosixCondition<Event>::Pulse`: a pulse that finds no waiter (no `cond_waiters_`, `armed_` or WaitMultiple caller)
  increments a latch counter, under the event's mutex.
- `PosixConditionBase::Wait`: with a latch pending (and a nonzero timeout) the wait takes one and returns success at
  once. It covers both a plain WaitForSingleObject and SignalAndWait (which has already done its signal half: the
  mutex is released). Zero-timeout polls (`XEvent::Query` / `Save`) do not take it.
- The hand-off queue, `pulse_gen` and the address wait are unchanged. A counter is exact here because batches do not
  overlap.
- stderr: `[pulse_latch] RainCondvar: latch enabled`, then `pulse with no waiter latched (n total)` / `consumed by
  guest thread X (n total)`: the first 20 of each, then every 100th. The hang snapshot's object line adds
  `latch= latched= consumed=` for a latched event.
- PC stress test: `out/tools/event_address_wait_test.exe rain|rainoff [s] [addr|cond]` replays the protocol (phase 2
  sleeps 0-2 ms between lock and wait, 6 phase-1 threads). `rainoff` stalls in the first round (addr and cond);
  `rain` ran 30 s clean in both (addr: 1883 rounds, 1865 latched and consumed, worst round 43 ms; cond: 1215 rounds,
  787 latched and consumed).

**Fix 2: the real alertable flag for SignalObjectAndWait** (`kernel_sigwait_alertable_r6`, bool, default true).
`NtSignalAndWaitForSingleObjectEx` takes (SignalHandle r3, WaitHandle r4, WaitMode r5, Alertable r6, Timeout r7). The
entry took r5 as "alertable"; the game passes `li r5,1` (WaitMode), so every guest SignalObjectAndWait ran alertable
(sliced wake-ups, APC checks). Now r6 is used; false = the old behaviour.

**Diagnostic:** `XObject::SignalAndWait` forgets an XMutant's recorded owner when its signal half frees it (the
calling thread holds it once), so hang snapshots show the real owner of `RainMutex`. No behaviour change.

**To check on the console:** the rain stages no longer freeze; stderr shows `[pulse_latch] RainCondvar: latch enabled`
and, when the race hits, `latched` / `consumed` lines in pairs.

Console run of the rain-fix package (`out/console_runs/rainfix1`, ~17 min: Cemetery, Lightning Storm, Manji Valley,
City After Dark, Mystical Forest): no freeze. `[pulse_latch] RainCondvar: latch enabled` on each rain stage, but no
latch was taken (the race did not happen in this run), so the fix is not yet proven by a caught race. One snapshot
fired: ~10 s without a Swap that then resumed by itself; Main XThread was running (state `run`, not in a wait) the whole
time, the job workers idle, so a long main-thread operation (a load?), not a deadlock.

## Main thread after the rain fix (2026-11-21, `out/console_runs/rainfix1`, Cemetery fights ~44 fps, 22.6 ms)

- Main per frame: busy ~15.9 ms; waits: semaphore S:F8000080 once a frame ~3.5 ms, critical section 0x829CA098 ~1.4 ms
  (~5 a frame), AnyQueueMutex M:F8000048 ~0.7 ms (41 a frame), ReleaseSemaphore ~0.7 ms (~78 a frame, the job
  semaphore F8000078). PulseEvent fell to ~0.1 ms with thread_event_address_wait.
- Render thread: busy ~16 ms, waits for main ~5.2 ms, swapchain ~0.2 ms (async present): after main, it is the next
  limit (16 ms is at the 60 fps budget on its own).
- The guest thread main waits for each frame (pattern: main Sets E:F8000070, it works, then main waits S:F8000080):
  "XThread2CE68EA0" (t000C in the hang snapshots), ~21 % CPU (~4.8 ms a frame). Its hot spots: renut::gpu::QueueDraw
  21 %, hand natives n_82545348 / n_82545A70 / n_82542E28 / n_82519E58 (the 2D layer job), mutexLock + mutexUnlock ~7 %,
  memcpy 4 %. It is a guest drawing thread: making QueueDraw cheaper shortens main's 3.5 ms wait.
- Main's own profile is flat (top n_825234A0 5.9 %), so its 15.9 ms of work has no single hot spot.

## Mutant fast paths (2026-11-21)

Goal: cheaper guest mutant waits and releases on main (AnyQueueMutex M:F8000048, 41 lock/unlock a frame, ~0.7 ms).
Code: `sdk/src/core/threading_posix.cpp` (`PosixCondition<Mutant>`, hooks in `PosixConditionBase::Wait` and
`WaitMultiple`). XMutant / XObject are unchanged: `WaitCallback` (owner_tid), the hang snapshot and the frame_chain
scopes still run on every successful wait, fast or slow.

- `thread_mutant_release_lockfree` (bool, default true): renut_nx_ultimate_base's port 146 (ChanseyIsTheBest,
  docs/ultimate-fixes.md row 146; tw2de_nx's). A release that frees the mutant takes the condition's mutex and
  notifies (on Horizon `svcSignalProcessWideKey`, even with nobody waiting) only when some thread is in the mutant's
  slow wait path. false = mutex + notify_all at every freeing release, as before.
- `thread_mutant_acquire_fast` (bool, default true): WaitForSingleObject on a free mutant (state word 0) is one
  compare-and-swap; on a mutant the caller already owns it is a relaxed increment of the recursion count. No mutex,
  no wait bookkeeping. SignalAndWait's armed wait and WaitForMultipleObjects keep the full path.

Protocol (also in the comment above `PosixCondition<Mutant>`): one atomic 64-bit word, owner key (per host thread,
never 0) in the low half, the number of threads in a slow section in the high half. A slow section is Wait() from just
before it takes the mutex to its return, or a whole WaitMultiple call that includes the mutant. The fast CAS needs the
whole word 0, so it can never slip between a slow path's `signaled()` and its `post_execution()`. A freeing release
clears the owner half with one seq_cst fetch_and, which also returns the slow count: 0 = nobody can be blocked, so no
mutex and no notify; nonzero = lock/unlock the mutex and notify the blocked (cond_waiters_). The waiter's increment and
the release's fetch_and are RMWs on the same word, so either the waiter sees the mutant free or the release sees the
waiter (the same argument as the semaphore/event address waits). The recursion count is written only by the owner.
`HeldByCurrentThread` no longer takes the mutex.

Semaphore release: left as is. With nobody blocked it already makes no syscall (`thread_signal_skip_idle`); with
waiters the svcSignalToAddress is the wake itself. A cheaper path would need a lock-free count, a larger change.

PC stress test: `sdk/tests/mutant_fast_test.cpp` (`out/tools/mutant_fast_test.exe <solo|lock|condvar|all> [s]
[old|new|fastonly|lockfreeonly]`): 6 threads lock with recursion (infinite, 1 ms, zero-timeout and WaitAny on
{mutant, idle event}) checked by a non-atomic counter and an "inside" flag; the guest condition variable (3 producers
lock/push/unlock/PulseEvent, 3 consumers SignalObjectAndWait(mutant, cv)); a 2 s watchdog. Host (Windows, 5 s each,
two runs): solo 82-83 ns a pair old -> 47-49 ns new; lock 409-436 k -> 886-897 k acquisitions/s; condvar 374-386 k ->
584-653 k items/s; freeing releases that skipped mutex+notify ~108 M of ~114 M on "new" (old: 66-67 M notifies, each a
syscall on Horizon). 3 x 20 s lock + condvar on "new": no stall, counter and item counts exact. Finding: the old
Release read owner_/count_ without the mutex, so a stray release by a former owner could drop the new owner's hold
(a data race; the new word makes it fail cleanly).

To check on the console: Cemetery fps and main's `[frame_chain]` time on M:F8000048; A/B with
`thread_mutant_acquire_fast = false` / `thread_mutant_release_lockfree = false` in the toml.

Console run of the mutant fast paths (`out/console_runs/mutex1`, rain stages): no freeze, and the rain fix is now proved:
`[pulse_latch] RainCondvar: consumed` 20+ times in one session (each one a freeze before the latch). AnyQueueMutex on
main fell from ~0.7 to ~0.09 ms a frame; Cemetery fights stay ~44 fps (main busy ~16.6 ms + S:F8000080 ~3.5 ms + critical
section ~1.4 ms dominate; render busy ~16 ms).

## QueueDraw on the 2D worker (2026-11-21)

**Console evidence.** In a steady Cemetery window in `out/console_runs/mutex1/rex_stderr.log`, 440 frames ran at
44.0 fps. Main was busy 16.75 ms/frame and waited 3.46 ms on S:F8000080 once per frame. Render was busy 15.87 ms,
waited 5.54 ms for work and reported 0.00 ms acquiring the video-state mutex in `Execute` (rounded to 0.01 ms).
The two threads overlap. These are measurements from the November 21 mutant build, before PGO retraining.

For the call sites, use the archived `../builds/2026-11-21_1056_tekken6_pgo_lto_mutex/renut.elf` with
`out/console_runs/mutex1/rex_profile.log`. `python tools/switch_profile_groups.py <log> <elf>` groups all guest workers
together. To isolate the drawing worker, take the sections headed `XThread1C67FB00` in reports 15 through 26 inclusive
(startup is report 1), sum the multiplicities on the printed `pc image+0x... xN` lines, and pass the offsets directly to
`aarch64-none-elf-addr2line -f -C -e <elf>`. This thread has the same 2D native callers as `XThread2CE68EA0` in
`rainfix1`. The 12 reports give 21.0% average CPU and 601 printed leaf-PC hits.

| On the 2D worker | Leaf hits | Interpretation |
|---|---:|---|
| `QueueDraw` (LTO clone) | 120/601 (20.0%) | Includes the inlined capture logic. 80 of the 120 PCs are in the chunk scan and changed-chunk path at `0x233ec8..0x233fcf`. |
| `mutexLock` / `mutexUnlock` | 20 + 24 (7.3%) | CPU samples in the primitives. Separately, sleeping stacks show `pthread_mutex_lock <- Push <- QueueDraw`, which identifies the render **queue** mutex. The leaf PCs alone cannot assign the 44 hits to individual locks or measure blocked time. |
| `memcpy` | 19 (3.2%) | Thread-wide. Several callers copy data, including captures and guest code. |

**Cost per immediate draw.** In `gpu/backend/queued.cpp`, `QueueDraw` checks deferred vertices and ready callbacks,
then holds `g_delta.mutex` from capture through publication. On a normal delta, `CaptureDelta` visits 39 readable
chunks even if nothing changed. It compares live device bytes with a shadow in 64-byte units, copies each changed
chunk to both shadow and capture, packs dirty VS/PS constant runs and copies user-pointer vertex/index data when
supplied. The first draw after a frame reset or device change copies the full 0x31B0 bytes into both buffers. Pooling
retains storage and transfers captures in batches of 32. In the archived ELF, `objdump -d` shows the two NEON loads at
`0x233f04/08`, the reduction and branch, then the next-iteration test at `0x233f34` (48 hits there alone).
That localizes work to the scan. Sampling cannot price a single load or establish a cache miss.

**Where the locks meet.** The delta lock serializes producers so shadow order matches replay order. Inside it,
`QueueCapture -> QueuePrefetched -> Push` takes `render_thread.cpp`'s `g_mutex` for each append. The render thread
takes that same mutex to swap out its batch and publish completions. It releases it before executing the batch.
The capture pool has a separate lock for refill/return. `Execute` takes `State().mutex` per locked item while
`DeviceForDraw` reconstructs state and `DrawStateLocked` uses it. Other users include shader/declaration destruction
(`gpu/d3d/hooks/shader.cpp`) and the frame callback in `hooks/device.cpp`. The ordinary producer path never acquires
the video-state mutex directly. The rounded render-side lock field cannot measure producer waiting on the queue.
For context, across the 22 reports used by the grouping script, 23.9% of printed render PCs land in
`ConvertVertices` and 22.8% in `PagesClean`. Changing queue publication leaves that render work in place.

**Experiments in priority order.** Each proposal needs its own init-only TOML cvar and a console A/B against the
same retrained build, clocks and stage. Compare S:F8000080, frame time, render busy and wait-main in steady fights.

1. Compare contiguous runs of readable chunks with the existing `Lines64DifferMask` in `gpu/line_compare.h`, then
   copy just the set bits in ascending order. Keep the existing short-tail comparison, unread gaps, constant masks,
   rebase and UP paths. The observed scan represents roughly 0.64 ms/frame of worker CPU (80/601 times the handoff's
   ~4.8 ms). Halving that work would save about 0.32 ms on the worker. Main benefits only where the worker finishes
   on its critical path. Risk is moderate: a wrong tail length or bit order can corrupt HUD textures or constants.
   Run `banjo_renut_snapshot_deltas_verify = true` on menus and fights to check reconstructed bytes before timing.
2. A/B the bounded multi-producer queue from ultimate-base port 105 under `dishonored_renut_lockfree_queue`.
   Keep the delta lock through publication and the per-item video-state lock. The CPU opportunity on this worker is a
   fraction of the ~0.35 ms/frame represented by **all** mutex leaf hits, plus any queue blocking avoided. The latter
   needs producer-side wait timing. Risk is higher: preserve global FIFO across draws, fences and callbacks, and test
   full-ring render-thread reentry, drain, idle wake and shutdown. Keep frame-chain accounting comparable. Credit
   ChanseyIsTheBest and the documented Dishonored/SA2 lineage in `THIRD_PARTY_NOTICES.md` if ported.
   [Upstream implementation notes, row 105](https://github.com/ChanseyIsTheBest/renut_nx_ultimate_base/blob/main/docs/ultimate-fixes.md)

## PGO retraining on the Switch (2026-11-21)

The player spent about three minutes each in Cemetery, Lightning Storm and Manji Valley, in that order.
`out/console_runs/pgo_20261121/banjo_pgo_1.stream` contains cumulative counts for that console session.
The November 19 profile (187 files) is archived in `pgo_20261119/`. A single run of `tools/pgo_merge.py`
produced 189 new `.gcda` files in `pgo/`. All 189 names match objects in `app/out/switch-pgo-use`.

For a retrain in an existing build directory, Ninja needs to recompile every object built with `-fprofile-use`.
The profile files are not Ninja inputs. For this run the cached objects were marked older than their sources,
and a dry run confirmed all 193 profiled compilations were scheduled. The CMake cache selects PGO use and Switch LTO.

The completed build is in `../builds/2026-11-21_tekken6_pgo_lto_reentrenado/`, with the matching unstripped
`renut.elf` beside `COPIAR_A_LA_SD/`. NRO SHA-256: `d0e920aef36f805dc9d014dbc7419e6c18b98b78545f92c40bc1eb728fa29dab`.
LTO reported two ODR warnings for `VkStructureType` and `VkFormat` in the SDK and plume Vulkan headers.
The packaged TOML enables the FPS display, stack sampling and frame-chain timing. The next console comparison
uses both logs from this NRO, in the same stage order. The training stream provides execution counts only.

### Console result with the retrained profile (2026-11-21)

Logs archived in `out/console_runs/pgo_lto_20261121/`. Stage labels follow the player's route. Each stage has
18 consecutive steady `[frame_chain]` reports (about 180 s). The intervening reports include menus and transitions.
The table numbers the FPS reports in stderr starting at 1. FPS uses total frames over elapsed time reconstructed
from the printed per-report FPS. Times are weighted by frames. Both this run and `mutex1` were handheld throughout
at CPU 1020, GPU 460.8 and memory 1331.2 MHz, with four process cores and stack sampling.

| Build and stage | Reports | Frames | FPS | Frame ms | Main busy ms | Wait S:F8000080 ms | Render busy ms | Render wait-main ms |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Previous (`mutex1`), Cemetery | 14–26 | 5,729 | 44.02 | 22.72 | 16.70 | 3.44 | 16.08 | 5.20 |
| Retrained, Cemetery | 7–24 | 8,389 | 46.57 | 21.48 | 15.28 | 3.96 | 14.80 | 5.48 |
| Retrained, Lightning Storm | 29–46 | 8,617 | 47.83 | 20.91 | 15.90 | 2.95 | 15.78 | 4.01 |
| Retrained, Manji Valley | 50–67 | 9,771 | 54.22 | 18.44 | 13.84 | 1.99 | 15.80 | 1.98 |

Cemetery gained 2.54 fps (+5.8%) between the two console sessions, with 1.24 ms less frame time. Main and render
work fell by 1.42 and 1.28 ms/frame respectively. They run concurrently. Main's wait on the drawing worker grew
by 0.52 ms. The comparison uses one session per build and does not control the exact fight inputs. Lightning Storm
and Manji Valley are same-build reference measurements for future tests. The rain latch recorded consumptions in
both rain segments and the watchdog emitted no hang snapshot in this capture.

Symbolization used the retrained package's archived ELF. Counting printed leaf PCs on the 2D worker
(`XThreadF400F60`, identified by the same 2D native callers), `QueueDraw` took 141/755 hits (18.7%) in Cemetery,
152/715 (21.3%) in Lightning Storm and 248/1,012 (24.5%) in Manji Valley. These are the interior profiler reports
9–23, 30–44 and 51–65 respectively, counting startup as report 1. In Cemetery the render thread had 687/2,529
hits in `ConvertVertices` and 573/2,529 in `PagesClean`. In Manji Valley the render thread had less idle time
waiting for work and the two functions still accounted for 1,480/3,107 hits. The producer-side capture experiment
above remains relevant to Cemetery. Manji Valley also merits checking render work if frame time stops improving.

## Render thread: what to port from renut_nx_ultimate_base (2026-11-21)

### What the Switch profile can attribute

The newest build is `pgo_lto_20261121`, symbolized with its archived `2026-11-21_tekken6_pgo_lto_reentrenado/renut.elf`.
The older `mutex1` capture has a later file timestamp and uses the archived `2026-11-21_1056_tekken6_pgo_lto_mutex/renut.elf`.
For each report, take the unnamed render thread with preferred core 1 and weight each printed leaf PC by its `xN`
count. Use `aarch64-none-elf-addr2line -f -C -e <matching ELF> 0xADDR` on the offsets as printed. Report 1 is startup.

| Console capture and report range | Render CPU | Leaf PCs | ConvertVertices | PagesClean | FlushVertexFillsLocked (exclusive) |
|---|---:|---:|---:|---:|---:|
| Cemetery, old, 15–26 | 74.0% | 2,056 | 531 (25.8%) | 469 (22.8%) | 219 (10.7%) |
| Cemetery, PGO, 9–23 | 71.4% | 2,529 | 687 (27.2%) | 573 (22.7%) | 282 (11.2%) |
| Lightning Storm, PGO, 30–44 | 77.8% | 2,606 | 661 (25.4%) | 515 (19.8%) | 330 (12.7%) |
| Manji Valley, PGO, 51–65 | 88.5% | 3,107 | 616 (19.8%) | 864 (27.8%) | 218 (7.0%) |

Cemetery's steady render busy time is 14.80 ms/frame. As an order-of-magnitude budget, multiplying by the
printed leaf fractions gives about 4.0 ms conversion, 3.4 ms card checks and 1.7 ms in the flush body.
These are proxies. The profiler prints a limited set of PCs per report and LTO puts some helpers in their
caller's symbol. Render spent another 5.48 ms waiting for work. Main busy plus just the 2D-worker wait already
total 19.24 ms, so 60 fps in Cemetery also needs progress on that side. Manji Valley's render wait for work
is only 1.98 ms, with 15.80 ms busy.

- **Vertex conversion.** In `render/vertex_format.cpp`, matching Copy32 layouts can bulk-swap whole streams.
  Other layouts convert one attribute at a time into a 16 KiB cached scratch block, then copy to upload memory.
  In Cemetery 470 of 687 conversion PCs land at four strided loads in the same unrolled loop
  (`0x20794c/60/80/a0` in the PGO ELF). Disassembly shows an 8-byte plus 4-byte load, REV32/REV, then
  8-byte plus 4-byte stores with independent source/destination stride. This identifies the three-word
  endian-swapped attribute path, consistent with float3, while leaving the semantic and source buffer unknown.
  Local callers include pending fills from `BindStreams`, immediate user-pointer stream 0, whole-buffer promotion,
  the persistent-cache second conversion in `TrackConvertedStream`, and geometry range/verification paths.
  `BindStreams` skips CPU conversion for a resident GPU-converted export stream or a cache hit. Fills are
  accumulated on draw and run per kick/submit. Printed stacks keep `FlushVertexFillsLocked` under the queued
  render item, but the hot conversion leaf distribution cannot be joined to those stacks to count calls by caller.
- **Cache proof.** `draw.cpp` uses `PagesClean` for cached stream/index/whole-geometry proofs (`CardsProve`,
  `ProveCopyRange`), transient stream/index/fetch reuse across kicks (`KickHeld`), stream stability checks and
  whole-kick replay. Same-interval proofs can avoid a scan. The reader canonicalizes physical memory, checks
  4 KiB cards across A/C/E aliases and the low 7F view, includes the preceding page for crossing stores, and
  records the last dirty flush serial. Dirty blocks go scalar, while clean 8/16-page groups compare serials
  in NEON. In Cemetery 393 of 573 leaf hits fall at the three clean-group card loads and the first serial
  address instruction (`0x1ce690/94/98/bc`). The other stages show the same hotspots. These PCs identify
  the wide clean path. They do not identify an alias, prove a cache miss or quantify dirty pages. The 86-line
  `PagesClean` body matches upstream exactly. Writer-side card changes leave the scan algorithm in place.
- **Flush body.** `FlushVertexFillsLocked` advances the proof interval, converts each pending fill, calls
  `TrackConvertedStream` for cacheable fills, resets the temporary stream table, and copies raw fetch ranges.
  In the PGO ELF the hot `0x1e4d58/5c/a0` PCs sit in a PMULL fingerprint loop inlined in this function.
  That work fits `TrackConvertedStream` hashing guest bytes on fills with no persistent copy. Promotion follows
  three matching flushes locally, then converts again into the persistent allocation to avoid reading back
  write-combined memory. This explains part of the exclusive flush cost. `memcpy` is a separate leaf symbol
  and includes other callers. The logs give neither pending-fill bytes nor the rate of promotion or expiry.

### Ranked ports and experiments

Line ranges are estimates of the relevant code or Tekken adaptation, excluding generated output and unrelated
changes in the upstream bundles. Expected gains are engineering scenarios in ms/frame on the stated thread or
GPU. They are unmeasured on Tekken and cannot be added to predict FPS. Names are relative to the corresponding
source tree. [Port descriptions](https://github.com/ChanseyIsTheBest/renut_nx_ultimate_base/blob/main/docs/ultimate-fixes.md)
· [Per-game turn-offs](https://github.com/ChanseyIsTheBest/renut_nx_ultimate_base/blob/main/docs/render-thread-turn-offs.md)
· [Off-by-default reasoning](https://github.com/ChanseyIsTheBest/renut_nx_ultimate_base/blob/main/docs/off-by-default.md)

| Order | Candidate and upstream code | Tekken status | Expected Tekken gain | Exactness, risk and turn-off | Effort |
|---:|---|---|---|---|---|
| 1 | Stream redeem, port 145. `render/draw.cpp` adds a quiet-flush serial, streak and redemption cap. 60 clean conversions restore eligibility for ordinary hash/cache promotion, at most four redemptions. | Absent. Locally three invalidations make an entry dynamic for that entry's lifetime. | Conditional. If 25–50% of Cemetery conversion time is on redeemable ranges and later reuse avoids those conversions, ~1–2 ms/frame gross render opportunity. Extra card scans, later proof costs and promotion reduce the net. Zero redemptions can make this slower. The 7–7.7 ms saving was tw2de_nx's. | Intended exact with complete writer marks and ordinary cache proofs. Off if no redemption log across representative gameplay, or if saved conversion does not repay the scans. Upstream cites Banjo's low hit rate and Deadpool's conservative off setting. | ~50–80 lines, 1 file. |
| 2 | Strided three-word conversion. The upstream `render/vertex_format.cpp` still uses the local copy kernel. Its additional finite-value pass is for Viva Piñata. Profile and prototype a specialized local loop only if the stride/format census concentrates the work. | Existing fast bulk swap, endian-specialized strided loop and 16 KiB scratch. A new kernel would be local work. | ~0.4–0.8 ms render if the identified loop becomes ~15–30% cheaper, using its ~2.75 ms Cemetery sample proxy. | Exact if every output byte, stride, endian mode, alignment and tail matches. Test upload boundaries and write combining. Leave upstream finite-value behavior out. It changes non-finite words and adds checks. | ~30–80 lines / 1–2 files for a targeted prototype. The upstream conversion-file diff is +52/-2, chiefly finite handling. |
| 3 | Evaluate store addresses once, port 160. `tools/store_cards.py` captures the address/base operand in generated scalar and machine-copy macros. | Absent in the checked local macro template and generated header. | Direct render saving near zero. Possible guest CPU and instruction-cache saving. No Tekken main/worker timing for the affected stores. FF XIII's reported smaller text is evidence of code size, not transferable frame time. | Intended exact for matched forms, preserving store/card order and address. Check macro variants and copy guards. Upstream compile-time fallback `REX_CARD_ADDR_TWICE`. Regeneration needs new PGO data for a clean comparison. | ~30–45 lines, 1 generator file plus generated header. |
| 4 | Cleaner takeover, port 80. Upstream `backend/video.cpp` publishes the active range under its queue lock and lets a waiting submit clean that range. | Partial. Local `RingCleaner::Wait` can drain queued ranges and waits for the one in progress. | 0–1.05 ms Cemetery aggregate opportunity from all other render waits, not a cleaner estimate. Expect tail-latency benefit only if active clean waits dominate. SASRT removed >50 ms cleaner waits. | Intended exact for stable upload bytes. Duplicate cache clean consumes bandwidth. Test ticket order and range lifetime through submit/recycle. Row 80 records false, current upstream definition is true. Use a cvar to test each way. | ~20–40 lines, 1 file. |
| 5 | Generated cards by view, port 140. `tools/store_cards.py` plus `app/CMakeLists.txt` gates marking to inspected views + predecessor pages and skips a zero card. | Absent for generated scalar stores. Local hand-native range writers already use a different path. | ~0 direct in `PagesClean`. Guest-side benefit unknown, including possible cost of extra test on frequent physical stores. | Intended exact for the card consumer set and its alias boundaries. Check physical apertures and overlapping/unaligned stores with verification. CMake `BLUR_CARD_VIEWS_ONLY` upstream defaults on. Census branch marks all pages. | ~20–40 lines / 2 files for this slice. |
| 6 | Bulk copies mark per run, SASRT item 39 inside base port 74. `cpu/banjo_hot_prelude.h`, generated `hot/banjo_hot_options.h` and `tools/localize_native.py` defer a card until the run changes page or flushes. Later ports 90/150 specialize pages and publication boundaries. Base port 39 is replay inheritance. | Absent for general bulk copies. Local `MarkStoreCardsAt/Host` batches explicit ranges, including CRT copies, and some hand natives already mark per range. | ~0 direct render saving. Guest gain unknown, depends on repeated marked stores per page and how often runs must flush. | Intended exact with every observable publication boundary covered. Delayed invalidation across a call, fence or release can reuse stale geometry. Audit the generator, retain per-store fallback, and treat the later call-elision separately. | ~80–200 lines / 3–4 files, generated files excluded. |
| 7 | Lazy depth decompress, port 45 **with 129**. `gpu/ultimate.*`, plume `plume_deko3d.h` and `plume_deko3d_commands.cpp` track pending depth by written view, materialize on read or view change, and repair bind serials and usage tracking. | Absent in plume. Local `banjo_renut_depth_words_lazy` defers word resolves at another layer. The local plume command file has no eager `resolveUnboundDepth` equivalent to replace. | No CPU credit. GPU gain unquantified, potentially zero with today's backend. The older docked ~0.9 ms/frame covers the depth-resolve category, not redundant decompressions. RR6's ~0.46 ms GPU win was against its eager path. | Intended exact relative to eager decompress if every sampled/aliased view is handled. Earlier lazy form had a missed target rebind. Preserve pending-view lifetime, read barriers and state. Verify depth samples and alias rebinds before deciding. | ~100–200 lines / 4–5 files including integration and prerequisites. |
| 8 | Depth restore candidates, SASRT item 54 in base port 54. `render/surfaces.cpp/.h` cache structural candidates per depth target and rebuild on table epoch changes. `gpu/sasrt_gpu_round.*` wires the toggle and A/B. | Absent, including the matching `DepthRestoreSourceLocked` path. Local depth handover/word resolves are separate. | ~0 on the current path. SASRT's 1.2 ms requires scans Tekken does not call. | Intended exact relative to the source restore path if epoch updates preserve lifetimes and live write serial tests. Adding EDRAM restore itself can change images. The upstream turn-off notes cite Sonic 4's sky for the underlying restore. | ~60–110 lines / 3–4 files for candidate caching alone, plus its missing restore prerequisite. |
| 9 | Cached export seeds, SASRT item 55 in base port 54. `render/draw.cpp`, plume `plume_deko3d.cpp`, and `gpu/sasrt_gpu_round.*` select CPU-cached export buffers, 256-byte-aligned padded windows and per-seed range cleans. | Partial. Local memexport has GPU-to-GPU reseeding and a CPU fallback, with a default GPU buffer. No cached/ranged-clean form. | ~0 if CPU seed bytes are zero. No seed count in these logs, so no positive estimate. SASRT's aggregate whole-run figure was flat. | Intended exact with coherent CPU/GPU ownership, aligned ranges, proper creation-time flush and barriers. Bad ordering can expose stale export bytes. Leave off until CPU-seeded export workload is demonstrated. | ~60–120 lines / 3–5 files. |
| 10 | Split long frames, port 32. `render/draw.cpp` plus `gpu/ultimate.*` submit/advance without present when upload use passes a threshold (192 MiB upstream), invalidate fast state and open the next list. | Partial mechanism only. Local kick/submit controls exist. No byte-threshold split matching this path. | 0 expected in steady fights below threshold. Helps memory spikes if encountered and may insert a wait. | Intended same command/present ordering. Check transient allocations and export lifetimes across slot advance. Use only for measured high-water pressure. | ~30–70 lines / 3 files. |
| Safety | Committed vertex and fetch reads, port 101. Upstream checks readable prefixes before the queued conversions and copies. | Absent locally. | No speed gain expected. Added locks and heap walks can cost render time. | Same bytes on valid inputs. Zero-fills the unreadable tail and declines incomplete cache entries. Keep off for a title proven not to need it. | ~90-130 lines / 3 files (draw.cpp, gpu/ultimate.*). |
| Later | Other render costs. Port 244 gates per-draw counts in `banjo_renut_tuning.*`, `sasrt_gpu_round.*` and plume. Port 105 puts a bounded MPSC ring in `backend/render_thread.cpp`. Port 145 also adds dirty texture rows in `render/textures.cpp` and target retention in `render/surfaces.*`. | Local report instrumentation and ordinary mutex queue. Missing the equivalent controls/ring/dirty-row and target-retention paths. Async present from port 145 is already here. | Counts: ~0.04–0.09 ms/frame at 310–370 draws if Banjo's 0.12–0.24 µs/draw transfers. Queue: the producer budget is in the QueueDraw section. No texture upload or target churn totals support a Tekken gain. | Counts affect diagnostics. Ring must preserve FIFO, callbacks, wakeups and shutdown. Dirty rows need exact layout/dirty coverage. Retention trades memory for fewer allocations and needs view/lifetime checks. | Counts ~60–120 lines / 5–7 files, ring ~150–300 / 1, dirty rows ~100–160 / 1–2, retention ~70–130 / 2. |

The local `line_compare.h` already has the four-line NEON mask used by pushed constants. Upstream's ~13 changed
lines fold a 1–3 line remainder into an overlapping last four lines, preserving the exact mask. That is
a possible follow-up for constant comparison, with no measured effect on either hot render routine here.
For draw skipping and replay inheritance, upstream's per-game rule is to keep the feature only where its
counter fires and validate whole gameplay sessions. Their absent prologues cannot be turned off to save Tekken
time. Likewise, the optional tiled cache is a GPU A/B and upstream reports mixed results across games.
Use the local `switch_perf_count_calls` switch and stack sampling only as needed for diagnostics. Credit
ChanseyIsTheBest and the original port authors in `THIRD_PARTY_NOTICES.md` if code is later ported.

### Counters needed before choosing implementation

Add these only in a future measurement build. Keep one aggregate line per category per 10 s, with frames, draws,
kicks, a stage marker, clock/mode and sampling enabled state. Use counters local to the render thread and sample
timers at a known rate to limit observer cost. Run separate correctness verification with card hashes enabled,
since verification changes the clean scan and conversion workload.

- `[vertex_work]` by fill/UP/whole/range/persistent-promotion caller and Copy32 bulk/three-word strided/other
  strided/expand path. Include conversions, converted vertices, source/destination bytes, format/stride/endian
  histogram, time, same-kick and cross-kick reuse, cache hits, promotions, cap refusals, evictions and duplicate
  conversions. Add a distinct PMULL call/byte/time total for `TrackConvertedStream` and raw fetch-copy totals.
- `[page_proof]` by persistent stream/index/mesh, reused upload stream/index/raw fetch, whole-kick replay and
  stability probe. Include calls, ranges, unique pages, repeated pages per kick, NEON16/NEON8/scalar pages,
  dirty pages per alias, preceding-page-only invalidations, clean/serial-new/invalid results, time and fallback
  hash bytes/time. Report `changed under clean cards` in verification sessions.
- `[dynamic_streams]` records dynamic fills and bytes, quiet streak buckets (0, 1–15, 16–59, 60+), projected
  redemptions and post-redemption reuse by stream key and range. Shadow measurement must account for the
  fact that `PagesClean` consumes card marks and stamps serials. Isolate or explicitly A/B any extra probes.
- `[submit_detail]` reports cleaner active/queued waits separately from GPU fence, drain and pacing, with
  count, bytes, p50/p95/max and takeover duplicates. Also capture peak upload MiB, CPU vs GPU export seeded
  bytes/time, target creates/reuses and original vs dirty texture upload bytes. GPU timing should count
  depth unbinds, read transitions, alternate-view binds and actual decompressions alongside frame GPU time.
- `[readable_ranges]` samples committed prefix lengths for the queued vertex and fetch paths (including
  physical aliases), checks, failures, zero-filled bytes and heap-walk time. Port 101 is absent locally:
  upstream `render/draw.cpp` and `gpu/ultimate.*` (~90–130 lines / 3 files) checks the physical heap where
  appropriate, converts/copies the readable prefix, zero-fills the tail and bars incomplete fills from caching.
  This is a correctness port, exact on valid ranges and deliberately different on invalid ones. Steady speed
  estimate is zero or a loss from per-fill heap locking and page walks. SASRT measured ~0.4 ms jointly with
  a separate stack-header check. Rank it ahead of performance work only if the census or a fault shows an
  invalid read. Upstream defaults on and documents turning it off for titles that prove they do not need it.

Measure matched fights in Cemetery, Lightning Storm and Manji Valley. Test one switch at a time with matching
clocks and identical diagnostics, and use `[frame_chain]` for main waits and render busy/idle along with frame
time and p90. The current logs establish hot instructions and broad budgets. They do not establish how many
streams can be redeemed, which proof callers own most pages, how much of other-wait is cleaner work, or how
many depth reads or CPU export seeds occur. No measurement build was made for this analysis.

## Render census on Switch (2026-11-22)

**Decision.** Defer upstream stream redemption for Cemetery and Lightning Storm. Almost all vertex conversion time in these fights falls outside the dynamic fills it could redeem. Prioritize an exact three-word strided conversion kernel. Investigate the stability caller of `PagesClean` separately.

### Capture and selection

Real console, measurement build `1ee830f3`. The accompanying profile reports stack sampling disabled. Source: `rex_stderr.log`, SHA-256 `cb284df65e76d6cc181818fc318774035c706776d45dbf33f23003f61af3f26c`. Number the 10 s render census headers from one. The log has three sustained fight-like blocks separated by menu/load transitions. Stage assignments for the first two follow the requested Practice order. The third stage is unconfirmed. Use only the complete steady windows below, weight per-frame counters by their reported frame counts, and calculate fps as frames divided by elapsed census seconds. Inputs are rounded by the logger. These are timings with instrumentation active.

| Per-frame metric unless specified | Cemetery (8-25) | Lightning Storm (30-48) | Third block (53-71) |
|---|---:|---:|---:|
| Frames / seconds / fps | 8,387 / 180.27 / 46.52 | 8,554 / 190.18 / 44.98 | 10,115 / 190.19 / 53.18 |
| ConvertVertices calls / vertices / ms | 209.2 / 94,503 / 4.802 | 377.3 / 97,903 / 4.953 | 290.3 / 72,302 / 4.136 |
| Three-word passes / attribute vertices / ms | 1,086 / 245,151 / 3.258 | 1,640 / 252,853 / 3.303 | 1,204 / 228,224 / 3.069 |
| Other copy / bulk / expand ms | 0.515 / 0.007 / 0.000 | 0.533 / 0.005 / 0.000 | 0.157 / 0.004 / 0.000 |
| Flush calls / fills / destination bytes / inclusive ms | 5 / 207 / 3,914,112 / 6.435 | 5 / 374 / 4,002,053 / 6.786 | 5 / 289 / 3,148,068 / 5.338 |
| Dynamic fills / converted bytes / ms | 0.70 / 729 / 0.0011 | 0.47 / 28 / 0.0004 | 58.09 / 408,176 / 0.4554 |
| Shadow probe ms / replacements over whole block | 0.0008 / 5,597 | 0.0006 / 691 | 0.0496 / 578,663 |
| Stability proof calls / page visits / dirty marks / ms | 360 / 894,622 / 999 / 3.181 | 497 / 1,217,056 / 963 / 4.318 | 413 / 1,021,831 / 751 / 3.628 |
| Persistent cache proof / texture proof ms | 0.118 / 0.236 | 0.154 / 0.192 | 0.119 / 0.211 |

**Stream redemption.** All selected windows reported zero clean and eligible conversion time and zero crossings of the 60-proof threshold. At printed precision the shadow classified the dynamic fills as unknown. It can lose history on replacement and cannot date a pending card. Even eliminating *every* measured dynamic conversion would recover only about 0.001 ms/frame in Cemetery and less than 0.001 ms/frame in Lightning Storm before upkeep. Budget approximately zero conversion savings there. The third block has a 0.455 ms/frame gross dynamic conversion budget, with no demonstrated redeemable portion. Its frequent shadow replacements warrant a collision-resistant diagnostic before making a stage-specific estimate there.

**Three-word conversion.** The timed passes account for 68% and 67% of conversion time in the first two fights. A 15-30% reduction in *elapsed time in those passes* would save 0.49-0.98 ms/frame in Cemetery and 0.50-0.99 ms/frame in Lightning Storm (0.46-0.92 in the third block). This is a sensitivity estimate. Verify byte equality across strides, endianness and tails, then measure an on/off build in matched console fights. Passes count an attribute over a chunk, so a vertex can appear several times. Flush, conversion and pass timers nest and their costs overlap.

**Store cards.** The stability path accounts for about 90% and 93% of measured proof time in the first two fights. Upload and replay round to zero across these windows. Page visits include repeated checks. The stability bucket combines whole-range promotion and stream/index stability checks. Split these callers and count repeated ranges per kick before estimating savings from narrower or reused proofs. This cost is separate from the timed shadow probe used to assess redemption.

## Four-vertex Copy32 A/B (2026-11-22)

The init-only `tekken_vertex_copy12` defaults off. Set it on to process strided three-word copies in groups of four vertices, with the existing per-word endian transform and a scalar tail. A native Windows guard-page check compared the new kernel with independent byte permutations in 4,200 cases across four endian modes, varying strides and vertex counts, checking padding and the last accessible byte. Console timing and visual verification are pending.

Use the same PGO+LTO NRO for both runs. Keep FPS, frame chain and render census enabled, with stack sampling off. Restart the title between runs. Use the same clocks, characters and Practice fights for three minutes each in Cemetery then Lightning Storm. Save stderr separately for each run. The census records the active switch as `copy12`. Compare three-word ms, converted workload, render busy, main semaphore wait, frame p90 and FPS over steady windows. The initial adoption target is at least 0.5 ms/frame less render busy in both stages with matching output.

The `pair-vtx` counter counts adjacent compatible Copy32 pairs times converted vertices in the strided path. Both offsets must be contiguous and both elements must require the same word transform. A run of three contributes two pairs. It measures potential for a later fusion experiment. It neither fuses attributes nor measures saved time.

## Copy12 A/B and rain / draw queue follow-up (2026-11-22)

The Switch A/B used handheld 1020/460.8/1331.2 MHz in both runs. The logs confirm copy12 off/on and particle natives off in both. Steady census windows (A 12-24 and 29-46, B 8-19 and 23-32, numbered from one) show 47.63 -> 43.41 fps in Cemetery and 43.40 -> 45.20 in Lightning Storm. B processed more three-word attribute vertices in Cemetery and fewer in Lightning Storm. Three-word time per attribute vertex rose from 13.47 to 13.93 ns and from 13.20 to 13.76 ns respectively. Keep copy12 off.

Use the same PGO+LTO NRO in both rain/queue packages, with copy12 and stack sampling off and the frame chain and render census on. A uses the recompiled particle routines. B uses the existing native routines, with the same rain latch in both. Close the game completely between variants and save stderr separately. In each run use the same clocks, characters and Practice inputs. Record Cemetery then Lightning Storm, about three minutes of combat each. Note when the rain becomes visible and any differences or stall.

The census associates draws with recently observed physical emitter slot pools using the renderer's alias mapping. These are approximate capacity ranges for the two known emitters, refreshed by their hooks. Collisions and unmappable pools are reported. A 48-byte stream overlapping a recent pool classifies a draw by guest topology. Whole-buffer draws without stream keys are reported as unclassified. The fill and proof data are overlap estimates, not a claim that every byte in a range is a particle. The all-quad timer covers ExpandIndices only and omits other setup and upload costs.

Stability repeats count matching physical start, byte length, proof serial and kick. The fixed hash table can miss repeats on replacement. The measured time includes the normal scan for the observed repeats, and does not establish that it can be skipped. Check for intervening writers before attempting reuse. The queue timer samples one in 16 draw publications per producer and measures acquisition wall time, including the uncontended fast path and preemption. The aggregate extrapolation across producers is a budget and does not establish where the main thread was blocked.

Consider the Bangai-O shared quad index buffer if particle draws are non-indexed quad lists with a meaningful expansion budget. For indexed quads verify actual index order before assessing eligibility. For triangles check the tagged fill and stability cost. Consider port 105 only if the queue acquisition budget is meaningful and tracks the main thread's semaphore wait. Both ports are pending console evidence.

## Rain, native particle A/B and render queue results (2026-11-22)

Real Switch, build `3b344611`, stack sampling off and copy12 off in both captures. The particle dispatch logs confirm recompiled routines in A and native routines in B. Both profiles show handheld 1020/460.8/1331.2 MHz in the sampled steady fights. A reports a transient 1785/76.8 MHz sample around the inter-stage transition, outside the selected combat windows. The first visible raindrop has no timestamp. Stages are assigned from the requested Cemetery then Lightning Storm order, using the sustained load blocks and excluding entry/exit windows. Neither capture emitted a hang snapshot. Both consumed latched rain pulses.

| A original / B native | Cemetery A | Cemetery B | Lightning Storm A | Lightning Storm B |
|---|---:|---:|---:|---:|
| Census window numbers (one-based) | 5-9 | 7-11 | 13-21 | 16-22 |
| Frames | 2,221 | 2,453 | 3,935 | 3,430 |
| FPS | 44.35 | 49.01 | 43.66 | 48.92 |
| ConvertVertices vertices/frame | 97,107 | 92,176 | 100,014 | 95,726 |
| ConvertVertices ms/frame | 4.967 | 4.863 | 4.978 | 4.965 |
| All measured quad expansion ms/frame | 0.101 | 0.109 | 0.106 | 0.120 |
| Draw queue lock estimated ms/frame | 0.119 | 0.075 | 0.083 | 0.077 |
| Stability proof ms/frame | 3.370 | 3.703 | 3.659 | 3.549 |
| Repeated stability calls/frame and ms/frame | 104 / 0.917 | 110 / 0.968 | 106 / 0.978 | 113 / 1.043 |
| Main wait on S:F8000080 ms/frame | 5.08 | 3.43 | 4.79 | 3.58 |
| Render busy ms/frame | 16.76 | 17.54 | 18.45 | 18.21 |

Census timing averages are frame-weighted. Frame-chain fields use the complete matching lines recovered from stderr in the interior of the selected blocks (4/4/8/6 per column for the reported render and semaphore figures). These independently clocked windows overlap approximately. B spent about 2.15 and 2.46 ms less wall time per frame in the two stages. The main wait shrank by about 1.65 and 1.21 ms respectively while conversion stayed near 5 ms. This points to CPU work or scheduling around the particle path. The semaphore measures work completion on a draw worker, so the difference cannot be charged directly to particle simulation. B also converted about 4-5% fewer vertices. Keep this as a positive single A/B and validate stability and visuals in longer, matched fights before making native particles the default.

Defer the Bangai-O quad-index and port 105 queue changes. All measured quad expansion accounts for 0.10-0.12 ms/frame, excluding setup and upload. The sampled queue lock estimate is 0.08-0.12 ms/frame, including uncontended acquisitions and scheduling. Four of the five original Cemetery windows report 0.062-0.066 ms/frame, with a single 0.335 ms/frame outlier. The queue data do not support treating the whole semaphore wait as contention.

Particle range attribution found zero classified draws, queued fills and stability proofs in both sessions. This does not identify the particle topology. A's second fight also produced hundreds of pool hash collisions per window. The next targeted trace should match full emitter buffer ranges at producer draw capture, before UP data is copied, and detect copies to staging if needed. `DrawStreamKeys` omits immediate UP streams and whole-buffer draws bypass that census, so either can escape the current matcher. Add direct ranges and topology only as needed. The ~1 ms/frame measured in repeated stability proofs is a budget for the whole scene. Establish intervening store-card writes and eligibility before attempting to reuse a proof.

Stderr SHA-256 A `20e1512dd1d180a5de204ed100d302f5ce8823dc584a6d182dda59bfcf834588`, B `4cafc3a3cced22b3a1302a19e47572dfe5fafa2e515462460cf24d3e85847e7d`.

## Single-build PagesClean experiment (2026-11-22)

The init-only `tekken_pages_clean_neon32` switch is off by default and enabled in this package. AArch64 can check 32 clean pages at once. The same three physical aliases and the low-memory fourth alias are tested for zero cards. The maximum of the 32 unsigned serials answers the same threshold test as checking each one. Ranges containing a dirty card fall through to the existing 16-page, 8-page and scalar paths. Page zero, aperture crossings and verification mode also use the existing paths. No cached proof is reused.

A host differential check with SIMDe ran 2,327 cases comparing the original and wide scan's return value, all cards and serials. Cases included both physical apertures, boundary pages, each dirty-card lane, unsigned serial extremes, varied offsets and verification mode. The same extracted test compiled for Cortex-A57. Timing and visuals remain unverified on hardware.

The package uses Switch PGO+LTO, native particles on, copy12 off, FPS and frame chain on, and the render census and stack sampling off. The player can use it normally. Previous console captures provide context for a future log, with different instrumentation and no exact matching of fight inputs.

### Single-build console result (2026-11-22)

The player ran Cemetery followed by Lightning Storm. Both stages sustained combat after entry. The profile reports handheld 1020/460.8/1331.2 MHz with stack sampling disabled. Stderr confirms native particle dispatch. This comparison uses the previous native-particle B capture as a reference. That capture had the render census on and different fight inputs. The new capture has frame-chain diagnostics and no per-proof timer. These whole-build differences cannot isolate the 32-page scan's contribution.

| Steady combat metric | Cemetery prior B | Cemetery wide scan build | Lightning Storm prior B | Lightning Storm wide scan build |
|---|---:|---:|---:|---:|
| FPS | 49.01 | 49.44 | 48.92 | 52.74 |
| Render busy ms/frame | 17.54 | 16.57 | 18.21 | 16.63 |
| Main wait on S:F8000080 ms/frame | 3.43 | 2.78 | 3.58 | 2.20 |

Current figures are from one-based frame-chain windows 8-12 (2,474 frames) and 16-22 (3,695 frames), excluding the mixed entry and exit windows. FPS uses the mean of the printed rates in the equal-length windows. Render and semaphore timings are weighted by reported frame counts. Average of the window p90 values is 23.26 ms in Cemetery and 21.40 ms in Lightning Storm.

The player clarified that the breakable section of Cemetery dropped the fight into an underground area that ran faster. The player then switched to Lightning Storm. Keep 49.44 fps as the estimate for the sustained, heavier Cemetery segment. Window 13 rises to 55.8 fps while render busy falls to 8.80 ms/frame. This is consistent with leaving the heavier segment, but the 10 s reports contain no markers for the floor break or the exit to the menu. Neither the underground FPS nor the relationship of the window 7 stall to the floor break can be established from the logs.

Before the sustained Cemetery block, window 7 shows a 5,445.5 ms maximum frame and one watchdog snapshot. The same window shows 11.99 ms/frame in Swap, of which 11.98 ms/frame is accounted to the ahead path. The snapshot records the main thread as running in guest-wait terms and several workers parked, with some waits resuming in the interleaved log. The game recovered on its own and continued through both maps, with latched RainCondvar pulses consumed. The capture establishes a transient stall without identifying its cause or linking it to the wider card scan. Keep the single package as an optional play build and retain the fallback switch.

Stderr SHA-256 `ed36dfb64c5b3ff12ef3052ded51458ff2cbb19f5d79d787a634ac0308df758d`. Profile SHA-256 `e51c200002aea5c005bdd844a889848744d0dbd1aa51921c113d5ef87e018a07`.

## Grouped draw snapshots and slow-frame report (2026-11-22)

The init-only `tekken_snapshot_delta_grouped` switch defaults off. On AArch64 with NEON comparison enabled, CaptureDelta builds the same packet from compile-time runs of adjacent 64-byte chunks, using `Lines64DifferMask` and copying changed lines in ascending order. Runs stop at excluded regions and at 32 chunks. Partial tails use the original byte count, and floating-point constant groups keep their existing packing. A host differential check using SIMDe produced identical delta masks, payloads and updated shadows in 29,536 cases for the narrow and full layouts. It changed each byte individually and also followed sequences of random writes across frames. The same extracted code compiled for Cortex-A57.

The single Switch PGO+LTO package enables native particles, the 32-page card scan, grouped snapshots and the frame chain. Copy12, stack sampling and render census stay off. The optional frame-chain switch `tekken_frame_chain_slow` adds at most one stderr line per 10 s window for the slowest Swap interval of at least 25 ms. It reports Swap and ahead time, main-thread waits and render batches and vertex fills completed during the interval. Render runs ahead or behind main, so these counters reflect completions during that interval. A batch can straddle a boundary and the fill time includes fingerprints and copies. Console timing and gameplay validation remain pending.

### Grouped snapshots console result (2026-11-22)

The supplied logs contain the new `[frame_slow]` line, native particle dispatch and handheld 1020/460.8/1331.2 MHz throughout, with stack sampling disabled. Stage assignment follows the previous Cemetery then Lightning Storm test order, using the rain latch and the intervening lower-load windows to bracket the fights. The log has no marker for Cemetery's breakable floor. Numbers describe the sustained heavy block.

| Steady fight metric | Cemetery previous | Cemetery grouped | Lightning Storm previous | Lightning Storm grouped |
|---|---:|---:|---:|---:|
| FPS | 49.44 | 50.71 | 52.74 | 51.29 |
| Render busy ms/frame | 16.57 | 15.76 | 16.63 | 16.16 |
| Main wait on S:F8000080 ms/frame | 2.78 | 2.83 | 2.20 | 2.55 |
| Mean window p90 ms | 23.26 | 21.89 | 21.40 | 21.36 |

The new capture uses frame-chain windows 8-25 (9,136 frames) and 29-45 (8,727 frames), excluding mixed entry/exit windows. As above, FPS and p90 are means of printed 10 s windows, and timings are weighted by frame counts. Using only the first five and seven steady windows to match the previous durations gives 50.22 and 51.01 fps, with 16.00 and 16.30 ms render busy. Both selections give lower measured render busy in both stages, but FPS improves only in Cemetery. The main-thread semaphore wait shows no improvement. Separate fights with different inputs and the new slow-frame timer cannot establish the grouped path's standalone gain.

The previous 5.45 s pause did not recur. No watchdog snapshot was emitted. The mixed stage-entry windows peaked at 411 ms and 313 ms respectively. In steady combat the worst reported frames were 35.9 and 31.1 ms. The slow-frame logger reported 18 Cemetery windows and 16 Lightning Storm windows. Median completed vertex-fill work in each selected slow Swap interval was 7.36 and 6.74 ms, respectively. A Lightning Storm interval logged 12.75 ms of completed fills. The counters attribute work by completion between main-thread swaps; they cannot label which render frame caused an individual spike. Cemetery also shows an interval with 11.91 ms of ahead wait and zero completed fills, so the slow intervals do not all have the same profile.

Keep the grouped path available under its cvar. The next evidence to seek is which conversions the renderer repeats, especially UP data, queued fills and whole-buffer promotions. Distinguish duplicate conversions from the fingerprints and fetch copies included in the fill timer. An adjacent-draw eligibility count would test whether FFXIII-style state reuse addresses a meaningful share of this work.

Stderr SHA-256 `b55ec80b8e36fe100fb93051fc8ca00a02e59a7708d98ed33e9ee9b57cd4a691`. Profile SHA-256 `7ad73a19552508262e30ba94094a0b7c95f57781e7c410e303db1678d03e14ad`.

## FFXIII store cards and two feasibility counters (2026-11-22)

The FFXIII store-card address change was adapted in `tools/store_cards.py` for scalar, D-form and MMIO-capable stores. It captures guest operands once and uses the existing store and card macros, preserving store-before-mark order, signed D-form displacement, physical-window translation and MMIO dispatch. Vector and CAS marking forms retain their existing rules. `REX_CARD_ADDR_TWICE` restores the previous definitions at compile time. Apply the regenerated macro block to the existing PCH and rebuild the Switch PGO+LTO NRO with Tekken's existing profile.

The extracted generated macros and the fallback were compared on the host with 32,000 stores per configuration across four configurations (both address branches, with cards on and off). Final memory, card bytes and the MMIO write trace matched. These tests establish output equality for the exercised stores. Hardware performance remains to be measured.

The single package also enables `tekken_ffxiii_census`. Its one 10 s stderr line counts queued draws, immediately consecutive draws with matching device chunks and draw ranges, and that subset with vertex transform group 0 marked dirty. It excludes UP and tessellation, and requires no intervening render item. This is a candidate count observed before drawing. It cannot prove unchanged geometry or texture bytes. The same line reports ReadLevel calls and input bytes and time, separately timing tiled levels, plus the most expensive level and the current Swap count at its completion. It covers CPU texture preparation on the render thread. Match load windows with the slow-frame log before considering parallel untile.

Source: [FFXIII store-card macros](https://github.com/recomp-nx/fnxf_xiii/blob/da9e745ff45e5bb827aaad4b7b5b958280e50a2a/tools/store_cards.py#L34), [repeat criteria](https://github.com/recomp-nx/fnxf_xiii/blob/da9e745ff45e5bb827aaad4b7b5b958280e50a2a/app/src/renut_engine/gpu/render/draw.cpp#L8262), [CPU texture preparation](https://github.com/recomp-nx/fnxf_xiii/blob/da9e745ff45e5bb827aaad4b7b5b958280e50a2a/app/src/renut_engine/gpu/render/textures.cpp#L462).

### FFXIII experiment console capture (2026-11-22)

The new census was present, native particles were active and stack sampling was disabled. Clocks were handheld 1020/460.8/1331.2 MHz. Stage assignment follows the requested Cemetery then Lightning Storm order, bounded by rain-latch announcements and load transitions. The steady render-census windows are 12-25 and 29-41, respectively, counted from one. Earlier frame-chain summaries are partially missing in mixed stderr. The independent profile confirms roughly 51 fps and 49 fps in the sustained blocks.

| Steady fight metric | Cemetery | Lightning Storm |
|---|---:|---:|
| Render frames | 7,087 | 6,312 |
| Approximate FPS (nominal 10 s windows) | 50.62 | 48.55 |
| Queued draws / adjacent matching state and range | 4,274,617 / 11,823 | 3,743,870 / 21,411 |
| Same state and range | 0.28% | 0.57% |
| Candidates with transform group 0 dirty | 0 | 0 |
| ReadLevel weighted ms/frame | 0.0040 | 0.0047 |
| Slowest individual ReadLevel in block | 3.29 ms | 4.43 ms |

For comparison, the prior grouped-snapshot build reached 50.71 and 51.29 fps. The new run added both the generated address change and the follow-up instrumentation. It also used a separate play session. The result shows no overall FPS improvement in this capture and cannot isolate the codegen change or diagnose the Lightning Storm difference. The 13 complete Storm frame-chain windows averaged 17.39 ms/frame render busy, versus 16.16 ms previously. There was no watchdog snapshot.

Defer the FFXIII adjacent-draw port. Across all 41 census windows, 36,717 of 9,993,630 draws matched the cheap state-and-range filter (0.37%), with zero passing the transform-write condition. The census does not attempt a geometry validity proof. A more general geometry reuse experiment needs a different candidate set and verified writes.

Keep parallel texture preparation out of the steady-fight plan. A 5.28 MiB level once took 24.50 ms near swap 3690, before the sustained rain blocks. A 266.68 ms slow frame occurred nearby at swap 3683, but the timing does not attribute it to that level. The logged long frames near either rain-stage entrance also cannot be attributed to ReadLevel. Storm's slow frames show repeated vertex-fill work, often around 6-8 ms completed between two main swaps. Separate conversion by caller, fingerprints and fetch copies to find the next opportunity.

Stderr SHA-256 `58c8bf57cc0d1389af494c5ac7e11d21fb7c7b6b19ea6e2a96734cea351b66bf`. Profile SHA-256 `9be21f8bdac2607fadd9832a0cf445c9be1bc7291013e82b674588d994f0ede0`.

## Transition stall trace (2026-11-22)

The `tekken_frame_chain_slow` report now saves the longest individual outer main-thread wait per Swap interval, with wait kind, object type and handle/address when supplied by the existing instrumentation. It also reports completed CPU `ReadLevel` time in the same interval, alongside the existing vertex-fill, render-batch and Swap-ahead fields. The slowest interval of at least 25 ms is printed every 10 s, plus up to two more when they reach 100 ms. The reported wait may be one of several shorter waits and GPU/CPU threads overlap. A texture call straddling a boundary is charged when it completes, so temporal association is a lead for investigation.

A synthetic-clock host test verified the selection and ordering of three long stalls, ordinary slow-frame selection in the following window, exclusion of nested waits and per-window reset of the texture subtotal. The single PGO+LTO package carries forward the current optimizations and disables the separate FFXIII census. Use it for normal play including map entries or the Cemetery floor break if it happens during play. Compare `ReadLevel`, `longest wait` and `ahead` in `[frame_slow]` against the worst intervals. Console timings are pending.

### Transition trace console result (2026-11-22)

Build `68b5d725`, handheld 1020/460.8/1331.2 MHz, native particles on, stack sampling off. In 39 logged slow Swap intervals, only three show nonzero rounded ReadLevel time (0.12, 0.79 and 63.48 ms). Of the 13 logged intervals of at least 100 ms, two show nonzero ReadLevel time. No watchdog snapshot fired.

| Swap | Interval ms | Main waits ms | Swap ahead ms | ReadLevel ms | Longest measured wait |
|---|---:|---:|---:|---:|---|
| 38 | 1059.01 | 970.59 | 0.00 | 0.00 | Event F8000154, 591.78 ms |
| 927 | 166.72 | 164.41 | 0.00 | 0.00 | Event 401B88E4, 148.76 ms |
| 3276 | 110.22 | 2.20 | 97.06 | 63.48 | Event F800007C, 1.81 ms |
| 5017 | 389.16 | 12.12 | 0.00 | 0.00 | SignalAndWait, 12.01 ms |
| 13075 | 302.97 | 11.53 | 0.00 | 0.00 | SignalAndWait, 11.38 ms |

Swap 3276 shows strong overlap between an ahead wait and expensive texture preparation in the render stream (95.75 ms of completed render batches). It precedes the first rain latch and cannot be assigned to Cemetery's floor break. The 63.48 ms total can include multiple levels or work started before the interval. Other 270-389 ms peaks recorded little main wait or Swap time and no completed ReadLevel work. Their remaining wall time is unclassified; logged wait time excludes other guest work and unscheduled time.

This supports a narrowly scoped large-level untile experiment for load spikes if the pattern recurs, separate from sustained fight FPS. First log the size and number of levels or stage totals when ReadLevel crosses a spike threshold to distinguish one large level from a burst. A parallel row path should preserve the bytes and keep small levels on the normal path. The repeated long outliers with little measured wait would require a targeted main-thread capture or measured load markers at the actual cause.

Assuming the same Cemetery then Lightning Storm order, sustained frame-chain windows 11-22 and 26-36 averaged 50.52 and 53.52 fps over 6,069 and 5,893 frames. Render busy was 16.47 and 15.19 ms/frame, with S:F8000080 waits of 3.03 and 1.36 ms/frame; average window p90s were 22.43 and 21.01 ms. The capture switched off the separate FFXIII census, and the bouts were separate from previous runs. FPS is not an isolated change measurement.

Stderr SHA-256 `f8178c368b7f0a8d329e1d54e8f568833d38e38aa95f1adae51c95aadfa2cf73`. Profile SHA-256 `13d526f583cff79da315b77e5432640ccc9bff5623b0cd8eae7875a75b8d26a4`.

### ReadLevel breakdown for slow frames (2026-11-22)

The `[frame_slow]` texture field now reports the accumulated CPU ReadLevel time, complete level count, and time and guest input size of the slowest completed level for that Swap interval. ReadLevel records a subresource preparation, which may be a slice. Reported input size is blocks times guest bytes per block, rounded to 4 KiB, and individual durations are rounded to 32 microseconds. One relaxed atomic snapshot transfers all four fields together. The counters saturate at 4,095 levels, about 33.55 s total, 2.10 s for the peak level and just under 256 MiB for the peak size.

The texture attribution test distinguished four smaller levels totalling about 62.5 ms from one level of similar total duration. An interval with no completed texture work showed zero. A saturation check confirmed that fields cannot overflow into one another. The single PGO+LTO package uses the previous transition package's settings. Use ordinary play and compare the next spike to swap 3276 in the previous capture. A single level dominating repeated spikes supports a targeted parallel untile path. Many smaller levels would instead support investigating how preparation is scheduled.

#### Console result (2026-11-22)

Build `6d5bfddd`, Switch handheld clocks 1020/460.8/1331.2 MHz, stack sampling disabled. There were 38 slow intervals logged, including 17 at or above 100 ms. Three intervals contained completed ReadLevel work. No hang snapshot was emitted.

| Swap | Interval ms | Swap ahead ms | ReadLevel ms | Levels | Slowest level ms / MiB |
|---|---:|---:|---:|---:|---:|
| 888 | 126.26 | 0.00 | 0.10 | 1 | 0.10 / 0.129 |
| 2074 | 105.90 | 92.19 | 57.89 | 430 | 3.49 / 0.781 |
| 11978 | 714.32 | 700.57 | 3.84 | 83 | 0.64 / 0.250 |

Swap 2074 resembles swap 3276 from the previous capture and again precedes the first rain latch. The slowest subresource accounts for about 6% of measured preparation in this burst. Parallelizing the single largest level would leave almost all of this batch's cost. If optimizing the burst, first count levels prepared immediately versus in `FlushTextureFillsLocked`, and whether they are needed by the next draw. A safe candidate is to distribute independent deferred fills across a small number of helpers during the same kick and join before submit. Moving work to another frame requires a separate proof of first use and of guest-data lifetime.

The larger swap 11978 was dominated by Swap-ahead blocking, with only 3.84 ms of completed ReadLevel work and 28.66 ms of render batches recorded at completion. The capture cannot assign the rest of the wait to any render subsystem. If it recurs, time the frame-ahead onset and the render stage in progress directly. No floor-break marker is present.

Three rain-latch announcements are visible, so do not assume there were only two uninterrupted fights. Sustained frame-chain windows 8-9, 14-20 and 24-35 show 50.75, 50.51 and 52.41 fps respectively. Stage names are unconfirmed in this capture.

Stderr SHA-256 `507b7bccefa9e64ffa186e76a98dc1fd1d63a106496182fd11110c93d6574f6b`. Profile SHA-256 `dae478e47950c0ef8467f3c96414443020659821b7972742640a5018a1b154a5`.

## Deferred texture batch and row preparation (2026-11-22)

The single Switch package enables two init-only options, both off by default. `tekken_texture_fill_helper` starts one helper for deko3d deferred fills with cached upload memory and at least 32 subresources and 1 MiB of guest input. It shares groups of four disjoint staging allocations with the render thread. Both process the existing deferred fills during the same kick. The render thread waits for completion before upload cache maintenance and submission. Command order and allocation ownership stay with the render thread. The helper is joined after the render thread stops. Small batches, uncached uploads and the separate FFXIII census use serial fills.

`tekken_texture_row_fast` keeps up to 64 KiB of reusable row scratch per copying thread and copies directly into staging when the host block size matches and endian is zero. Other formats keep the existing endian and decoding path. Tests with the actual tiling and conversion helpers compared legacy and revised output (including padding) in 768 cases across linear, tiled, 2D and 3D textures. The same cases were run through 60 successive helper batches across five start/stop cycles with byte-identical output for stable input.

The one-package test retains the previous frame diagnostics. `[texture_batch]` reports the first eligible batch and later batches above 8 ms, rate-limited, with Swap number, fill counts by thread and wall time. `[frame_slow]` still sums per-level CPU duration, which can exceed wall time when workers overlap. Use the batch line to measure elapsed time and look for the prior 430-subresource transition. Console performance and visuals are pending.

### Deferred texture batch console result (2026-11-22)

Build `d3feaa13`, Switch handheld 1020/460.8/1331.2 MHz, stack sampling disabled, native particle dispatch confirmed. The first eligible batch at swap 1757 had 62 fills, including 10 completed on the helper, and took 2.40 ms wall time. This confirms the helper entered production work. The capture has no further batch line. Later batches below 8 ms and those within the two-second logging cooldown are omitted.

Among 35 logged slow Swap intervals, one had completed ReadLevel work (0.13 ms in a 133.36 ms startup interval). Eleven intervals exceeded 100 ms. The 430-level, 57.89 ms preparation burst and 700.57 ms ahead wait from the prior capture did not appear. Large stalls of 290-391 ms around stage entry and menus persist, with no recorded texture work in those intervals. No watchdog snapshot fired. The sessions have different loads. This does not establish a reduction for the previous 430-level burst or identify the remaining stalls.

| Sustained block, by capture order | Frames / frame-chain windows | FPS | Render busy ms/frame | Main S:F8000080 ms/frame | Mean window p90 ms |
|---|---:|---:|---:|---:|---:|
| First, presumed Cemetery | 6,126 / 7-18 | 50.98 | 16.45 | 2.65 | 22.36 |
| Second, presumed Lightning Storm | 5,810 / 22-33 | 48.38 | 17.45 | 2.75 | 22.69 |

The observed frame rate did not improve across both maps. Keep the texture options experimental. For comparable load spikes, report a short per-window aggregate of eligible batch counts, fill counts by thread and worst wall time. For the remaining long stalls, prioritize host-stage markers over expanding texture parallelism. For steady fights, resume attribution of vertex fills to conversion, copies and content checks.

Stderr SHA-256 `bcdb825deb7407701638979823b022ccbaf433eb3e93b904fcdafe22081fd621`. Profile SHA-256 `03a4b40fc66a10d9288b796286935882ec6f2bd864d2cb30fab42ce901aad8fa`.

## Vertex work and render wait tracing (2026-11-22)

The texture-helper test did not isolate the sustained FPS drops. This build
keeps its configuration and adds diagnostics through `tekken_pipeline_trace`
(init-only, off by default). A normal console session covering both rainy
stages is enough for the test. Collect `rex_stderr.log`.

- `[pipeline_vertex]` reports calls, KiB and ms per render frame in 10 s
  windows. `queued` measures deferred conversions, `up` measures vertices
  supplied directly by a draw call, `whole` measures conversion of a whole
  range into the cache, and `promote` measures reconversion to promote a
  stream. All four count output bytes. `hash` measures vertex, index and fetch
  hashing on the render thread and counts input bytes. `fetch_queued`,
  `fetch_now` and `fetch_pool` measure deferred copies, immediate copies and
  copies into the cache. `flush_n/ms` covers all of `FlushVertexFillsLocked`,
  including work from the other columns when it runs inside that function.
  Adding those columns to the flush would double-count that work. This test
  keeps `tekken_render_census` and `switch_profile_stacks` disabled.
- `[pipeline_ahead]` reports the two stages with the most overlap with a
  frame-ahead wait of at least 25 ms, at most twice per second. Use `swap`
  to correlate with `[frame_slow]`. `history_missing` is the initial part of
  the interval missing from the 512-transition history. `batch` and `frame`
  are general labels for regions without a more specific stage.
  `between_batches` includes queue waits and time when the render thread is
  paused outside a batch. `state_lock` requires `tekken_frame_chain`, which
  already measures the contended branch. Wall time can include time when the
  thread is descheduled. This trace covers frame-ahead waits once the game
  reaches that point.

### Render trace console result (2026-11-22)

Build `970de172`. The log confirms a GPU clock of 460.8 MHz and memory at
1331.2 MHz. The two fight blocks are assigned to Cemetery and Lightning Storm
based on the agreed order because the log lacks map identifiers. The selected
windows cover sustained load and exclude menus, transitions and three early
windows with interleaved vertex lines. FPS is render frames divided by wall
time. Per-frame costs are weighted by each window's frame count. The
`frame_chain` windows are matched by order and count, with slightly different
boundaries.

| Block | Pipeline windows / frames | FPS | Render busy ms/frame | Main busy ms/frame | S:F8000080 ms/frame | Deferred conversion ms/frame | Hash ms/frame | Total flush ms/frame |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Cemetery, by order | 9–18 / 5003 | 50.0 | 16.87 | 14.73 | 2.96 | 4.714 | 1.429 | 6.423 |
| Lightning Storm, by order | 22–33 / 5823 | 48.5 | 17.46 | 15.80 | 3.20 | 5.476 | 1.622 | 7.402 |

Deferred conversion rose from 268 calls and 3.63 MiB of output per frame to
288 calls and 4.10 MiB. Hashing read 4.83 and 5.47 MiB per frame. In the
selected windows, all three `fetch` paths round to 0.000 ms/frame. `whole`
and `promote` together stayed below 0.02 ms/frame in each window. The flush
already includes deferred conversion and some hashing. `hash` combines
several callers and does not measure how much work can be avoided. The trace
measures wall time and adds clock reads, so these FPS figures describe the
instrumented run.

Five frame-ahead waits of 26.83–47.51 ms were recorded. Four carried the
general `batch` label. The wait at counter 3053 overlapped the vertex flush
for 32.20 ms. The specific flush operation is unknown, and descheduling
remains possible. `pipeline_ahead` counts completed Swaps. For example, its
26.83 ms wait at counter 3720 matches `frame_slow` for Swap 3721, which took
49.89 ms.

There were 37 `frame_slow` lines, 11 of at least 100 ms. Those 11 showed
0.00 ms in frame-ahead at the log's precision. The longest was 403.40 ms at
Swap 3300, with 12.04 ms of measured game waits, 6.70 ms of completed render
batches and no `ReadLevel` calls. Only one of the 37 lines included
`ReadLevel`, a single 0.13 ms level at Swap 888. The texture helper took
work at Swap 2128 (62 fills, 12 on the helper, 2.59 ms wall time). No hang
snapshot was recorded. Most of the time in transition stalls remains
unattributed.

**Next build at this point.** First explore an exact way to share large,
independent deferred conversions within the same kick, with the render
thread participating. Keep cache proofs and promotions ordered and consume
the output only after conversion completes. Measure flush wall time and
the effect on `S:F8000080`, since another thread can compete for CPU or
memory bandwidth. A hypothetical 20% reduction in that conversion would save
0.94 and 1.10 ms/frame of gross render time. Net savings depend on scheduling
overhead and actual overlap. Second, attribute hashes by caller
(`TrackConvertedStream`, exact cache proofs and others) before skipping
reads. The 1.43–1.62 ms/frame figures are measured costs. Keep `Copy12` off
based on its earlier A/B and defer `stream_redeem` based on the previous
census. Investigate long stalls outside frame-ahead separately.

At the observed FPS, reaching 55 requires reducing wall time by about
1.82 and 2.44 ms/frame. Reaching 60 requires 3.34 and 3.96 ms/frame. Render
thread savings shorten the frame only when they reduce its critical path.

Stderr SHA-256 `4468f5dcb089cfd4f7894ed771cfae097cf2d5518e481813105c1a11630d91cd`.

### Shared vertex conversions and hash attribution (2026-11-22)

`tekken_vertex_fill_helper` is init-only and off by default. With deko3d
and cached staging, the render thread shares up to 512 pending fills with
the same helper that prepares textures. A batch needs at least 16 eligible
fills and 256 KiB of output. Each eligible fill contains only `Copy32`
attributes. Floating-point expansion and builds running the full render
census use the serial path. If the helper is unavailable, the render thread
converts all fills as before.

Each new fill has its own destination in the upload ring. The two threads
claim groups of four. Fill descriptions stay alive until both threads finish,
before staging cache maintenance, submission and fence completion. After
the shared conversions, the render thread walks the fills in order to handle
promotions, cache checks and the remaining copies. Both batch types share one
helper. Shutdown waits for it before freeing resources. Kick points and the
guest synchronization protocol are preserved.

With `tekken_pipeline_trace` on, `pipeline_share` reports batches, eligible
fills, fills completed on the helper and wall ms per frame. `queued` still
reports conversion count and bytes. For shared fills, its timing includes
the complete batch and the wait for the helper. This time is also included in
`flush_n/ms`. `pipeline_hash` breaks actual hashes down into stability probes
in `TrackConvertedStream`, exact stream, index and fetch proofs, and seed
hashes for persistent copies. Each field reports calls, KiB read and ms per
frame. A hit in the kick's hash memo avoids rereading the range.

The local check used the production scheduler and scalar conversion paths.
It compared 2290 eligible fills byte for byte, including bytes outside their
destinations. It covered all four endian modes, interleaved ranges, cases
below and above the thresholds, and serial fallback after shutdown. Switch
behavior and the impact on FPS, flushes, `S:F8000080` and queue timings
remained to be measured at this point. The test package kept the trace build's
settings and enabled `tekken_vertex_fill_helper`.

### Vertex helper console result (2026-11-22)

Build `7cdf102d`. The log reports a GPU clock of 460.8 MHz and memory at
1331.2 MHz. The fight blocks are assigned by the agreed map order. Sustained
windows 8–16 and 21–27 are numbered from the `pipeline_vertex` headers
that survived in the file. Some lines were truncated or interleaved with other
data. The selected windows retained the frame count, deferred conversion,
total hash and corresponding render line. Block averages for all
subcounters and total flush time could not be recovered.

| Block | Frames / s | FPS before → after | Deferred conversion ms/frame before → after | Render busy ms/frame before → after | Hash ms/frame before → after | S:F8000080 ms/frame before → after |
|---|---:|---:|---:|---:|---:|---:|
| Cemetery, by order | 4332 / 90.04 | 50.00 → 48.11 | 4.714 → 3.319 | 16.87 → 15.22 | 1.429 → 2.094 | 2.96 → 2.93 |
| Lightning Storm, by order | 3240 / 70.09 | 48.49 → 46.23 | 5.477 → 3.475 | 17.46 → 16.46 | 1.622 → 2.367 | 3.20 → 2.13 |

Conversion fell by 1.40 and 2.00 ms/frame at similar output volumes (3768
and 4106 KiB versus 3712 and 4197 KiB), although the fill count and mix
changed. Complete `pipeline_share` lines confirm that the helper converted
107.88 of 222.00 fills and 164.54 of 310.00 fills per frame in two fight
windows. FPS fell in both blocks. Separate fights do not isolate the cost of
contention or the helper's effect on the game. One readable window in the
second fight showed 17.78 ms main-thread busy time (45.9 FPS). Lower render
busy time helps frame time when the render thread is on the critical path.

In the readable fight reports, `TrackConvertedStream` stability probes
accounted for over 99% of reported hash time. Two windows showed 2.084 and
2.352 ms/frame on that path, compared with 0.004 and 0.008 ms/frame for exact
stream proofs. Total hash time rose 0.66–0.74 ms/frame versus the reference.
Keep the vertex helper off for a build aimed at improving FPS. The next
experiment is to space out promotion probes for expensive streams that
change frequently while they lack a persistent copy. Preserve content proofs
for every copy that gets used. Measure skipped hashes, promotions, hits,
extra conversions and FPS. The roughly 2 ms is measured hash cost. Delaying
profitable promotions could lead to more conversions.

Seven frame-ahead lines were recorded. The longest wait, 2304.99 ms at
counter 2755, matches the 2328.88 ms interval at Swap 2756. The trace labeled
the entire wait `batch`. That interval included 3.35 ms of completed vertex
fills and 0.35 ms of `ReadLevel`. The general label leaves the cause
unknown, including any role the helper may have played. Another 14
`frame_slow` lines exceeded 100 ms, all with frame-ahead rounding to
0.00 ms. No hang snapshot was recorded. Follow up on the 2.3 s stall
separately from sustained FPS.

Stderr SHA-256 `150efe567db390e6f177c3025303edab0ec5c45bc886b1e5e60a2309bb2ac3a8`.

### Vertex promotion probe backoff (2026-11-22)

`tekken_stream_hash_backoff` is init-only and off by default. It applies
only in `TrackConvertedStream` to streams eligible for promotion that have
no persistent copy and at least 8 KiB of input. The first hash establishes
the sample. After two consecutive observed changes, the next probe is set
4 kicks out. Further changes extend the interval to 8 and then at most
16 kicks. A matching hash restores the normal probe cadence. Skipped kicks
do not count as matches. The copy uses current data and starts without a use
proof, as in the previous path. Copy validation, store cards, conversion
output and work publication follow the existing flow.

The first `pipeline_vertex` line includes `skip_n/KiB` (skipped promotion
hashes and input KiB), `made_n/KiB` (completed promotions and output KiB),
`cached_bind` and `kick_bind` (persistent and ring reuses in `BindStreams`).
All are per-frame averages. The mesh and whole-range paths have their own
checks and are outside these two bind counters. Compare `skip_n/KiB` and
`pipeline_hash` alongside `queued`, `promote`, render busy time and FPS.
More deferred conversions can offset part of the savings if a profitable
stream takes longer to promote.

The local test ran the promotion logic with a simulated hash and pool.
For an 8 KiB stream that changed over 100 consecutive kicks, it checked
10 probes versus 100 in the original mode. It verified that probing resumes
and creates a new copy once the content stabilizes, the copy starts without
a use proof, discarding it resets the state, and the original cadence
applies below 8 KiB. The hardware benefit remained to be measured at this
point.

The PGO+LTO test package enabled this option and tracing, disabled
`tekken_vertex_fill_helper` and kept the other settings from the previous
build. Use the first trace package with the helper off as the main
reference for FPS and render time. The package with sharing enabled
provides the earlier hash breakdown.

## Promotion probe back-off on the Switch (2026-11-22)

### Latest Switch test with promotion probe backoff

PGO+LTO NRO from `f52a6e7d` with `tekken_stream_hash_backoff=true`, the
vertex helper off and tracing enabled. GPU clock was 460.8 MHz and EMC
was 1331.2 MHz. The log does not identify the map or mark when Cemetery's
floor breaks.
There is a short stretch at 53.29 FPS (window 8), followed by two long
fight blocks separated by light load. The table uses their order and
leaves the Cemetery/Lightning Storm assignment provisional. FPS is
render frames divided by wall time. Timings are weighted by frame count.
Main-thread timing comes from the matching `frame_chain` reports.

| Block | Windows / frames | FPS | Render busy ms/frame | Main busy ms/frame | S:F8000080 ms/frame | Deferred conversion ms/frame | Total hash ms/frame | Flush ms/frame |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| First long block | 13–20 / 3727 | 46.55 | 17.22 | 14.88 | 4.93 | 4.989 | 0.871 | 6.154 |
| Second long block | 24–32 / 4651 | 51.63 | 15.53 | 15.54 | 2.36 | 4.911 | 0.862 | 6.052 |

The first block skipped 29.18 hashes and 2201 KiB/frame. The second skipped
31.32 and 2102 KiB/frame. The remaining `TrackConvertedStream` probes took
0.863 and 0.851 ms/frame. The renderer created 2.09 and 2.70 persistent
copies per frame, and `BindStreams` recorded 551.53 and 341.36 persistent
copy uses respectively. The previous package lacks equivalent counters for
those last metrics. The first trace package, with both vertex experiments
off, recorded 50.00/48.49 FPS and 1.429/1.622 ms/frame of total hashing.
The latest log measures reads skipped by the backoff policy. Hash time
fell by 0.56/0.76 ms/frame between captures. The fights and their workloads
differed, so both the timing difference and FPS changes have uncertain
attribution. The wait on `S:F8000080` rose in the slower block.

Five frame-ahead waits of 39.75–94.70 ms were recorded. Four were in
`batch` and one 54.74 ms wait was in `vertex`. The large texture burst
returned at Swap 2108 with 429 levels, 54.72 ms of accumulated `ReadLevel` time,
a 3.49 ms slowest level, and 94.70 ms in frame-ahead within a 112.58 ms
interval. The only `texture_batch` line was at Swap 2325 (62 fills, 12 on
the helper, 2.53 ms wall time). These records cover different Swaps. The
longest interval was 824.96 ms at Swap 39, with 592.85 ms in the
`F8000154` event wait. Other stalls around 300–406 ms still occurred
outside frame-ahead. The 2.3 s wait did not recur, and no hang snapshot was
recorded in this run.

The raw log is archived at
`out/console_runs/stream_hash_backoff_20261122/rex_stderr.log`
(local, ignored by Git). SHA-256
`4f4895444e2c86b11f01a5842b2241132b28a4ff98fc227f1634bd572784a21c`.
Parts of the log are interleaved. Extract lines by prefix and check fields
before averaging.

### Final play package

`../builds/2026-11-22_tekken6_final/COPIAR_A_LA_SD/switch/tekken6-nx/`
contains `renut.nro` and `katamari.toml`. `renut.elf` is next to
`COPIAR_A_LA_SD`. This package reuses the instrumentable PGO+LTO binary from
`f52a6e7d` that produced the latest log. Both copies were verified by
SHA-256. The play configuration differs from the latest test and still
needs its own console run. Linking completed with exit code 0 and the two
existing Vulkan header ODR warnings.

~~~toml
banjo_show_fps = true
tekken_native_particles = true
tekken_pages_clean_neon32 = true
tekken_snapshot_delta_grouped = true
tekken_vertex_copy12 = false
tekken_texture_row_fast = false
tekken_texture_fill_helper = false
tekken_vertex_fill_helper = false
tekken_stream_hash_backoff = false
tekken_frame_chain = false
tekken_frame_chain_slow = false
tekken_render_census = false
tekken_ffxiii_census = false
tekken_pipeline_trace = false
switch_profile_stacks = false
~~~

Native particles gained 4.66 and 5.26 FPS in the Switch A/B. The 32-page
scan and grouped snapshots preserved output in differential tests and
remain enabled based on the lower render times observed with those builds.
Keep the FFXIII store-card address change, which is active in the binary.
The texture options are off in the play package while their effect on
sustained performance remains unproven. `Copy12` took longer per vertex.
The vertex helper cut conversion by 1.40/2.00 ms, while FPS fell in both
blocks of that run. Backoff reduces hash work and remains experimental
given the differing FPS and waits across blocks. `stream_redeem` and
FFXIII consecutive-draw reuse remain deferred because their measured
potential was low here.

NRO SHA-256 `a583e5e78506c3f447690cf12cd9de13727213134f7ac4a44d9df3795b29d1b2`.
ELF SHA-256 `1bcec20cca0173b137ad5ce8fed6fb085ae5cc8d40f8da5087814b9ef59aa192`.


## Render and main thread round (2026-11-23)

The `render-thread-analysis` branch was reviewed (the store-card macro change evaluates the same address once,
signed displacement kept) and fast-forwarded into master. Built-in Switch defaults (`app/katamari_switch_defaults.toml`)
now carry every setting the console proved, so the SD's `katamari.toml` only needs player options.

Console runs, handheld 1020/460.8/1331.2 MHz, Cemetery, three fights (Lili vs Panda, Kazuya vs Jin, Lili vs Kazuya),
render census and frame chain on in all of them:

| Build | Fights fps | Finding |
|---|---|---|
| Skin census (`builds/2026-11-23_tekken6_censo_piel`) | 51-52 / ~54 / 49.5-51 | ~65,000 of the ~93,000 vertices converted a frame (3.6 of 4.9 ms) read the native skinning jobs' output (stride 64 and 32); stride 48 (~40,000) is main's copy of it (`sub_82533C00`) |
| `tekken_vertex_raw_layout` | 52.5-54 / ~54.6 / 50-51 | queued conversion 4.9 -> 2.75 ms (all bulk), render busy ~14.5 ms; main became the limit. Visuals checked by hand |
| + `tekken_frame_chain_priority = 58` | 52.4-53.5 / 53.4-55 / 50.2-51.6 | main's wait on S:F8000080 3.0 -> 1.4 ms (its releasers are the five skinning workers, not the 2D worker); render then waited ~2.9 ms "other" |
| + `tekken_ring_cleaner_priority = 45` | 53.3-54.8 / 54.7-55.8 / 51.3-53.7 | the render thread's wait for the ring cleaner 2.9 -> 0.55 ms; render waits for main again (3.3 / 6.3 / 3.5 ms) |

- `tekken_vertex_raw_layout` (`render/vertex_format.cpp`, `BindStreams` in `render/draw.cpp`): word-copy streams keep
  the guest offsets; the stream is bound with the guest stride (deko3d `applyVtxStrides` in
  `plume_deko3d_commands.cpp` binds a draw's own non-zero stride over the pipeline's) and converted with one bulk swap,
  up to the last vertex's extent. Its key leaves the layout out (`kRawStreamLayout`) so declarations could share it,
  though the converted vertex count did not fall (no sharing happens in these fights).
- `tekken_frame_chain_priority` / `tekken_frame_chain_object` (`sdk/src/system/xobject.cpp`): guest threads that wait
  on or release the object get that host priority once. `tekken_ring_cleaner_priority` (`backend/video.cpp`).
- Census additions (`tekken_render_census`): `skin jobs` line (skin output pages tagged by `NoteSkinOutput`) and
  `fills by stride`.

Main thread now, per Cemetery frame: busy ~13.5-14.5 ms, S:F8000080 ~1.2 ms, critical section 0x829CA098 ~0.8-1.3 ms
(3-6 entries), and its largest single cost is `sub_82533C00` (~1.3 ms, inlined in the copy of `sub_825234A0`): a
strided 48-byte copy of the skinning output that stalls on its source loads (candidate: a hand native with prefetch,
through `app/src/cpu/hot/hand.txt`). Other open items: motion blur (Options > Other options > Motion blur) shows on
the PC build but not on the Switch; the story mode's XMA decoder at ~21 % of a core.

### The skinning job queue's critical section (2026-11-24)

`tekken_cs_trace = "829CA098"` (diagnostic, `sdk/src/kernel/xboxkrnl/xboxkrnl_rtl.cpp`; it slows the game ~5 fps, its
hold times are inflated) showed 0x829CA098 is the skinning job queue's lock: the five workers (callers 0x82548450 and
0x82548AE4) and the main thread take it ~150 times a Cemetery frame, each hold ~10 us, and a taker that finds it held
sleeps on the host waiter (the section has no spin count and `kernel_critsec_spin` is 0) and wakes ~0.15 ms later.
`tekken_cs_spin = "829CA098:1024"` (now a built-in Switch default) spins on that section only before the waiter: the
main thread's wait on it fell from 0.8-1.3 to ~0.3 ms a frame. Console, frame chain on, no census: Cemetery fights
54.9-56.1 / 57.1-57.7 / 53.5-55.3 fps. The render thread still waits for main ~3.5 ms (6 ms in Kazuya vs Jin); main
is busy ~13.8 ms and waits ~1-1.3 ms on S:F8000080.

### Skinned-vertex gather native and the audio fix (2026-11-24)

- `tekken_hand_skin_gather` (hand native of `sub_82533C00` in `app/src/cpu/hot/banjo_hand_natives.h`, line in
  `hand.txt`, the hot copies regenerated with `tools/post_codegen.py`): the same copy and sum with the source
  prefetched 8 elements ahead. Console, Cemetery: main busy 13.2-14.6 ms as before, fights 54-55.6 / 56.4-57.3 /
  52.1-52.9 fps. No gain: **off by default**, to be removed at the next tidy-up. Lesson: a guest address written in a
  string outside `app/src/cpu/hot/` (a cvar description) makes `localize_native.py` treat it as hooked and pull its
  callers out of the copies; keep guest addresses in comments or under `hot/`.
- That run's audio broke up (audible distortion): ~160-250 mixer requests without data every 10 s in Lili vs
  Panda, 0 on 2026-11-22. The frame-chain priorities had put the main thread and the skinning workers above the game's
  sound threads, which the game raises itself (KeSetBasePriorityThread, ignored here). `tekken_guest_priority_raise =
  57` (`sdk/src/system/xobject.cpp`, built-in default) runs every guest thread whose guest priority is above 0 at host
  0x39, applied by the thread at its next wait: ~15 threads (guest priorities 1, 15, 16, 59). Console, Lili vs Panda:
  0 requests without data in the whole fight; 52-53 fps (main busy ~14.7 ms, ~1 ms more than before, so some of the
  raised priority-1 threads may be busy ones: next, raise only the high guest priorities).

### Which guest threads to raise (2026-11-24)

Three console runs of Lili vs Panda (Cemetery), audio clean (0 mixer requests without data) in all of them:
every high guest priority at 0x39: 52-53 fps; all of them at 0x3A (the main thread's level): 52-54; only guest
priorities 1-16 (the sound threads) at 0x39 and the rest (the 2D drawing worker, guest priority 59, ~24 % of a core)
left at 0x3B: 52.4-54.1. The sound priority costs nothing measurable; the earlier 54.9-56.1 of the same pairing was
that run's own load. Kept: `tekken_guest_priority_raise = 57` with `tekken_guest_priority_raise_max = 16` (above it a
thread stays at 0x3B). The main thread now: busy ~14 ms, ~2 ms on the skinning workers' semaphore, ~0.35 ms on their
queue lock; the render thread waits for it ~4.3 ms a frame.

### Rain particle prefetch; renderer A/B for Azazel and motion blur (2026-11-24)

- Stack-sampled console profile (Cemetery, Lili vs Panda): main's work is flat (largest the skinned gather,
  9.3 %); three rain worker threads at ~16 % of a core each, two thirds in `Native05` (`tekken_particle_native.cpp`):
  ~18 % on each particle's first record load, ~11 % on the sin/cos tables, ~5 % on the shared slot counter's atomic,
  ~3 % on FPCR switches. `tekken_native_particles_prefetch = 4` prefetches the A/B records ahead in both natives
  (no result changes). Console: Lili vs Panda 52-54 -> 54-55.7 fps with the frame chain on.
- Azazel (Arcade's last fight) draws only his gold accessories, not his crystal body; the motion blur option shows on
  the PC build, not on the Switch. No shader/pipeline failure in the detailed log. A toml turning off ten renderer
  skips (resolve_skip, depth_handover, skip_dead_draws, dead_raster, blend_skip, proof_depth_only,
  depth_words_lazy, edram4_alias_by_shape, resolved_retire_idle, pass_narrow_end) changed neither. A second toml
  (readback_memexport on and twelve shader-compiler shortcuts off: alpha_spec, early_z, early_hoist, early_alpha,
  structured_cf, uam_uniform_branches, vs_outputs, no_texture_signs, tess_spec, texcoords_unswapped,
  identity_remap, skin_once) **hung the GPU twice** (white screen, hard power-off): do not ship that combination.
  Next: compare the Switch renderer's handling of these draws with the PC path offline rather than by console A/B.

### Azazel's crystal body: a 2D texture sampled as a cube (2026-11-25)

Tools: the PC build runs again (`OnMain()` is a function on Windows: the runtime DLL exports no thread_local data;
`--banjo_cpu_native_bulk=false` avoids the old intro crash "Call to invalid or unregistered function at guest address
0x00011C00", which the bulk copies cause on PC only). `app/src/tekken_memsnap.cpp` (PC only, game window in front): P =
guest memory snapshot to `memsnap/` (`tools/memsnap_diff.py`), O = write `tekken_poke`, C = ask RenderDoc for a capture.
Launchers in the project root: `JUGAR_PC_TEKKEN6.bat`, `CAPTURAR_AZAZEL_RENDERDOC.bat` (portable RenderDoc 1.47 in
`../herramientas/`; scripted replays with `qrenderdoc --python`).

- On the PC build (D3D12) Azazel's body is drawn but dark blue/black (Xbox 360: translucent purple/pink crystal); on
  the Switch it is not drawn at all. Motion blur shows on neither.
- RenderDoc (`../capturas_renderdoc/azazel_frame31346.rdc`, event 664): the body's pixel shader declares a Texture2D
  and a TextureCube. Renut's shared texture indices give fetch slots 0, 1, 2 the 2D textures 0x29F (1024x1024 BC1),
  0x3F6 (1024x1024 BC3), 0x3E2 (**128x128 BC1, 3 mips**) and every cube index the null cube. The shader samples the
  128x128 texture (a 2D fetch constant, dimension 2D) with tfetchCube; Renut files it in the 2D table and the cube
  read gets the null cube. On Xenos a cube fetch of a 2D texture selects the face's coordinates on the single image.
- Fix to make: XenosRecomp records which fetch slots a shader samples as cubes (a `textureCubeMask` next to
  `textureFetchMask` in `ShaderCacheEntry`); FillTextures gives such a slot holding a 2D texture a host cube made of
  that image on all six faces (made once per texture). Then check the Switch, where the draw vanishes (deko3d).
- Memory facts: the motion blur option is very likely the u32 at 0x827B6650 (0 = on, 1 = off, the list index; four
  snapshots alternating); 0x82C7F16C / 0x82C7F188 / 0x82C7BBF2 count 1, 3, 4 over Arcade stages 1, 3, 4 but are NOT
  the stage (writing 8 broke the next stage's loading).

## Azazel's crystal body: the cube twin (2026-11-25)

Fixed on PC and on the Switch (console test: Azazel drawn whole at 57-60 fps, no hang).

- **Cause:** the body's pixel shader samples fetch slot 2 with tfetchCube, but the game puts a 128x128 BC1 *2D* texture there. Xenos reads it anyway. Renut put the texture in the 2D table, so the cube read got the null cube: the body was dark on PC and invisible on the Switch.
- **Translator:** XenosRecomp sets shaderFlags bit `kShaderFlagCubeShift + slot` (bits 4..31) for every tfetchCube.
  - These are `SHADER_FLAG_CUBE_SHIFT` in `shader_recompiler.cpp` and `ShaderCubeFetchMask` in `shader_cache.h`.
  - The Switch shader inputs must be regenerated with `build-logs/switch_shaders_local.ps1` so that `shader_cache_switch.cpp` carries the bits.
- **Renderer:**
  - `draw.cpp`'s `FillTextures` ends with a pass over `g_draw_cube_mask`: a slot read as a cube that holds a 2D texture gets `CubeTwinSlotLocked` (in `textures.cpp`).
  - `CubeTwinSlotLocked` creates a CUBE texture holding the image on all six faces.
  - The twin is filled from the guest bytes through the normal staging upload (buffer -> image), every frame for textures up to 256x256 and on a content change otherwise.
  - GPU-written textures get no twin.
- **What failed first:** the first Switch build copied the 2D image to the faces with an image-to-image copy (`dkCmdBufCopyImage`, BC1 into cubemap layers). The GPU hung at Azazel's fight, with the main thread waiting on a fence forever. D3D12 took that copy fine.
- **Logging:** the `[cube]` line is RGPU_WARN, but on the Switch it did not reach `rex_stderr.log`.

## Motion blur: the stencil reference on D3D12 (2026-11-25)

The blur now shows on the PC build (checked by hand in Practice). The Switch is not tested yet.

- **The option:** it is not 0x827B6650 (that holds the unlock bitmasks). The menu writes the global u32 0x8297E3D4 (0 = on), through sub_821A82B8.
- **The pass:**
  - sub_8221DD08 (post chain) calls sub_8221CC80 when sub_8242BB78 says so. A counter in `tekken_memsnap.cpp` (PC log `[blur]`, every 5 s) showed about 60 passes a second in fights.
  - It redraws the characters into an R16G16_FLOAT velocity target. Each of these draws runs with stencil mode 2 (sub_82522D60: REPLACE, reference 1).
  - Three full-screen blends follow: a scene copy, then two velocity blurs. They use stencil NOTEQUAL 0, so they draw only on the marked pixels.
  - The engine records all of this in its own list and runs it later, so nothing happens during the guest call itself.
- **Cause:** plume's D3D12 pipelines never set `stencilRef` (checkStencilRef applies it with OMSetStencilRef), so every draw ran with reference 0. Renut's key had reference 1 (logged), but the stencil stayed 0 and the blends drew nothing.
  - Fix: `stencilRef = desc.stencilReference;` in the D3D12GraphicsPipeline constructor (`plume_d3d12.cpp`).
  - deko3d bakes the reference (dkCmdBufSetStencil with `p->stencilReference`), so if the Switch still lacks the blur, the cause is elsewhere.
- **Tools:** the hook on sub_8221CC80 needs `tools/post_codegen.py`, so that `direct_calls.py` routes its callers through the hook. Before that, the call was direct and the counter read 0.

### Motion blur on the Switch: still missing (2026-11-25)

The pass runs on the Switch too, but the blur does not show. Console tests in Practice, with the blur option on:

| Test | Result |
|---|---|
| A: `banjo_deko3d_zcull = false` | no blur |
| B: `banjo_deko3d_depth_compression = false` | no blur |
| C: `tekken_vertex_raw_layout`, `banjo_deko3d_push_constants` and `banjo_deko3d_draw_resolves` off | no blur |
| D: `banjo_cpu_native_bulk = false` | unsure |

Ruled out, with the evidence:
- **Stencil state (`tekken_stencil_census`, stderr):** it reaches the pipelines as on PC.
  - Velocity draws: R16G16_FLOAT target, a 1024x576 depth-stencil target, reference 1, REPLACE.
  - Blends: reference 0, NOTEQUAL.
  - deko3d's stencil op and compare mapping is right, and `dkCmdBufSetStencil` gets the reference.
- **Vertex data (`banjo_renut_diag_stream_dump_vs = "3DEAC931E5EA68AF"`, the velocity VS; now also on stderr):** checked with the machine copies on and off (E1, E2).
  - The stream is 48 bytes a vertex: POSITION0, TEXCOORD0, TEXCOORD1 = the previous position.
  - The previous position differs from the current one.
  - TEXCOORD1.w, the per-character fade of sub_821C6738 (0.3 to 1.0), reaches 1.0 in most dumps.
  - So the velocity input is right on the console.

Next suspect: the blend shader (ps F9D1DD2D81C36418) only sums samples whose scene-copy alpha is >= 1.
- That alpha is written by the `g_vColor` quad. The quad writes alpha only (colour mask 8), under stencil NOTEQUAL 0, into the copy target, which is then resolved to the texture the blends sample.
- If the Switch loses that alpha (the stencil test of that quad, the alpha-only write, or the resolve), the blend returns the unblurred scene, which is what the console shows.
- Checking this needs a GPU readback of the copy texture on the console. There is no tool for it yet.

### Motion blur on the Switch: solved, it was the game's own governor (2026-11-25)

The section above went the wrong way. Renut draws the blur correctly on the Switch. The game does not ask for it.

How it was found:
- Resolve traces (`banjo_renut_diag_resolve_trace`): every PC fight frame has the blur block (a 1024x576 velocity target, then the blends resolved back into the scene); none of the Switch frames do.
- The bloom chain's other format (11 on PC, 20 on the Switch) is only `renut_post_process_precision` (High on PC, Game on the Switch).
- `tekken_blur_gate_log` (`app/src/tekken_memsnap.cpp`, `[blur-gate]` on stderr every 5 s) re-runs the checks of the gate `sub_8242BB78`, in order:
  1. the camera state, `sub_8221ED10`;
  2. the scan of ids 2359..2410, `sub_8235D570`;
  3. the option at 0x8297E3D4;
  4. `sub_821C6570` on `[[0x82735C70 object]+888]+584`: its +84 and +180 must be non-zero, and it answers with its +88.
- In fights the Switch fails the last check in 70–95 % of frames, the PC in 0–2 of 300. The failing field is item+88.
- `sub_821C6738` sets item+88 only while two floats of the object behind the handle at 0x8273593C are below 80 % (95 % in some states):
  - +124 is the last frame's CPU time;
  - +136 is the last frame's GPU time (`sub_82515980`);
  - `sub_82219B70` keeps both as percentages of a frame, clamped to 999.9.
- So it is the game's performance governor. In fights the PC runs at 30–55 % CPU and 46–70 % GPU; the Switch at 60–145 % for both. The blur then shows only now and then, which looked like a flickering draw bug.

What changed:
- `tekken_blur_always` (default false by default, also in the player's `katamari.toml`; true for worst-case measurements): when the menu option is on and only item+88 says no, the gate says yes. The menu option alone decides. Cost on the console: ~3 ms a frame, Practice 60 → 51–52 fps. With the option off: 0 blur frames, 60 fps.
- Always on: `[blur]` on stderr and in the PC log, so measurements and logs from other people say whether the blur was drawn:
  - one line when the menu option changes;
  - every 10 s, how many frames drew it and how many of those passed the governor's limit.
- `sub_8242BB78`'s bulk native copy was dropped (`localized_bulk.txt`, `banjo_hot_bulk_5.inc`) for the hook (one console sample).
- The VS screen before a fight asks for the blur itself, even with the option off (the trails on the characters). It shows the same on PC: this is by design.
- `tekken_blur_gate_force` was folded into `tekken_blur_always`.

### PGO without unrolling, peeling or the tracer (2026-11-25)

`BANJO_SWITCH_NO_UNROLL=ON` (CMake option, default OFF) adds `-fno-tracer -fno-unroll-loops -fno-peel-loops` to the PGO-use build, an idea from the TTT2 Switch port's notes.
- The NRO is 2.8 MB smaller: 65.96 MB against 68.79 MB.
- Console A/B (Cemetery, the same fight): median 54.2 fps against 53.7, and main busy 13.71 ms against 13.77. That is within the noise.
- `app/out/switch-pgo-use` is configured with it ON.

## Overclock and the blur governor (2026-11-26)

At 1785 MHz the motion blur shows in far more fight frames than at 1020 MHz, without `tekken_blur_always`. That fits
the governor above: a faster CPU lowers +124 (CPU % of a frame) under the 80 % limit more often. On the Switch the
"GPU" figure (+136) also includes the wait for Renut's render thread, so it falls with the CPU clock too.

## Worker pool cap (2026-11-26, packages W0 / W1 / W2)

`tekken_workers_per_pool` (`app/src/tekken_workers.cpp`) caps the game's two job worker pools (3 + 2 workers, all
running the same job queue). The idea was to stop five workers fighting the main and render threads for four cores.
- A cap of 2 hung after one fight: some job waits for a worker that is not there.
- A cap of 4 gave no fps over the game's own five.
- Kept as a diagnostic, default 0 (the game's count).

## Where the main thread's frame goes (2026-11-26, package P, sampling on)

Cemetery fights at 1020 MHz, `switch_profile_stacks` and `tekken_frame_chain`:
- The main thread is busy about 14 ms a frame and waits about 3 ms more: ~1.5 ms on the semaphore F8000080 that the
  five skinning workers release, ~1.5 ms on other objects. A frame is ~18 ms (55 fps); without the waits it would fit
  in 16.6 ms.
- The skinning workers work in bursts, about 9.4 ms of CPU a frame between them; CPU in all ~345 % of 400 %.
- So the frame is limited by the main thread plus what it waits for, not by the GPU.

## Render thread: stream hash back-off A/B (2026-11-26, packages R_A / R_B)

The render thread's own time is mostly SwapWords (the bulk byte swap of skinned vertices, raw layout),
PagesCleanImpl (store-card proofs) and TrackConvertedStream (the promotion hash).
`tekken_stream_hash_backoff` (skip the promotion hash of streams that keep changing) gave no fps on the console, so
it stays off. The render thread is not the frame's limit today.

## Skinning: record prefetch distances (2026-11-26, packages S0 / S1 / S2)

`tekken_native_skin_prefetch` 8 / `_out` 0 (reference), 8 / 4 and 16 / 4, the same fight (Lili and Armor King,
Cemetery). The stage has two areas with different loads, so only the heavy area's 10 s reports were compared: 55.5,
55.8 and 56.3 fps. Within the noise; the output prefetch also raised the render thread's busy time (11.8 -> 13.3
ms). The defaults stay (8 / 0).

## Skinning: bone matrices swapped once a job (2026-11-27, packages T, U, V)

Console profile T (sampling on): the five skinning workers spend 81 % of their time in the native kernel
(`sub_82548758` 70 %, `sub_825481F8` 11 %), about 7.6 ms of CPU a frame. With the record prefetch the record loads no
longer stand out: the samples spread over the loop's 288 instructions a record, among them 37 byte swaps and 33
address masks for the four bone matrices (7 vectors each, loaded and swapped again for every record that names the
bone), and 17 stack spills.

`tekken_native_skin_bones` (`app/src/cpu/tekken_skin_native.cpp`, on by default):
- A pre-pass over the job's records swaps each named bone's 7 vectors once into a per-thread table (256 slots by index
  minus the job's first index; a job outside them runs the old loop). The same 16-byte-aligned guest vectors and the
  same swap give the same values.
- The record loop then reads the table: 187 instructions a record and 8 spills. The 112 multiplies and adds are the
  same, in the same order, so the output is identical.
- Checks: a standalone host test against the old loop (800 random jobs, unaligned tables, out-of-range indices), and
  `tekken_native_skin_check` in Ryujinx: 89 million records, 0 differ, no job past the table.
- Console A/B U (first version): 3.4 fps lost. The pre-pass read each record cold, a full cache miss a record
  (~290 ns x ~18,000 records = the 5 ms the workers gained). Fixed by prefetching 16 records ahead in the pre-pass,
  which also leaves the records in the cache for the main loop.
- Console A/B V (fixed, sampling on in both): the kernel's samples fell 24 % (91 -> 69 a 10 s report) and the fight
  went from 56.4 to 57.2 fps.

Next candidates for the main thread's waits: the other wait (~1.5 ms on "obj", probably the animation worker whose
time is `Native05`), and writing the skinned vertices once in the GPU's layout (the render thread now reads, swaps and
hashes ~2 MB of them a frame).

## Character select in 2D: a save setting, not a bug (2026-11-27)

A character select with only the 2D portraits on a grey wall (no 3D stage or models) came from one save. In
`MAINDATA.SAV`'s "SYS" header, bytes 0x14 and 0x15 were 1 (a new save has 0). With them set to 0 the same save shows
the 3D select, in Ryujinx and on the console. Which in-game option they are is not known. Loading, I/O and the
renderer were all ruled out first (the I/O log of that screen was clean: the game never asks for the 3D scene).

The save's checksum is the big-endian u32 at offset 0: a reflected CRC-32 (table at 0x8207AE80), initial value 0, no
final xor, over bytes 4 .. size (size = the u32 at offset 4). `sub_8243D230` checks it (through `sub_82366C38`); a
wrong one gives "The save file appears to be damaged". In Python:
`struct.pack('>I', zlib.crc32(data[4:size], 0xffffffff) ^ 0xffffffff)`.
