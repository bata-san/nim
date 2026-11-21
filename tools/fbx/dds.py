# raw/<tag>.bin (extract) -> <out>/textures/<tag>.dds, plus materials.json describing parts and textures.
import json, pathlib, struct, sys
import tris
out=pathlib.Path(sys.argv[1]); tex=out/'textures'; tex.mkdir(parents=True,exist_ok=True)
a=tris.load(); C=tris.CFG
for tag,t in a['texture_info'].items():
    data=(tris.WORK/'raw'/(tag+'.bin')).read_bytes()
    fmt,w,h,d,n,m=(t[k] for k in ('format','width','height','depth','array','mips'))
    block={70:8,71:8,72:8,73:16,74:16,75:16,76:16,77:16,78:16,79:8,80:8,81:8,82:16,83:16,84:16,94:16,95:16,96:16,97:16,98:16,99:16}.get(fmt)
    bpp={2:16,10:8,24:4,28:4,29:4,35:4,49:2,56:2,61:1,87:4,88:4,91:4}.get(fmt)
    assert block or bpp, (tag, fmt)
    def size(l):
        x,y,z=(max(1,v>>l) for v in (w,h,d))
        return (((x+3)//4)*((y+3)//4)*block if block else x*y*bpp)*z
    if sum(size(i)*n for i in range(m))!=len(data):
        full=max(w,h,d).bit_length()
        assert sum(size(i)*n for i in range(full))==len(data),(tag,len(data))
        m=full
    flags=0x1|0x2|0x4|0x1000|(0x80000 if block else 0x8)|(0x20000 if m>1 else 0)|(0x800000 if d>1 else 0)
    caps=0x1000|(0x400008 if m>1 else 0)|(8 if d>1 or n==6 else 0)
    cube=n==6 and d==1
    caps2=0xFE00 if cube else 0x200000 if d>1 else 0
    pitch=size(0)//d if block else w*bpp
    dds=b'DDS '+struct.pack('<31I',124,flags,h,w,pitch,d if d>1 else 0,m,*([0]*11),32,4,struct.unpack('<I',b'DX10')[0],0,0,0,0,0,caps,caps2,0,0,0)
    dds+=struct.pack('<5I',fmt,4 if d>1 else 3,4 if cube else 0,1 if cube else n,0)
    (tex/(tag+'.dds')).write_bytes(dds+data)
    t.update(file=f'textures/{tag}.dds',stored_mips=m)
parts=[{k:v for k,v in p.items() if k not in ('indices','triangles','primitives','odd')}|{'triangle_count':len(p['triangles']),'alpha_mask':p['part'] in C.get('alpha_parts',[])} for p in a['parts']]
(out/'materials.json').write_text(json.dumps({'model':C['model'],'skeleton':C['skeleton'],'rig':C['rig'],'clip':C['clip'],
 'permutation':a['permutation'],'permutation_count':a['permutation_count'],'parts':parts,'textures':a['texture_info'],
 'baked_textures':{'method':'texture-space bake: the original VS and PS draw every opaque triangle into a 4096x4096 atlas (a geometry shader moves each triangle to BakedUV); 2-texel edge dilation, then nearest-texel fill',
  'uv_set':'BakedUV (GameUV keeps the original texcoords)',
  'opaque_base_color.png':'G-buffer RT0 albedo (sRGB); alpha = pixels the original PS wrote (alpha_mask parts discard)',
  'opaque_roughness.png':'1 - smoothness (RT1 normal length)','opaque_metallic.png':'RT2.r','opaque_occlusion.png':'RT2.g below 0.5, x2',
  'opaque_emission_color.png':'albedo where RT2.g encodes emission','opaque_emission_strength.png':'16-bit, decoded emission / 64 (material multiplies by 64)',
  'opaque_normal_world_reference.png':'RT1 world normal at bind pose (reference only, not wired)'}},indent=2))
print('dds',len(a['texture_info']))
