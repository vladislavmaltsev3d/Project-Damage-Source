# передаём Surface Peel в быстрый расчёт и возвращаем данные меша
# SPDX-License-Identifier: GPL-3.0-or-later
"""Array bridge between Blender meshes and the native Peeling build.

Reads the mesh, hands it to project_damage_peeling.peeling_generate and writes
the result back. The native side returns, for every output vertex and corner,
which original elements it interpolates, so all custom data (UV maps, colours,
generic attributes, deform weights) is carried over here without per-element
Python. Polygon triangulation is Blender's own BEAUTY operator.
"""
from pathlib import Path
import importlib.util
import sys
import bmesh
import bpy
import numpy as np

_module=None
# Stored by the native side, or topology Blender rebuilds itself.
# Selection is not inherited either: new elements start unselected, as in BMesh.
_MANAGED={'position','.corner_vert','.corner_edge','.edge_verts','material_index','sharp_face','sharp_edge','custom_normal','project_damage_effect_id',
          '.select_vert','.select_edge','.select_poly'}
_PROPERTY={'FLOAT':('value',1,np.float32),'INT':('value',1,np.int32),'INT8':('value',1,np.int32),'BOOLEAN':('value',1,bool),
           'FLOAT_VECTOR':('vector',3,np.float32),'FLOAT_COLOR':('color',4,np.float32),'BYTE_COLOR':('color',4,np.float32),
           'FLOAT2':('vector',2,np.float32),'INT32_2D':('value',2,np.int32),'QUATERNION':('value',4,np.float32),'FLOAT4X4':('value',16,np.float32)}


def load():
    """The native module, or None when this installation has no build of it."""
    global _module
    if _module is not None:return _module
    from . import core_loader
    roots=[Path(core_loader.core_dir())]
    abi='cp'+str(sys.version_info.major)+str(sys.version_info.minor)
    platform='macos-' if sys.platform=='darwin' else 'windows-' if sys.platform=='win32' else 'linux-'
    roots+=[r for r in (Path(__file__).parent/'core').glob(platform+'*'+abi)]
    for root in dict.fromkeys(roots):
        for path in root.glob('project_damage_peeling*'):
            if path.suffix not in {'.so','.pyd'}:continue
            spec=importlib.util.spec_from_file_location('project_damage_peeling',path)
            if spec is None or spec.loader is None:continue
            module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
            if getattr(module,'peeling_full_native',0)>=1:
                _module=module
                return module
    return None


def drawing_input(paths,p,factor):
    paths=[np.asarray(path,np.float64).reshape(-1,3) for path in paths if len(path)]
    if not paths:return None
    return dict(points=np.ascontiguousarray(np.concatenate(paths),np.float64).ravel(),
                offsets=np.cumsum([0]+[len(path) for path in paths],dtype=np.int32),
                areas=p.peeling_draw_shape=='AREAS',close_strokes=p.peeling_close_strokes,
                radius=p.peeling_stroke_width*max(.001,p.global_scale/100.)/factor*.5,snap=p.peeling_paint_snap/factor)


def _get(collection,name,count,dtype=np.int32,comps=1):
    out=np.empty(count*comps,dtype)
    collection.foreach_get(name,out)
    return out


def _triangles(mesh,offsets,corner_vert):
    """(Tx3 corners, T polygon) for the polygons, without changing the mesh."""
    sizes=np.diff(offsets)
    if len(sizes) and (sizes==3).all():
        return np.arange(len(corner_vert),dtype=np.int32),np.arange(len(sizes),dtype=np.int32)
    corner=mesh.attributes.new('_pd_peel_corner','INT','CORNER')
    face=mesh.attributes.new('_pd_peel_face','INT','FACE')
    corner_name,face_name=corner.name,face.name
    corner.data.foreach_set('value',np.arange(len(corner_vert),dtype=np.int32))
    face.data.foreach_set('value',np.arange(len(sizes),dtype=np.int32))
    bm=bmesh.new();temp=None
    try:
        bm.from_mesh(mesh)
        bmesh.ops.triangulate(bm,faces=bm.faces[:],quad_method='BEAUTY',ngon_method='BEAUTY')
        temp=bpy.data.meshes.new('_pd_peel_triangulated')
        bm.to_mesh(temp)
        corners=_get(temp.attributes[corner_name].data,'value',len(temp.loops))
        polys=_get(temp.attributes[face_name].data,'value',len(temp.polygons))
        return corners,polys
    finally:
        bm.free()
        if temp is not None:bpy.data.meshes.remove(temp)
        mesh.attributes.remove(mesh.attributes[corner_name])
        mesh.attributes.remove(mesh.attributes[face_name])


def _read_deform(mesh):
    """Per-vertex (group ids, weights), or None when no vertex has weights."""
    rows=[([g.group for g in v.groups],[g.weight for g in v.groups]) for v in mesh.vertices]
    return rows if any(ids for ids,_ in rows) else None


def _group_weights(deform,group_index,count):
    out=np.zeros(count,np.float32)
    if deform is None or group_index is None:return out
    for i,(ids,weights) in enumerate(deform):
        if group_index in ids:out[i]=weights[ids.index(group_index)]
    return out


