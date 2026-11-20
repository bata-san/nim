# nim

**Renderer status:** the complete game renderer port is unfinished. Map rendering now includes
original-shader SSAO, sky/atmosphere, local lights and probes, light shafts, post-processing and
TAA. Engine values without decoded sources are listed as estimates; missing unestimated
inputs stop the affected draw. Transparency and sky objects remain partial, with known
compositing issues. See [current progress](docs/PROGRESS.md) for implemented and pending work.
The normal build is `build/Release/nim.exe`.

A fast, faithful asset viewer for Bungie's *Marathon* (2026, Tiger engine).
Goal: every line of text and dialogue, every model, map, texture and sound — rendered with the game's own shaders.

## Project layout

- `src/`, `include/` and `third_party/` build the viewer and its libraries.
- `tools/` contains the viewer, CLI, headless renderer and the shader dump script. `tests/` contains the CTest targets.
- `docs/` contains [current progress](docs/PROGRESS.md), [architecture](docs/ARCHITECTURE.md), [shader resource bindings](docs/GAME_RESOURCES.md) and the [entity survey](docs/ENTITY_SURVEY.md).
- `build/` is the local CMake build. The headless commands below use the ignored `out/` directory for their outputs.

## Build

Requires Windows, Visual Studio 2022 (or Build Tools) and CMake ≥ 3.25.

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

## Viewer (GUI)

```
build/Release/nim.exe                 # opens the Steam install automatically
build/Release/nim.exe 80A878C8        # ...and shows this static mesh (or dynamic model)
```

A minimal, terminal-style window built around one prompt line at the top: `nim ›` takes a page
word (`places`, `models`, `scenery`, `voices`, `codex`, `films`, `text`, `files`, `log`, `config`) or any
keyword, which **find** looks up across places, voices, codex, films and text at once (each group
opens its page filtered to the keyword). The page words also sit on the right of the line; Ctrl+L
focuses the prompt. Pages: **places** (whole maps, with their time of day and weather), **models**
(2.3k characters, gear and machines with materials and animation) and **scenery** (57k static meshes)
with live 3D thumbnails, **voices** (lines in 13 languages with voice playback), **codex** (the
in-game codex: collections, records in 13 languages and their audio logs), **films** (USM with
HCA/ADX audio), **text** (every localized string beside a second language), **files** (package
entries + hex) and **log**.

Everything is drawn with the game's own shaders: the G-buffer pass with each material's DXIL,
then the game's deferred `global_lighting_and_shading` and its `final_combine` film curve
("GAME SHADING"; "NIM GGX" switches to nim's own lighting). Height fog is evaluated by the game's
shading pass from the tune popup. Entity meshes take their materials (and material variants)
from the entity component that places them; skinned meshes show their bind pose.

The UI is set in the game's own fonts (PP Fraktion Mono, Marathon Shapiro, KH Interference,
Noto Sans JP/KR/SC/TC), read from your install at startup; nothing is bundled.

MAPS: a map's weather comes from its world channel controllers (the activity scripts that set
world global channels such as rain_amount). The map details offer a Heat Cascade toggle where the
map has one (Outpost) and a state per controller; rain-aware materials are kept dry under cover
by an overhead visibility map nim draws from straight above (an estimate, listed with the others).

Camera: left drag orbits, middle drag pans, wheel zooms, right drag flies (WASD/QE, Shift
faster, Ctrl slower, wheel = speed), F frames, Ctrl+S saves a PNG to `captures/` next to the exe.

Material animation is evaluated every frame in both vertex and pixel shaders, using a shared
clock and the installed global-channel defaults. The inspector provides play/pause, time,
speed, and live object-channel controls; changing a channel does not reload the model.
Entity channel initial constants and large material-variant tables are also read.
See [current progress](docs/PROGRESS.md) for rendering changes, validation commands and remaining shader limitations.

## Command line

The Steam install is detected automatically; otherwise pass `--install <Marathon dir>`.
Oodle is loaded from the game's own `bin/x64/oo2core_9_win64.dll`.

```
build/Release/nimcli pkgs              # list packages
build/Release/nimcli ls 120            # entries of package 0x120
build/Release/nimcli types             # entry type/class histogram
build/Release/nimcli read 80A4001D     # hex preview of a tag
build/Release/nimcli verify            # decrypt + decompress every block
build/Release/nimcli strings out.csv   # every localized string, 13 languages
build/Release/nimcli dialogue out.csv  # every spoken line: speaker, text, audio event, WEM streams
build/Release/nimcli find 80808635     # list static meshes
build/Release/nimcli static 80A655FC   # mesh groups, LODs, techniques, vertex layouts
build/Release/nimcli technique 80A655FB  # material: textures, TFX bytecode disassembly + VM result
build/Release/nimcli shader 80A65559 ps.dxil  # extract a shader (DXIL); view with `dxc -dumpbin ps.dxil`
build/Release/nimcli extract 13A 41 shaders   # every entry of one type in a package (41 = shader bytecode)
build/Release/nimcli dynamic 80A4A8D0  # dynamic model: meshes, parts, render stages, LODs
build/Release/nimcli refs 80A4A8D0 8080BADB   # tags of a class that reference a tag
build/Release/nimcli globals           # render globals: engine pipelines, scopes, global channels
build/Release/nimcli scope 80A74035    # scope bytecode (shared cbuffers)
build/Release/nimcli fonts out/fonts   # the game's UI fonts (for your own inspection)
build/Release/nimcli weather 80AF9550    # a map's world channel controllers (weather, Heat Cascade) and their states
build/Release/nimcli wem 80A1B2C3 out/line.wav   # decode a WEM stream or a dialogue audio event to WAV (NIM_LANGUAGE=6 for Japanese)
build/Release/nimcli wem-audit         # decode every dialogue WEM and report failures
build/Release/nimcli codex out/codex.md ja  # the in-game codex: collections, records, audio logs (Markdown)
build/Release/nimcli movies -v         # list movies (USM): size, codecs, audio tracks, subtitles
build/Release/nimcli movie-audio USM out/m.wav 1   # decode one ADX/HCA track of a movie
build/Release/nimcli movie-video USM out/m.m1v 8   # dump the MPEG-1 elementary stream (first 8 MB)
build/Release/nimcli movie-frames USM out/frames 60 30   # decode frames from 60 s as BMP, plus subtitles
```

### Shading test

```
build/Release/nimshade 80A878C8 out/shade --size 1600x1000
build/Release/nimshade 80A48549 out/animated --game --time 2
build/Release/nimshade 80A48549 out/benchmark --game --benchmark 480
build/Release/nimshade 80AF9550 out/outpost --heat-cascade   # a map with its Heat Cascade state
build/Release/nimshade 80AF9550 out/rain --weather outpost_ambient=STATE_HASH
```

The renderer loads original game DXIL and assets. It currently rejects lit rendering when
required engine inputs or passes are missing; it does not write a substitute composite.
`--game` remains accepted, but all Lit rendering now requires original game processing.
G-buffer diagnostic visualizations are separate from game-fidelity validation.
Requires a D3D12 GPU with Shader Model 6.6, or pass `--warp`.

GPU faults: set `NIM_D3D_DEBUG=1` before starting any of the tools to enable the D3D12 debug layer
(if installed) and DRED; messages and the faulting operation go to `nim_d3d.log` in the current
directory.

nim only reads installed files. It never touches the running game, and it ships no game data or Oodle binaries.
