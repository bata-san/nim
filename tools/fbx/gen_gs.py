# Writes one geometry shader per opaque part: it keeps the original VS outputs, interpolates them
# at the atlas triangle (exact at depth 0.9, edge-dilated copy at 0.1), and the original PS shades it.
import json, pathlib, subprocess, sys
import tris
DXC = r'C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/dxc.exe'
SIZE = int(sys.argv[1]); PAD = float(sys.argv[2]); PASS = int(sys.argv[3]); OUT = pathlib.Path(sys.argv[4])
OUT.mkdir(parents=True, exist_ok=True)
CHUNK = 2048  # primitives per constant array: larger static arrays read back wrong on the GPU
a = tris.load()
uv = json.loads(pathlib.Path('uv.json').read_text())

def signature(p):
    t = pathlib.Path(f'gs/{p}.vs.txt').read_text().splitlines()
    i = next(k for k, l in enumerate(t) if l.strip() == '; Output signature:')
    rows = []
    for l in t[i + 4:]:
        if l.strip() == ';':
            break
        name, index, mask, reg, sysv = l[2:].split()[:5]
        rows.append((name, int(index), len(mask), int(reg)))
    return rows

offset = 0
for sub in a['parts']:
    if sub['stage'] != 0:
        continue
    p = sub['part']
    sig = signature(p)
    fields = []
    for n, (name, index, width, reg) in enumerate(sig):
        assert reg == n, (p, sig)
        sem = 'SV_Position' if name == 'SV_Position' else f'{name}{index}'
        fields.append((f'f{n}', f'float{width}' if width > 1 else 'float', sem))
    table = []
    for prim in sub['primitives']:
        if prim < 0:
            table.append('float4(-1,-1,-1,-1),float4(-1,-1,0,0)')
        else:
            t = uv[offset + prim]
            # face order is (n+1, n, n+2) for odd strip triangles; the GS sees (n, n+2, n+1)
            (u0, v0), (u1, v1), (u2, v2) = (t[1], t[2], t[0]) if sub['odd'][len(table)] else t
            table.append(f'float4({u0:.8g},{v0:.8g},{u1:.8g},{v1:.8g}),float4({u2:.8g},{v2:.8g},0,0)')
    offset += len(sub['triangles'])
    pos = next(f[0] for f in fields if f[2] == 'SV_Position')
    body = '\n'.join(f'    {t} {n} : {s};' for n, t, s in fields)
    mix = '\n'.join(f'    o.{n} = w.x * v[0].{n} + w.y * v[1].{n} + w.z * v[2].{n};' for n, _, _ in fields if n != pos)
    chunks = [table[i:i + CHUNK] for i in range(0, len(table), CHUNK)]
    first = PASS * CHUNK
    chunk = chunks[PASS] if PASS < len(chunks) else ['float4(0,0,1,0),float4(0,1,0,0)']
    if PASS >= len(chunks): first = 0xFFFFFF00  # no primitive reaches it: nothing drawn this pass
    arrays = f'static const float4 kUV[{2 * len(chunk)}] = {{ {",".join(chunk)} }};'
    fetch = (f'    if (id < {first} || id >= {first + len(chunk)}) return;\n'
             f'    ab = kUV[(id - {first}) * 2]; c4 = kUV[(id - {first}) * 2 + 1];')
    src = f'''struct V {{
{body}
}};
{arrays}
static const float kSize = {SIZE}.0;
V mixv(V v[3], float3 w, float2 uv, float z) {{
    V o;
{mix}
    o.{pos} = float4(uv * 2 - 1, z, 1);
    return o;
}}
[maxvertexcount(6)]
void main(triangle V v[3], uint id : SV_PrimitiveID, inout TriangleStream<V> s) {{
    float4 ab = 0, c4 = 0;
{fetch}
    if (ab.x < 0) return;
    float2 t[3] = {{ ab.xy, ab.zw, c4.xy }};
    float2 q0 = t[0] * kSize, q1 = t[1] * kSize, q2 = t[2] * kSize;
    float la = length(q1 - q2), lb = length(q2 - q0), lc = length(q0 - q1), per = la + lb + lc;
    float2 e1 = q1 - q0, e2 = q2 - q0;
    float area = abs(e1.x * e2.y - e1.y * e2.x) * 0.5;
    if (per <= 0 || area <= 0) return;
    float3 ib = float3(la, lb, lc) / per;
    float k = min(1 + {PAD} / max(2 * area / per, 1e-4), 24);
    float2 inc = ib.x * t[0] + ib.y * t[1] + ib.z * t[2];
    [unroll] for (int j = 0; j < 3; ++j) {{
        float3 e = float3(j == 0, j == 1, j == 2);
        s.Append(mixv(v, ib + k * (e - ib), inc + k * (t[j] - inc), 0.1));
    }}
    s.RestartStrip();
    [unroll] for (int j = 0; j < 3; ++j) s.Append(mixv(v, float3(j == 0, j == 1, j == 2), t[j], 0.9));
}}
'''
    (OUT / f'{p}.gs.hlsl').write_text(src)
    r = subprocess.run([DXC, '-T', 'gs_6_0', '-E', 'main', '-O3', str(OUT / f'{p}.gs.hlsl'), '-Fo', str(OUT / f'{p}.gs.dxil')],
                       capture_output=True, text=True)
    print('pass', PASS, 'part', p, len(table), 'primitives', len(chunks), 'chunks', r.returncode, r.stderr[-500:])
    assert r.returncode == 0
