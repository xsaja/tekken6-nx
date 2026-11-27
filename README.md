# tekken6-nx

**Tekken 6** (Xbox 360) on the Nintendo Switch, as native homebrew. The game's code is statically recompiled with
[ReXGlue](https://github.com/rexglue/rexglue-sdk) and compiled for the Switch, and its Direct3D calls are drawn directly
on the Switch GPU's own API, deko3d, by [reNut](https://github.com/masterspike52/reNut)'s Direct3D-level renderer.

> [!IMPORTANT]
> This repository and its releases contain **no game data**. You need your own copy of the Xbox 360 disc:
> *Tekken 6 (Europe) (En,Ja,Fr,De,Es,It,Ko,Ru) (Rev 1)*, whose `default.xex` has the SHA-256
> `ee138a4fe8d32a0edc131451377f4377041a49626153fa992db24f5420a7f15c`.

## How to install

1. Open the installer page: **https://xsaja.github.io/tekken6-nx-installer/** (Chrome or Edge on a computer).
2. Choose your game: the **disc image** (`.iso`) or a **folder** you extracted it into, and press **Create
   tekken6-nx.zip**. Everything runs in your browser and the game files never leave your computer: the page checks your
   copy, downloads the build and makes the game's shaders from your own files.
3. Extract the zip to the root of the SD card (it holds the `switch/tekken6-nx/` folder).

```text
/switch/tekken6-nx/
  tekken6-nx.nro
  LICENSES.txt
  tekken6.toml          (your settings; written at the first start when it is missing)
  deko3d_glsl.bin       (the game's shaders, made from your copy)
  game_root/            (the files of your disc)
```

The game adds `deko3d_shaders.bin` and `pipeline_list.bin` there as you play (the shaders compiled for the Switch GPU,
and the list of what it drew, so later sessions prepare it ahead).

> [!CAUTION]
> **Launch it through a forwarder made with [Sphaira](https://github.com/ITotalJustice/sphaira), set to a 39-bit address
> space, and always from the forwarder's icon on the HOME menu.**
> - Launched from the forwarder's icon, the game gets **all four CPU cores**. It needs the fourth: on three it runs
>   clearly slower.
> - Launching the NRO from Sphaira's list instead runs it on three cores.

> [!NOTE]
> **The first time you play** after installing or updating, the game compiles its shaders for the Switch GPU as it meets
> them, so it can stutter for a moment in new places. They are saved next to the NRO and later sessions run smoothly.

## Controls

By default the face buttons work **by position, as on the Xbox 360 pad**: the bottom button is the Xbox's A, the right
one the Xbox's B, and so on. ZR / ZL are RT / LT, R / L are RB / LB, + is Start and − is Back.

To use the buttons by letter instead, set `input_xbox_layout = false` in `tekken6.toml`. Each player's buttons can be
remapped there (`[input_p1]`, `[input_p2]`).

## Settings

`tekken6.toml`, next to the NRO, holds only your options. Everything else is built into the NRO. If you delete it, the
game writes the default one again at the next start.

| Setting | What it does |
| --- | --- |
| `frame_cap` | Frame rate cap: `"Off"` (the game's own, default) or `"30"` |
| `user_language` | The game's language. By default the console's; `1` English, `2` Japanese, `3` German, `4` French, `5` Spanish, `6` Italian, `7` Korean, `12` Russian |
| `tekken_blur_always` | Motion blur. `false` (default) keeps the game's own rule, which draws it only when it thinks it has time to spare (on the Switch, now and then). `true` draws it whenever it is on in the game's options: about 3 ms a frame |
| `input_xbox_layout` | Face buttons by position (`true`, default) or by letter (`false`) |
| `[input_p1]`, `[input_p2]` | Each player's buttons: for every Xbox button, the Switch buttons that press it, e.g. `a = "B, ZR"` |

## Known issues

- **Character select with 2D portraits only** (no 3D stage or models): this comes from a setting stored in some save
  files, not from the port. See [docs/tekken6-notes.md](docs/tekken6-notes.md#character-select-in-2d).
- **Frame rate:** menus and the lighter stages hold 60 fps at the console's stock clocks; the heaviest fights run in the
  mid 50s. See [Performance](#performance).

## Performance

The port runs at the console's stock clocks on four cores. The game draws its 3D image at its own 1024x576, as on the
Xbox 360, in handheld and docked, scaled to the screen. The release NRO is built with link-time optimisation and PGO from console sessions. In the heaviest fights the frame is limited by the
game's main thread and by the jobs it waits for (character skinning above all), not by the GPU. Every optimisation was
checked against the original functions, call by call: the measurements are in [docs/switch-port-log.md](docs/switch-port-log.md).

## How it works

- **The game's code is translated, not emulated.** ReXGlue translates every function of the game's program to C++, which
  is then compiled for the Switch, and gives the game what it expects from an Xbox 360: its system, files, audio and
  controllers.
- **A Direct3D-level renderer on deko3d.** The game's Direct3D calls are replaced by reNut's renderer (as the
  Banjo-Kazooie: Nuts & Bolts and Beautiful Katamari ports adapted it), drawn with [plume](https://github.com/renderbag/plume)
  on [deko3d](https://github.com/devkitPro/deko3d). No GPU command stream is written or parsed and no EDRAM is modelled.
- **Shaders made from your copy.** The release NRO holds no shader of the game. The installer page takes them from your
  files and makes `deko3d_glsl.bin`: XenosRecomp translates each one to HLSL, DXC compiles it to SPIR-V, SPIRV-Cross
  turns that into GLSL, and the port's passes trim it. The console compiles each one with uam the first time it is used
  and keeps the result.
- **Hand-written and machine-optimised functions.** The game's hottest functions (skinning, particles, audio, matrix
  helpers) run as native ARM code written for them, and many more as optimised machine copies; each was checked against
  the generated original.

## Documentation

| Document | What it explains |
| --- | --- |
| [docs/switch-port-log.md](docs/switch-port-log.md) | The Switch port in order: every finding, fix and optimisation, how it was checked and what the console measured |
| [docs/tekken6-notes.md](docs/tekken6-notes.md) | Facts about the game: extraction, codegen, the renderer's device layout, shaders, known issues |
| [docs/release.md](docs/release.md) | How a release is made: the NRO, the shader files, the installer page |
| [docs/reference/building.md](docs/reference/building.md) | How to build the PC test build and the Switch NRO |
| [docs/reference/optimising-functions.md](docs/reference/optimising-functions.md) | How the game's functions are made faster: machine copies, hand-written natives from console profiles |
| [docs/reference/codegen-crash-fixes.md](docs/reference/codegen-crash-fixes.md) | The code generation crashes found and how each was fixed |
| [docs/plan.md](docs/plan.md), [docs/progress.md](docs/progress.md) | The port's phases and its early progress |

## Credits

This port stands on the work of these people and projects:

- **[mahmoud-mess](https://github.com/mahmoud-mess)**: tekken6-recomp, the Tekken 6 ReXGlue configuration, runtime fixes
  and the ISO extractor this port started from.
- **[ChanseyIsTheBest](https://github.com/ChanseyIsTheBest)**: renut-nx (Banjo-Kazooie: Nuts & Bolts), the deko3d
  adaptation of the reNut renderer, plume's deko3d back end, the shader passes, katamari-nx, sa2-nx, futari-nx,
  renut_nx_ultimate_base, the installer page this one is made from, and the Ryujinx fork used for automated tests.
- **[masterspike52](https://github.com/masterspike52)**: reNut, the original Direct3D-level renderer.
- **[StevensND](https://github.com/StevensND)**: nfsmw-nx: the Horizon (Switch) layer of the SDK, the profiling and PGO
  method, the deko3d guide and devlog, the console overlay notes and stack profiler.
- **[NebadasSwifty](https://github.com/nebadasSwifty)**: masseffect-nx, the structure of `app/`.
- **[madelrandel-blip](https://github.com/madelrandel-blip)**: NFSMW Recompiled, where the ports above began.
- **[recomp-nx](https://github.com/recomp-nx)**: fnxf_xiii's store-card marking.
- **[ReXGlue](https://github.com/rexglue/rexglue-sdk)** (Tom Clay and contributors), derived in part from
  **[Xenia](https://xenia.jp)** (Ben Vanik and contributors).
- **[XenosRecomp](https://github.com/hedge-dev/XenosRecomp)** (hedge-dev and contributors): the shader translator.
- **[plume](https://github.com/renderbag/plume)** (renderbag), **[deko3d](https://github.com/devkitPro/deko3d)** (fincs),
  **uam**, **[libnx](https://github.com/switchbrew/libnx)** and **[devkitPro](https://devkitpro.org)**.
- **SwitchWakerHD**: the uam patches.
- **[Sphaira](https://github.com/ITotalJustice/sphaira)** (ITotalJustice): the forwarders that give the port its four cores.
- **SaltyNX** (masagrator): the overlay's shared-memory protocol.
- **Dear ImGui** (Omar Cornut), **FFmpeg**, **libmspack** (Stuart Caie) and the other libraries listed in
  [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
- **Ryujinx / Ryubing** contributors.

## License

GPL-3.0 ([LICENSE](LICENSE)); the SDK (`sdk/`) is BSD-3-Clause and the shader translator (`shaders/`) MIT. See
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for every project this port is built on and its license.

Tekken is a trademark of Bandai Namco Entertainment. This project is not affiliated with or endorsed by Bandai Namco,
Nintendo or Microsoft.
