# tekken6-nx — porting plan

Static recompilation of Tekken 6 (Xbox 360) with ReXGlue: PC (Vulkan) as the intermediate target, Nintendo Switch
(deko3d, Cortex-A57 at 1020 MHz) as the final target.

## 1. Input game data (phase 0 inventory)

| Item | Value |
|---|---|
| Disc image | Tekken 6, Europe, Rev 1, XGD2 (game partition at 0xFD90000), 7.84 GB |
| Title ID | `4E4D07FC` |
| Media ID | `7E3BCAE2` |
| Execution version | 0.0.0.3 (disc 1/1) |
| `Default.xex` | 9,220,096 bytes, SHA-256 `ee138a4fe8d32a0edc131451377f4377041a49626153fa992db24f5420a7f15c` |
| XDK (static libs) | 2.0.8276 for every library (D3D9, XAUDIO2, XAPILIB, XBOXKRNL, XMEDIA, ...) |
| Data files | `DATA301`..`DATA320` `.BIN`/`.IDX`, `DINF_300.BIN`, `AvatarAssetPack`, `nxeart`, `$SystemUpdate` |
| Title update / DLC | none |

The XEX hash is exactly the one tekken6-recomp accepts (`tools/build_from_iso.py`, `SUPPORTED_XEX`), so all its
address-keyed data applies unchanged. XDK 2.0.8276 is between Katamari (2.0.5632) and Banjo-Kazooie: Nuts & Bolts
(2.0.7645 / TU 2.0.7978), closest to Banjo, which is the XDK the reNut renderer was written against: the D3D device
layout and state tables must still be re-derived for 2.0.8276 (katamari-nx `tools/renut_device_offsets_*.py`,
`renut_state_tables.py`, `renut_sigmatch.py`).

## 2. Real state of tekken6-recomp (main, 0.9.4, commit c8d1de0)

- PC only (Windows D3D12/Vulkan, macOS via MoltenVK). Boots, menus, Practice mode and fights render according to its
  README; macOS reaches 60 fps on M1 Max. The ISO-to-play builder flow exists (portable Windows builder 0.9.4) — it is
  further along than "incomplete". There is no known-broken list.
- Renderer: the SDK's generic Xenos (PM4 + EDRAM emulation) GPU plugin, not reNut. Fights need
  `--execute_unclipped_draw_vs_on_cpu=true` (a Xenos-emulation workaround; expected to be irrelevant under reNut, which
  hooks D3D calls instead of interpreting the command stream — to be confirmed in phase 2).
- Codegen config (`tekken6_config.toml`, 259 lines): 1 `indirect_calls` (0x82390788), 2 rexcrt (memcpy 0x826A1130,
  memset 0x826A03B0), 4 midasm hooks (startup spin yield 0x8251C1FC; internal resolution 0x82219DB4/0x82219A28;
  offscreen resolution 0x821AB410), 38 `[functions]`, 12 `[[switch_tables]]`, 1 `[[invalid_instructions]]`, all
  register-diet options off.
- App code (`src/tekken6_app.h`): 364 `InstallVirtualCallThunk<...>` vtable tail-call thunks installed after XEX load;
  device aliases (`\Device\Image`, `\Device\Harddisk0\Partition0` -> Partition1), root-sector probe at 0x800,
  `null_device_sectors_per_allocation_unit = 0x40` (Cache1 bootstrap needs 0x8000 bytes per allocation unit).
- SDK: upstream rexglue-sdk `nightly-20260904-0c7b01a0` plus one 651 KB patch (~104 files). No Switch support in it.
  - Platform-neutral parts a Switch port must keep: DATA*.IDX/BIN `XMemDecompressSegmentTD` bridge (`system/xfile.cpp`),
    storage-probe IOCTL (`xboxkrnl_io.cpp`), host path / null device geometry, wait/multi-wait rework
    (`threading_posix.cpp`), child-thread CPU inheritance (`xthread.cpp`), codegen fixes (function graph, scanner,
    partial switch tables, indirect-call template, midasm hook config), SPIR-V `exp_adjust` fix.
  - Not needed on Switch: D3D12/ROV/fused resolve, MoltenVK/Metal, Windows crash reporter, launchers, builder GUI.
- The `Iterate` branch snapshot (zip in the root folder, 2026-10-16) is older than main: same config and app code,
  smaller patch, no macOS work. Main is the reference.
- Its code is incorporated here under GPL-3.0 and credited in `CREDITS.md`.

## 3. Recommended combination of the bases

**Skeleton: katamari-nx.** It is the only base with the complete PC + Switch pipeline (SDK fork v0.10.0 with the
Horizon layer, reNut/plume/deko3d renderer, post-codegen chain, hot-function tooling, PGO, NRO packaging). renut-nx
docs define the method; sa2-nx and nfsmw-nx (deko3d branch) are the catalogue of solved deko3d problems.

