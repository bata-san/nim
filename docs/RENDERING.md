# Renderer fidelity status

The requested complete game renderer port is **not implemented**. Game DXIL and material TFX
alone do not define the entire frame graph or its engine-generated inputs. No claim of
pixel equality with the running game has been established.

## Substitution removal

The current source removes the previous custom IBL, fog, background, exposure and ACES
helpers, constant filler textures, guessed object-channel values, forced detail fades,
material non-finite-to-zero repairs, sampler fallback and automatic GGX renderer switching.
The remaining custom fullscreen shader is for explicitly selected G-buffer diagnostics only;
its Lit mode is rejected.

Missing scalar/vector externs, resources, shared constant buffers, authored samplers and
unimplemented TFX operations now raise errors. The GUI reports rendering failure instead of
showing a substituted scene. Consequently, normal lit previews and many thumbnails are
currently unavailable. This is an explicit incomplete implementation, not a fidelity fix.

Numeric externs track which bytes were supplied. Original frame cb13 and shared object scopes
can be supplied through ExternStore; set_render_inputs forwards that context to a loaded scene.
No producer currently populates the full context from a running game or decoded map.

The original global_lighting_and_shading and final_combine DXIL loaders remain. Their required
inputs are validated before render-target state transitions. Transparency remains blocked
until its shared inputs, MRT outputs and pass ordering have been reconstructed.

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
