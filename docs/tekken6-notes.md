# Tekken 6 specific notes

Every Tekken 6 specific fix and its technical reason. Addresses are guest addresses in `Default.xex`
(SHA-256 `ee138a4f...a7f15c`, Europe/USA Rev 1).

## Game facts

- Title ID `4E4D07FC`, media ID `7E3BCAE2`, XGD2 disc, XDK 2.0.8276 (all static libraries).
- No title update and no DLC exist for this revision on the disc used.
- Data lives in `DATA301..320.BIN` with `.IDX` indexes and `DINF_300.BIN`; the runtime needs the
  `XMemDecompressSegmentTD` sparse-address bridge from tekken6-recomp's SDK patch (`system/xfile.cpp`) to read them.

## Phase 1 fixes (extraction, skeleton, codegen)

| Change | Where | Technical reason |
|---|---|---|
| `[[switch_tables]] selector_mask` support | `sdk/src/codegen/config.cpp`, `sdk/include/rex/codegen/function_types.h`, `sdk/src/codegen/builders/control_flow.cpp` | two descriptor dispatchers (0x8256D5E0, 0x8256CBF0) index their table by the low nibble of r7; the emitted switch must mask the index (`switch (r7 & 0xF)`) |
| a jump table whose entries reach an earlier function is rejected as a whole | `sdk/src/codegen/function_scanner.cpp` (`detectJumpTable`) | keeping the in-function prefix turned the remaining selectors into a default trap; such a `bctr` dispatches through CTR instead (tekken6-recomp SDK fix) |
| per-function `non_volatile_as_local` dropped from three vector helpers (0x82513308, 0x82513BC0, 0x82513BE0) | `app/overrides.toml` | optimisation only; the skeleton's SDK has no per-function form; revisit with the register-diet audit |
| all register-localisation codegen keys off | `app/renut_config.toml` | the address data was validated with these off; each is turned on later after `read_before_write.py` / `non_argument_audit.py` on this XEX |
| FFmpeg 4.4 generated headers added (`config.h`, `libavutil/avconfig.h`, `libavutil/ffversion.h`, `libavcodec/*_list.c`, `libavformat/*_list.c`) | `sdk/thirdparty/FFmpeg` | FFmpeg's own `.gitignore` kept them out of the skeleton's repository; the SDK's `libavutil` does not compile without them. Copied from the upstream SDK's FFmpeg checkout (same 4.4 tree) and force-added |
| `tools/pc_env.bat` finds Visual Studio with vswhere and uses its bundled clang | `tools/pc_env.bat`, `cfg_host.bat`, `build_host.bat` | no machine-specific paths in versioned files; every path quoted (the Visual Studio folder contains spaces) |
| `tools/codegen.ps1` resolves `-Log` before changing directory | `tools/codegen.ps1` | a relative log path was resolved inside `app/` |
| `tools/post_codegen.py` and `tools/extract_iso.py` read the game from `../game` | tools | game data lives outside the repository; `post_codegen.py` takes the XEX from the manifest (`game_xex()`) so `ro_constants.py` gets `--xex` and keeps the 478 import-record loads as loads (without it they were folded as constants) |
| `localize_native.py` creates `app/generated/hot` | `tools/localize_native.py` | a fresh codegen has no such folder |
| Katamari hot lists emptied (`hand.txt`, `localized.txt`, `localized_bulk.txt`) | `app/src/cpu/hot` | their addresses are Katamari's |

