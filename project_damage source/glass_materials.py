# подбираем стеклу материал ближайшей исходной грани
# SPDX-License-Identifier: GPL-3.0-or-later
"""Glass damage inherits the closest source face shader unless explicitly overridden."""
import numpy as np
from mathutils.bvhtree import BVHTree
from mathutils import Vector

def inherit(source,mesh,out):
    from .fracture_assets import effective_type
    p=source.project_damage
    glass=p.glass_hit_enable or p.glass_drawn_enable
    fracture=(p.fract_enable or p.fract_through_enable) and effective_type(p)=='GLASS'
    if not (glass or fracture) or not len(source.data.materials):return
    ids=np.asarray(out.get('effect_ids',()),dtype=np.int32)
    classes=np.asarray(out.get('tri_class',()),dtype=np.int32)
    if len(ids)!=len(mesh.polygons) or len(classes)!=len(ids):return
    from .material_controls import CLASS_PROPS
    select=np.zeros(len(ids),bool)
    for cls,field in CLASS_PROPS.items():
        if getattr(p,field) is not None:continue
        if fracture and cls in (1,2,3) and any(getattr(p,'fracture_l%d_material'%level) is not None for level in (1,2,3)):continue
        select |= ((ids==19) if glass else False)&(classes==cls)
        select |= ((ids==4) if fracture else False)&(classes==cls)
    faces=np.flatnonzero(select)
    if not len(faces):return
    src=source.data
    mats=list(src.materials)
    valid={m for m in mats if m is not None}
    if not valid:return
    slots={}
    def target(mat):
        if mat not in slots:
            slot=next((i for i,m in enumerate(mesh.materials) if m==mat),None)
            if slot is None:slot=len(mesh.materials);mesh.materials.append(mat)
            slots[mat]=slot
        return slots[mat]
    if len(valid)==1:
        slot=target(next(iter(valid)))
        for i in faces:mesh.polygons[int(i)].material_index=slot
        return
    co=np.empty(len(src.vertices)*3);src.vertices.foreach_get('co',co);co=co.reshape(-1,3)
    polygons=[tuple(f.vertices) for f in src.polygons]
    bvh=BVHTree.FromPolygons([Vector(v) for v in co],polygons,all_triangles=False)
    for i in faces:
        f=mesh.polygons[int(i)]
        center=sum((mesh.vertices[v].co for v in f.vertices),Vector())/len(f.vertices)
        hit,normal,index,distance=bvh.find_nearest(center)
        if index is None:continue
        slot=src.polygons[index].material_index
        mat=mats[slot] if slot<len(mats) else None
        if mat is not None:f.material_index=target(mat)
