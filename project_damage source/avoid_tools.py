# старый редактор защитных областей, оставленный для совместимости
# SPDX-License-Identifier: GPL-3.0-or-later
"""Define the existing core's FACE/BOOLEAN avoid mask on the original mesh."""
import bpy
import bmesh
from . import ownership, live, effect_layout


# собираем видимые настройки этого раздела
def draw(layout, context):
    return                                                              
    source=ownership.context_source(context)
    p=source.project_damage
    box=effect_layout.section(layout,'Avoid Group')
    pass
    row=box.row(align=True);row.scale_y=1.5
    row.operator('project_damage.define_avoid_group',text='Define from Selected Faces',icon='FACESEL')
    box.label(text='Select faces on the original in Edit Mode.',icon='INFO')
    if p.avoid_attr:
        toggles=box.column(align=True)
        for name,label in (('avoid_chips','Edge Damage'),('avoid_surf','Surface Damage'),('avoid_corner','Corner Damage'),('avoid_build','Buildup / Deposits'),('avoid_fract','Fracture')):
            toggles.prop(p,name,text=label,toggle=True)


# действие «Define Avoid Group»
class PROJECT_DAMAGE_OT_define_avoid_group(bpy.types.Operator):
    bl_idname='project_damage.define_avoid_group'
    bl_label='Define Avoid Group'
    bl_description='Store the selected faces of the original mesh as the Avoid Group. Replaces the existing group membership, not the geometry'
    bl_options={'REGISTER','UNDO'}

    @classmethod
    def poll(cls,context):
        source=ownership.context_source(context)
        return (context.mode=='EDIT_MESH' and source is not None and context.active_object==source
                and len(context.objects_in_mode)==1 and source.library is None and source.data.library is None)

    def execute(self,context):
        source=ownership.context_source(context)
        bm=bmesh.from_edit_mesh(source.data);bm.faces.ensure_lookup_table();bm.faces.index_update()
        selected={f.index for f in bm.faces if f.select}
        if not selected:
            self.report({'WARNING'},'Select at least one face on the original mesh')
            return {'CANCELLED'}
        name=source.project_damage.avoid_attr or 'ProjectDamage_Avoid'
        attr=source.data.attributes.get(name)
        if attr is not None and (attr.domain!='FACE' or attr.data_type!='BOOLEAN'):
            self.report({'ERROR'},'Choose a FACE / Boolean attribute, or clear Avoid Group to create one')
            return {'CANCELLED'}
        previous=live._APPLYING;live._APPLYING=True
        try:
            bpy.ops.object.mode_set(mode='OBJECT')
                                                                               
            if source.data.users>1:source.data=source.data.copy()
            mesh=source.data
            attr=mesh.attributes.get(name) or mesh.attributes.new(name=name,type='BOOLEAN',domain='FACE')
            attr.data.foreach_set('value',[i in selected for i in range(len(mesh.polygons))])
            source.project_damage.avoid_attr=attr.name
            mesh.update()
        finally:
            if context.active_object==source and source.mode!='EDIT':bpy.ops.object.mode_set(mode='EDIT')
            live._APPLYING=previous
        live._mark_dirty(source)
        self.report({'INFO'},'Avoid Group saved from %d selected faces'%len(selected))
        return {'FINISHED'}


CLASSES=(PROJECT_DAMAGE_OT_define_avoid_group,)
