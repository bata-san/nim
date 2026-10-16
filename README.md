# nim

**Renderer status:** the complete game renderer port is unfinished. Approximate lighting,
placeholder textures and silent fallback have been removed. Missing original frame inputs
now stop rendering, so lit previews and many thumbnails are currently unavailable. See
[rendering status](docs/RENDERING.md) and [original game resources](docs/GAME_RESOURCES.md). The separate current build is `build-strict/Release/nim.exe`.


A fast, faithful asset viewer for Bungie's *Marathon* (2026, Tiger engine).
Goal: every line of text and dialogue, every model, map, texture and sound — rendered with the game's own shaders.

Design: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)

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

Laid out after goriath: a HOME page and document tabs — **STATICS** (57k static meshes) and
**ENTITIES** (2.3k dynamic models, rigid and skinned) as package lists with live 3D thumbnail
cards and a live view, **TEXT** (every localized string beside a second language), **DIALOGUE**
(speaker, line, audio event), **PACKAGES** (entry table + hex) and **LOG**.

Everything is drawn with the game's own shaders: the G-buffer pass with each material's DXIL,
then the game's deferred `global_lighting_and_shading` and its `final_combine` film curve
("GAME SHADING"; "NIM GGX" switches to nim's own lighting). Height fog is evaluated by the game's
shading pass from the tune popup. Entity meshes take their materials (and material variants)
from the entity component that places them; skinned meshes show their bind pose.

The UI is set in the game's own fonts (PP Fraktion Mono, Marathon Shapiro, KH Interference,
Noto Sans JP/KR/SC/TC), read from your install at startup; nothing is bundled.

Camera: left drag orbits, middle drag pans, wheel zooms, right drag flies (WASD/QE, Shift
faster, Ctrl slower, wheel = speed), F frames, Ctrl+S saves a PNG to `captures/` next to the exe.

Material animation is evaluated every frame in both vertex and pixel shaders, using a shared
clock and the installed global-channel defaults. The inspector provides play/pause, time,
speed, and live object-channel controls; changing a channel does not reload the model.
Entity channel initial constants and large material-variant tables are also read.
Rendering changes, validation commands, and remaining special-shader limitations:
[docs/RENDERING.md](docs/RENDERING.md).

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
```

### Shading test

```
build/Release/nimshade 80A878C8 out/shade --size 1600x1000
build/Release/nimshade 80A48549 out/animated --game --time 2
build/Release/nimshade 80A48549 out/benchmark --game --benchmark 480
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