Pending before the first copy batch (phase 7): `banjo_hand_natives.h` and `banjo_hot_guest.cpp` still define natives
named after Banjo/Katamari addresses (`n_82C111C0`, `n_82759C98`, ... and the `katamari_hand_*` keys). They are not
bound to any Tekken address (empty `hand.txt`), but `switch_hot_bulk.py` treats every address named in `app/src` as a
hook and would refuse copies of Tekken functions at those addresses. Remove them (the generic memcpy/memset natives
get re-bound to Tekken's 0x826A1130 / 0x826A03B0 only through the console-profile gate).

Codegen result (Default.xex Rev 1): 25,234 functions, 104 source shards, 109 MB of C++, 17.5 s. One warning:
`sub_826FBDA0` is 1.36 MB of C++ (over `max_file_size_bytes`); it is a real VMX-heavy function (2,496-byte frame,
vector constant setup), not a boundary error. 18 null-entry switch cases emit traps (the tables' own null entries).

Switch toolchain note: devkitPro packages `switch-zlib` and `switch-libexpat` but no `switch-zstd`; the SDK's
non-NVK Switch link line names `zstd`, so it is built from `app/thirdparty/reblue-XenosRecomp/thirdparty/zstd`
in phase 3.

## Known requirements inherited from tekken6-recomp (to be verified in this port)

| Item | Reason |
|---|---|
| midasm hook 0x8251C1FC (startup spin yield) | the startup queue spins without yielding and starves other threads |
| midasm hooks 0x82219DB4 / 0x82219A28 | internal 3D resolution is stored as 1024x576 here; hook allows lower sizes |
| midasm hook 0x821AB410 | 1024x1024 offscreen target (resource id 0x20000004) |
| 364 vtable tail-call thunks | codegen does not emit these targets as functions |
| `null_device_sectors_per_allocation_unit = 0x40` | Cache1 bootstrap requires 0x8000 bytes per allocation unit |
| `\Device\Image`, `\Device\Harddisk0\Partition0` aliased to Partition1 | the game opens these device paths |
| `execute_unclipped_draw_vs_on_cpu=true` (Xenos plugin only) | Practice mode and fights do not render without it on the Xenos emulator path |

## Phase 2 fixes (PC build and boot)

| Change | Where | Technical reason |
|---|---|---|
| kept six small files from katamari-nx's `app/src/me` and `app/src/native` (`me_verify.h`, `me_platform_compat.h`, `masseffect/banjo_pmull_fingerprint.h`, `me_pc_stubs.cpp`, `nfsmw_nativo_captura.{h,cpp}`); the PM4 native renderer (`me_native_system`, EDRAM shaders) stays out of the build | `app/src`, `app/CMakeLists.txt`, `renut_app.h`, `renut_engine/hooks.cpp` | phase 1 deleted both folders whole, but the verify counters, the Switch thread helpers, the PMULL fingerprint and the PNG capture (automated runs) are included by kept code. The D3D ring-space wait now yields instead of `me::native::WaitRingProgress` |
| `icon/app.rc` dropped from the PC sources; NRO NAME "Tekken 6", AUTHOR "xsaja", icon `tools/switch/icon/tekken6_icon.jpg` (optional, not present yet) | `app/CMakeLists.txt` | Katamari's icons were removed in phase 1 |
| 364 vtable tail-call thunks, as a table | `app/src/tekken6_vtable_thunks.inc`, `app/src/tekken6_guest.cpp` | codegen does not emit these four-instruction thunks as functions; registered in `OnPostLoadXexImage` with `FunctionDispatcher::SetFunction` before guest startup |
| mid-asm hook bodies `Tekken6StartupWait`, `ApplyTekkenInternalResolution`, `ApplyTekkenOffscreenResolution` | `app/src/tekken6_guest.cpp` | declared by the generated code (`app/overrides.toml`); same env keys as tekken6-recomp (`TEKKEN6_*`) |
| `null_device_sectors_per_allocation_unit = 0x40`; `\Device\Image` and `\Device\Harddisk0\Partition0` linked to Partition1; Partition1 root-sector probe on | `app/src/renut_app.h` | Cache1 bootstrap geometry check (0x8000 bytes per allocation unit); the storage bootstrap opens those device names and reads the "Josh" sector at 0x800 of the volume root |
| SDK hunks from tekken6-recomp's patch that applied as they are: null-device geometry (`RuntimeConfig`, `runtime.cpp`, `null_device.*`), storage-probe IOCTL (`xboxkrnl_io*.cpp`), `XMemDecompressSegmentTD` DATA bridge (`system/xfile.*`), child-thread CPU inheritance and wait diagnostics (`xthread.*`, `xboxkrnl_threading.cpp`), `xam_user.cpp`, `function_dispatcher.cpp`, `thread_state.cpp`, `context.h` (`last_indirect_lr`), unhandled-AV context dump (`xmemory.cpp`) | `sdk/` | platform-neutral runtime fixes the game needs to read its data |
| SDK hunks ported by hand onto the fork's versions: root-sector probe (`HostPathDevice::EnableRootSectorProbe`, `HostPathEntry::Open`, `HostPathFile` virtual Josh sector next to the fork's read windows), `WaitHandle::debug_state`, `KernelState::DumpKernelObjects`, `XObject` wait scopes | `sdk/include/rex/filesystem/devices/host_path_*`, `sdk/src/filesystem/devices/host_path_*`, `thread.h`, `kernel_state.cpp`, `xobject.cpp` | the fork changed these files (CRLF and read caches), so the hunks did not apply |
| `rexcrt_memcpy` / `rexcrt_memset` take guest addresses from r3/r4/r5 (tekken6-recomp) and still mark store cards (`MarkStoreCardsHost`) | `sdk/src/kernel/crt/memory.cpp` | the raw hooks keep the physical-heap offset on Windows; the store-card marking is the fork's post-codegen contract |
| not ported: `threading_posix.cpp` wait/multi-wait rework and `pacing_log.h` / renderer / D3D12 / macOS hunks; the `last_indirect_lr` codegen template; the mmio fault probe | — | `threading_posix.cpp` only builds for POSIX/Switch and the fork rewrote it (Horizon layer): ported in phase 3; the rest is Xenos-plugin or debug only |
| `tools/run_pc.ps1` reads the game from `../game` | tools | it pointed at Katamari's folder |
| `tools/run_pc.ps1` quotes every argument | tools | `Start-Process` joins the list with spaces and the project path has one ("Tekken 6"): CLI11 rejected the split arguments and the game exited with code 1 and no log |
| `XamShowMessageBoxUI` waits for a click on the SDK's ImGui dialog | runtime (no change) | after Start the game asks through a message box (no save data); a minimised automated run never answers it and stays on "Accessing storage device". Automated runs pass `--headless=true` (focused button chosen) |
| `--execute_unclipped_draw_vs_on_cpu=true` needed on the emulated renderer | runtime flag | fights draw black without it (HUD only), as in tekken6-recomp |
| `cache:` not mounted, `XFileXctdCompressionInformation` unimplemented | runtime (no change) | logged errors only; the game boots, loads menus and fights as in tekken6-recomp, which registers the same NullDevices |
| `mnk_controls` (app keyboard layer) left off | `app/src/renut_engine/mnk_controls.cpp` | it writes into Nuts & Bolts' movement input; keyboard play uses the SDK's `--mnk_mode=true` instead |

Automated PC test input (title to an Arcade fight, emulated renderer):
`--headless=true --execute_unclipped_draw_vs_on_cpu=true --input_script=15:START,22:A,27:DOWN,29:A,33:A,37:A,41:A,45:A,49:A,53:A,60:X,62:X,... --input_script_sticks=60`

reNut anchors found so far (XDK 2.0.8276, from the kernel imports): Swap `sub_824DA2E0` (only VdSwap caller),
SetRingBufferParameters `sub_824D5060` (VdInitializeRingBuffer), CDevice_Uninitialize `sub_824E2F80`
(VdShutdownEngines + VdSetGraphicsInterruptCallback), engine init `sub_824E2B90` (VdInitializeEngines).

## Renut's device layout (2026-11-05)

Tekken 6's XDK 2.0.8276 Direct3D device is N&B title update 3's layout (XDK 2.0.7978), not Katamari's (2.0.5632):
Direct3D_CreateDevice `sub_824D99F0` allocates 24064 = 0x5E00 bytes and stores BehaviorFlags at +23980, as N&B. Counted
in Tekken's D3D code (0x824C0000..0x82500000), 24 of 25 fields of katamari-nx's zone table appear at N&B's offset and
none at Katamari's (VertexDeclaration 11824, IndexBuffer 12444, targets 12448, tiling 12736, PresentInterval 13596,
CommandBuffer 13516, auto surfaces 14824, SwapCount 16560, the display mode 21544..21556, Swap 21532 / 21624..21632,
gamma 23952 / 23956, BehaviorFlags 23980). The exception to check: VerticesRingEnd, N&B 13444 not seen (Katamari's
13428 is used twice).

