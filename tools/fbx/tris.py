# Shared by the tools/fbx scripts. The working directory is the model's work folder: config.json
# (written by fbx_export.py), asset.json and raw/ (extract), gs/ (shader dumps), uv.json (unwrap).
import json, pathlib
WORK = pathlib.Path.cwd()
CFG = json.loads((WORK / 'config.json').read_text())


def load():
    a = json.loads((WORK / 'asset.json').read_text())
    if 'parts' in CFG:
        a['parts'] = [p for p in a['parts'] if p['part'] in CFG['parts']]
    for sub in a['parts']:
        idx = sub['indices']
        faces, prims, odd, start = [], [], [], 0
        for i, v in enumerate(idx):
            if v == 65535:
                start = i + 1
                continue
            if i - start < 2:
                continue
            j, k = idx[i - 2:i]
            odd.append((i - start) % 2 == 1)
            if j == k or j == v or k == v:
                prims.append(-1)  # degenerate: still counts as an input-assembler primitive
                continue
            prims.append(len(faces))
            faces.append([j, k, v] if (i - start) % 2 == 0 else [k, j, v])
        sub['triangles'] = faces
        sub['primitives'] = prims  # SV_PrimitiveID -> triangle index (-1 degenerate)
        sub['odd'] = odd           # strip parity: the GS receives odd triangles as (n, n+2, n+1)
    return a
