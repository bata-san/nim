# Original game resource binding

This implementation uses the installed game's shader bytecode, TFX programs, texture payloads,
sampler tags, constant buffers and scope definitions. Missing producers are not replaced by
custom lighting shaders or constant textures. A complete functioning game renderer is still
unfinished: the resource binding layer is not the full frame graph.

## Implemented resource path

- Decode each technique's `used_scopes` mask against the ordered render-globals scope table.
- Inherit each shader stage's textures, samplers, external SRVs/UAVs and constant-buffer owners
  from those scopes, then apply the technique's own assignments in binding order.
- Load referenced constant-buffer data with precedence over inline data, regardless of size.
  Opaque uint/float bit patterns are preserved; NaNs used as bit fields are not repaired.
- Bind vertex and pixel scopes independently, including b1, b13 and b2-b11. The material root
  signature stays within the D3D12 64-DWORD limit and contains only authored samplers.
- Resolve external resource views using their actual dimensions, format, structured stride,
  element count and range. Cube arrays remain cube arrays; 3D resources remain volumes.
- Execute original compute DXIL with caller-supplied dispatch dimensions and UAV resources.
  TFX `pop_uav` and the game's `0x5B` SRV assignment are decoded without numeric stand-ins.
- Read PS output signatures and reject missing MRT attachments, rather than dropping outputs.
- Do not manufacture vertex semantics, missing color streams, skin streams or shared cbuffers.

The resource provider and render-pass caller still have to create/populate the engine-owned
resources, specify their actual views and keep them alive through GPU execution. Compute
support does not automatically reconstruct dispatch order, counters, history or resource barriers.

## Installed game resource index

`nimcli render-resources [TECHNIQUE]` prints a TSV mapping for shader registers and output
signatures. It performs structural inspection only, without fabricated TFX inputs or GPU work.

The current installation contains:

| Item | Count |
|---|---:|
| Named pipeline slots | 700 |
| Assigned pipeline techniques | 684 |
| Unassigned slots (`FFFFFFFF`) | 16 |
| Distinct shader headers read | 734 |
| Resource-binding rows | 5128 |
| Binding rows whose source is still unresolved | 198 |
| Structural decoding errors | 0 |

The 198 unresolved rows comprise 180 SRVs and 18 cbuffers. They remain explicitly unresolved;
the index is not proof that those producers have been implemented. Nor is it a GPU capture.

```powershell
build/Release/nimcli.exe render-resources > out/native-render-resources.tsv
build/Release/nimcli.exe render-resources 80A4852F > out/80A4852F-native-resources.tsv
```

## 80A48549 translucent material

Technique `80A4852F` uses scopes frame `80A74028`, view `80A74035`, rigid_model `80A74042`
and transparent `80A74077`. Its pixel shader is `80A48525`.

| PS input | Original source |
|---|---|
| t0 | texture 80A46CC9 |
| t1 | texture 80A46C53 |
| t2 | texture 80A46C92 |
| t3 | texture 80A46C0C |
| t4 | texture 80A46C93 |
| t11 | transparent extern 40 byte 0x00, 2D |
| t13 | transparent extern 40 byte 0x10, 2D |
| t15 | transparent extern 40 byte 0x20, 2D |
| t20 | transparent extern 40 byte 0x48, 2D |
| t21 | transparent extern 40 byte 0x50, **3D** |
| t23 | transparent extern 40 byte 0x60, 2D |
| t25 | transparent extern 40 byte 0x78, **3D** |

The PS writes **SV_Target0 and SV_Target4**. The earlier single-HDR-target renderer could not
represent this contract. The 3D inputs and target4's producer/consumer semantics still need
reconstruction; assigning arbitrary white volumes or extra targets would not solve that.

## Build and remaining work

Use `build/Release` for the current executables and [PROGRESS.md](PROGRESS.md) for headless validation results.

The main remaining work is the game's frame graph and its engine-generated scene inputs:
lighting volumes/probes, shadows, sky/atmosphere, transparent scattering and auxiliary targets,
object state, exposure, grading, bloom and temporal histories. See [PROGRESS.md](PROGRESS.md).
