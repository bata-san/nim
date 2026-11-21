# Debug view of the estimated AI simulation (docs/PROGRESS.md 3.69).
#
#   ./build/Release/nimcli.exe map-gameplay 80AFB43D --json out/gameplay_outpost.json
#   ./build/Release/nimcli.exe ai-sim 80AFB43D --seconds 120 --player 150,-40,-12 --json out/ai_outpost.json
#   python tools/re/plot_ai_sim.py out/gameplay_outpost.json out/ai_outpost.json out/ai_outpost.png
#
# Left: firing areas (orange), drone splines (blue), every agent's trail over the run (grey) and
# its final position and facing, coloured by alert (green unaware, yellow suspicious, red alerted),
# with the sight cone of each agent that ended aware of the player. The player (camera) is the
# black star. Right: awareness of the player over time per agent (the indicator's value), with the
# suspicious / alerted thresholds. All behaviour is 推定.
import json
import math
import sys

import matplotlib

matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import Polygon, Wedge

gp, sim_path, dst = sys.argv[1], sys.argv[2], sys.argv[3]
g = json.load(open(gp))
s = json.load(open(sim_path))
frames = s['frames']
colors = ['#2a9d4a', '#e0b000', '#d03030']

fig, (ax, ax2) = plt.subplots(1, 2, figsize=(24, 11), dpi=100, gridspec_kw={'width_ratios': [3, 2]})
ax.scatter([p[0] for p in g['statics']], [p[1] for p in g['statics']], s=0.2, c='#c8c8c8', linewidths=0)
for fa in g.get('firing_areas', []):
    ax.add_patch(Polygon([v[:2] for v in fa['outline']], closed=True, facecolor='#ffd27f', edgecolor='#d08a00', linewidth=0.4, alpha=0.45))
for pg in g.get('point_groups', []):
    if len(pg['spline']) > 1:
        ax.plot([v[0] for v in pg['spline']], [v[1] for v in pg['spline']], color='#3070d0', linewidth=1.0)

n = len(frames[-1]['agents'])
for k in range(n):
    ax.plot([f['agents'][k][0] for f in frames], [f['agents'][k][1] for f in frames], color='#606060', linewidth=0.5, alpha=0.6)
last = frames[-1]
xs, ys = [], []
for k, a in enumerate(last['agents']):
    x, y, yaw, aw, alert = a[0], a[1], a[3], a[4], a[5]
    xs.append(x), ys.append(y)
    ax.plot([x, x + 2.5 * math.cos(yaw)], [y, y + 2.5 * math.sin(yaw)], color='k', linewidth=0.6)
    ax.scatter([x], [y], s=14, c=colors[alert], edgecolors='k', linewidths=0.3, zorder=3)
    if alert:
        sight = s['squads'][k][2]
        ax.add_patch(Wedge((x, y), sight, math.degrees(yaw) - 60, math.degrees(yaw) + 60, facecolor=colors[alert], alpha=0.12))
for d in last['drones']:
    ax.scatter([d[0]], [d[1]], s=40, marker='D', c=colors[d[5]], edgecolors='k', linewidths=0.4, zorder=3)
px, py = s['player'][0], s['player'][1]
ax.scatter([px], [py], s=220, marker='*', c='k', zorder=4)
pad = 40
xs += [px]
ys += [py]
ax.set_xlim(min(xs) - pad, max(xs) + pad)
ax.set_ylim(min(ys) - pad, max(ys) + pad)
ax.set_aspect('equal')
ax.set_title(f"AI simulation (estimated rules) at t={last['t']:.0f}s: {n} agents, {len(last['drones'])} drones; trails grey, alert green/yellow/red")
ax.set_xlabel('x (m)')
ax.set_ylabel('y (m)')

t = [f['t'] for f in frames]
for k in range(n):
    series = [f['agents'][k][4] for f in frames]
    if max(series) > 0:
        ax2.plot(t, series, linewidth=0.8)
for k in range(len(last['drones'])):
    series = [f['drones'][k][4] for f in frames]
    if max(series) > 0:
        ax2.plot(t, series, linewidth=1.2, linestyle='--')
ax2.axhline(0.35, color=colors[1], linewidth=0.8)
ax2.axhline(1.0, color=colors[2], linewidth=0.8)
ax2.set_ylim(-0.02, 1.05)
ax2.set_title('awareness of the player (indicator value) for agents that noticed anything')
ax2.set_xlabel('t (s)')
fig.tight_layout()
fig.savefig(dst)
print('wrote', dst)
