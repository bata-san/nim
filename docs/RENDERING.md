> **Outdated in parts.** Current status and next steps: [PROGRESS.md](PROGRESS.md) (source of truth).

# Renderer fidelity status

The requested complete game renderer port is **not implemented**. Game DXIL and material TFX
alone do not define the entire frame graph or its engine-generated inputs. No claim of
pixel equality with the running game has been established.

## Current implementation

Map rendering uses original game DXIL for SSAO, near/far sky lookups, sky ambient, local lights,
reflection/diffuse probe volumes, volumetrics, bloom, tonemapping and TAA. Static geometry and
placed opaque entities and sky objects render in the GUI and nimshade.
The normal build is `build/Release/nim.exe`.

Unknown engine-owned constants, incomplete froxel inputs and viewer-produced shadow/depth
inputs are explicitly listed as estimates in the UI and logs. Missing unestimated inputs,
resources, shared constant buffers or authored samplers fail the affected draw. This is a
partial reconstruction; it has not been shown to match the game pixel for pixel.

Transparent stage 8 now runs over the HDR image with the current frame's near/far sky,
atmospheric transmittance LUT and linear depth. Sky and transparent SV_Target4 outputs feed
the motion texture used by TAA. Froxels, transparent light-probe inputs, sorting and several
sky-object inputs/compositing details remain unfinished. See [PROGRESS §3.9](PROGRESS.md#39-透明物の大気とモーション-2026-10-18).

The notes below record resource-binding research and earlier fidelity work. The progress
table is authoritative when older notes disagree with the implemented pipeline.

## Original resource binding update

The original scope inheritance, per-stage cbuffer binding, native compute/UAV path, resource
view metadata and MRT output validation are documented in [GAME_RESOURCES.md](GAME_RESOURCES.md).
All 700 named pipeline slots were indexed; 734 shader headers were read. There are 198 unresolved
resource-source rows. These changes add the original resource path, not a complete frame graph.

## Missing work for complete reproduction

- Decode the actual frame graph: graphics/compute passes, resources, formats, attachments,
  barriers, clears, pass ordering, previous-frame histories and optional feature conditions.
- Reconstruct map lighting, shadows, ambient volumes, reflection probes, atmosphere and sky.
- Reconstruct object transforms, SH/AO inputs, material channels and animation state.
- Bind real opaque scene color/depth, refraction, scattering and auxiliary transparent outputs.
- Run original exposure, bloom, grading, distortion and temporal processing with real inputs.
- Compare matching frames from the actual game at each intermediate target and final output.

Existing optimization captures compare two versions of the former approximation, **not** nim
against the game. Their reported identical pixels do not establish game fidelity. Sharing
unchanged textures and PSOs remains; former filler-based optimizations have been removed.

## GPU reference capture

A short set of complete GPU frame captures of the actual game is more useful than a ten-minute
timing trace. A screenshot establishes appearance but not draw state or pass dependencies.
CPU process memory alone does not capture the GPU command stream, bindings or resource state.
A GPU capture can help decode those contracts; several scenes and temporal samples are needed
to validate more than a single frame. Capture support for the installed game is unverified.

The already-running build-rel/Release/nim.exe was locked during linking. The modified source
was compiled separately into build-strict/Release/nim.exe, nimcli.exe and nimshade.exe. The
running process was not terminated or replaced. No runtime rendering or regression tests
were performed for this change.
