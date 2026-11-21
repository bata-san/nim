import bpy
import json
import math
import pathlib
import sys
from mathutils import Matrix, Quaternion, Vector
bpy.ops.wm.read_factory_settings(use_empty=True)
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parent))
import tris
a=tris.load()
fbx=pathlib.Path(sys.argv[sys.argv.index('--')+1])
bpy.ops.import_scene.fbx(filepath=str(fbx),use_custom_normals=True,use_image_search=False,colors_type='LINEAR')
meshes=sorted([ob for ob in bpy.context.scene.objects if ob.type=='MESH'],key=lambda ob:ob.name)
rig,=[ob for ob in bpy.context.scene.objects if ob.type=='ARMATURE']
C=tris.CFG
n_tri=sum(len(p['triangles']) for p in a['parts'])
n_mesh=1+sum(p['stage']!=0 for p in a['parts'])
assert len(meshes)==n_mesh,(len(meshes),n_mesh)
# Blender's FBX importer merges faces that repeat the same three vertices; the file keeps them all.
dup=0
for sub_ in a['parts']:
    pass
_seen={}
for sub_ in a['parts']:
    key=sub_['stage']!=0 and sub_['part']
    for t_ in sub_['triangles']:
        k_=(key,frozenset(t_))
        dup+=k_ in _seen; _seen[k_]=1
assert sum(len(ob.data.polygons) for ob in meshes)==n_tri-dup,(sum(len(ob.data.polygons) for ob in meshes),n_tri,dup)
assert len(rig.data.bones)==len(a['bones'])
assert len(set(m.name for ob in meshes for m in ob.data.materials))==len(a['parts'])
assert all(len(p.vertices)==3 for ob in meshes for p in ob.data.polygons)
assert all(ob.data.has_custom_normals for ob in meshes)
assert all('GameUV' in ob.data.uv_layers for ob in meshes)
assert all('GameTangentPacked' in ob.data.color_attributes for ob in meshes)
assert all(len(v.groups)>=1 and abs(sum(g.weight for g in v.groups)-1)<1e-3 for ob in meshes for v in ob.data.vertices)
assert all(any(m.type=='ARMATURE' and m.object==rig for m in ob.modifiers) for ob in meshes)
imgs={pathlib.Path(i.filepath).name:i for i in bpy.data.images}
for n in ('opaque_base_color.png','opaque_roughness.png','opaque_metallic.png','opaque_emission_color.png'):
    assert n in imgs and tuple(imgs[n].size)==(4096,4096), (n, list(imgs))
surface=next(ob for ob in meshes if ob.name.endswith('_Surface'))
assert list(surface.data.uv_layers.keys())==['BakedUV','GameUV']
a0=rig.animation_data.action
lo,hi=a0.frame_range
nf=len(a['frames'])
assert abs(hi-lo-(nf-1))<1e-3, a0.frame_range
assert C['clip'] in a0.name,a0.name
def m(t):
    q,p,s=t
    return Matrix.LocRotScale(Vector(p),Quaternion((q[3],*q[:3])).normalized(),(s,s,s))
names=[b['name'] for b in a['bones']]
parents=[b['parent'] for b in a['bones']]
max_anim_error=0
for frame in sorted({0,nf//4,nf//2,3*nf//4,nf-1}):
    bpy.context.scene.frame_set(round(lo)+frame)
    world=[]
    for i,t in enumerate(a['frames'][frame]):
        expected=m(t)
        if parents[i]>=0:
            expected=world[parents[i]] @ expected
        world.append(expected)
        actual=rig.matrix_world @ rig.pose.bones[names[i]].matrix
        max_anim_error=max(max_anim_error,(actual.translation-expected.translation).length)
assert max_anim_error<0.001,max_anim_error
bpy.context.scene.frame_set(round(lo))
opaque=[sub for sub in a['parts'] if sub['stage']==0]
verts_src=[a['vertices'][k][0] for k in {i for s in opaque for f in s['triangles'] for i in f}]
verts_actual=[surface.matrix_world @ v.co for v in surface.data.vertices]
bounds_error=max(abs(fn(v[i] for v in verts_src)-fn(v[i] for v in verts_actual))
                 for i in range(3) for fn in (min,max))
assert bounds_error<0.001,bounds_error
print(f'verified FBX: {n_tri} triangles ({dup} same-vertex duplicates merged by the Blender importer), {n_mesh} meshes, {len(a["parts"])} materials, both UV sets, custom normals, {len(a["bones"])} bones and skin, {nf} frames')
print(f'bounds max error {bounds_error:.7g} m, sampled animation bone-position error {max_anim_error:.7g} m, textures {len(bpy.data.images)}')


# preview render of the imported FBX
scene=bpy.context.scene
scene.frame_set(round(lo))
scene.render.engine='BLENDER_EEVEE_NEXT'
scene.render.resolution_x=scene.render.resolution_y=1024
world=bpy.data.worlds.new('w'); world.use_nodes=True; world.node_tree.nodes['Background'].inputs[1].default_value=1.0
world.node_tree.nodes['Background'].inputs[0].default_value=(0.6,0.6,0.6,1); scene.world=world
for ob in meshes:
    if '_FX_' in ob.name: ob.hide_render=True
sun=bpy.data.objects.new('sun',bpy.data.lights.new('sun','SUN')); sun.data.energy=3; sun.rotation_euler=(0.8,0.2,0.6); scene.collection.objects.link(sun)
cam=bpy.data.objects.new('cam',bpy.data.cameras.new('cam')); scene.collection.objects.link(cam); scene.camera=cam
import mathutils
for name,eye in C['preview_eyes']:
    cam.location=eye
    d=mathutils.Vector(C['look'])-cam.location
    cam.rotation_euler=d.to_track_quat('-Z','Y').to_euler()
    scene.render.filepath=str(fbx.parent/f'preview_{name}.png')
    bpy.ops.render.render(write_still=True)
print('preview rendered')
