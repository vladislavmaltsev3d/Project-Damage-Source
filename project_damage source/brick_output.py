# храним созданные кирпичи отдельно от повреждённой стены
# SPDX-License-Identifier: GPL-3.0-or-later
"""Refresh one separate brick-layer mesh per source without welding its islands."""
from . import output_collections
from .blender_compat import enable_nodes
import bpy
import numpy as np
STAMP='project_damage_bricks_of'
def bricks_of(source):
    uid=source.get('project_damage_uid')
    return next((o for o in bpy.data.objects if uid and o.get(STAMP)==uid),None)
def place_bricks(context,source,out,factor):
    from . import build,ownership
    obj=bricks_of(source);faces=out.get('bricks_polys',())
    if not len(faces):
        if obj is not None:
            mesh=obj.data;bpy.data.objects.remove(obj,do_unlink=True)
            if mesh.users==0:bpy.data.meshes.remove(mesh)
        return None
    name=source.name+'_project_damage_bricks'
    mesh=build._result_mesh(name,out['bricks_points']/factor,faces,build.ShadingProvenance.generated_object())
    ids=mesh.attributes.new('project_damage_brick_id','INT','FACE')
    ids.data.foreach_set('value',np.asarray(out['bricks_piece'],dtype=np.int32))
    from .material_defaults import get
    mat=source.project_damage.brick_front_material or get('brick')
    if mat is None:
        mat=bpy.data.materials.get('ProjectDamage_BrickLayer')
        if mat is None:
            mat=bpy.data.materials.new('ProjectDamage_BrickLayer');enable_nodes(mat)
            mat.diffuse_color=(.42,.13,.055,1)
            shader=mat.node_tree.nodes.get('Principled BSDF')
            shader.inputs['Base Color'].default_value=mat.diffuse_color
            shader.inputs['Roughness'].default_value=.82
    mesh.materials.append(mat)
    mortar_faces=np.asarray(out['bricks_piece'])<0
    if mortar_faces.any():
        mortar=source.project_damage.brick_seam_material or get('mortar')
        if mortar is None:
            mortar=bpy.data.materials.new('ProjectDamage_BrickMortar');enable_nodes(mortar)
            mortar.diffuse_color=(.42,.40,.34,1)
            shader=mortar.node_tree.nodes.get('Principled BSDF')
            shader.inputs['Base Color'].default_value=mortar.diffuse_color
            shader.inputs['Roughness'].default_value=.95
        mesh.materials.append(mortar)
        mesh.polygons.foreach_set('material_index',mortar_faces.astype(np.int32))
    if obj is None:obj=bpy.data.objects.new(name,mesh)
    else:
        old=obj.data;obj.data=mesh
        if old.users==0:bpy.data.meshes.remove(old)
    obj[STAMP]=source['project_damage_uid'];ownership.bind(source,obj)
    output_collections.link_object(context,source,obj)
    obj.matrix_world=source.matrix_world
    return obj
