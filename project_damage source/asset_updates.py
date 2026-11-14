# переключаем варианты эффектов и переносим их сохранённые настройки
# SPDX-License-Identifier: GPL-3.0-or-later
"""Presentation helpers and isolated Plaster zone authoring (28 Oct 2026)."""
import bpy
from bpy.app.handlers import persistent
REGISTRATION_SAFE=1


def toggle_box(layout,p,name,title,icon='MODIFIER', *, show_content=False):
    box=layout.box();row=box.row(align=True);row.alignment='CENTER';row.scale_y=1.5
    row.prop(p,name,text=title,toggle=True,icon='CHECKMARK')
    from . import editorial
    editorial.button(row,title)
    return box.column(align=True) if getattr(p,name) or show_content else None


def _fabric_mode_set(p,value):
    from . import live
    previous=live._APPLYING
    live._APPLYING=True
    try:
        p.fabric_paint_only=bool(value)
        if value:
            if not p.fabric_paint_holes_enable and not p.fabric_paint_edges_enable:
                p.fabric_paint_holes_enable=True
        else:
            p.fabric_paint_holes_enable=False
            p.fabric_paint_edges_enable=False
    finally:
        live._APPLYING=previous


# действие «Fabric Placement»
class PROJECT_DAMAGE_OT_fabric_placement(bpy.types.Operator):
    bl_idname='project_damage.fabric_placement'
    bl_label='Fabric Placement'
    bl_options={'UNDO','INTERNAL'}
    mode:bpy.props.EnumProperty(items=(('PROCEDURAL','Procedural','Generate tears automatically'),('DRAW','Draw','Use drawn tears; an empty drawing leaves the cloth intact')))

    @classmethod
    def description(cls,context,properties):
        return ('Draw tears with Grease Pencil. An empty drawing leaves the cloth intact.'
                if properties.mode=='DRAW' else 'Generate tears automatically using Interior Tears and Edge Tears.')

    def execute(self,context):
        from . import ownership
        source=ownership.context_source(context)
        if source is None:return {'CANCELLED'}
        source.project_damage.fabric_placement=self.mode
        return {'FINISHED'}


def _fabric_geometry_set(p,value):
    from . import live
    previous=live._APPLYING;live._APPLYING=True
    try:
        active=p.fabric_enable or p.fabric_solid_enable
        solid=bool(value);p.fabric_solid=solid;p.fabric_flat=not solid
        p.fabric_enable=bool(active and not solid);p.fabric_solid_enable=bool(active and solid)
        if active and solid:p.id_data.project_damage_input.use_modifiers=True
    finally:live._APPLYING=previous


class PROJECT_DAMAGE_OT_fabric_geometry(bpy.types.Operator):
    bl_idname='project_damage.fabric_geometry';bl_label='Fabric Geometry';bl_options={'UNDO','INTERNAL'}
    mode:bpy.props.EnumProperty(items=(('FLAT','Flat','Tear a thin cloth surface'),('SOLIDIFY','Solidify','Tear a panel with thickness and a rounded cut rim')))
    @classmethod
    def description(cls,context,properties):
        return 'Flat: tears a thin surface. Solidify: prepares a closed panel with thickness. Both modes keep the same settings and drawings.'
    def execute(self,context):
        from . import ownership
        source=ownership.context_source(context)
        if source is None:return {'CANCELLED'}
        source.project_damage.fabric_geometry=self.mode
        return {'FINISHED'}


def _axis_callbacks(axis):
    field = 'ground_up_' + axis
    def get(p):return abs(getattr(p, field)) > 1e-8
    def set(p,on):setattr(p, field, 1.0 if on else 0.0)
    return get,set