`tools/renut_device_offsets_tekken.py` puts the 152 offset lines katamari-nx tagged back to N&B's values (the device
size back to 0x5E00). Guest data still at Katamari's addresses (8 sites): the XDK build qword is Tekken's `0x827D6FD0`
(2.0.8276.0, `0002000020544000`); the format table, VdGlobalDevice's import record, D3DCAPS9, the full viewport, the
XPhysicalAlloc no-large-pages flag and the HiZ owner are still to find. GetDeviceCaps's 304-byte copy does not exist
in Tekken's D3D range (probably not linked: the hook would stay unregistered).

Hook map: `tools/renut_hook_map_tekken.txt` (21 of 101 so far, each with its evidence: device creation, present, swap,
resolve, kick-off, ring parameters, render targets, viewport, scissor, gamma, CreateTexture / CreateSurface).

Census note: few game call sites reach the D3D functions (the most called have 10-11 callers). Tekken draws through
its own layer and, it seems, the XDK's inline forms that write the device and the ring directly (the ring reserve
`sub_824D55A8`, CommandBuffer +13516..13528, is called by the small packet writers). This decides how much Renut's
function hooks can see; the census of porting-guide-renderer section 2 has to settle it before more hooks.

### Inline census (2026-11-05, static, a static pass over the generated code)