def _capture_normals(mesh):
    """Keep artist-authored normals outside the peeling region."""
    if not mesh.has_custom_normals:return None
    mesh.calc_loop_triangles()
    count=len(mesh.loop_triangles)
    points=_get(mesh.vertices,'co',len(mesh.vertices),np.float32,3)
    triangles=_get(mesh.loop_triangles,'vertices',count,np.int32,3)
    loops=_get(mesh.loop_triangles,'loops',count,np.int32,3)
    normals=_get(mesh.corner_normals,'vector',len(mesh.corner_normals),np.float32,3)
    return points,triangles,normals.reshape(-1,3)[loops].ravel()


def _restore_normals(mesh,shading,native,offsets,corner_vert,stamp):
    if shading is None:return
    # Recompute only the new/bent skin; copy the old interpolation outside it.
    mesh.normals_split_custom_set(np.zeros((len(mesh.loops),3),np.float32))
    mesh.update()
    current=_get(mesh.corner_normals,'vector',len(mesh.corner_normals),np.float32,3)
    points=_get(mesh.vertices,'co',len(mesh.vertices),np.float32,3)
    values=native.peeling_restore_normals(*shading,points,offsets,corner_vert,stamp,current)
    mesh.normals_split_custom_set(np.asarray(values,np.float32).reshape(-1,3))


def _capture_attributes(mesh):
    """Copies of every custom attribute, plus which ones were active."""
    stored=[]
    for attr in mesh.attributes:
        if attr.name in _MANAGED or attr.data_type not in _PROPERTY:continue
        prop,comps,dtype=_PROPERTY[attr.data_type]
        count=len(attr.data)
        values=np.empty(count*comps,dtype)
        attr.data.foreach_get(prop,values)
        stored.append((attr.name,attr.domain,attr.data_type,prop,comps,values.reshape(count,comps)))
    state=dict(active=mesh.attributes.active.name if mesh.attributes.active else None,
               color=mesh.color_attributes.active_color_name if mesh.color_attributes else None,
               render_color=mesh.color_attributes.render_color_index if mesh.color_attributes else -1,
               uv=mesh.uv_layers.active.name if mesh.uv_layers.active else None,
               uv_render=next((u.name for u in mesh.uv_layers if u.active_render),None),
               uv_clone=next((u.name for u in mesh.uv_layers if u.active_clone),None))
    return stored,state


def _interpolated(values,src,weights,dtype):
    used=src>=0
    picked=values[np.where(used,src,0)]            # M x 3 x comps
    w=(weights*used)[:,:,None]
    if dtype is bool:return ((picked!=0)&(w>0)).any(axis=1)
    mixed=(picked.astype(np.float64)*w).sum(axis=1)
    return np.rint(mixed).astype(dtype) if dtype==np.int32 else mixed.astype(dtype)


def _write(mesh,result,stored,state,deform):
    points=np.asarray(result['points'],np.float32)
    offsets=np.asarray(result['poly_offsets'],np.int32)
    corner_vert=np.asarray(result['corner_vert'],np.int32)
    edges=np.asarray(result['edges'],np.int32)
    face_count=len(offsets)-1
    # Not clear_geometry(): that also forgets the vertex group names, which live on the mesh.
    empty=bmesh.new()
    try:empty.to_mesh(mesh)
    finally:empty.free()
    mesh.vertices.add(len(points));mesh.vertices.foreach_set('co',points.ravel())
    mesh.edges.add(len(edges));mesh.edges.foreach_set('vertices',edges.ravel())
    mesh.loops.add(len(corner_vert));mesh.loops.foreach_set('vertex_index',corner_vert)
    mesh.loops.foreach_set('edge_index',np.asarray(result['corner_edge'],np.int32))
    mesh.polygons.add(face_count);mesh.polygons.foreach_set('loop_start',offsets[:-1])
    mesh.polygons.foreach_set('material_index',np.asarray(result['material'],np.int32))
    mesh.polygons.foreach_set('use_smooth',np.asarray(result['smooth'],np.int32)!=0)
    mesh.edges.foreach_set('use_edge_sharp',np.asarray(result['edge_sharp'],np.int32)!=0)
    mesh.update(calc_edges=False)
    stamp=mesh.attributes.get('project_damage_effect_id') or mesh.attributes.new('project_damage_effect_id','INT','FACE')
    stamp.data.foreach_set('value',np.asarray(result['stamp'],np.int32))
    vert_src,vert_w=np.asarray(result['vert_src']),np.asarray(result['vert_w'])
    corner_src,corner_w=np.asarray(result['corner_src']),np.asarray(result['corner_w'])
    face_src,edge_src=np.asarray(result['face_src']),np.asarray(result['edge_src'])
    for name,domain,data_type,prop,comps,values in stored:
        dtype=values.dtype.type if values.dtype!=bool else bool
        if domain=='POINT':new=_interpolated(values,vert_src,vert_w,dtype)
        elif domain=='CORNER':new=_interpolated(values,corner_src,corner_w,dtype)
        elif domain=='FACE':new=values[face_src]
        elif domain=='EDGE':
            new=np.where((edge_src>=0)[:,None],values[np.maximum(edge_src,0)],False if dtype is bool else 0)
        else:continue
        try:attr=mesh.attributes.get(name) or mesh.attributes.new(name,data_type,domain)
        except RuntimeError:continue
        if attr is None:continue
        attr.data.foreach_set(prop,np.ascontiguousarray(new,values.dtype).ravel())
    if state['active'] and state['active'] in mesh.attributes:mesh.attributes.active=mesh.attributes[state['active']]
    if state['uv'] and state['uv'] in mesh.uv_layers:mesh.uv_layers.active=mesh.uv_layers[state['uv']]
    if state['uv_render'] and state['uv_render'] in mesh.uv_layers:mesh.uv_layers[state['uv_render']].active_render=True
    if state['uv_clone'] and state['uv_clone'] in mesh.uv_layers:mesh.uv_layers[state['uv_clone']].active_clone=True
    if state['color']:mesh.color_attributes.active_color_name=state['color']
    if state['render_color']>=0 and state['render_color']<len(mesh.color_attributes):mesh.color_attributes.render_color_index=state['render_color']
    if deform is not None:_write_deform(mesh,deform,vert_src,vert_w)
    mesh.update()


