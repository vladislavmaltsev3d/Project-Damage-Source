# выносим бетонную арматуру в отдельный объект результата
# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent closed reinforcement output for Concrete Damage and V2."""
from . import output_collections
import bpy
STAMP='project_damage_concrete_rebar_of'

def place(context,source,out,factor):
    from . import build,ownership,recording_tools
    uid=source.get('project_damage_uid')
    old=next((o for o in bpy.data.objects if uid and o.get(STAMP)==uid),None)
    faces=out.get('concrete_rebar_polys',())
    if not len(faces):
        if old is not None:
            mesh=old.data;bpy.data.objects.remove(old,do_unlink=True)
            if mesh.users==0:bpy.data.meshes.remove(mesh)
        return None
    name=source.name+'_project_damage_concrete_rebar'
    mesh=build._result_mesh(name,out['concrete_rebar_points']/factor,faces,build.ShadingProvenance.generated_object())
    mesh.materials.append(recording_tools.region_material(source.project_damage,'concrete_rebar_material',''))
    if old is None:old=bpy.data.objects.new(name,mesh)
    else:
        prior=old.data;old.data=mesh
        if prior.users==0:bpy.data.meshes.remove(prior)
    old[STAMP]=uid;ownership.bind(source,old)
    output_collections.link_object(context,source,old)
    old.matrix_world=source.matrix_world
    return old
