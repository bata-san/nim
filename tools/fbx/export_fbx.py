import bpy
from array import array
import json
import math
import pathlib
import sys
from mathutils import Matrix, Quaternion, Vector

output=pathlib.Path(sys.argv[sys.argv.index('--')+1])
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parent))
import tris
a=tris.load()
baked_uv=json.loads(pathlib.Path('uv.json').read_text())
C=tris.CFG
M=C['model']
ALPHA_PARTS=set(C['alpha_parts'])  # original PS discards (alpha test)
opaque_parts=[p for p in a['parts'] if p['stage']==0]
at=0
for p in opaque_parts:
    p['uv_offset']=at; at+=len(p['triangles'])
bpy.ops.wm.read_factory_settings(use_empty=True)
scene=bpy.context.scene
scene.unit_settings.system='METRIC'
scene.render.fps=30
scene.frame_start=1
scene.frame_end=len(a['frames'])
arm=bpy.data.armatures.new(C['skeleton'])
rig=bpy.data.objects.new(M+'_Rig',arm)
scene.collection.objects.link(rig)
rig.show_in_front=True
rig['GameSkeleton']=C['skeleton']
rig['GameRig']=C['rig']
bpy.context.view_layer.objects.active=rig
rig.select_set(True)

def transform(data):
    q,loc,s=data
    return Matrix.LocRotScale(Vector(loc),Quaternion((q[3],*q[:3])).normalized(),(s,s,s))

parents=[x['parent'] for x in a['bones']]
names=[x['name'] for x in a['bones']]
bpy.ops.object.mode_set(mode='EDIT')
for i,b in enumerate(a['bones']):
    j=arm.edit_bones.new(names[i])
    j.tail=(0,0.2,0)
    if parents[i]>=0:
        j.parent=arm.edit_bones[names[parents[i]]]
    j.matrix=transform(b['bind'])
bpy.ops.object.mode_set(mode='OBJECT')
poses=[rig.pose.bones[n] for n in names]
rest=[arm.bones[n].matrix_local.copy() for n in names]
local_inverse=[(rest[p].inverted() @ rest[i] if p>=0 else rest[i]).inverted() for i,p in enumerate(parents)]
for i,pb in enumerate(poses):
    pb.rotation_mode='QUATERNION'

def image(path,data=False):
    img=bpy.data.images.load(str(output/'textures'/path),check_existing=True)
    if data:
        img.colorspace_settings.name='Non-Color'
    return img

base=image('opaque_base_color.png')
rough=image('opaque_roughness.png',True)
metal=image('opaque_metallic.png',True)
emc=image('opaque_emission_color.png')
ems=image('opaque_emission_strength.png',True)

def mat(sub):
    fx=sub['stage']==8
    id=sub['part']
    m=bpy.data.materials.new(('FX' if fx else 'Surface')+f'_{id:02d}_'+sub['technique'])
    m.use_nodes=True
    bs=m.node_tree.nodes.get('Principled BSDF')
    m['GameTechnique']=sub['technique']
    m['GamePart']=id
    m['GameRenderStage']=sub['stage']
    m['OriginalTextureSlots']=json.dumps(sub['textures'])
    if fx:
        # The two meshes are bounds for volumetric display, not a visible surface.
        m.diffuse_color=(0.29,0.37,0.42,0.0 if id in C.get('fx_invisible',[]) else 0.28)
        bs.inputs['Base Color'].default_value=m.diffuse_color
        bs.inputs['Alpha'].default_value=m.diffuse_color[3]
        bs.inputs['Roughness'].default_value=0.2
        m['PreviewMode']='Invisible volume proxy' if id in C.get('fx_invisible',[]) else 'Approximate transmission'
    else:
        nt=m.node_tree
        def tex(img):
            node=nt.nodes.new('ShaderNodeTexImage'); node.image=img
            node.interpolation='Linear'
            return node
        b=tex(base); nt.links.new(b.outputs['Color'],bs.inputs['Base Color'])
        nt.links.new(tex(rough).outputs['Color'],bs.inputs['Roughness'])
        nt.links.new(tex(metal).outputs['Color'],bs.inputs['Metallic'])
        nt.links.new(tex(emc).outputs['Color'],bs.inputs['Emission Color'])
        mul=nt.nodes.new('ShaderNodeMath'); mul.operation='MULTIPLY'; mul.inputs[1].default_value=64.0
        nt.links.new(tex(ems).outputs['Color'],mul.inputs[0]); nt.links.new(mul.outputs[0],bs.inputs['Emission Strength'])
        if id in ALPHA_PARTS:
            nt.links.new(b.outputs['Alpha'],bs.inputs['Alpha'])
            m.surface_render_method='DITHERED'
            m['AlphaMode']='Mask (original pixel shader discards)'
        m['PreviewMode']='Texture-space bake of the original pixel shader G-buffer'
    return m

