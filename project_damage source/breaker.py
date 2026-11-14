# добавляем каттеры и связываем их с повреждаемым объектом
# ProjectDamage for Blender - worn edge damage cut into mesh objects.
# Copyright (C) 2026 Cornelius Dammrich
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <https://www.gnu.org/licenses/>.
"""The ProjectDamage Breaker: spawn/remove operators and the linked-breaker UIList.

The Update operator reads these breakers through params.py's fill_breakers.
A breaker is a Blender Empty carrying Object.project_damage_breaker, linked
into a source mesh's ProjectDamageProps.breakers collection, mirroring the C4D ProjectDamage
Breaker object (cpp/source/project_damage_breaker.cpp) and its IN_EXCLUDE link
(PROJECT_DAMAGE_BREAK_LIST, oproject_damage.res).
"""
import math

import bpy
from . import ownership
import mathutils

from . import params
from . import live
from . import props as props_mod


def effective_mode(br):
    """EffectiveBreakerMode (project_damage_breaker.cpp): a child DERIVES Custom (3),
    the stored value is never written, removing the child restores it."""
    return 3 if br.type != 'EMPTY' or br.children or br.project_damage_breaker.cutter is not None else int(br.project_damage_breaker.mode)


class PROJECT_DAMAGE_UL_breakers(bpy.types.UIList):
    def draw_item(self, context, layout, data, item, icon, active_data, active_prop):
        from . import boolean_break
        boolean_break.draw_breaker_row(layout, item)


# действие «Add Plane Breaker»
class PROJECT_DAMAGE_OT_breaker_add(bpy.types.Operator):
    bl_idname = "project_damage.breaker_add"
    bl_label = "Add Plane Breaker"
    bl_description = "Create and link a procedural Plane Breaker at the 3D cursor"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        obj = ownership.context_source(context)
        return obj is not None and obj.type == 'MESH' and not obj.name.endswith("_project_damage")

    def execute(self, context):
        obj = ownership.context_source(context)
        props = obj.project_damage
        factor = params.unit_factor(context)
                                                                         
                                                                        
                                                                                                    
                                                                            
                                                                           
                                                          
        corners = [mathutils.Vector(c) for c in obj.bound_box]
        lo = mathutils.Vector((min(c[i] for c in corners) for i in range(3)))
        hi = mathutils.Vector((max(c[i] for c in corners) for i in range(3)))
        length = (hi.x - lo.x) * 1.2 * factor
        width = (hi.y - lo.y) * 1.2 * factor
        half_h = (hi.z - lo.z) * 0.5
        spawn_local = (lo + hi) * 0.5
        spawn_local.z += half_h * 0.85

        n = 0
        for row in props.breakers:
            if row.obj is not None:
                tail = row.obj.name.rsplit("_", 1)
                if len(tail) == 2 and tail[1].isdigit():
                    n = max(n, int(tail[1]))
        seed_n = len(props.breakers)                                                                    
        br = bpy.data.objects.new("ProjectDamage Breaker_%d" % (n + 1), None)
        (obj.users_collection[0] if obj.users_collection
         else context.scene.collection).objects.link(br)
                                                                              
                                                                    
                                                                         
                                                                             
                                 
        br.matrix_world = obj.matrix_world @ mathutils.Matrix.Rotation(math.pi / 2.0, 4, 'X')
        br.matrix_world.translation = context.scene.cursor.location.copy()
        bp = br.project_damage_breaker
        bp.is_breaker = True
        from . import quality_defaults
        quality_defaults.breaker(br)
        bp.length = length
        bp.width = width
                                                                                                                 
                                                                         
                                                                               
                                                                              
                                                                           
                                                                     
                                                                           
                                                                    
        L = max(length, width)
        headroom = half_h * 0.15 * factor
        peak = (bp.macro_h + bp.turb_h + bp.fine_h + bp.rim_amp) / 100.0
        strength = 0.8 * headroom / (peak * L) if peak > 0.0 and L > 0.0 else 1.0
        bp.strength = min(max(strength, 0.02), 4.0) * 100.0
        bp.seed = 12345 + 1009 * seed_n                                            
        row = props.breakers.add()
        row.obj = br
        props.ui_boolean_enable = True
        props.breaker_index = len(props.breakers) - 1
        props_mod._sync_display(bp, context)                                              
                                                                                                   
                                                                    
        obj.select_set(False)
        br.select_set(True)
        context.view_layer.objects.active = br
        return {'FINISHED'}


