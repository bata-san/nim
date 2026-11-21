import json, pathlib, sys, numpy as np
from PIL import Image
import tris
root=pathlib.Path(sys.argv[1]); S=int(sys.argv[2])
names=['gbuffer0_albedo','gbuffer1_smoothness','gbuffer2','gbuffer1_normal']
best=np.zeros((S,S),np.float32); out={n:np.zeros((S,S,3),np.uint8) for n in names}
for d in sorted(root.glob('pass*')):
    if not d.is_dir(): continue
    z=np.fromfile(d/'lighting-depth.bin','<f4').reshape(S,S)
    take=z>best+1e-3
    best[take]=z[take]
    for n in names:
        im=np.asarray(Image.open(d/(n+'.png')).convert('RGB'))
        out[n][take]=im[take]
print('depth values',np.unique(np.round(best,3))[:10])
exact=best>0.5; any_=best>0
# per-face exact coverage
a=tris.load(); uv=json.load(open('uv.json')); off=0; total_miss=0
for sub in a['parts']:
    if sub['stage']!=0: continue
    n=len(sub['triangles']); miss=0
    for t in uv[off:off+n]:
        t=np.array(t); pts=[t.mean(0)]+[0.8*x+0.2*t.mean(0) for x in t]
        if not any(any_[min(S-1,int((1-p[1])*S)),min(S-1,int(p[0]*S))] for p in pts): miss+=1
    print('part',sub['part'],'faces',n,'missed',miss); total_miss+=miss; off+=n
np.save(root/'coverage.npy',best)
for n in names: Image.fromarray(out[n]).save(root/(n+'.png'))
print('exact px',exact.sum(),'any px',any_.sum())
