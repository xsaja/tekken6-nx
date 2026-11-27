# Building tekken6-nx

You need your own copy of the game: the code generator and the shader tools read it. Nothing made from it is committed
(`.gitignore`): `app/assets`, `app/generated`, `out/`, `app/out/`. The PGO profile (`pgo/*.gcda`) is ignored too for now;
see [release.md](../release.md).

## Tools

- Windows 10/11, LLVM clang and the Visual Studio Build Tools with their CMake and Ninja (`tools/pc_env.bat` names the
  paths) for the host tools and the PC build.
- [devkitPro](https://devkitpro.org): devkitA64, libnx, deko3d and switch-tools, for the Switch build.
- Python 3.12 with `xxhash` and `Pillow` (`tools/build_switch.bat` finds it through the `py` launcher, or set `PYEXE`).
- The deko3d host tools (`out/deko3d_tools`: SPIRV-Cross and the PC uam) from `tools/deko3d_build_tools.sh`.

## 1. The game

```bat
python tools\extract_iso.py "Tekken 6 (Europe) (En,Ja,Fr,De,Es,It,Ko,Ru) (Rev 1).iso"
```

It extracts the disc into `../game` and prints `default.xex`'s header (check the SHA-256 in the README). Make
`app/assets` point at that folder.

## 2. Host tools, code generation and the PC build

```bat
tools\build_host.bat
tools\build_pc.bat
```

`build_host.bat` builds the code generator (`rexglue`). The PC build runs it on `app/renut_manifest.toml` (or run
`tools/codegen.ps1` by hand), then the post-codegen passes (`tools/post_codegen.py`: the localized machine copies, the
direct calls into hooks, the store-card rules) and builds the PC app with Renut's renderer on Vulkan or D3D12. Run it
with `tools/run_pc.ps1`.

## 3. The shaders

Tekken keeps its shaders in the data banks: one pixel-shader set per stage in `DATA302` (NSXR packages), a few in
`DATA303` / `DATA304`, the vertex shaders in `DATA301`. `tools/dump_tekken_shaders.py` decodes `DATA301`-`DATA313`
(XCTD: LZX blocks, with `out/xctd.exe` that `tools/build_xctd.bat` builds) one at a time and writes every shader
container into `out/renut_shader_dump`, with the containers of the game image.

```bat
python tools\dump_tekken_shaders.py
powershell -File tools\switch_shaders.ps1
```

`switch_shaders.ps1` translates every container (Renut's XenosRecomp to HLSL and SPIR-V, SPIRV-Cross to GLSL, the
`tools/deko3d_glsl_opt.py` passes) and writes `app/out/renut_switch/` (the GLSL package and the shader table).

## 4. The Switch NRO

```bat
tools\build_switch.bat
```

builds `app/out/switch/renut.nro` (incremental after the first configure). The release build is
`tools\build_switch_release.bat`: its own folder (`app/out/switch-pgo-use`), link-time optimisation and the PGO profile
in `pgo/`. A folder configured with `-DBANJO_RENUT_RELEASE_SHADERS=ON` holds no game shader and reads `deko3d_glsl.bin`
beside the NRO; `tools/renut_release_shaders.py` writes the files it builds with (see [release.md](../release.md)).

The settings are compiled into the NRO (`app/katamari_switch_defaults.toml`); the player's options are
`app/katamari_switch.toml`, written beside the NRO as `tekken6.toml` when it is missing.

## 5. PGO

A profile is taken with a `-DBANJO_PGO=generate` build played on the console (the `.gcda` files it writes under
`sdmc:/switch/tekken6-nx/pgo/` go into `pgo/`); `-DBANJO_PGO=use` builds with it. See
[switch-port-log.md](../switch-port-log.md), "PGO retraining on the Switch".

## Tests

- PC: `tools/run_pc.ps1`, `tools/run_pc_verify.ps1` (the hand-written natives against the generated originals).
- Ryujinx (the fork in `../ryu-build`, data in `../ryu-data`): `tools/run_ryujinx.ps1 -Seconds N -Name <run> -Toml
  <toml>`; `input_script` in the toml drives the menus, `nfsmw_captura_cada_s` takes captures.
- Console: `switch_profile_stacks = true` and `tekken_frame_chain = true` in `tekken6.toml` write
  `logs/rex/rex_profile.log` (per-thread samples) and the frame chain on `rex_stderr.log`;
  `tools/switch_hot_symbols.py` names the samples from the unstripped ELF of the same build.