- Tekken draws through D3D calls: no game code writes the ring outside the D3D draw functions, and nothing calls through
  the device's state-call tables. The two draw functions are outside the 0x824C0000..0x82500000 range:
  `sub_826B6828` (DrawVertices-like) and `sub_826B6C18` (DrawIndexedVertices: IndexBuffer +12444, ring +48 / limit +56,
  KickOff when full, `li r5,8704` = DRAW_INDX). The ring cursor is `m_pRing` +48 (not +10908, an earlier mistake).
- The game reaches the device through the object at 0x82C66180 (+4 holds it, written by `sub_82522C48`); about 147
  functions hold it, 16 touch device fields.
- Inline writes (XDK inline forms) in 14 functions: fetch-constant bitfields at +1152 + 24*i (sampler/texture edits)
  with the dirty mask +24, ALU constants at +1920 + 16*n with masks +0 / +8. These are Renut's `Device::m_Pending` and
  `m_Constants` image (d3d.h), read at draw time, so they stay visible to a renderer that hooks the two draws.
- 81 D3D functions receive the device from game code; the most called: `824E40D8` (15), CullMode `824D0350` (13),
  the packet writers `824E3D48` / `824D2710` / `824DFCD8` / `824D28B8` / `824E3F08`, alpha-test and stencil setters.

### First reNut runs (2026-11-05)

| Problem | Fix | Where |
|---|---|---|
| Hooks moved to Tekken's addresses but never called: post-codegen had turned the calls into direct `__imp__sub_...` calls while the hooks were still at Katamari's addresses | fresh codegen + `tools/post_codegen.py` (direct_calls.py excludes hooked functions it finds in the app sources); 12 hooked functions checked, 0 direct call sites left | app/generated |
| The app's path wizard (an ImGui dialog, created only under reNut) waited for a click before the game started | skipped when `--game_data_root` is given and exists | app/src/renut_app.h OnFinalizePaths |
| `GuestGetCurrentThreadId` imported Katamari's `sub_824C1C10` | Tekken's `sub_826B5528` (`lwz r11,256(r13); lwz r3,332(r11)`) | hooks/device.cpp:80 |
| The device snapshot's last chunk is 48 bytes in N&B's layout (Katamari: 32) | static_assert 48, three NEON loads | guest/command_buffers.cpp TakeLastChunk |
| `widescreen.cpp` (built) registered 11 hooks at N&B game functions | all `BANJO_RENUT_UNHOOKED_HOOK_RAW` | gpu/guest/widescreen.cpp |
| XenosRecomp ignored Tekken's captured shaders: its container scan requires field1C == 0 && field20 == 0, Tekken's VS containers have field20 = 0xC0 | a file that is exactly one container at offset 0 (a run-time capture) is taken whatever those fields hold | app/thirdparty/reblue-XenosRecomp/XenosRecomp/main.cpp:138 |
| The frame loop stalled after the first Swap: Tekken sets a vertical blank callback (`sub_82522C38`, via D3DDevice_SetVerticalBlankCallback `sub_824D9EA8`, device +0x40A0) that signals its frame pipeline; the original device init registers the interrupt handler (`sub_824E2B90` -> `sub_824D3AE0` -> `sub_824D9B38`), which reNut's CreateDevice hook replaces; N&B sets no such callback | a 60 Hz "reNut VBlank" guest thread: +0x40A4 += 1, then the callback with D3DVBLANKDATA {VBlank, Swap (+0x40B0), 0} | hooks/device.cpp StartVBlankThread |
| Crash ~3.5 s in: C0000005 in the unhooked `sub_824DCE60` (chain sub_825303E8 <- sub_82532D30): it is **D3DVertexShader_Bind** in XDK 2.0.8276's five-argument form (shader, Flags, declaration, strides, pixel shader); it reads the stream count at declaration +28, but the declarations are reNut's records (CreateVertexDeclaration hook), which lack the XDK fields | `D3DVertexShader_Bind_tekken_hook` (drops Flags and the pixel shader, forwards to reNut's N&B handler) hooked at `sub_824DCE60`; `XGRegisterPixelShader_hook` hooked at `sub_824E3D10` (the game registers its pixel shaders itself from `sub_82532A88`); `tools/direct_calls.py --redo` so their callers go through the hooks | hooks/shader.cpp:659-676 |
| "Intro movie black under reNut": a test artefact. The emulated renderer is black at 3-15 s too with the same input script (START at 15 s skips the attract movie) | confirmed: with no input the attract movie plays under reNut from ~20 s (the emulated renderer ~30 s), same shots, colours and overlay (`renut_movie`) | |
| Frame rate fell over a run (58 -> 9 fps) and fights showed only the HUD: reNut ignored the primitive-reset index (0xFFFF / 0xFFFFFFFF) of Tekken's indexed triangle and line strips. The cut counted in each draw's lo..hi range, so every cut strip converted 65,536 vertices (4.4-15 GB of vertex conversion per 5 s; render thread 99.9 % busy in ConvertVertices); after rebasing, a 16-bit cut became a real index (garbage triangles); 32-bit cut strips failed the vertex-count check and were dropped | `MarkStripCuts`: for strips whose hi is the reset value, cuts are marked, lo/hi recomputed without them, and written as the host's 0xFFFF / 0xFFFFFFFF (plume's strip pipelines restart there), in the rebased and the cached index copies | render/draw.cpp ~3700-4014 |

