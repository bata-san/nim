# nim

A fast, faithful asset viewer for Bungie's *Marathon* (2026, Tiger engine).
Goal: every line of text and dialogue, every model, map, texture and sound — rendered with the game's own shaders.

Design: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)

## Build

Requires Windows, Visual Studio 2022 (or Build Tools) and CMake ≥ 3.25.

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

## Use

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
```

### Shading test

```
build/Release/nimshade 80A878C8 out/shade --size 1600x1000
```

Renders a static mesh with the game's own compiled shaders, run unmodified on D3D12:
the G-buffer vertex and pixel shaders (DXIL), with material constants computed by nim's TFX
bytecode VM and the game's BC textures. It writes the four G-buffer targets the pixel shader
outputs (`gbuffer0_albedo.png`, `gbuffer1_normal.png`, `gbuffer2.png`) and `composite.png`, which
adds simple lighting. The game's own lighting pass is not run yet. Requires a D3D12 GPU with
Shader Model 6.6, or pass `--warp`.

nim only reads installed files. It never touches the running game, and it ships no game data or Oodle binaries.