def create_mesh(name,subsets):
    rows=a['vertices']
    ids=sorted({v for sub in subsets for poly in sub['triangles'] for v in poly})
    remap={v:n for n,v in enumerate(ids)}
    poly=[tuple(remap[id] for id in t) for sub in subsets for t in sub['triangles']]
    mesh=bpy.data.meshes.new(name)
    obj=bpy.data.objects.new(name,mesh)
    scene.collection.objects.link(obj)
    mesh.from_pydata([rows[i][0] for i in ids],[],poly)
    mesh.update()
    assert len(mesh.polygons)==len(poly)
    opaque=subsets[0]['stage']==0
    if opaque:
        mesh.uv_layers.new(name='BakedUV')
    mesh.uv_layers.new(name='GameUV')
    for i,vertex in enumerate(mesh.vertices):
        assert vertex.index==i
    normals=[Vector(rows[i][1]).normalized() for i in ids]
    mesh.normals_split_custom_set_from_vertices(normals)
    skin=[obj.vertex_groups.new(name=n) for n in names]
    for i,src in enumerate(ids):
        for bone,weight in zip(rows[src][4],rows[src][5]):
            if weight>0:
                skin[bone].add([i],weight,'REPLACE')
    tangent=mesh.color_attributes.new(name='GameTangentPacked',type='FLOAT_COLOR',domain='POINT')
    for i,src in enumerate(ids):
        tangent.data[i].color=tuple(0.5+v*0.5 for v in rows[src][3])
    modifier=obj.modifiers.new('GameSkin','ARMATURE')
    modifier.object=rig
    obj.parent=rig
    at=0
    raw_uv=array('f',[0])*(len(mesh.loops)*2)
    new_uv=array('f',[0])*(len(mesh.loops)*2)
    for si,sub in enumerate(subsets):
        mesh.materials.append(mat(sub))
        for j,tri in enumerate(sub['triangles']):
            p=mesh.polygons[at]
            p.material_index=si
            p.use_smooth=True
            for k,l in enumerate(p.loop_indices):
                src=ids[mesh.loops[l].vertex_index]
                u,v=rows[src][2]
                raw_uv[2*l:2*l+2]=array('f',(u,1-v))
                if opaque:
                    new_uv[2*l:2*l+2]=array('f',baked_uv[sub['uv_offset']+j][k])
            at+=1
    mesh.uv_layers['GameUV'].uv.foreach_set('vector',raw_uv)
    if opaque:
        mesh.uv_layers['BakedUV'].uv.foreach_set('vector',new_uv)
        mesh.uv_layers.active_index=0
        mesh.uv_layers['BakedUV'].active_render=True
    print('Created',name,len(poly),flush=True)
    obj['GameModel']=M
    obj['GameLOD']='Highest (mask bit 0)'
    if not opaque:
        obj['GamePart']=subsets[0]['part']
    return obj

create_mesh(M+'_Surface',[p for p in a['parts'] if p['stage']==0])
for p in a['parts']:
    if p['stage']!=0:
        purpose=C.get('fx_purpose',{}).get(str(p['part']),'Transparent')
        create_mesh(f"{M}_FX_{p['part']:02d}_{purpose}",[p])

previous_rot=[None]*len(poses)
for i,f in enumerate(a['frames'],start=1):
    for j,(bone,t) in enumerate(zip(poses,f)):
        basis=local_inverse[j] @ transform(t)
        loc,rotation,scale=basis.decompose()
        if previous_rot[j] and rotation.dot(previous_rot[j])<0:
            rotation.negate()
        previous_rot[j]=rotation.copy()
        bone.location=loc
        bone.rotation_quaternion=rotation
        bone.scale=scale
        for prop in ('location','rotation_quaternion','scale'):
            bone.keyframe_insert(prop, frame=i,group=bone.name)
action=rig.animation_data.action
action.name=C['clip']
scene.name=C['clip']  # FBX take name
for curve in action.fcurves:
    for key in curve.keyframe_points:
        key.interpolation='LINEAR'
scene.frame_set(1)
bpy.context.view_layer.update()
bpy.ops.export_scene.fbx(
    filepath=str(output/(M+'.fbx')),axis_forward='-Y',axis_up='Z',
    use_selection=False, object_types={'ARMATURE','MESH'},
    apply_unit_scale=True,mesh_smooth_type='OFF',use_tspace=False,
    add_leaf_bones=False, armature_nodetype='NULL', use_custom_props=True,
    colors_type='LINEAR',bake_anim=True,bake_anim_use_all_actions=False,
    bake_anim_use_nla_strips=False,bake_anim_step=1,bake_anim_simplify_factor=0,
    path_mode='COPY',embed_textures=True)
print('FBX exported',len(bpy.data.meshes),'meshes',sum(len(m.polygons) for m in bpy.data.meshes),'triangles',scene.frame_end,'frames')