def add_properties(cls):
    from . import live
    cls.__annotations__['concrete_surface_distortion']=bpy.props.FloatProperty(name='Distortion',default=0.,min=0.,max=4.,soft_max=2.,update=live.touch_prop,
        description='Warp procedural surface noise without changing the damaged patch outline; 0 keeps legacy sampling')
    cls.__annotations__['concrete_surface_roughness']=bpy.props.FloatProperty(name='Roughness',default=45.,min=0.,max=100.,subtype='PERCENTAGE',update=live.touch_prop,
        description='Blend broad ripples into fine aggregate pits; 45% preserves the previous coarse/fine balance')
    cls.__annotations__['concrete_surface_strength']=bpy.props.FloatProperty(name='Strength',default=100.,min=0.,max=400.,subtype='PERCENTAGE',update=live.touch_prop,
        description='Procedural surface relief multiplier. 0 removes surface noise; 100% preserves the previous relief')
    cls.__annotations__['fract_proximity_amount']=bpy.props.IntProperty(name='Proximity Fractures',default=6,min=0,soft_max=100,update=live.touch_prop,
        description='Crack count for the Default Proximity asset. Independent of the three base layer Amount values')
    cls.__annotations__['fabric_geometry']=bpy.props.EnumProperty(name='Fabric Geometry',
        items=(('FLAT','Flat','Tear a thin cloth surface'),('SOLIDIFY','Solidify','Cut a panel with thickness and rounded rims')),
        get=lambda p:int(p.fabric_solid_enable or (not p.fabric_enable and p.fabric_solid)),set=_fabric_geometry_set,update=live.touch_prop)
    cls.__annotations__['fabric_placement']=bpy.props.EnumProperty(
        name='Placement',items=(('PROCEDURAL','Procedural','Generate tears automatically'),('DRAW','Draw','Only use drawn tears; an empty drawing leaves the cloth intact')),
        get=lambda p:int(p.fabric_paint_only),set=_fabric_mode_set,update=live.touch_prop)
    cls.__annotations__['plaster_zone_group']=bpy.props.StringProperty(name='Plaster Zone',default='',update=live.touch_prop,
        description='Only Plaster Displacement uses this group. Weight 1: full relief; 0: no relief. Empty: entire mesh')
    for axis in 'xyz':
        get,set=_axis_callbacks(axis)
        cls.__annotations__['ground_axis_'+axis]=bpy.props.BoolProperty(name=axis.upper(),
            get=get,set=set,update=live.touch_prop,
            description='World up direction for Ground Cracks. Several axes combine into a diagonal. With all axes off, Ground Cracks are skipped')


class PROJECT_DAMAGE_OT_setting_info(bpy.types.Operator):
    bl_idname='project_damage.setting_info';bl_label='Setting Help'
    text:bpy.props.StringProperty()
    def invoke(self,context,event):return context.window_manager.invoke_popup(self,width=760)
    def execute(self,context):return {'FINISHED'}
    def draw(self,context):
        import textwrap
        for line in textwrap.wrap(self.text,58):self.layout.label(text=line)


_zone_source = None


def _finish_zone_edit():
    global _zone_source
    source = _zone_source
    _zone_source = None
    try:
        from . import build
        result = build.result_of(source)
        if result is not None:
            result.hide_set(False)
            source.hide_set(True)
            if bpy.context.view_layer.objects.active == source:
                source.select_set(False)
                result.select_set(True)
                bpy.context.view_layer.objects.active = result
    except (ReferenceError, RuntimeError, AttributeError):
        pass


def _watch_zone_edit():
    try:
        if _zone_source is None:
            return None
        if _zone_source.mode in {'EDIT', 'WEIGHT_PAINT'}:
            return .2
    except ReferenceError:
        pass
    _finish_zone_edit()
    return None


# действие «Effect Zone»
class PROJECT_DAMAGE_OT_plaster_zone(bpy.types.Operator):
    bl_idname='project_damage.plaster_zone';bl_label='Effect Zone';bl_options={'REGISTER','UNDO'}
    action:bpy.props.EnumProperty(items=(('ASSIGN','Pick in Edit Mode','Select source vertices, then press Apply to assign them to the effect zone'),('PAINT','Weight Paint','Paint the effect zone on the original mesh'),('APPLY','Apply','Apply selected vertices or painted weights and update the effect'),('CLEAR','Clear','Use the whole mesh again')))
    def execute(self,context):
        global _zone_source
        from . import ownership,paint,build,live
        source=_zone_source if _zone_source is not None else ownership.context_source(context)
        if source is None:return {'CANCELLED'}
        p=source.project_damage
        if self.action=='CLEAR':p.plaster_zone_group='';return {'FINISHED'}
        if self.action=='APPLY':
            selected=None
            if source.mode=='EDIT':
                import bmesh
                bm=bmesh.from_edit_mesh(source.data)
                bm.verts.index_update()
                selected=[v.index for v in bm.verts if v.select]
            paint._activate(context,source)
            group=source.vertex_groups.get(p.plaster_zone_group)
            if group is None:
                self.report({'WARNING'},'Choose a vertex group or paint an effect zone first')
                return {'CANCELLED'}
            if selected is not None and selected:group.add(selected,1.,'REPLACE')
            result,reports=build.rebuild_object(context,source)
            for level,message in reports:self.report(level,message)
            if _zone_source is None:_zone_source=source
            _finish_zone_edit()
            return {'FINISHED'} if result is not None else {'CANCELLED'}
        previous=live._APPLYING;live._APPLYING=True
        try:
            group=source.vertex_groups.get(p.plaster_zone_group) if p.plaster_zone_group else None
            if group is None:group=source.vertex_groups.new(name='PD_PlasterZone')
            p.plaster_zone_group=group.name
            paint._activate(context,source)
            source.vertex_groups.active_index=group.index
            result=build.result_of(source)
            if result is not None:result.hide_set(True)
            _zone_source=source
            bpy.ops.object.mode_set(mode='EDIT' if self.action=='ASSIGN' else 'WEIGHT_PAINT')
        finally:live._APPLYING=previous
        if not bpy.app.timers.is_registered(_watch_zone_edit):
            bpy.app.timers.register(_watch_zone_edit,first_interval=.2)
        return {'FINISHED'}


