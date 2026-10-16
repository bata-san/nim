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
```

nim only reads installed files. It never touches the running game, and it ships no game data or Oodle binaries.