reNut fight (run `renut_cut2`, 150 s): menu with its city background, Arcade intro, Lars vs Bob with full stage, crowd,
HUD, K.O., replay and round 2; 58-60 fps throughout, render thread 4.6-4.8 ms and GPU ~3.7 ms a frame, ~980 draws a
frame. Not compared frame by frame with the emulated renderer yet (random opponent and stage); to check: a strong white
glow around Lars seen on the emulated run, heavy blur on the opponent at 60 s (likely the game's motion blur).
Intermittent early exit under reNut (3 of about 9 launches died 1-4 s after start, in `ImGui::Begin` from
FpsOverlayDialog::OnDraw): fixed. The app's dialogs are SDK ImGuiDialogs but call the ImGui built into renut.exe, whose
only context is the overlay's (`overlay::Start`, gpu/overlay/overlay_ui.cpp). `StartRenderer` starts two pumps (the
overlay's frame pump and the SDK-menus pump); when the SDK pump queued first, `Begin` ran in a context that never had a
`NewFrame` (no font: read of 0x8). Forced with a 300 ms delay on the overlay pump: 3/3 crashes; with the fix 0/3.
Fix: `overlay::Start` builds one overlay frame on the UI thread before either pump starts (overlay_ui.cpp:483). After
it: 0 failures in 20 launches (with and without captures). Left for phase 5: those dialogs draw between the overlay's
frames, so the FPS overlay is probably not visible under reNut; they should be drawn inside the overlay frame.

First reNut images (run `renut_bind3`, 60 s, after two rebuilds that compiled ~1,400 captured shaders): battle-position dialog,
main menu, Offline Mode submenu with the 3D character, Arcade intro card, versus loading, STAGE 1 loading; no crash, no
shader or draw warnings. The title-owned front buffer (Swap to 0x829F4D1C, a header in .data) is captured fine.

Movies (static reading, 2026-11-05): the XDK's XMedia2 XMV player, WMV9/VC-1 decoded on the CPU (no Bink / CRI).
Tekken's wrapper `tcb_device_movie_t`: open `sub_82341C88` -> XMedia2CreateXmvPlayer `sub_82584490`; per frame
`sub_823418C0` -> player +0x48 `sub_82591648` -> +0x4C `sub_82593A18`, which copies the Y/U/V planes with rexcrt memcpy
into the game's linear textures (resources 0x20000401..406, double-buffered), drawn by the game's own sprite layer with
shader 0x1F0C0000. All D3D calls on the path are hooked; the planes reach reNut as CPU-written linear textures (the
"dynamic" case of `RefreshLinearLocked`, render/textures.cpp), with store cards marked by the rexcrt memcpy hook.

### Open (reported in manual tests, manual pad test 2026-11-05)

- Main menu: "NOW LOADING" stays a long time (black background, no city, no character) before the background and Lars
  appear in the Offline Mode menu; on the console this takes almost no time. Unmeasured: compare with the emulated
  renderer and look at the I/O (the log's `[io] SLOW` reads of DATA3xx.BIN) and at reNut's first-use costs (shader
  pipeline creation on first draw, texture uploads).
- Arcade character select: grey screen with no character models, frame for frame the same under the emulated renderer
  (runs cs_emu / cs_renut, parked 27 s on the screen): not a reNut bug; open (runtime or emulation side).
- Fighters invisible in most fights (stage, HUD and effects drawn): their pixel shaders were not in reNut's
  build-time cache. They are NOT compiled at run time (the image has no HLSL compiler; "2.0.6534.1" is just the offline
  compiler's version string): every stage has its own set, stored whole in DATA302.BIN (one NSXR package per stage,
  22 of them, plus DATA303 entries 80 / 2894 and DATA304 entry 4): 5,838 unique containers (5,837 ps), which cover
  every captured pixel shader but 4. Fix: a dump tool (below).

### Shader dump plan (static reading, 2026-11-05; the scripts are not in the repo)

- DATA301..313.BIN are XCTD files (magic 0x0FF512ED; the game's XDK code decodes them, sub_824C8BF8 / sub_824CC358):
  16-byte header, block size 0x8000 << ((flags >> 4) & 3) = 0x20000, then a big-endian u32 table of decoded sizes
  (one per raw block); each block is a stream of XMem LZX frames (u16 compressed size, or FF + u16 decoded + u16
  compressed), decodable on its own with libmspack lzxd (sdk/thirdparty/libmspack), 17-bit window. DATA314 is a raw
  PAC, DATA315..320 raw ASF/WMV videos.
- .IDX: big-endian (u32 offset in 0x8000 units, u32 size) per entry in the decoded space (0 / 0xFFFFFFFF = empty).
  Entries are packages (u32 count, then (offset, size) pairs) of NTXR textures, NDXR models, BONE, PAC sound, XML, NSXR.
- NSXR: "NSXR", u32 size, u16 version 2, u16 count, then shader containers at any byte offset.
- Vertex shaders: one global NSXR in DATA301 entry 0, compressed a second time by a Namco LZSS-like coder not
  identified yet (~668 shaders; ~603 already captured). Ways: find the guest decoder (trace the first read of DATA301
  entry 0) and port it, or a one-time memory dump of the decoded NSXR.
- The executable holds 70 unique containers (NSXR at 0x827FE8B0..0x82801DC0).
- Tool to write: tools/dump_tekken_shaders.py (+ a small host LZX decoder over libmspack), writing the capture format
  (`{vs|ps}_<xxh3_64>.bin`) into the dump folder; validate against every captured hash.

### Shader dump: how to run it (offline, resumable)

1. `python tools\dump_tekken_shaders.py` (repo root). Builds out\xctd.exe on first use (tools\build_xctd.bat, Visual
   Studio's clang + the SDK's libmspack), decodes DATA301..313 one bank at a time into out\shader_dump_work, writes
   every container to out\renut_shader_dump as `{vs|ps}_<xxh3>.bin`, deletes the decoded bank and marks it
   `DATA3xx.done`. After a power cut, run it again: finished banks are skipped. First run (2026-11-05): 13 banks, no bad
   block, about 2.5 minutes, 6,511 containers (669 vs, 5,842 ps), all 3,297 captures covered.
2. Vertex shaders still compressed (DATA301 entry 0): start the game once with `--tekken6_dump_vs_nsxr=true` (any
   renderer that hooks CreateVertexShader, i.e. reNut). The first vertex shader created saves the decoded package as
   `app\out\pc\vs_nsxr_<address>.bin`; then `python tools\dump_tekken_shaders.py --vs-dump app\out\pc\vs_nsxr_*.bin`.
3. `tools\build_pc.bat` compiles the dump into reNut's shader cache. XenosRecomp now leaves shaders DXC rejects out of
   the cache with a warning instead of failing the build (10 in the full dump: "coord uninitialized").

Result with the full dump (run `dumpcheck2`, 2026-11-05): no crash, fighters drawn (Kazuya-like vs Wang), stage black:
100 vertex shaders missing ("not in the shader cache", all vs, captured), i.e. the stage's vertex shaders from the
still-compressed DATA301 package. `--tekken6_dump_vs_nsxr=true` saved nothing: no "NSXR" header within readable
memory behind the vertex shaders the game creates (likely each container is copied to its own allocation first).
Open: find where the game decodes DATA301 entry 0 (or its copy step) to dump the package, or keep feeding captures.
The first version of the walk crashed (it left the image at 0x82000000); it now skips image containers and checks every
page with Memory::LookupHeap / QueryProtect.

`cache:` tried and reverted (2026-11-05): it is the XDK file cache (`cache:\$cache$\dir` + one copy per cached disc
file: DINF_300.BIN, DATA301..313.IDX), not a store of portraits or thumbnails; without it the game reads the same
files from `game:`. Mounting a writable host folder as `cache:` made it write `$cache$\dir` and DINF_300.BIN's copy,
then stall startup on the attract screens (START ignored; probably an async copy, or the stubbed IoDismountVolume /
ObReferenceObject). Not the cause of the grey character select / empty stage thumbnails. If revisited: use a folder
other than out\pc_user\cache (the Xenos plugin's shader cache lives there). Next suspect for the missing portraits and
thumbnails: `NtQueryInformationFile(XFileXctdCompressionInformation)`, unimplemented, queried on every DATA3xx.BIN open.

### Missing 2D images: our runtime vs tekken6-recomp (2026-11-05)

tekken6-recomp 0.9.4 (S:\T6recomp; its launcher forces the D3D12 ROV path, which hangs the GTX 1080 Ti with
DEVICE_HUNG and exits with code 3; `S:\T6recomp\jugar_recomp_rtv.bat` starts it with `--render_target_path_d3d12=rtv`)
shows, from the same game files, what our build lacks under both renderers: the boot logos (Bandai Namco, Namco, Dolby
Digital), the Kazuya artwork behind the battle-position dialog, the character portraits of the select screens, the
stage thumbnails. So it is our runtime (the katamari-nx SDK fork, the post-codegen steps, or the app), not reNut.
Ruled out with 22 s emulated boot runs (logos expected at 4-14 s, our captures black): the fork's file read caches
(`--masseffect_io_window_kb=0 --masseffect_io_ranges_mb=0`), the fork's enhancements
(`--banjo_cpu_enhancements=false --banjo_noncpu_enhancements=false`: the attract movie starts earlier, still no logos).
Same kernel errors in both logs (cache:, XctdCompressionInformation, \Device\Image, the same stubs). Next: differential
bisect, e.g. our generated code without post-codegen, or tekken6-recomp's SDK hunks we did not port (rex_app.cpp,
mmio, threading_posix is POSIX-only), or log file opens in both at debug level and compare.

Resolved (2026-11-06, manual test of the PC build): boot logos, the Kazuya artwork, the character
select portraits and the stage thumbnails all show under reNut, and the fighters draw on every Practice stage tried.
No code changed since the full shader dump (commit 89b0f365, rebuild 2026-11-05 10:36): those 2D screens were missing
pixel shaders in reNut's cache. The "same under the emulated renderer" finding came from automated runs: run_pc.ps1
starts the game minimised (unless -Visible) and the tests passed --headless=true; the bisect agent's runs
(logo_base, logo_audio: muted vs unmuted) showed the same black frames, so automated captures are not a reliable test
of the 2D screens. Judge those screens on a visible, interactive run.

### Phase 2 status and open visual issues (2026-11-06, manual tests)

Phase 2 is closed for the project's goal (the Switch): under reNut on PC the game runs from the boot logos through the
select screens to fights on every Practice stage tried, with the pad. Keyboard play is out of scope (the target is the
Switch). Open visual issues, for later:
- a wall texture flickered once, very briefly, on one stage;
- Lili's dress is far too shiny on some stages (specular / environment term, or a texture/sampler mismatch).
Next: phase 3 (Switch build with the deko3d renderer, NRO, title screen in Ryujinx), after the weekly usage reset.

## Codegen options: the register diet (2026-11-19, phase B)

`non_volatile_as_local`, `non_volatile_as_local_keep = ["v64-v127"]`, `elide_lr_stores` and `non_argument_as_local`
(with an 80-function `non_argument_as_local_keep`) are on in `app/renut_config.toml`, with `setjmp_address =
0x826C8510` / `longjmp_address = 0x826C81F0` (verified by shape with `tools/ppc_dis.py`). Phase A is unchanged.
Audited on codegens made only for it, outside `app/generated` (temporary configs with absolute `includes`,
`file_path` and `out_directory_path`; `rexglue.exe codegen <manifest>` from `app/`, about 25 s each, no
post_codegen):

- `out/audit/gen_na` (phase A + setjmp / longjmp): `python tools/non_argument_audit.py out/audit/gen_na
  --noreturn=826C81F0`: ENTRY 295, RET 20, HANDOVER 0. ENTRY: 218 save / restore helpers (kept by name), 46 MSVC
  unwind funclets (`addi r31,r12,-N`), the stack probe sub_826A2614 (r12), sub_824F5654 / sub_826B4684 (r11), and
  the movie codec family that saves / restores v32-v63 through sub_8270B528 / sub_8270B870. RET: 17 of that family,
  sub_822109C0 / sub_82212108 (`subfe r10,r11,r11`), sub_826B2648. All 80 non-helper ENTRY / RET functions are kept.
- `out/audit/gen_nv` (phase A + `non_volatile_as_local` + keep + `elide_lr_stores`): `python
  tools/read_before_write.py out/audit/gen_nv --noreturn=826C81F0`: 8 functions, none a split piece or funclet (each
  has its own `mflr r12; bl __savegprlr_N` prologue): setjmp (CR into the jump buffer, codegen-handled, no direct
  caller), sub_824BF420 / sub_824BF7B0 (`xor r30,r30,r30`), and five vector pixel filters next to the movie decoder
  whose flagged reads sit on paths their own flags exclude (sub_826F9DE8 / sub_826FBDA0 / sub_82651878: rounding
  vectors set and read under the same r7 / r9 tests, r7 / r9 never written in between; sub_82618328: loop-carried
  edge flags; sub_826513E8: a v24 lane result unused when r8 != 0). So 0 `share_registers` marks (a wrong mark is
  itself harmful: read_before_write.py's docstring).
- `elide_lr_stores`: a scan of every `mflr` in `out/renut_image.bin` (14,824): all feed a prologue save except three
  C runtime helpers that save LR in r31 around one call and restore it (sub_826A61E0, sub_826B468C, sub_826B46B0),
  katamari-nx's sub_827F91DC shape. longjmp has 3 direct callers (0x826C5574 / 0x826C5598 / 0x826C55BC).
- `out/audit/gen_all` (the final config): `non_argument_audit.py --with-option`: 21,784 of 25,234 functions localize
  the registers, HANDOVER 0 (its ENTRY / RET are all kept functions); `read_before_write.py`: the same 8;
  `read_before_write.py --volatile`: 0. 0 `ctx.lr =` stores left; `__savegprlr` / `__restgprlr` references 19,158 -> 36.
- Hooks: the four mid-asm hooks (`config/renut_hooks.toml`) take their registers by reference and the codegen passes
  the local (`Tekken6StartupWait(r11)`, `ApplyTekkenInternalResolution(r11, ctx.r10)`); their bodies use only those
  arguments. No built REX_HOOK_RAW or native in `app/src` reads a non-volatile or non-argument register from ctx
  (tekken_skin_native.cpp only writes r9-r11 for its callee; tekken_diag_2d.cpp, banjo_xaudio_native.cpp,
  banjo_pool_lock.cpp, townsfolk.cpp, which do, are not in the build). `hot/hand.txt` is empty.

Open risks for the console test:
- the intro movies: the same codec family that broke katamari-nx's movies. Here v64-v127 are kept from the start and
  all its v32-v63 functions are in the keep list; four of them (sub_825D6328, sub_825D73A0, sub_82608D20,
  sub_827080C8) are hooked by tekken_video_wmv3.cpp, so check the movies with the FFmpeg path both on and off;
- `elide_lr_stores`: ctx.lr is stale everywhere, so the profiler's caller attribution and the `lr=` lines of the
  WMV3 / pool diagnostics become meaningless; guest code that walks the stack through saved LR words (only exception
  dispatch, which the runtime does not run) would see garbage;
- the five vector filters' reads are judged false positives by reading their flag tests; if a stage or movie shows
  corrupt pixels, they are the first suspects (the test: that register in `non_volatile_as_local_keep`, as
  katamari-nx did for v69 / v72; not `share_registers`, which these normal functions must not have);
- the localized copies in `app/generated/hot` must come from the new code (post_codegen.py rebuilds them).

## Character select in 2D

If the character select shows only the 2D portraits on a grey wall (no 3D stage or models), the save file has a setting
that asks for that select: in `MAINDATA.SAV`'s "SYS" header (`renut/<profile>/4E4D07FC/00000001/TK65SaveData/`), bytes
0x14 and 0x15 are 1 instead of 0. Which option of the game's menus sets them is not known yet. With both set to 0 (and the
checksum rewritten) the same save shows the 3D select again, with its progress kept.

The checksum is the big-endian u32 at offset 0: a reflected CRC-32 with initial value 0 and no final xor, over bytes
4 .. size, where size is the big-endian u32 at offset 4 (`sub_8243D230` checks it through `sub_82366C38`). A wrong one
makes the game report the save as damaged. In Python:

```python
import struct, zlib
data = bytearray(open('MAINDATA.SAV', 'rb').read())
data[0x14:0x16] = b'\0\0'
size = struct.unpack('>I', data[4:8])[0]
data[0:4] = struct.pack('>I', zlib.crc32(bytes(data[4:size]), 0xffffffff) ^ 0xffffffff)
open('MAINDATA.SAV', 'wb').write(data)
```

Details of how it was found: [switch-port-log.md](switch-port-log.md), "Character select in 2D: a save setting, not a
bug".