def draw_plaster_zone(layout,context,p):
    from . import effect_layout,ownership
    source=ownership.context_source(context)
    box=effect_layout.section(layout,'Effect Zone')
    box.prop_search(p,'plaster_zone_group',source,'vertex_groups',text='Vertex Group')
    row=box.row(align=True);row.scale_y=1.5
    row.operator('project_damage.plaster_zone',text='Pick in Edit Mode',icon='GROUP_VERTEX').action='ASSIGN'
    row.operator('project_damage.plaster_zone',text='Weight Paint',icon='WPAINT_HLT').action='PAINT'
    if p.plaster_zone_group:row.operator('project_damage.plaster_zone',text='',icon='X').action='CLEAR'
    row=box.row();row.scale_y=1.5;row.enabled=bool(p.plaster_zone_group)
    row.operator('project_damage.plaster_zone',text='Apply',icon='CHECKMARK').action='APPLY'


def ground_rubble_material(mesh,out,p):
    import numpy as np
    classes=np.asarray(out.get('tri_class',()),dtype=np.int32)
    ids=np.asarray(out.get('effect_ids',()),dtype=np.int32)
    if len(ids)!=len(mesh.polygons) or len(classes)!=len(ids):return
    indices=np.asarray(out.get('ground_rubble_polys',()),dtype=np.int32)
    if not len(indices):return
    mask=np.zeros(len(mesh.polygons),dtype=bool)
    mask[indices]=True
    if not mask.any():return
    from .material_defaults import get
    mat=p.buildup_material or get('gravel')
    slot=next((i for i,m in enumerate(mesh.materials) if m==mat),None)
    if slot is None:slot=len(mesh.materials);mesh.materials.append(mat)
    indices=np.empty(len(mesh.polygons),dtype=np.int32)
    mesh.polygons.foreach_get('material_index',indices);indices[mask]=slot
    mesh.polygons.foreach_set('material_index',indices)


_CLASSES=(PROJECT_DAMAGE_OT_setting_info,PROJECT_DAMAGE_OT_plaster_zone,PROJECT_DAMAGE_OT_fabric_placement,PROJECT_DAMAGE_OT_fabric_geometry)

@persistent
def migrate_fracture_modes(_unused=None):
    objects=getattr(bpy.data,'objects',None)
    if objects is None:return False
    from . import live
    previous=live._APPLYING;live._APPLYING=True
    try:
        for obj in objects:
            p=getattr(obj,'project_damage',None)
            if p is not None and p.fract_mode=='PROXIMITY':
                from .fracture_assets import effective_type
                p.fract_prox_mode={'RADIAL':'RADIAL','GLASS':'GLASS'}.get(effective_type(p),'DEFAULT')
                p.fract_mode='PROCEDURAL'
    finally:live._APPLYING=previous
    return True

def _deferred_fracture_migration():
    return None if migrate_fracture_modes() else .1

# подключаем свойства и действия этого модуля к Blender
def register():
    for cls in _CLASSES:bpy.utils.register_class(cls)
    bpy.types.WindowManager.project_damage_edge_details=bpy.props.BoolProperty(default=False)
    for callback in tuple(bpy.app.handlers.load_post):
        if getattr(callback,'__module__',None)==__name__ and getattr(callback,'__name__',None)=='migrate_fracture_modes':
            bpy.app.handlers.load_post.remove(callback)
    bpy.app.handlers.load_post.append(migrate_fracture_modes)
    if not migrate_fracture_modes() and not bpy.app.timers.is_registered(_deferred_fracture_migration):
        bpy.app.timers.register(_deferred_fracture_migration,first_interval=.1)
# убираем подключения модуля при выключении аддона
def unregister():
    if bpy.app.timers.is_registered(_watch_zone_edit):bpy.app.timers.unregister(_watch_zone_edit)
    _finish_zone_edit()
    if bpy.app.timers.is_registered(_deferred_fracture_migration):bpy.app.timers.unregister(_deferred_fracture_migration)
    if migrate_fracture_modes in bpy.app.handlers.load_post:bpy.app.handlers.load_post.remove(migrate_fracture_modes)
    if hasattr(bpy.types.WindowManager,'project_damage_edge_details'):del bpy.types.WindowManager.project_damage_edge_details
    for cls in reversed(_CLASSES):bpy.utils.unregister_class(cls)