# действие «Link Breaker»
class PROJECT_DAMAGE_OT_breaker_link(bpy.types.Operator):
    bl_idname = "project_damage.breaker_link"
    bl_label = "Link Breaker"
    bl_description = "Add an empty row - pick an existing ProjectDamage Breaker in it"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        obj = ownership.context_source(context)
        return obj is not None and obj.type == 'MESH' and not obj.name.endswith("_project_damage")

    def execute(self, context):
        props = ownership.context_source(context).project_damage
        props.breakers.add()
        props.ui_boolean_enable = True
        props.breaker_index = len(props.breakers) - 1
        return {'FINISHED'}


# действие «Remove Breaker»
class PROJECT_DAMAGE_OT_breaker_remove(bpy.types.Operator):
    bl_idname = "project_damage.breaker_remove"
    bl_label = "Remove Breaker"
    bl_description = "Unlink the active breaker from this object (the Empty stays in the scene)"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        obj = ownership.context_source(context)
        return (obj is not None and obj.type == 'MESH'
                and len(obj.project_damage.breakers) > 0)

    def execute(self, context):
        props = ownership.context_source(context).project_damage
        if (0 <= props.breaker_index < len(props.breakers)
                and getattr(props.breakers[props.breaker_index], 'from_collection', False)):
            self.report({'WARNING'}, 'This cutter comes from Boolean Objects: take it out of the collection instead')
            return {'CANCELLED'}
        props.breakers.remove(props.breaker_index)
        props.breaker_index = max(0, min(props.breaker_index, len(props.breakers) - 1))
                                                                       
                                                                                
                                                                              
                   
        live._mark_dirty(ownership.context_source(context))
        return {'FINISHED'}


# действие «Selected as Breakers»
class PROJECT_DAMAGE_OT_breaker_from_selected(bpy.types.Operator):
    bl_idname='project_damage.breaker_from_selected'
    bl_label='Selected as Breakers'
    bl_options={'REGISTER','UNDO'}
    @classmethod
    def poll(cls,context):
        src=ownership.context_source(context)
        return src is not None and src.type=='MESH' and context.mode=='OBJECT' and any(o!=src and props_mod._cutter_poll(src.project_damage_breaker,o) for o in context.selected_objects)
    def execute(self,context):
        src=ownership.context_source(context);p=src.project_damage
        linked={r.obj for r in p.breakers if r.obj}
        added=0
        for cut in tuple(context.selected_objects):
            if cut==src or cut in linked or not props_mod._cutter_poll(src.project_damage_breaker,cut):continue
            cut.project_damage_breaker.is_breaker=True;cut.project_damage_breaker.enabled=True
            from . import quality_defaults
            quality_defaults.breaker(cut)
            cut.display_type='WIRE';cut.hide_render=True
            p.breakers.add().obj=cut;linked.add(cut);added+=1
        if added:p.ui_boolean_enable=True
        p.breaker_index=max(0,len(p.breakers)-1);live._mark_dirty(src)
        self.report({'INFO'},'ProjectDamage: %d cutters linked'%added)
        return {'FINISHED'}


_CLASSES = (PROJECT_DAMAGE_OT_breaker_from_selected, PROJECT_DAMAGE_UL_breakers, PROJECT_DAMAGE_OT_breaker_add, PROJECT_DAMAGE_OT_breaker_link, PROJECT_DAMAGE_OT_breaker_remove)


# подключаем свойства и действия этого модуля к Blender
def register():
    for cls in _CLASSES:
        bpy.utils.register_class(cls)
    from . import break_surface
    break_surface.register()
    from . import boolean_break
    boolean_break.register()


# убираем подключения модуля при выключении аддона
def unregister():
    from . import break_surface
    break_surface.unregister()
    from . import boolean_break
    boolean_break.unregister()
    for cls in reversed(_CLASSES):
        bpy.utils.unregister_class(cls)
