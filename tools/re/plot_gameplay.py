# Top-down overview of an activity's gameplay placements (docs/PROGRESS.md 3.65).
#
#   ./build/Release/nimcli.exe map-gameplay 80AFB43D --json out/gameplay_outpost.json
#   python tools/re/plot_gameplay.py out/gameplay_outpost.json out/gameplay_outpost.png
#
# Grey: the map's static placements. Spawn points are coloured by archetype (the entity the node
# names), loot containers are small squares, script objects are labelled with their module name.
import json
import sys
from collections import Counter, defaultdict

import matplotlib

matplotlib.use('Agg')
import matplotlib.pyplot as plt

src, dst = sys.argv[1], sys.argv[2]
d = json.load(open(src))
fig, axes = plt.subplots(1, 2, figsize=(22, 11), dpi=110)

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
for o in d['scripts']:
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
fig.tight_layout()
fig.savefig(dst)
print('wrote', dst)
