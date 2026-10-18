"""Measure the original froxel consumer at visible surfaces from --dump-lighting.

Reports the volume's contribution only, before analytic distance fog and grading.
Does not infer the game's correct environment state from an image.
Usage: python tools/re/analyze_atmosphere.py OUT_DIR
Requires numpy and Pillow. Game-derived inputs/outputs belong in ignored out/.
"""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image


def sample_volume(volume, uvw):
    """D3D normalized coordinates, linear filtering, clamp addressing."""
    d, h, w, channels = volume.shape
    p = uvw * np.array([w, h, d]) - 0.5
    lo = np.floor(p).astype(np.int64)
    f = p - lo
    result = np.zeros((*p.shape[:-1], channels), dtype=np.float64)
    for z in range(2):
        for y in range(2):
            for x in range(2):
                index = np.clip(lo + [x, y, z], 0, np.array([w, h, d]) - 1)
                weight = np.prod(np.where([x, y, z], f, 1 - f), axis=-1)
                result += volume[index[..., 2], index[..., 1], index[..., 0]] * weight[..., None]
    return result


def analyze(folder):
    meta = json.loads((folder / "lighting.json").read_text())
    if "view_depth_near" not in meta or "froxel_mapping" not in meta:
        raise ValueError("Rerun nimshade --dump-lighting: this dump lacks camera/froxel depth metadata")
    w, h = meta["width"], meta["height"]
    volume_info = next((t for t in meta["atmosphere"] if t["file"] == "lighting-froxels.bin"), None)
    if volume_info is None:
        raise ValueError("The dump has no froxel volume; enable atmosphere and froxels")
    volume = np.fromfile(folder / volume_info["file"], dtype="<f2").reshape(
        volume_info["depth"], volume_info["height"], volume_info["width"], 4).astype(np.float64)
    depth = np.fromfile(folder / "lighting-depth.bin", dtype="<f4").reshape(h, w)
    surface = np.isfinite(depth) & (depth > 0)
    near, _, curve, scale = meta["froxel_mapping"]
    view_depth = np.divide(meta["view_depth_near"], depth, out=np.zeros_like(depth), where=surface)
    z = np.clip(np.maximum((view_depth - near) * scale, 0) ** curve, 0, 1)
    y, x = np.mgrid[:h, :w]
    rgba = sample_volume(volume, np.stack(((x + 0.5) / w, (y + 0.5) / h, z), axis=-1))
    transmission = rgba[..., 0] * rgba[..., 1]
    visible = transmission[surface]
    if not visible.size or not np.isfinite(visible).all():
        raise ValueError("No finite surface transmittance")
    result = {
        "surface_pixels": int(surface.sum()),
        "volume_transmittance_p10_p50_p90": np.quantile(visible, [0.1, 0.5, 0.9]).tolist(),
        "surface_fraction_below_half_transmittance": float(np.mean(visible < 0.5)),
        "interior_transmittance_min_max": [float(rgba[..., 1][surface].min()), float(rgba[..., 1][surface].max())],
        "note": "Froxel transmittance only; excludes analytic fog, light in-scatter and grading."
    }
    (folder / "atmosphere-summary.json").write_text(json.dumps(result, indent=2) + "\n")
    pixels = np.where(surface, np.clip(transmission, 0, 1), 0)
    Image.fromarray(np.rint(pixels * 255).astype(np.uint8)).save(folder / "froxel-transmittance.png")
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("folder", type=Path)
    print(json.dumps(analyze(parser.parse_args().folder), indent=2))
