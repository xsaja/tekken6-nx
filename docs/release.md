# Making a release

A release is two things: this repository's GitHub release with the NRO, and the installer page that turns a player's
own copy of the game into the SD card folder. The model is Beautiful Katamari's port
([katamari-nx](https://github.com/ChanseyIsTheBest/katamari-nx) and its
[installer page](https://github.com/ChanseyIsTheBest/katamari-nx-installer)), whose page is
[renut_nx_ultimate_installer](https://github.com/ChanseyIsTheBest/renut_nx_ultimate_installer) with a `game.json` for
the game.

## What is distributed and what is not

| Distributed | Not distributed |
|---|---|
| `tekken6-nx.nro`: the recompiled code, the runtime and the renderer, built with `BANJO_RENUT_RELEASE_SHADERS=ON` (no game shader inside) | The game's files, shaders, movies, audio |
| The installer page and its release files (`manifest.json`, the shader table's variant list, `pipeline_list.bin`, `LICENSES.txt`) | `deko3d_glsl.bin` (made in the player's browser from their files) |
| The sources (this repository) | `app/generated`, `app/assets`, `out/` |

## Steps

### 1. The NRO

1. Configure a release folder with `-DBANJO_PGO=use -DBANJO_SWITCH_LTO=ON -DBANJO_RENUT_RELEASE_SHADERS=ON`
   (`tools\build_switch_release.bat` passes these to a new folder; an existing `app/out/switch-pgo-use` keeps the
   options of its first configure, so check `BANJO_RENUT_RELEASE_SHADERS` in its `CMakeCache.txt`).
2. `python tools/renut_release_shaders.py` writes `app/out/renut_switch_release/`: the shader table without SPIR-V
   (built into the NRO) and, for tests only, `sd/deko3d_glsl.bin` (the file the installer will make).
3. Build, then test the NRO with that `deko3d_glsl.bin` beside it, in Ryujinx and on the console: the logos, the menus,
   the character select and several stages.
4. The release asset is the NRO renamed `tekken6-nx.nro`, with `LICENSES.txt` (the licenses of every component it
   links: see `THIRD_PARTY_NOTICES.md`).

### 2. The installer page

A repository of its own (for example `tekken6-nx-installer`), made from renut_nx_ultimate_installer:

- **`game.json`** for Tekken 6: the package name (`tekken6-nx`) and NRO name, the files kept as they are (`default.xex`),
  the files the game needs (the `DATA301`-`DATA313` banks), how the rest is packed (LZ4), and the movies and audio.
- **The shaders.** Katamari's shaders are plain containers in `default.xex` and in its model files. Tekken's are inside
  the `DATA301`-`DATA313` banks, XCTD files of LZX blocks (`tools/dump_tekken_shaders.py` reads them with `xctd.exe`),
  with the pixel shaders in NSXR packages. The page already carries an LZX decoder (`wasm/lzx.mjs`); what is new is
  reading the XCTD block table and the NSXR packages in JavaScript, as `dump_tekken_shaders.py` does, and listing in
  the build's `manifest.json` which records hold shaders (as Katamari's entry lists its 57 records). This is the main
  piece of work before the first release.
- **The release files** (`release/manifest.json` with the NRO's and the files' SHA-256, `pipeline_list.bin` from console
  sessions so the first start compiles fewer shaders, `variants.txt`), made with the installer repository's
  `tools/make_release.py`, and checked with its `test/full_node.mjs --reference` against the files of
  `tools/renut_release_shaders.py`.
- **The page's text:** the same as Katamari's, plus the forwarder notice: a Sphaira forwarder with a 39-bit address
  space, launched from its icon, for the four cores. Before the release, check on a console with a new forwarder
  whether the first launch (which writes the four-core NPDM) needs a second launch, and say so on the page if it does.
  And the known issue of the 2D character select
  ([tekken6-notes.md](tekken6-notes.md#character-select-in-2d)).
- **Deployment:** the page's GitHub Actions workflow downloads `tekken6-nx.nro` from the latest release of this
  repository, checks it against `manifest.json` and publishes the page on GitHub Pages.

### 3. The repository

- **History:** the working history holds test notes that are not meant for publication. GitHub gets only the local
  branch `publish`: snapshots of the working branch's tree, each with the previous snapshot as its only parent.
  `sh tools/publish_snapshot.sh "message"` adds one, then `git push origin publish:main`. A local pre-push hook
  (`.git/hooks/pre-push`, not versioned) refuses any other branch.
- **PGO profile:** decide whether `pgo/*.gcda` (counts and function names, no game code) is committed so the release NRO
  can be rebuilt bit for bit; it is ignored today.
- **Icon:** `app/CMakeLists.txt` takes `tools/switch/icon/tekken6_icon.jpg` when it exists. Use artwork that can be
  redistributed (not the game's cover).
- Tag the commit (`v1.0.0`), write the release notes (what works, the known issues, the forwarder notice) and attach
  the NRO.

## Settings shipped in the NRO

- `banjo_forwarder_program_id = "auto"`: any forwarder (an id outside the 01... range of system and retail titles)
  gets the four-core NPDM, so no player has to know their forwarder's id.
- The player's file is `tekken6.toml` (`katamari.toml` is still read when it is the only one).
- No logs (`log_level = "off"`); `rex_stderr.log` and `rex_crash.log` stay for crash reports.

## State of the first release (2026-11-27)

- **Shaders:** `tools/tekken_shader_records.py` finds 70 shaders in `default.xex` and 5,838 in 129 records of the XCTD
  banks `DATA302`-`DATA304`. The other 964 (the vertex shaders of `DATA301`'s first entry, an NSXR package packed a
  second time by a coder of the game's own: a marker byte `FF`, the 8-byte header, then an LZSS-like stream not
  matched by any common variant) are built into the NRO (`tools/renut_release_shaders.py --keep`); the renderer reads
  the installer's package first and the NRO's own for the rest.
- **Release NRO:** 51.4 MB (`-DBANJO_RENUT_RELEASE_SHADERS=ON`).
- **Installer:** [xsaja/tekken6-nx-installer](https://github.com/xsaja/tekken6-nx-installer) (page: https://xsaja.github.io/tekken6-nx-installer/): the
  base's page with Tekken's `game.json`, the `xctd` coding, and the translator and uam rebuilt from this repository with
  Emscripten 6.0.11. In Node it makes 5,908 shaders and 11,221 variants; the NRO with those files played in Ryujinx
  (menus, character select, a fight) with no shader missing and no compile failure.
- **Console test:** the zip made by the installer, extracted to a clean SD card and started from a new Sphaira forwarder
  (39-bit), ran smoothly from its first launch.
- **Release notes:** [release-notes/v1.0.0.md](release-notes/v1.0.0.md).
- **Published 2026-11-27:** both repositories public, the page on GitHub Pages, the tag `v1.0.0` with
  `tekken6-nx.nro` and `LICENSES.txt`.