**SDK strategy:** start from katamari-nx's `sdk/` (v0.10.0 + Horizon layer; NVK is optional and stays off) and port
into it, file by file, only the platform-neutral hunks of the tekken6-recomp patch listed above. The tekken6-recomp
patch targets a newer upstream nightly, so it will not `git apply` cleanly; each ported hunk gets an entry in
`docs/tekken6-notes.md`. Fallback if the codegen schema differs too much: run codegen with tekken6-recomp's patched
generator and feed the output to the katamari runtime (decided in phase 1 after a dry diff of the two codegen trees).

What comes from each source:

| Source | Taken |
|---|---|
| katamari-nx | repo layout (`app/`, `sdk/`, `tools/`, `dist/`), CMake + presets, Switch toolchain file, `post_codegen.py` chain, codegen crash-fix tools (`pointer_fixpoint.py`, `gap_sweep.py`, `switch_check.py`, `read_before_write.py`, `non_argument_audit.py`), reNut deko3d renderer, shader chain, hot tooling (`localize_native.py`, `switch_hot_*.py`), English stack sampler (`sdk/src/ui/switch_perf.cpp`), PGO flow, `rexglue_switch_add_nro` packaging |
| tekken6-recomp | all Tekken-specific data: `tekken6_config.toml` entries, the 364 vtable thunks, device aliases, midasm hooks (startup spin, internal/offscreen resolution), platform-neutral SDK fixes, `tools/xdvdfs` extractor |
| renut-nx docs | phase order, section-2 censuses, enhancement list and keys, section 6b exclusions, PGO/LTO procedure, controls |
| sa2-nx | Switch input driver (`switch_input_driver.cpp`, 5 slots, per-player maps), frame pacing on the game's present interval, docked 1080p / handheld 720p swapchain resize, release without game shaders in the NRO |
| nfsmw-nx (deko3d branch) | devlog fixes as a checklist (vertex-buffer bind overflow slack, resolved-texture pitch, ZCULL invalidation, uam limits, depth sampling swizzle, single-thread uam) |
| nfsmw-nx (main) | SaltyNX template (`extras/saltynx-template`, GPL-3.0), console-overlay rules |
| bejeweled3-nx | lockstep frame-hash check and `check_switch_build.py` idea as extra verification |
| Dear ImGui | performance overlay (phase 5), as a git submodule under `third_party/imgui` (MIT) |

**To remove from katamari-nx when importing** (exhaustive list in katamari-nx `docs/optimising-functions.md`
"Stale mentions" and `docs/katamari-notes.md`):
- Codegen data: `app/overrides.toml` entries, `app/config/renut_funcs.toml`, `non_argument_as_local_keep` and
  `non_volatile_as_local_keep` lists in `renut_config.toml`, `indirect_promotions.txt`, `hot_edges.txt`,
  `hot_functions.txt`, `locals_optout.txt`, `lr_write_same_choice.json`, `r1_local_choice.json`.
- Hot code: `app/src/cpu/hot/{hand.txt,localized.txt,localized_bulk.txt}`, Katamari natives in
  `banjo_hand_natives.h`, `katamari_hand_*` keys, `katamari_stage_diag.cpp`, `katamari_keys.cpp` stubs.
- Unbuilt banjo-nx / nfsmw leftovers (`app/src/me`, `app/src/native`, `frameHooks.cpp`, `townsfolk.cpp`, ...): deleted,
  because `switch_hot_bulk.py` treats every identifier under `app/src` as a hook and they would block copies.
- Renderer hook addresses and device offsets (`d3d/hooks/*`, `banjo_renut_select.*`, `renut_hook_map_katamari.txt`,
  XDK-2.0.5632 constants such as VdGlobalDevice 0x82000750, device size 0x5700).
- `pgo/` (all `.gcda`), icons (`tools/switch/icon/katamari_icon.jpg`, `app/icon/*`), NACP block
  (name/author/version), `REX_SWITCH_FALLBACK_EXE`, forwarder NPDM/JSON (program id), `dist/switch/*`.
- Settings: `katamari_switch*.toml`, `katamari_builtin_config.h.in`, `katamari.toml` naming; DLC key
  `katamari_dlc_unlock_packs` (Tekken 6 has no DLC; the generic installer code stays unused).
- Katamari shader tooling (`katamari_files.py`, `katamari_shader_files.py`, N&B hash lists).

Naming: keys move from `banjo_*`/`katamari_*` to `tekken6_*` where the code is app-side; SDK keys keep their names.
The NACP author is `xsaja`, set once in `app/CMakeLists.txt` (`rexglue_switch_add_nro(... AUTHOR "xsaja")`), and
every package step reads the NACP back from the NRO to check it.

## 4. Phase outline

