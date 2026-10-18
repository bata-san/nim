"""Analyze nim's surface/probe coverage without reading a running game.

Input: nimshade --dump-lighting --no-lights --no-sky-ambient --no-ssao.
The geometric union is diagnostic; it does not establish game influence semantics.
"""
import json
from pathlib import Path
import sys

import numpy as np
from PIL import Image


def analyze(directory):
    directory = Path(directory)
    meta = json.loads((directory / "lighting.json").read_text())
    w, h = meta["width"], meta["height"]
    depth = np.fromfile(directory / "lighting-depth.bin", "<f4").reshape(h, w)
    light = np.fromfile(directory / "lighting-diffuse.bin", "<f2").reshape(h, w, 4).astype(float)
    y, x = np.mgrid[:h, :w]
    clip = np.stack((2*(x+.5)/w-1, 1-2*(y+.5)/h, depth, np.ones_like(depth)), -1)
    inverse = np.linalg.inv(np.array(meta["vp"]).reshape(4, 4, order="F"))
    world = clip @ inverse.T
    with np.errstate(divide="ignore", invalid="ignore"):
        world = world[..., :3] / world[..., 3:4]
    surface = (depth > 0) & np.isfinite(world).all(-1)
    world[~surface] = 0
    union = np.zeros((h, w), bool)
    counts = []
    for p in meta["probes"]:
        x, y, z, q = p["rotation"]
        rotation = np.array([[1-2*(y*y+z*z), 2*(x*y-z*q), 2*(x*z+y*q)],
                             [2*(x*y+z*q), 1-2*(x*x+z*z), 2*(y*z-x*q)],
                             [2*(x*z-y*q), 2*(y*z+x*q), 1-2*(x*x+y*y)]])
        extents = np.array(p["extents"])
        if np.any(extents < 1e-8):
            counts.append(0)
            continue
        local = (world - p["center"]) @ rotation / extents
        inside = surface & (np.abs(local) < .999).all(-1)
        union |= inside
        counts.append(int(inside.sum()))
    nonzero = np.max(np.abs(light[..., :3]), -1) > 1e-6
    image = np.zeros((h, w, 3), np.uint8)
    image[surface & ~union] = (230, 70, 70)
    image[union & nonzero] = (60, 200, 90)
    image[union & ~nonzero] = (230, 190, 40)
    Image.fromarray(image).save(directory / "probe-coverage.png")
    result = dict(surface_pixels=int(surface.sum()), geometric_union_pixels=int(union.sum()),
                  nonzero_probe_pixels=int((surface & nonzero).sum()),
                  inside_union_with_zero_light=int((union & ~nonzero).sum()),
                  contributing_geometric_volumes=sum(n > 0 for n in counts))
    print(directory, json.dumps(result))
    (directory / "probe-coverage-summary.json").write_text(json.dumps(result, indent=2))


for arg in sys.argv[1:]:
    analyze(arg)