def _write_deform(mesh,deform,src,weights):
    bm=bmesh.new()
    try:
        bm.from_mesh(mesh)
        layer=bm.verts.layers.deform.verify()
        for vert,ids,row in zip(bm.verts,src.tolist(),weights.tolist()):
            merged={}
            for i,w in zip(ids,row):
                if i<0 or w<=0:continue
                for group,value in zip(*deform[i]):merged[group]=merged.get(group,0.)+w*value
            dv=vert[layer]
            for group,value in merged.items():dv[group]=value
        bm.to_mesh(mesh)
    finally:bm.free()


def build(mesh,main,paper,drawing,group_index,gravity,adaptive,seed,budget,vertex_groups=None):
    native=load()
    if native is None:
        raise RuntimeError('Peeling needs the updated native module. Reinstall the add-on and restart Blender')
    nv,nc,nf,ne=len(mesh.vertices),len(mesh.loops),len(mesh.polygons),len(mesh.edges)
    points=_get(mesh.vertices,'co',nv,np.float32,3)
    corner_vert=_get(mesh.loops,'vertex_index',nc)
    offsets=np.empty(nf+1,np.int32);mesh.polygons.foreach_get('loop_start',offsets[:-1]);offsets[-1]=nc
    material=_get(mesh.polygons,'material_index',nf)
    smooth=_get(mesh.polygons,'use_smooth',nf,bool).astype(np.int32)
    edges=_get(mesh.edges,'vertices',ne,np.int32,2)
    sharp=_get(mesh.edges,'use_edge_sharp',ne,bool).astype(np.int32)
    attr=mesh.attributes.get('project_damage_effect_id')
    stamp=_get(attr.data,'value',nf) if attr is not None and attr.domain=='FACE' and attr.data_type=='INT' else np.zeros(nf,np.int32)
    deform=_read_deform(mesh) if vertex_groups is not False else None
    weights=_group_weights(deform,group_index,nv) if group_index is not None else np.empty(0,np.float32)
    shading=_capture_normals(mesh)
    tri_corners,tri_poly=_triangles(mesh,offsets,corner_vert)
    from . import surface_guidance
    guide=None;guided=np.empty(0,np.float32)
    if surface_guidance.enabled():
        guide=lambda p,q:np.ascontiguousarray(surface_guidance.field(p,q)['vertex_normals'],np.float64)
        if drawing is not None:
            quads=corner_vert[tri_corners].reshape(-1,3)
            quads=np.column_stack((quads,quads[:,2]))
            normals=surface_guidance.field(points.reshape(-1,3).astype(np.float64),quads)['face_normals']
            total=np.zeros((nf,3),np.float64);np.add.at(total,tri_poly,normals)
            total/=np.maximum(np.linalg.norm(total,axis=1,keepdims=True),1e-20)
            guided=total.astype(np.float32).ravel()
    result=native.peeling_generate(points,offsets,corner_vert,tri_corners,tri_poly,material,smooth,stamp,edges,sharp,
                                   weights,guided,np.asarray(gravity,np.float32),main,paper,drawing,adaptive,seed,budget,guide)
    stats={k:result[k] for k in ('faces','limited','contour_edges','small_patches','tears') if k in result}
    if result.get('drawing_empty'):stats['drawing_empty']=True
    if not result['faces']:return stats
    stored,state=_capture_attributes(mesh)
    _write(mesh,result,stored,state,deform)
    _restore_normals(mesh,shading,native,np.asarray(result['poly_offsets'],np.int32),np.asarray(result['corner_vert'],np.int32),np.asarray(result['stamp'],np.int32))
    stats['backend']='C++'
    if 'paper_contour_edges' in result:stats['paper_contour_edges']=result['paper_contour_edges']
    return stats
