# выносим бетонную арматуру в отдельный объект результата
# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent closed reinforcement output for Concrete Damage and V2."""
from . import output_collections
import bpy
STAMP='project_damage_concrete_rebar_of'

def place(context,source,out,factor,*,props=None,instance=0):
    from . import build,ownership,recording_tools,wall_stack
    props=source.project_damage if props is None else props
    uid=source.get('project_damage_uid')
    old=next(wall_stack.output_objects(source,STAMP,instance),None)
    faces=out.get('concrete_rebar_polys',())
    if not len(faces):
        if old is not None:
            mesh=old.data;bpy.data.objects.remove(old,do_unlink=True)
            if mesh.users==0:bpy.data.meshes.remove(mesh)
        return None
    name=source.name+'_project_damage_concrete_rebar'+(f' #{instance}' if instance else '')
    mesh=build._result_mesh(name,out['concrete_rebar_points']/factor,faces,build.ShadingProvenance.generated_object())
    mesh.materials.append(recording_tools.region_material(props,'concrete_rebar_material',''))
    if old is None:old=bpy.data.objects.new(name,mesh)
    else:
        prior=old.data;old.data=mesh
        if prior.users==0:bpy.data.meshes.remove(prior)
    if instance:old[wall_stack.INSTANCE]=instance
    old[STAMP]=uid;ownership.bind(source,old)
    output_collections.link_object(context,source,old)
    old.matrix_world=source.matrix_world
    return old
