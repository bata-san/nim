import bpy, json, math, pathlib, sys
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parent))
import tris
a=tris.load()
bpy.ops.wm.read_factory_settings(use_empty=True)
opaque=[s for s in a['parts'] if s['stage']==0]
rows=a['vertices']
ids=sorted({v for s in opaque for t in s['triangles'] for v in t})
remap={v:n for n,v in enumerate(ids)}
polys=[tuple(remap[v] for v in t) for s in opaque for t in s['triangles']]
mesh=bpy.data.meshes.new('surface'); mesh.from_pydata([rows[i][0] for i in ids],[],polys); mesh.update()
assert len(mesh.polygons)==len(polys)
for p,t in zip(mesh.polygons,polys): assert tuple(p.vertices)==t
ob=bpy.data.objects.new('surface',mesh); bpy.context.scene.collection.objects.link(ob)
bpy.context.view_layer.objects.active=ob; ob.select_set(True)
mesh.uv_layers.new(name='BakedUV')
bpy.ops.object.mode_set(mode='EDIT'); bpy.ops.mesh.select_all(action='SELECT')
margin=float(sys.argv[sys.argv.index('--')+1])
bpy.ops.uv.smart_project(angle_limit=math.radians(66),island_margin=margin,area_weight=1.0,correct_aspect=True,scale_to_bounds=False)
bpy.ops.uv.pack_islands(rotate=True,margin=margin,shape_method='CONCAVE')
bpy.ops.object.mode_set(mode='OBJECT')
uv=mesh.uv_layers['BakedUV'].data
out=[[tuple(uv[l].uv) for l in p.loop_indices] for p in mesh.polygons]
pathlib.Path(sys.argv[sys.argv.index('--')+2]).write_text(json.dumps(out))
used=sum(abs((b[0]-a_[0])*(c[1]-a_[1])-(c[0]-a_[0])*(b[1]-a_[1]))/2 for a_,b,c in out)
print('unwrapped',len(out),'faces, uv area used',used)
