"""Inventory event candidates and authored placement links for every map selection."""

import argparse
import csv
import json
import re
import subprocess
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path


def has_event_name(module):
    path = module.lower().split("\\")
    name = path[-1]
    return (
        name.endswith(".es")
        or any(part in {"events", "dynamic_events", "encounters", "boss_encounters"} for part in path)
        or name.startswith(("enc_", "pf_dcon", "dcon_combiner", "compiler_aura_terminals"))
        or any(word in name for word in ("encounter", "lockdown", "convoy", "flyover"))
    )


def write_csv(path, columns, rows):
    with path.open("w", encoding="utf-8-sig", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(columns)
        writer.writerows(rows)


def walk_links(scripts, root, max_depth):
    frontier = [root]
    seen = {root}
    for depth in range(max_depth + 1):
        following = []
        for index in frontier:
            yield depth, scripts[index]
            if depth < max_depth:
                for linked in scripts[index]["links"]:
                    if linked not in seen:
                        seen.add(linked)
                        following.append(linked)
        frontier = following


def read_rows(path):
    with path.open(encoding="utf-8-sig", newline="") as stream:
        return list(csv.DictReader(stream))


def audit_assets(cli, out, jobs):
    placements = read_rows(out / "placements.csv")
    entities = sorted({row["entity"] for row in placements if row["entity"]})
    models = {row["model"] for row in placements if row["model"]}

    def entity_assets(entity):
        def probe(command, pattern):
            result = subprocess.run([*cli, command, entity], capture_output=True, encoding="utf-8")
            return result.returncode, sorted(set(re.findall(pattern, result.stdout, re.MULTILINE)))

        fx_status, fx = probe("entity-fx", r"\bsystem ([0-9A-F]{8})\b")
        audio_status, mappings = probe("entity-sounds", r"^ +((?:name|loop) [^\r\n]+)")
        return entity, fx_status, audio_status, "|".join(fx), "|".join(mappings)

    rig_output = subprocess.check_output([*cli, "skeletons"], encoding="utf-8")
    rigs = {}
    for line in rig_output.splitlines():
        match = re.fullmatch(r"(\w+) +bones +\d+ +rig (\w*) *\( *\d+\) +clips +(\d+) +models (.+)", line)
        if match:
            skeleton, rig, clips, tags = match.groups()
            for model in tags.split():
                if model in models:
                    rigs[model] = (skeleton, rig, clips)

    with ThreadPoolExecutor(max_workers=jobs) as pool:
        write_csv(out / "entity-assets.csv", ("entity", "fx_status", "audio_status", "particle_systems",
                  "clip_sound_table"), pool.map(entity_assets, entities))
    write_csv(out / "model-rigs.csv", ("model", "skeleton", "rig", "compatible_clips"),
              ((model, *rigs.get(model, ("", "", 0))) for model in sorted(models)))
    print(f"{len(entities)} entities and {len(models)} models audited")


def audit_clips(cli, out, jobs):
    rigs = {}
    for row in read_rows(out / "model-rigs.csv"):
        if int(row["compatible_clips"]):
            rigs.setdefault((row["skeleton"], row["rig"]), []).append(row["model"])

    def list_clips(models):
        output = subprocess.check_output([*cli, "clipfit", models[0], "-v"], encoding="utf-8")
        return models, re.findall(r"^ +([0-9A-F]{8}) +(\S+) +frames +(\d+)\b", output, re.MULTILINE)

    def read_clip(clip):
        output = subprocess.check_output([*cli, "clip", clip, "0"], encoding="utf-8")
        return clip, re.findall(r"event frame +([0-9]+) kind ([0-9]+) class (\w+) name (\w+) value (\w+)", output)

    with ThreadPoolExecutor(max_workers=jobs) as pool:
        clips_by_model = {model: clips for models, clips in pool.map(list_clips, rigs.values()) for model in models}
        clips = sorted({clip for values in clips_by_model.values() for clip, _, _ in values})
        markers = dict(pool.map(read_clip, clips))
    write_csv(out / "model-clips.csv", ("model", "clip", "codec", "frames"),
              ((model, *clip) for model, values in sorted(clips_by_model.items()) for clip in values))
    write_csv(out / "clip-markers.csv", ("clip", "frame", "kind", "class", "name", "value"),
              ((clip, *marker) for clip, values in markers.items() for marker in values))

    entity_sounds = {
        row["entity"]: dict(re.findall(r"name ([0-9A-F]{8}): ([^|]+)", row["clip_sound_table"]))
        for row in read_rows(out / "entity-assets.csv")
    }
    pairs = sorted({(row["model"], row["entity"]) for row in read_rows(out / "placements.csv")
                    if row["model"] in clips_by_model and row["entity"] in entity_sounds})
    write_csv(out / "clip-audio-matches.csv", ("model", "entity", "clip", "frame", "marker", "audio_events"),
              ((model, entity, clip, frame, name, entity_sounds[entity][name])
               for model, entity in pairs for clip, _, _ in clips_by_model[model]
               for frame, kind, _, name, _ in markers[clip] if kind == "5" and name in entity_sounds[entity]))
    print(f"{len(clips)} compatible clips decoded")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("out", type=Path)
    parser.add_argument("--nimcli", type=Path, default=Path("build/Release/nimcli.exe"))
    parser.add_argument("--install", type=Path)
    parser.add_argument("--jobs", type=int, default=3)
    stage = parser.add_mutually_exclusive_group()
    stage.add_argument("--assets-only", action="store_true", help="reuse placements.csv and scan rig, FX and sound references")
    stage.add_argument("--clips-only", action="store_true", help="reuse the asset reports and decode compatible clips and sound markers")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be at least 1")
    args.out.mkdir(parents=True, exist_ok=True)
    cli = [str(args.nimcli.resolve())]
    if args.install:
        cli.extend(("--install", str(args.install.resolve())))
    if args.assets_only:
        audit_assets(cli, args.out, args.jobs)
        return
    if args.clips_only:
        audit_clips(cli, args.out, args.jobs)
        return

    maps, palette = [], {}
    output = subprocess.check_output([*cli, "map-event-palettes"], encoding="utf-8")
    for line in output.splitlines():
        kind, *fields = line.split("\t")
        if kind == "map":
            maps.append(fields)
        elif kind == "event":
            key = tuple(maps[-1][:3])
            palette.setdefault(key, []).append(fields)

    def load_map(selection):
        bubble, activity, index, *_ = selection
        tag = bubble if activity == "-" else activity
        path = args.out / f"{bubble}-{activity}-{index}.json"
        subprocess.run(
            [*cli, "map-gameplay", tag, "--exact", "--map-index", index, "--json", str(path)],
            stdout=subprocess.DEVNULL,
            check=True,
        )
        return json.loads(path.read_bytes())

    maps_report, components, placements, palette_report = [], [], [], []
    routes, squads = [], []
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for selection, gameplay in zip(maps, pool.map(load_map, maps), strict=True):
            bubble, activity, index, name, count, hidden, hidden_fx = selection
            key = (bubble, activity, index)
            entries = palette.get(key, [])
            if len(entries) != int(count):
                raise ValueError(f"{key}: incomplete palette output")
            palette_report.extend((*key, *row) for row in entries)
            palette_ids = {row[0] for row in entries}
            total_candidates = 0
            placed_candidates = 0
            named_components = 0
            scripts = gameplay["scripts"]
            point_groups = {group["component"]: group for group in gameplay["point_groups"] if group["spline"]}
            squads_by_component = {squad["component"]: squad for squad in gameplay["squads"]}
            for root, script in enumerate(scripts):
                module = script["module"]
                if module.startswith("native "):
                    continue
                named_components += 1
                component = script["component"]
                defined_event = component.zfill(16) in palette_ids
                candidate = defined_event or has_event_name(module)
                if candidate:
                    total_candidates += 1
                points = {}
                if candidate:
                    for depth, linked in walk_links(scripts, root, 3):
                        owner = linked["component"]
                        if depth <= 2:
                            for point in linked["points"]:
                                points.setdefault(point["id"], (depth, owner, point))
                        if group := point_groups.get(owner):
                            for point_index, position in enumerate(group["spline"]):
                                routes.append((*key, component, depth, owner, point_index, *position))
                        if squad := squads_by_component.get(owner):
                            characters = (gameplay["characters"][member]["entity"] for member in squad["members"])
                            nodes = (point["id"] for point in squad["points"])
                            squads.append((*key, component, depth, owner, "|".join(characters), "|".join(nodes)))
                    placed_candidates += bool(points)
                    for depth, owner, point in points.values():
                        placements.append((*key, component, depth, owner, point["id"],
                                           point["entity"], point["model"], *point["p"]))
                components.append((*key, component, script["definition"], module, int(candidate),
                                   int(defined_event), len(script["points"]), len(points) if candidate else "",
                                   len(set(script["links"])), "|".join(script["names"])))
            maps_report.append((*key, name, named_components, total_candidates, placed_candidates, count, hidden, hidden_fx))

    map_key = ("bubble", "activity", "map_index")
    write_csv(args.out / "maps.csv", (*map_key, "name", "script_components", "event_candidates",
              "candidates_with_placements", "palette_entries", "hidden_nodes", "hidden_fx_nodes"), maps_report)
    write_csv(args.out / "components.csv", (*map_key, "component", "definition", "module", "event_candidate",
              "in_palette", "direct_placements", "placements_within_two_links", "linked_components", "field_names"),
              components)
    write_csv(args.out / "placements.csv", (*map_key, "event_component", "link_depth", "owner_component",
              "node", "entity", "model", "x", "y", "z"), placements)
    write_csv(args.out / "routes.csv", (*map_key, "event_component", "link_depth", "route_component",
              "point_index", "x", "y", "z"), routes)
    write_csv(args.out / "squads.csv", (*map_key, "event_component", "link_depth", "squad_component",
              "character_entities", "spawn_nodes"), squads)
    write_csv(args.out / "palette.csv", (*map_key, "id", "name", "placements", "x", "y", "z"), palette_report)
    print(f"{len(maps_report)} map selections, {len(components)} script rows, {len(placements)} placement links")


if __name__ == "__main__":
    main()
