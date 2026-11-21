# Top-down overview of an activity's gameplay placements (docs/PROGRESS.md 3.65).
#
#   ./build/Release/nimcli.exe map-gameplay 80AFB43D --json out/gameplay_outpost.json
#   python tools/re/plot_gameplay.py out/gameplay_outpost.json out/gameplay_outpost.png
#
# Grey: the map's static placements. Left: spawn points coloured by archetype (the entity the node
# names). Middle: loot containers by zone label, script objects labelled with their module name.
# Right: squad spawn positions (3.66) coloured by the squad's first character (names are 推定),
# firing areas (orange) and point-group splines (blue, e.g. scan drone paths) (3.67).
import json
import sys
from collections import Counter, defaultdict

import matplotlib

matplotlib.use('Agg')
import matplotlib.pyplot as plt

src, dst = sys.argv[1], sys.argv[2]
d = json.load(open(src))
fig, axes = plt.subplots(1, 3, figsize=(33, 11), dpi=100)

statics = d['statics']
xs = [p['p'][0] for p in d['spawns'] + d['loot']]
ys = [p['p'][1] for p in d['spawns'] + d['loot']]
pad = 40
box = (min(xs) - pad, max(xs) + pad, min(ys) - pad, max(ys) + pad)


def background(ax, title):
    ax.scatter([p[0] for p in statics], [p[1] for p in statics], s=0.2, c='#c8c8c8', linewidths=0)
    ax.set_xlim(box[0], box[1])
    ax.set_ylim(box[2], box[3])
    ax.set_aspect('equal')
    ax.set_title(title)
    ax.set_xlabel('x (m)')
    ax.set_ylabel('y (m)')


# Left: spawn points by archetype.
ax = axes[0]
by_entity = defaultdict(list)
for s in d['spawns']:
    by_entity[s['entity']].append(s)
background(ax, f"{d['activity']}: spawn points ({len(d['spawns'])}) by archetype entity")
cmap = plt.get_cmap('tab20')
for i, (e, pts) in enumerate(sorted(by_entity.items(), key=lambda kv: -len(kv[1]))):
    ax.scatter([p['p'][0] for p in pts], [p['p'][1] for p in pts], s=9, color=cmap(i % 20), label=f'{e} ({len(pts)})')
ax.legend(loc='upper left', fontsize=7, markerscale=2)

# Right: loot containers by zone label, script objects by module.
ax = axes[1]
background(ax, f"loot containers ({len(d['loot'])}) by zone label, script objects ({len(d['scripts'])})")
zones = Counter(p['labels'][0] if p['labels'] else '-' for p in d['loot'])
for i, z in enumerate(sorted(zones, key=lambda z: -zones[z])):
    pts = [p for p in d['loot'] if (p['labels'][0] if p['labels'] else '-') == z]
    ax.scatter([p['p'][0] for p in pts], [p['p'][1] for p in pts], s=6, marker='s', color=cmap(i % 20), label=f'zone {z} ({len(pts)})')
seen = set()
for o in [o for o in d['scripts'] if not o['module'].startswith('native')]:
    name = o['module'].replace('\\', '/').split('/')[-1].replace('.bs', '').replace('.es', '')
    for p in o['points'][:1]:
        x, y = p['p'][0], p['p'][1]
        if abs(x) < 1e-3 and abs(y) < 1e-3:
            continue
        ax.plot(x, y, marker='*', color='black', markersize=7)
        if (name, round(x / 20), round(y / 20)) not in seen:
            seen.add((name, round(x / 20), round(y / 20)))
            ax.annotate(name, (x, y), fontsize=5.5, xytext=(3, 3), textcoords='offset points')
ax.legend(loc='upper left', fontsize=7, markerscale=2)

# Right: squads by character, over their firing areas (3.67) and point-group paths.
ax = axes[2]
squads = d.get('squads', [])
chars = d.get('characters', [])
areas = d.get('firing_areas', [])
background(ax, f"squads ({len(squads)}) by first character, firing areas ({len(areas)}), paths")
from matplotlib.patches import Polygon
for fa in areas:
    ax.add_patch(Polygon([v[:2] for v in fa['outline']], closed=True, facecolor='#ffd27f', edgecolor='#d08a00', linewidth=0.4, alpha=0.5))
for pg in d.get('point_groups', []):
    if len(pg['spline']) > 1:
        ax.plot([v[0] for v in pg['spline']], [v[1] for v in pg['spline']], color='#3070d0', linewidth=1.0)
by_char = defaultdict(list)
for sq in squads:
    if sq['members'] and sq['points']:
        c = chars[sq['members'][0]]
        by_char[c['name'] or c['item']].append(sq)
for i, (name, sqs) in enumerate(sorted(by_char.items(), key=lambda kv: -len(kv[1]))):
    pts = [p for sq in sqs for p in sq['points']]
    ax.scatter([p['p'][0] for p in pts], [p['p'][1] for p in pts], s=10, color=cmap(i % 20), label=f'{name} ({len(sqs)} squads)')
ax.legend(loc='upper left', fontsize=7, markerscale=2)
fig.tight_layout()
fig.savefig(dst)
print('wrote', dst)