| Phase | Goal | Exit check |
|---|---|---|
| 1 | Extract ISO to `../game` (tekken6-recomp xdvdfs, built on the host), import skeleton, strip Katamari data, port Tekken config + SDK fixes, codegen + post-codegen | generated code compiles for PC |
| 2 | PC boot to menu and a fight with reNut (Vulkan back end) | fight playable, pad and keyboard checked |
| 3 | deko3d-only renderer for Switch (no NVK), censuses from porting-guide-renderer §2 first | NRO draws title in Ryujinx |
| 4 | Build the Ryujinx fork; automated runs with own data folder (`../ryu-data`), captures every 15 s | no visual difference vs PC |
| 5 | ImGui overlay (L3+R3, off by default), SaltyNX block, `profile_stacks.flag` sampler | zero cost when off |
| 6 | Enhancements with keys and A/B, excluding §6b | 0 DIFFERENCE |
| 7 | Machine copies (`--merge`), hand natives only from console profiles | 0 DIFFERENCE, compared counts > 0 |
| 8 | PGO + LTO, after the last copy batch and hand natives | optimised build boots |
| 9 | Console package | NACP author = xsaja |

Section-2 censuses to take in phase 3 for Tekken 6: draw origin (recorded command buffers vs direct calls), resource
origin (D3D creation vs game-built), state setting (hooked setters vs inline writes), memexport use, shadow passes,
thread load, gamma ramp, MSAA/tessellation/point sprites, where the shaders live (XEX vs `DATA*.BIN` archives), heap
and texture memory, internal resolution source (tekken6-recomp already hooks the 1024x576 stores).

## 5. Profiler and overlay design (phase 5, decided now)

- Use katamari-nx's English `switch_perf.cpp` (same sampler as nfsmw-nx, already producing the format the
  `switch_hot_*.py` tools parse: `==== N s | mode: ...`, `-- thread "name": CPU N%`, `pc image+0x... xN`,
  `stack image+0x... <- ...`). Rename the flag file to `profile_stacks.flag` in `<NRO folder>/logs/rex/`, keep the
  `switch_profile_stacks` key, announce `Stack sampling: enabled (1 ms)` in the log, keep the >45 ms hitch section.
- nfsmw-nx has no ImGui metrics HUD; the overlay is new code on the SDK's ImGui/deko3d path, fed by the same
  counters the sampler already reads (`svcGetInfo` thread ticks, clocks). GPU load from deko3d timestamps.

## 6. Risks

| # | Risk | Mitigation |
|---|---|---|
| R2 | Two SDK lines (katamari fork of v0.10.0 vs patched nightly-20260904): codegen schema and runtime may diverge | dry diff of codegen trees in phase 1; fallback plan in section 3 |
| R3 | reNut needs D3D hook addresses and device offsets for XDK 2.0.8276; none exist yet for Tekken | sigmatch tools from katamari-nx; Banjo's 2.0.7645 layout as starting point |
| R4 | Tekken renders through paths tekken6-recomp only solved for the Xenos emulator (unclipped draw VS on CPU) | census in phase 2; if reNut hits the same draws, port the CPU path |
| R5 | 60 fps fighter on an A57: frame budget 16.7 ms on four cores; Banjo targets are 30 fps | measure early (phase 4 rough timing, phase 5 console profile) |
| R6 | DATA archive bridge writes decoded `.dec` streams: SD card space and I/O | measure size in phase 1, decide cache location |
| R7 | Host RAM: 15.7 GB total, ~5.9 GB free at inventory; Ryujinx runs need >= 8 GB free | close other programs before emulator runs |
| R8 | Code size: generated code for a 9 MB XEX plus copies; NRO and heap limits | watch NRO size per batch (~11 KB per copy) |
| R9 | Shaders must come from own files; release NRO must not contain game shaders | installer-style `deko3d_glsl.bin` built locally from `../game` |

## 7. Decisions needed

1. **SPIRV-Cross source:** katamari-nx's `tools/deko3d_build_tools.sh` builds SPIRV-Cross from source; needs either the
   Vulkan SDK's copy or one more git clone (KhronosGroup/SPIRV-Cross).

## 8. What must be installed or supplied

| Item | Why | Status |
|---|---|---|
| devkitPro (devkitA64, libnx, deko3d, switch-tools: nacptool, elf2nro; `aarch64-none-elf-addr2line`), MSYS2 clang64 | Switch build, host uam/SPIRV-Cross tools, address translation | missing |
| Vulkan SDK (dxc, spirv tools) | shader chain, PC Vulkan validation | missing |
| .NET SDK 10.0.301 or newer (10.0 band) | Ryujinx fork (`global.json`) | missing (runtime only) |
| Python packages `xxhash`, `Pillow` | katamari-nx tools | missing |
| Standalone Ninja on PATH | optional: VS 18 bundles Ninja 1.13.2 | present via VS |
| Clang | VS 18 bundles clang 22.1.3 + MSVC 14.51 + Windows SDK 10.0.26100 | present |
| prod.keys, title.keys, firmware dumped from the console | Ryujinx (phase 4) | later |
