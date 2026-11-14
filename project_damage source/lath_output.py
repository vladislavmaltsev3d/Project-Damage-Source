# размещаем деревянные элементы отдельно от поверхности стены
# SPDX-License-Identifier: GPL-3.0-or-later
"""One owned wood object per source, with a face ID per closed plank piece."""
from . import output_collections
import bpy
import numpy as np
STAMP='project_damage_lath_of'
def laths_of(source):
    uid=source.get('project_damage_uid')
    return next((o for o in bpy.data.objects if uid and o.get(STAMP)==uid),None)
def place_laths(context,source,out,factor):
    from . import build,ownership
    old=laths_of(source);faces=out.get('lath_polys',())
    if not len(faces):
        if old is not None:
            mesh=old.data;bpy.data.objects.remove(old,do_unlink=True)
            if mesh.users==0:bpy.data.meshes.remove(mesh)
        return None
    name=source.name+'_project_damage_laths'
    mesh=build._result_mesh(name,out['lath_points']/factor,faces,build.ShadingProvenance.generated_object())
    ids=mesh.attributes.new('project_damage_lath_piece','INT','FACE')
    ids.data.foreach_set('value',np.asarray(out['lath_piece'],dtype=np.int32))
    mat=source.project_damage.lath_material
    nail=None
    if source.project_damage.lath_riven_enable:
        from . import riven_lath
        riven_lath.piece_uvs(mesh,10.0/factor)
    elif source.project_damage.lath_framing_enable:
        from . import riven_lath,stud_framing
        riven_lath.piece_uvs(mesh,10.0/factor)
        stud_framing.split_nails(mesh,source.project_damage.lath_stud_width/factor)
        if mat is None:mat=stud_framing.wood_material()
        nail=stud_framing.nail_material()
    if mat is None:
        from .material_defaults import get
        mat=get('wood_planks')
    if source.project_damage.lath_material is None:
        from . import live
        previous=live._APPLYING;live._APPLYING=True
        try:source.project_damage.lath_material=mat
        finally:live._APPLYING=previous
    mesh.materials.append(mat)
    if nail is not None:mesh.materials.append(nail)
    if old is None:old=bpy.data.objects.new(name,mesh)
    else:
        prior=old.data;old.data=mesh
        if prior.users==0:bpy.data.meshes.remove(prior)
    old[STAMP]=source['project_damage_uid'];ownership.bind(source,old)
    output_collections.link_object(context,source,old)
    old.matrix_world=source.matrix_world
    return old
