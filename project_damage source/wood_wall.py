# деревянный вариант наружного слоя повреждённой стены
# SPDX-License-Identifier: GPL-3.0-or-later
from pathlib import Path
import bpy
from . import live

def changed(self,context):
    if self.concrete_wall_type=='WOOD' and self.surface_damage_material is None:
        name='Wood Planks Shader | Project Damage'
        mat=bpy.data.materials.get(name)
        path=Path(__file__).parent/'blends/wood_mat.blend'
        if mat is None and path.exists():
            with bpy.data.libraries.load(str(path),link=False) as (src,dst):
                if name in src.materials:dst.materials=[name]
            mat=bpy.data.materials.get(name)
        if mat:self.surface_damage_material=mat
    live.touch_prop(self,context)
