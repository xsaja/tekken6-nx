# Progress log

## Phase 0 — preparation and analysis (2026-11-04)

- Repository created; `.gitignore` excludes game data, generated code, builds, NRO/ELF, logs, profiles, keys,
  firmware, emulator data and local configuration.
- Inventory: one ISO (Europe Rev 1, XGD2), title ID `4E4D07FC`, XDK 2.0.8276, `Default.xex` hash matches the one
  tekken6-recomp supports. No title update, no DLC. Two extra archives in the root folder: the tekken6-recomp portable
  Windows builder 0.9.4 and an older `Iterate` branch snapshot (not used).
- Host: Windows 10, Xeon W-2133, 15.7 GB RAM, GTX 1080 Ti, 510 GB free on S:. Present: git 2.56, Python 3.12/3.14,
  CMake 4.4.4, VS 18 Community (MSVC 14.51, clang 22.1.3, Ninja 1.13.2, Windows SDK 10.0.26100), .NET 10 runtime.
  Missing: devkitPro, Vulkan SDK, .NET SDK, Python `xxhash`/`Pillow`.
- References cloned read-only into `../refs/` with submodules (tekken6-recomp, katamari-nx, renut-nx, sa2-nx,
  bejeweled3-nx, nfsmw-nx main and deko3d-implementation, Ryujinx fork, imgui).
- Documentation read; result in `docs/plan.md`.
- Nothing compiled.

## Phase 1 — extraction and codegen (2026-11-04)

- Host tools installed: devkitPro (devkitA64 r30 / GCC 16.1, libnx 4.12, deko3d 0.5.0, switch-tools 1.13.1,
  switch-zlib, switch-libexpat), Vulkan SDK 1.4.363, .NET SDK 10.0.403, Python `xxhash` and `Pillow`.
- ISO extracted to `../game` with `tools/extract_iso.py` (6.7 GB, 3 min 50 s); XEX SHA-256 verified.
- Skeleton imported from katamari-nx (tracked files only); removed: `pgo/`, icons, unbuilt banjo-nx / nfsmw sources,
  Katamari shader and hook-map tools, Katamari docs (method guides kept in `docs/reference/`).
- Tekken codegen data from tekken6-recomp placed in `app/overrides.toml` and `app/config/renut_hooks.toml`;
  register-localisation keys off.
- Generator built with Visual Studio's clang 22.1.3 (`tools/cfg_host.bat`, `tools/build_host.bat`): 767 steps, 131 s.
- Codegen: 25,234 functions, 17.4 s. All four mid-asm hooks and both masked switch tables present in the output.
- Post-codegen chain (`tools/post_codegen.py`): completes, 8 min 53 s; 329 read-only vector loads folded, 4,542 tiny
  leaves inlined, store cards on 112,806 stores.
- Fixes: see `docs/tekken6-notes.md`, "Phase 1 fixes".
- Not done yet (phase 2): compiling the generated code and the app for PC; reNut hooks for Tekken; Katamari renderer
  hook addresses; Tekken app code (vtable thunks, device aliases).

## Phase 2 — PC build, boot, reNut (started 2026-11-04)

- App sources made to build again after phase 1's removals (six helper files back from katamari-nx, the PM4 native
  renderer left out); Tekken app code ported from tekken6-recomp: 364 vtable thunks, mid-asm hook bodies, device
  aliases, Partition1 root-sector probe, null-device geometry 0x40.
- tekken6-recomp SDK patch: the platform-neutral runtime hunks applied (18 files as they are, 8 by hand onto the
  fork's versions). Not yet: `threading_posix.cpp` (Switch only, phase 3).
- reNut: Tekken's D3D state tables found in the image by structure (101 render states at `0x827D6A20`, 20 sampler
  states at `0x827D6EE0`, same layout as Katamari); the 222 state hooks remapped by index. The other 101 hooks
  (device creation, present, draw, clear, resolve, shaders, resources) still carry Katamari's addresses: they need
  hand mapping, because no reference generated tree (N&B / Katamari) is available for `renut_sigmatch.py`.
