# Agent instructions (Codex / Claude)

1. **Start with [docs/PROGRESS.md](docs/PROGRESS.md).** It is the source of truth for what is done,
   in progress and next, plus the reverse-engineering notes. When anything (docs, comments, code,
   your memory) disagrees, check the progress table first, then `git log`, and fix the stale side.
2. Update docs/PROGRESS.md in the same commit as the work it describes.
3. Rules that always apply (details in PROGRESS.md §2):
   - Render with game resources only; no fallback rendering. Unknown engine values are explicit,
     listed estimates (`ExternStore::note_estimate` / `estimate*`), shown as 推定 in the UI.
   - Never touch the running game (BattlEye); the install is read-only; no GPU/memory capture.
   - Never commit or ship game data or `oo2core_9_win64.dll`. `out/` is gitignored scratch.
   - "Implement" means: build, test, commit, push, release when appropriate, then verify.
   - Reply to the user in Japanese.
4. Build: `cmake --build build --config Release`; tests: `./build/Release/nim_tests.exe`;
   headless render check: `./build/Release/nimshade.exe 80AF9550 out/x --eye 155,-60,-12 --look 125,-28,-26 --map-radius 90`.
5. Shader analysis: `sh tools/re/dump.sh TAG` → `out/sh/TAG.hl` (pseudo-HLSL).
