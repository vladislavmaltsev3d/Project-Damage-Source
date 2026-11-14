# переносим данные исходной поверхности на новые осколки
# SPDX-License-Identifier: GPL-3.0-or-later
"""Attribute transfer helpers for fracture meshes."""
import numpy as np
from mathutils import Matrix,Vector

REST='project_damage_fracture_rest_matrix'
FORM='Matched / Complementary'


def _read(data,field,width=1,dtype=np.float32):
    out=np.empty(len(data)*width,dtype=dtype);data.foreach_get(field,out)
    return out.reshape(-1,width) if width>1 else out


def _world(source,obj):
    stored=obj.get(REST)
    if stored is not None and len(stored)==16:
        return source.matrix_world@Matrix(np.asarray(stored,dtype=float).reshape(4,4).tolist())
    return obj.matrix_world.copy()


def _copy_attributes(old,new,point_sources,point_weights,corner_sources,corner_weights,face_sources):
    fields={'FLOAT':('value',1,np.float32),'INT':('value',1,np.int32),'BOOLEAN':('value',1,bool),
            'FLOAT_VECTOR':('vector',3,np.float32),'FLOAT2':('vector',2,np.float32),
            'FLOAT_COLOR':('color',4,np.float32),'BYTE_COLOR':('color',4,np.float32)}
    for attr in old.attributes:
        if attr.name.startswith('.') or attr.name in {'position','project_damage_weld_id','_pd_rough_n'} or attr.domain=='EDGE':continue
        spec=fields.get(attr.data_type)
        if spec is None:continue
        field,width,dtype=spec;values=_read(attr.data,field,width,dtype)
        if attr.domain=='FACE':out=values[face_sources]
        elif attr.domain in {'POINT','CORNER'}:
            ids,weights=(point_sources,point_weights) if attr.domain=='POINT' else (corner_sources,corner_weights)
            if dtype in (bool,np.int32):out=values[ids[np.arange(len(ids)),weights.argmax(axis=1)]]
            else:out=np.einsum('ijk,ij->ik',values[ids],weights) if width>1 else np.einsum('ij,ij->i',values[ids],weights)
        else:continue
        target=new.attributes.get(attr.name) or new.attributes.new(attr.name,attr.data_type,attr.domain)
        target.data.foreach_set(field,np.asarray(out).ravel())
                                                                              
    if len(old.edges):
        original_edges=_read(old.edges,'vertices',2,np.int32)
        by_pair={tuple(sorted(e)):i for i,e in enumerate(original_edges)}
        new_edges=_read(new.edges,'vertices',2,np.int32);source_edge=np.full(len(new_edges),-1,np.int32)
        supports=[set(map(int,ids[w>1e-7])) for ids,w in zip(point_sources,point_weights)]
        for i,(a,b) in enumerate(new_edges):
            pair=supports[a]|supports[b]
            if len(pair)==2:source_edge[i]=by_pair.get(tuple(sorted(pair)),-1)
        valid=source_edge>=0
        for attr in old.attributes:
            if attr.domain!='EDGE' or attr.name.startswith('.') or attr.data_type not in fields:continue
            field,width,dtype=fields[attr.data_type];values=_read(attr.data,field,width,dtype)
            shape=(len(new_edges),width) if width>1 else (len(new_edges),)
            out=np.zeros(shape,dtype=dtype);out[valid]=values[source_edge[valid]]
            target=new.attributes.get(attr.name) or new.attributes.new(attr.name,attr.data_type,'EDGE')
            target.data.foreach_set(field,out.ravel())
    for layer in old.uv_layers:
        values=_read(layer.data,'uv',2,np.float32)
        uv=np.einsum('ijk,ij->ik',values[corner_sources],corner_weights)
        target=new.uv_layers.get(layer.name) or new.uv_layers.new(name=layer.name)
        target.data.foreach_set('uv',uv.astype(np.float32).ravel());target.active_render=layer.active_render
    if old.uv_layers.active and new.uv_layers.get(old.uv_layers.active.name):new.uv_layers.active=new.uv_layers[old.uv_layers.active.name]


def _groups(source,target,ids,weights,mesh):
    snapshots=[]
    for group in source.vertex_groups:
        values=np.zeros(len(mesh.vertices),np.float32)
        for v in mesh.vertices:
            for item in v.groups:
                if item.group==group.index:values[v.index]=item.weight;break
        snapshots.append((group.name,np.einsum('ij,ij->i',values[ids],weights)))
    target.vertex_groups.clear()
    for name,out in snapshots:
        dest=target.vertex_groups.new(name=name);valid=np.flatnonzero(out>1e-7)
        if not len(valid):continue
        if np.ptp(out[valid])<1e-7:dest.add(valid.tolist(),float(out[valid[0]]),'REPLACE')
        else:
            for i in valid:dest.add([int(i)],float(out[i]),'REPLACE')