- NRO metadata: NAME "Tekken 6", AUTHOR "xsaja".
- PC build (2026-11-05): links with `-DBANJO_RENUT_RENDERER=ON -DKATAMARI_RENUT_HOOKS=OFF` (missing PM4-renderer keys
  in `app/src/me/me_keys.cpp`; XenosRecomp's `dxc-bin` binaries copied from the Vulkan SDK and the Windows SDK, they
  are not in the repository). Empty `out/renut_shader_dump` (Katamari's shader dumper does not apply).
- First PC runs, emulated renderer (Xenos plugin), automated with `input_script` (`out/pc_runs/*`):
  - `boot1`: attract video with PRESS START, audio, DATA*.BIN reads; 0 crashes.
  - title -> storage access -> "Select battle position preference" -> main menu (Scenario Campaign, Offline Mode,
    Gallery, Profile, Options) -> Offline Mode -> Arcade Battle -> Lars vs CPU, rounds, KO.
  - `fight2`: the fight drawn complete (stage, characters, HUD) with `--execute_unclipped_draw_vs_on_cpu=true`;
    without it the 3D scene is black (`fight1`), as in tekken6-recomp.
- Not done in phase 2: reNut (the 101 non-state hooks and the XDK 2.0.8276 device offsets, then shaders); pad and
  keyboard checked by hand.

## Phase 3 — Switch build (started 2026-11-09)

- Milestone 1 (2026-11-09): `app/out/switch/renut.nro` links (56.5 MB, NACP name "Tekken 6", author xsaja), deko3d only, no NVK.
  - Host tools: `uam.exe` built with MSYS2 clang64 (installed into devkitPro's msys2; the dkp repos are in its `pacman.conf`,
    no `dkp-pacman`), SPIRV-Cross copied from the Vulkan SDK to `out/deko3d_tools/spirv-cross-build/`. Switch libs: `switch-libzstd`.
  - Shader chain `tools/switch_shaders.ps1` (needs a Python path edit on this host): 6,862 shaders, 6,736 DKSH, 126 fail in
    uam ("array index must be < N"): to look at.
  - Fixes: `tools/build_switch.bat` (VS Community path, Python 3.12, NVK off; pass ninja flags after `--`), plume's NVK
    library check only when NVK is linked, FFmpeg `config.h` `__SWITCH__` branch (header already in the tree).
  - Not run yet: the NRO in Ryujinx (milestone 2).
- Milestone 2 (2026-11-09): the NRO runs in the Ryujinx fork (built to `../ryu-build`, data in `../ryu-data` with the
  keys and firmware copied, `game_root` a junction to `../game`, `tools/run_ryujinx.ps1` adapted). 240 s without a crash,
  game loop alive (13-24 fps in the emulator, 22 profile samples). Fixes: SaltyNX probe disabled (its stderr write faulted
  at 38 s; phase 5), `REX_SWITCH_FALLBACK_EXE` pointed at `/switch/tekken6-nx/renut.nro` (Ryujinx starts homebrew without
  argv, so `game_root` was not found and the game exited at 12 s). Ryujinx needs ~8 GB free: `dotnet build-server shutdown` first.
- Milestone 3 (2026-11-09): the title/attract sequence draws in Ryujinx through reNut on deko3d: run `emu4` (150 s, captures every
  10 s with `nfsmw_captura_cada_s = 10` in the toml, `out/emu_t3.toml`; 12 PNGs of 1920x1080 in `out/emu_runs/emu4`): the
  PRESS START / INSERT COIN screen over the feather background, the attract movie with Nina in full 3D with textures, FREE PLAY /
  CREDIT(S): 0. No crash. Emulator speed 13-24 fps (mean 23), not a console figure. Not checked yet: input (no pad script on
  Switch), the 126 shaders that uam rejects, the first logos (captures began at 10 s), the fight, the real console.
- The 126 shaders uam rejected (2026-11-09): XenosRecomp's `CubeMapData.cubeMapDirections` had 2 entries (9 only under
  `REBLUE_RECOMP`); Tekken's pixel shaders issue 4 cube ops, so DXC's output wrote out of bounds (accepted on PC, "array index
  must be < 2" in uam). `shader_common.h` now uses 9 for `RENUT_RECOMP` too: 6,872 / 6,872 shaders compile. Ryujinx (`emu6`):
  12 captures, no crash, 20-50 fps (mean ~25 after 30 s). One earlier run (`emu5`, 7.75 GB free) sat at 0 fps and made no
  captures; it did not repeat. PC not rebuilt yet: its shader cache still has the old header, so check Lili's shiny dress after
  `tools\build_pc.bat` (possibly the same bug, a cube-map reflection read out of range).
- Scripted input on Switch (2026-11-09, Ryujinx): `input_script` (SDK cvar, toml) works: title -> main menu (~35 s) -> "NOW LOADING" for a fight (~90 s)
  (`out/emu_t4.toml`, `out/emu_t5.toml`: input_script + `headless = true` + captures every 5-10 s + `switch_profile_stacks = true`).
  Then the game stalls at ~0 fps with no crash. `emu8` (420 s, stacks): the `plume::Deko3DDevice::compileWorker` thread was running uam
  on the loading screen (only ~228 variants compiled in 7 min in Ryujinx). Precompiling the variants on the PC
  (`tools/deko3d_variant_cache.py --list app/out/pc/pipeline_list.bin --list <the Switch run's pipeline_list.bin> --merge <the run's deko3d_shaders.bin>`,
  4,104 variants, 13.6 MB, 0 failed; copy as `deko3d_shaders.bin` beside the NRO) did NOT fix it: `emu9` stalled again at ~30 s with no
  variant compiled at run time (cache size unchanged) and every thread waiting in the kernel (the render thread idle; a worker doing
  `pread` on the DATA files now and then). So the stall is not the shader compile alone. Not diagnosed: the guest main thread's wait.
  The logos (4-14 s) were black in the Switch captures (10 s, 15 s): unverified whether the game draws them.
