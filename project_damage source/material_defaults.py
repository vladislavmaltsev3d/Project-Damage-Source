# подгружаем стандартные материалы из файлов аддона по мере надобности
# SPDX-License-Identifier: GPL-3.0-or-later
"""Lazy library defaults. Explicit material overrides always win."""
from pathlib import Path
import bpy

NAMES={
    'brick':'Bricks Material | Project Damage',
    'mortar':'Bricks Mortats Material | Project Damage',
    'rebar':'Rebars Material Rusty | Project Damage',
                                                                             
    'wood_planks':'Wood Planks Shader | Project Damage',
    'roots':'Roots Shader | Project Damage',
    'framing':'Framing Timber Shader | Project Damage',
    'grass_dry':'PD_Grass_Dry',
    'grass_fresh':'PD_Grass_Fresh',
    'gravel':'Gravel_Procedural',
}


def get(kind):
    name=NAMES[kind]
    if kind in {'grass_dry','grass_fresh','gravel'}:
                                                                              
                                                                                
        for material in bpy.data.materials:
            if (material.get('project_damage_scatter_library_material')==name
                    and not material.get('project_damage_shader_uv')):
                return material
        path=Path(__file__).parent/'blends/gravel_grass_mat.blend'
        with bpy.data.libraries.load(str(path),link=False) as (source,target):
            if name not in source.materials:raise RuntimeError('Missing scatter material: '+name)
            target.materials=[name]
        material=target.materials[0]
        material['project_damage_scatter_library_material']=name
        return material
    material=bpy.data.materials.get(name)
    if material is not None:return material
    filename = {'rebar':'rebars_material.blend','wood_planks':'wood_mat.blend','roots':'roots_mat.blend','framing':'stud_material.blend'}.get(kind,'bricks_mat.blend')
    path=Path(__file__).parent/'blends'/filename
    with bpy.data.libraries.load(str(path),link=False) as (source,target):
        if name not in source.materials:
            raise RuntimeError('Default material missing in '+filename+': '+name)
        target.materials=[name]
    return target.materials[0]
