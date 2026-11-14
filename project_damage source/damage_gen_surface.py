# согласуем новый генератор с поверхностью выбранного меша
import math
import bpy
import bmesh
import numpy as np
from mathutils import Matrix, Vector
from mathutils.bvhtree import BVHTree


def frame(source,side):
    points=[source.matrix_world@v.co for v in source.data.vertices]
    if not points:raise RuntimeError('The target mesh is empty')
    axis=min(range(3),key=lambda i:(source.dimensions[i],-i)) if side=='AUTO' else 'XYZ'.index(side[-1])
    normal=source.matrix_world.to_3x3().col[axis].normalized()
    if side.startswith('N'):normal=-normal
    tangent=source.matrix_world.to_3x3().col[(axis+1)%3].normalized()
    tangent=(tangent-normal*tangent.dot(normal)).normalized()
    bitangent=normal.cross(tangent).normalized()
    basis=Matrix((tangent,bitangent,normal)).transposed()
    coords=[basis.transposed()@p for p in points]
    lo=Vector(tuple(min(p[i] for p in coords) for i in range(3)))
    hi=Vector(tuple(max(p[i] for p in coords) for i in range(3)))
    world=basis.to_4x4();world.translation=basis@Vector(((lo.x+hi.x)*.5,(lo.y+hi.y)*.5,hi.z))
    return world,max(hi.x-lo.x,.001),max(hi.y-lo.y,.001),max(hi.z-lo.z,.001)


def fit(obj,s,source):
    world,width,height,depth=frame(source,s.surface_axis);obj.matrix_world=world
    if hasattr(s,'radius'):
        scale=min(width,height)/max(s.width,s.height,.001)
        for name in ('radius','depth','rim_height','clod_size','debris_size','plate_size','crack_width','pavement_thickness','slab_size','terrain','terrain_scale','resolution'):
            if hasattr(s,name):setattr(s,name,getattr(s,name)*scale)
        s.blast='CUSTOM'
    s.width=width;s.height=height
    s['_resolution_reference']=s.resolution


def _faces(r):
    verts=np.asarray(r['face_verts'],dtype=np.int32);sizes=np.asarray(r['face_sizes'],dtype=np.int32)
    starts=np.r_[0,np.cumsum(sizes)]
    return [tuple(int(i) for i in verts[a:b]) for a,b in zip(starts[:-1],starts[1:])]


