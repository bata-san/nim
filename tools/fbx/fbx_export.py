# Export a skinned dynamic model with one animation clip to FBX, its opaque materials baked from the
# game's own shaders.
#
#   python tools/fbx/fbx_export.py MODEL CLIP [--out out/fbx] [--eye=x,y,z --look=x,y,z]
#                                  [--size 4096] [--margin 0.002] [--fx-names 23=VolumeInner,...]
#
# Run from the repository root after `cmake --build build --config Release`. Needs Blender 4.5 and
# the Windows SDK dxc.exe. Writes out/fbx/MODEL/ (game data: never commit) and works in
# out/fbx-work/MODEL/.
#
# How the bake works: nimshade draws every opaque part with its original VS and PS, but a geometry
# shader generated here (gen_gs.py) moves each triangle to its place in a Blender Smart UV atlas,
# so the G-buffer the original PS writes *is* the texture. The GS looks the atlas triangle up by
# SV_PrimitiveID in a constant table; tables over 2048 primitives read back wrong, so the bake runs
# in passes of 2048 primitives and composite.py merges them by depth (exact triangle 0.9, its
# 2-texel dilated copy 0.1).
import argparse, json, math, os, pathlib, re, shutil, subprocess, sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
TOOLS = pathlib.Path(__file__).resolve().parent
NIMSHADE = ROOT / 'build/Release/nimshade.exe'
EXTRACT = ROOT / 'build/Release/nimfbx_extract.exe'
BLENDER = pathlib.Path(os.environ.get('BLENDER', r'C:/Program Files/Blender Foundation/Blender 4.5/blender.exe'))
DXC = pathlib.Path(os.environ.get('DXC', r'C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/dxc.exe'))
CHUNK = 2048

ap = argparse.ArgumentParser()
ap.add_argument('model'); ap.add_argument('clip')
ap.add_argument('--out', default=str(ROOT / 'out/fbx'))
ap.add_argument('--eye'); ap.add_argument('--look')
ap.add_argument('--size', type=int, default=4096)
ap.add_argument('--margin', type=float, default=0.002)
ap.add_argument('--pad', type=float, default=2)
ap.add_argument('--fx-names', default='')
args = ap.parse_args()
model, clip = args.model.upper(), args.clip.upper()
work = ROOT / 'out/fbx-work' / model
out = pathlib.Path(args.out) / model
work.mkdir(parents=True, exist_ok=True)
(work / 'gs').mkdir(exist_ok=True)


def run(cmd, env=None, cwd=work, log=None):
    r = subprocess.run([str(c) for c in cmd], cwd=cwd, env={**os.environ, **(env or {})},
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
    if log:
        (work / log).write_text(r.stdout)
    if r.returncode:
        sys.exit(f'failed ({r.returncode}): {" ".join(map(str, cmd))}\n{r.stdout[-3000:]}')
    return r.stdout


def blender(script, *extra, log):
    text = run([BLENDER, '-b', '--factory-startup', '--python', TOOLS / script, '--', *extra], log=log)
    if 'Traceback' in text:
        sys.exit(f'{script} failed:\n{text[-3000:]}')
    return text


print('probe', model, clip, flush=True)
probe = run([NIMSHADE, model, work / 'probe', '--size', '256x256', '--clip', clip, '--no-shadows'], log='probe.log')
skeleton, rig = re.search(r'animation skeleton (\w+) rig (\w+)', probe).groups()
center = [float(x) for x in re.search(r'bounds center \(([^)]*)\) radius', probe).group(1).split(',')]
radius = float(re.search(r'bounds center \([^)]*\) radius ([\d.]+)', probe).group(1))
dump = run([NIMSHADE, model, work / 'dump', '--size', '256x256', '--no-shadows'],
           env={'NIM_BAKE_GS_DIR': str(work / 'gs'), 'NIM_BAKE_DUMP_ONLY': '1'}, log='dump.log')
opaque = [int(x) for x in re.findall(r'bake part (\d+) topology 5', dump)]
if len(opaque) != len(re.findall(r'bake part ', dump)):
    sys.exit('only strip topology is supported')
alpha = []
for p in opaque:
    for stage in ('vs', 'ps'):
        text = run([DXC, '-dumpbin', work / f'gs/{p}.{stage}.dxil'])
        (work / f'gs/{p}.{stage}.txt').write_text(text)
        if stage == 'ps' and 'dx.op.discard' in text:
            alpha.append(p)

print('extract', model, 'skeleton', skeleton, 'rig', rig, flush=True)
run([EXTRACT, model, skeleton, rig, clip], log='extract.log')
asset = json.loads((work / 'asset.json').read_text())
fx = [p['part'] for p in asset['parts'] if p['stage'] != 0]
look = [float(x) for x in args.look.split(',')] if args.look else center
eye = [float(x) for x in args.eye.split(',')] if args.eye else \
    [c + d * radius * 1.6 for c, d in zip(look, (0.55, -0.7, 0.45))]
far = [c + d * radius * 1.6 for c, d in zip(look, (-0.6, 0.65, 0.35))]
config = {'model': model, 'skeleton': skeleton, 'rig': rig, 'clip': clip, 'parts': opaque + fx,
          'alpha_parts': alpha, 'eye': eye, 'look': look,
          'fx_purpose': dict(x.split('=') for x in args.fx_names.split(',') if x),
          'preview_eyes': [['a', eye], ['b', far]]}
(work / 'config.json').write_text(json.dumps(config, indent=1))

print('unwrap', flush=True)
print(blender('unwrap.py', args.margin, 'uv.json', log='unwrap.log').strip().splitlines()[-1])
sys.path.insert(0, str(TOOLS))
os.chdir(work)
import tris
passes = max(math.ceil(len(p['primitives']) / CHUNK) for p in tris.load()['parts'] if p['stage'] == 0)
bake = work / f'b{args.size}'
for k in range(passes):
    print('bake pass', k + 1, 'of', passes, flush=True)
    run([sys.executable, TOOLS / 'gen_gs.py', args.size, args.pad, k, bake / f'gs{k}'])
    for f in (work / 'gs').glob('*.?s.dxil'):
        shutil.copy(f, bake / f'gs{k}')
    run([NIMSHADE, model, bake / f'pass{k}', '--size', f'{args.size}x{args.size}',
         '--eye', ','.join(map(str, eye)), '--look', ','.join(map(str, look)), '--no-taa', '--no-aa', '--no-ssr',
         '--no-auto-exposure', '--no-shadows', '--no-environment', '--dump-lighting'],
        env={'NIM_BAKE_GS_DIR': str(bake / f'gs{k}')}, log=f'pass{k}.log')
print(run([sys.executable, TOOLS / 'composite.py', bake, args.size]))
print(run([sys.executable, TOOLS / 'finalize.py', bake, bake / 'tex']))

if out.exists():
    shutil.rmtree(out)
print(run([sys.executable, TOOLS / 'dds.py', out]))
for f in (bake / 'tex').glob('*.png'):
    shutil.copy(f, out / 'textures')
print(blender('export_fbx.py', out, log='export.log').strip().splitlines()[-1])
print(blender('verify_fbx.py', out / f'{model}.fbx', log='verify.log').strip().splitlines()[-4:])
ref = run([NIMSHADE, model, work / 'reference', '--size', '1024x1024', '--eye', ','.join(map(str, eye)),
           '--look', ','.join(map(str, look))], log='reference.log')
shutil.copy(work / 'reference/gbuffer0_albedo.png', out / 'preview_game_albedo.png')
shutil.copy(work / 'reference/composite_game.png', out / 'preview_game_renderer.png')
print('wrote', out)
