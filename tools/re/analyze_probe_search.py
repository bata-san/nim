"""Summarize nimcli probe-search evidence without turning heuristic matches into probes.

Usage: python tools/re/analyze_probe_search.py out/probe-search-final
Game-derived reports stay under the input directory (out/), never in the repository.
"""
import argparse
import collections
import csv
import json
from pathlib import Path


def analyze(root: Path) -> dict:
    def rows(name):
        with (root / name).open(encoding="utf-8", newline="") as f:
            yield from csv.DictReader(f, delimiter="\t")

    errors = list(rows("errors.tsv"))
    if errors:
        raise ValueError(f"scan contains {len(errors)} read/parse errors; resolve these first")
    textures = {x["tag"]: x for x in rows("textures.tsv")}
    residual = {k: v for k, v in textures.items() if v["candidate"] == "true" and not v["known_roles"]}
    owners = collections.defaultdict(set)
    sources = collections.defaultdict(set)
    for ref in rows("references.tsv"):
        if ref["target"] in residual:
            owners[ref["target"]].add(ref["source_class"])
            sources[ref["target"]].add(ref["source"])
    groups = collections.Counter()
    evidence = []
    for tag, t in residual.items():
        cls = owners[tag]
        if t["subtype"] == "1":
            kind = "2D asset via 80805350" if cls == {"80805350"} else "2D material resource"
        elif t["subtype"] == "2":
            kind = "cube wrapper (80807F48)" if cls == {"80807F48"} else "cube material resource"
        elif cls == {"80807F6A"}:
            kind = "render-global lobe lookup"
        elif cls == {"8080810E"}:
            kind = "water-displacement globals resource"
        elif cls == {"80808070"}:
            kind = "render-global volume resource"
        elif cls == {"8080B1A7"}:
            kind = "map volume (atmosphere lookup layout)"
        else:
            kind = "3D material resource"
        groups[kind] += 1
        evidence.append({"tag": tag, "kind": kind, "package": t["package"],
                         "dimensions": [int(t[k]) for k in ("width", "height", "depth", "array")],
                         "source_classes": sorted(cls), "sources": sorted(sources[tag])})
    record_classes = collections.Counter()
    record_packages = collections.Counter()
    for rec in rows("records.tsv"):
        record_classes[rec["source_class"]] += 1
        if rec["source_class"] == "80807F4B":
            record_packages[rec["package"]] += 1
    channel_ranges = collections.defaultdict(set)
    for use in rows("channel_uses.tsv"):
        channel_ranges[use["opcode"]].add(int(use["index"]))
    result = {
        "texture_headers": len(textures),
        "known_roles": dict(collections.Counter(t["known_roles"] for t in textures.values() if t["known_roles"])),
        "residual_candidates": len(residual), "residual_groups": dict(groups),
        "residual_3D_seven_plane_candidates": [k for k, t in residual.items()
                                              if t["subtype"] == "3" and int(t["width"]) % 7 == 0],
        "record_source_classes": dict(record_classes), "collection_packages": dict(record_packages),
        "channel_operand_ranges": {k: [min(v), max(v)] for k, v in channel_ranges.items()},
        "limits": ["Raw references are candidate evidence, not validated ownership.",
                   "Groups describe resource provenance and shape; they do not prove engine semantics.",
                   "No new texture matches the known seven-plane 3D diffuse format. Other formats are not ruled out.",
                   "Runtime cascaded SH grids are shader-produced resources, separately investigated.",
                   "Localized packages are excluded by the source scan."],
        "residual_evidence": evidence,
    }
    (root / "analysis.json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    result = analyze(parser.parse_args().directory)
    print(json.dumps({k: v for k, v in result.items() if k != "residual_evidence"}, ensure_ascii=False, indent=2))
