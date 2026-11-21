# Composite G-buffer atlas -> material textures (nearest-texel dilation into the gutters).
import pathlib, sys, numpy as np
from PIL import Image
from scipy import ndimage
root=pathlib.Path(sys.argv[1]); out=pathlib.Path(sys.argv[2]); out.mkdir(parents=True,exist_ok=True)
EM_SCALE=64.0
cov=np.load(root/'coverage.npy')>0
_,idx=ndimage.distance_transform_edt(~cov,return_indices=True)
def fill(a): return a[idx[0],idx[1]]
def img(n,mode='RGB'): return np.asarray(Image.open(root/(n+'.png')).convert(mode))
alb=fill(img('gbuffer0_albedo'))
smooth=fill(img('gbuffer1_smoothness','L')).astype(np.float32)/255
g2=fill(img('gbuffer2')).astype(np.float32)/255
metal=g2[...,0]
G=g2[...,1]; enc=np.where(G>=0.5,2*G-1,0)
em=np.where(G>=0.5,np.maximum(0,2**((enc-0.538)/0.0769)-1/128),0)
occl=np.where(G>=0.5,1,np.clip(G*2,0,1))
alpha=(cov*255).astype(np.uint8)
Image.fromarray(np.dstack([alb,alpha])).save(out/'opaque_base_color.png')
Image.fromarray(((1-smooth)*255+0.5).astype(np.uint8)).save(out/'opaque_roughness.png')
Image.fromarray((metal*255+0.5).astype(np.uint8)).save(out/'opaque_metallic.png')
Image.fromarray((occl*255+0.5).astype(np.uint8)).save(out/'opaque_occlusion.png')
emc=np.where((em>0)[...,None],alb,0).astype(np.uint8)
Image.fromarray(emc).save(out/'opaque_emission_color.png')
Image.fromarray(np.clip(em/EM_SCALE*65535+0.5,0,65535).astype(np.uint16)).save(out/'opaque_emission_strength.png')
Image.fromarray(fill(img('gbuffer1_normal'))).save(out/'opaque_normal_world_reference.png')
print('textures', alb.shape, 'emissive px', int((em>0).sum()), 'max emission', float(em.max()), 'metal px', int((metal>0.01).sum()))
