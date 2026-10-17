"""Compare nim's stencil with original authored CSG at scene surface positions.

Requires NumPy. Input is an ignored out/ directory from nimshade BUBBLE OUT
--probe-index N --dump-probe-mask. Never reads the running game.
"""
import json
import pathlib
import struct
import sys

import numpy as np


def verify(directory):
    directory = pathlib.Path(directory)
    meta = json.loads((directory / "probe-mask.json").read_text())
    w, h = meta["width"], meta["height"]
    depth = np.fromfile(directory / "probe-depth.bin", dtype="<f4").reshape(h, w)
    actual = np.fromfile(directory / "probe-stencil.bin", dtype="u1").reshape(h, w) != 0
    y, x = np.indices((h, w))
    clip = np.stack(((x + .5) * 2 / w - 1, 1 - (y + .5) * 2 / h, depth, np.ones_like(depth)), axis=-1)
    inverse = np.linalg.inv(np.array(meta["vp"]).reshape(4, 4).T)
    world = clip @ inverse.T
    valid = (depth > 0) & (np.abs(world[..., 3]) > 1e-10)
    world /= np.where(valid, world[..., 3], 1)[..., None]
    expected = np.zeros((h, w), dtype=bool)
    boundary = np.zeros((h, w), dtype=bool)
    shapes = directory / "probe-shapes.bin"
    records = []
    if shapes.exists() and shapes.stat().st_size:
        data = shapes.read_bytes()
        count, relative = struct.unpack_from("<QQ", data, 8)
        base = 16 + relative + 16
        for i in range(count):
            c = base + i * 0xD0
            if struct.unpack_from("<Q", data, c)[0] != 0xD0:
                raise ValueError("unknown authored shape stride")
            operation = struct.unpack_from("<I", data, c + 0x10)[0]
            matrix = struct.unpack_from("<16f", data, c + 0x30)
            records.append((operation, matrix))
    if not records:
        records = [(0, meta["local_to_world"])]
    for operation, matrix in records:
        if operation not in (0, 1):
            raise ValueError(f"unknown CSG operation {operation}")
        local = world @ np.linalg.inv(np.array(matrix).reshape(4, 4).T).T
        distance = np.max(np.abs(local[..., :3]), axis=-1)
        inside = distance < 1
        # Ignore the immediate float/raster precision band at authored boundaries.
        boundary |= np.abs(distance - 1) < 2e-4
        expected = (expected | inside) if operation == 0 else (expected & ~inside)
    valid &= ~boundary
    differences = (actual != expected) & valid
    errors = int(differences.sum())
    positives = int((expected & valid).sum())
    print(f"probe {meta['index']}: {valid.sum()} surface samples, {positives} inside, {errors} mismatches")
    if errors:
        raise ValueError("GPU stencil disagrees with original authored CSG")
    return positives, int((~expected & valid).sum())


if __name__ == "__main__":
    positives = negatives = 0
    for path in sys.argv[1:]:
        inside, outside = verify(path)
        positives += inside
        negatives += outside
    if not positives or not negatives:
        raise ValueError("verification views must exercise both interior and exterior surfaces")
