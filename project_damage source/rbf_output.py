# размещаем отдельные части старого генератора Rigid Body Fracture
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned fracture geometry and source-data transfer; never add/remove physics."""
import numpy as np
from . import debris_output,ownership


def objects_of(source):return debris_output.objects_of(source,kind='rbf')

def place(context,source,out,factor,source_snapshot=None,uv_layer_name=None):
    p=source.project_damage
    volumes=np.asarray(out.get('rbf_piece_volume',()),dtype=np.float64)
    centers=np.asarray(out.get('rbf_piece_center',()),dtype=np.float64).reshape(-1,3)/factor
    if len(centers)!=len(volumes) or not np.isfinite(centers).all() or not np.isfinite(volumes).all():
        raise ValueError('Invalid rigid-body piece centers/volumes')
    ids=np.asarray(out.get('rbf_piece',()),dtype=np.int32)
    if ids.size and (ids.min()<0 or ids.max()>=len(volumes)):
        raise ValueError('Rigid-body piece index outside volume table')
    data={'debris_'+key:out.get('rbf_'+key,())
          for key in ('points','polys','piece','base_poly','uvs','normals')}
    data['debris_source_uvs']=bool(uv_layer_name)
    outputs=debris_output.place_debris(context,source,data,factor,source_snapshot,uv_layer_name,
                                      kind='rbf',centers=centers)
    for obj in outputs:
        piece=int(obj['project_damage_rbf_piece'])
        world=obj.matrix_world.copy();obj.parent=None;obj.matrix_world=world
        obj['project_damage_rbf_volume_cm3']=float(volumes[piece])
    return outputs

def focus(context,source,result,pieces):
    if not pieces:return
    result.hide_set(True);result.hide_render=True
    if ownership.can_focus_result(context,source):
        for obj in tuple(context.selected_objects):obj.select_set(False)
        target=pieces[0];target.hide_set(False);target.select_set(True)
        context.view_layer.objects.active=target