def build(context,obj,s,settings,fn,markers,kind):
    from . import damage_gen_common as G
    src=s.source
    if src is None or src.type!='MESH' or src==obj:raise RuntimeError('Assign a Project Damage mesh surface')
    world,width,height,depth=frame(src,s.surface_axis)
    obj.matrix_world=world
    settings=dict(settings,width=width,height=height)
    settings['ground_thickness' if kind.key=='CRATER' else 'bed_thickness']=0.0
    result=fn(settings,markers)
    if not result['ok']:raise RuntimeError('No damage geometry was generated')
    base=src.data.copy();base.transform(world.inverted_safe()@src.matrix_world)
    base.update()
    bvh=BVHTree.FromPolygons([v.co for v in base.vertices],[list(p.vertices) for p in base.polygons])
    top=max(v.co.z for v in base.vertices)+max(width,height,depth)+1
    epsilon=max(width,height,depth)*1e-6
    def hit(x,y):return bvh.ray_cast(Vector((x,y,top)),Vector((0,0,-1)))[0]
    pts=np.asarray(result['points'],dtype=np.float64).copy();faces=_faces(result)
    mats=np.asarray(result['face_mat'],dtype=np.int32)
    native_materials=kind.materials()
    old=list(obj.data.materials)
    offset=int(obj.data.get('pd_surface_material_base',-1))
    if offset>=0:
        for i in range(len(native_materials)):
            if offset+i<len(old) and old[offset+i] is not None:native_materials[i]=old[offset+i]
    original_count=len(base.materials)
    if not original_count:
        base.materials.append(None);original_count=1
    for mat in native_materials:base.materials.append(mat)
    bm=bmesh.new();bm.from_mesh(base)
    detail_mesh=None;out=None
    try:
        if kind.key=='CRATER':
            ground=[f for f,m in zip(faces,mats) if m==0]
            heights=BVHTree.FromPolygons([Vector(v) for v in pts],ground)
            cast_height=max(float(pts[:,2].max()),0)+1
            limit=max(1000,int(settings.get('max_verts',200000)))
            spacing=max(float(settings.get('resolution',.05)),1e-4)
            selected=[]
            for face in bm.faces:
                center=face.calc_center_median();p=hit(center.x,center.y)
                if face.normal.z>.05 and p is not None and abs(p.z-center.z)<epsilon*10:selected.append(face)
            edges=set(e for f in selected for e in f.edges)
            if edges:
                length=max(e.calc_length() for e in edges)
                budget=max(0,limit-len(bm.verts)-len(pts))
                cuts=min(192,max(0,math.ceil(length/spacing)-1),max(0,int(math.sqrt(budget/max(1,len(selected))))-1))
                if cuts:bmesh.ops.subdivide_edges(bm,edges=list(edges),cuts=cuts,use_grid_fill=True)
            layers={name:bm.verts.layers.float.get(name) or bm.verts.layers.float.new(name) for name,values in result.get('attrs',{}).items() if len(values)==len(pts)}
            changed=set()
            for vert in bm.verts:
                p=hit(vert.co.x,vert.co.y)
                if p is None or abs(p.z-vert.co.z)>epsilon*10:continue
                sample,normal,face_index,distance=heights.ray_cast(Vector((vert.co.x,vert.co.y,cast_height)),Vector((0,0,-1)))
                if sample is None:continue
                triangle=ground[face_index][:3]
                a,b,c=(Vector(pts[i]) for i in triangle)
                e0=b-a;e1=c-a;q=sample-a
                d00=e0.dot(e0);d01=e0.dot(e1);d11=e1.dot(e1);d20=q.dot(e0);d21=q.dot(e1)
                den=d00*d11-d01*d01
                v=(d11*d20-d01*d21)/den if abs(den)>1e-20 else 0.0
                w=(d00*d21-d01*d20)/den if abs(den)>1e-20 else 0.0
                weights=(1-v-w,v,w)
                for name,layer in layers.items():vert[layer]=sum(float(result['attrs'][name][i])*weight for i,weight in zip(triangle,weights))
                delta=sample.z
                lower=bvh.ray_cast(p-Vector((0,0,epsilon*4)),Vector((0,0,-1)))[0]
                if lower is not None:delta=max(delta,-max(0,p.z-lower.z-epsilon*8)*.9)
                if abs(delta)>epsilon:
                    vert.co.z+=delta;changed.add(vert)
            for face in bm.faces:
                if face.normal.z>.05 and any(v in changed for v in face.verts):
                    face.material_index=original_count;face.smooth=True
            bm.normal_update()
        detail_mesh=G.mesh_from_result(result,'_PD_SurfaceDetails')
        details=bmesh.new()
        try:
            details.from_mesh(detail_mesh);details.verts.ensure_lookup_table();details.faces.ensure_lookup_table()
            good=[]
            for vert in details.verts:
                p=hit(vert.co.x,vert.co.y)
                good.append(p is not None)
                if p is not None:vert.co.z+=p.z+(epsilon*2 if kind.key=='TILES' else 0)
            invalid_parts=set()
            part=np.asarray(result.get('face_part',[]))
            if len(part)==len(faces):
                invalid_parts={int(part[f.index]) for f in details.faces if part[f.index]>=0 and any(not good[v.index] for v in f.verts)}
            remove=[f for f in details.faces if (len(part)==len(faces) and part[f.index]>=0 and int(part[f.index]) in invalid_parts) or (kind.key=='CRATER' and mats[f.index]==0) or any(not good[v.index] for v in f.verts)]
            if remove:bmesh.ops.delete(details,geom=remove,context='FACES')
            loose=[v for v in details.verts if not v.link_faces]
            if loose:bmesh.ops.delete(details,geom=loose,context='VERTS')
            for f in details.faces:f.material_index=original_count+int(mats[f.index]) if f.index>=0 else original_count
            details.to_mesh(detail_mesh)
        finally:details.free()
        bm.from_mesh(detail_mesh)
        out=bpy.data.meshes.new(obj.name+'_Surface')
        bm.to_mesh(out);out.update()
        out['pd_surface_material_base']=original_count
        all_mats=list(base.materials)
        values=np.array([f.material_index for f in out.polygons],dtype=np.int32)
        data={'_mesh':out,'_materials':all_mats,'face_mat':values,'_preserve_shading':True}
        G.track_source(obj,src,True)
        return data,'On object: '+src.name
    except Exception:
        if out is not None and out.users==0:bpy.data.meshes.remove(out)
        raise
    finally:
        bm.free();bpy.data.meshes.remove(base)
        if detail_mesh is not None:bpy.data.meshes.remove(detail_mesh)
