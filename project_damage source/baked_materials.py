# даём менять материалы уже применённого результата
from collections import OrderedDict
import bpy
import numpy as np
from . import ownership

_COUNTS = OrderedDict()

def is_applied(obj):
    return bool(obj is not None and obj.type=='MESH'
                and (obj.get('project_damage_applied') or obj.get('project_damage_baked_backup'))
                and not obj.get('project_damage_uid')
                and not getattr(obj,'project_damage_og_object',None)
                and not any(obj.get(key) for key in ownership.STAMPS+ownership.LOOSE_STAMPS))

def target(context):
    active=getattr(context,'active_object',None)
    if is_applied(active):return active
    source=ownership.context_source(context)
    return source if is_applied(source) else None

def invalidate(depsgraph=None):
    if depsgraph is None:
        _COUNTS.clear();_GROUPS.clear()
        return
    if not _COUNTS:return
    for update in depsgraph.updates:
        obj=update.id.original
        mesh=obj if isinstance(obj,bpy.types.Mesh) else obj.data if isinstance(obj,bpy.types.Object) and obj.type=='MESH' and update.is_updated_geometry else None
        if mesh is not None:_COUNTS.pop(mesh.session_uid,None)

def used_slots(obj):
    mesh=obj.data
    key=mesh.session_uid
    signature=(len(mesh.polygons),len(obj.material_slots))
    cached=_COUNTS.get(key)
    if cached is not None and cached[0]==signature:
        _COUNTS.move_to_end(key)
        return cached[1]
    indices=np.empty(len(mesh.polygons),np.int32)
    mesh.polygons.foreach_get('material_index',indices)
    slots=tuple(int(i) for i in np.unique(indices) if i<len(obj.material_slots))
    _COUNTS[key]=(signature,slots)
    if len(_COUNTS)>64:_COUNTS.popitem(last=False)
    return slots

_FRAGMENT_STAMPS=('project_damage_cellf_of','project_damage_rbf_of','project_damage_debris_of','project_damage_fill_piece_of','project_damage_split_piece_of','project_damage_fragment_of','project_damage_bfrag_of')
_GROUPS=OrderedDict()

def inner_mask(obj):
    from .material_update import face_values

    mesh=obj.data
    mask = np.zeros(len(mesh.polygons), bool)
    for name,kind in (('project_damage_physics_inner','BOOL'),('_pd_fill_outer','ZERO'),('project_damage_debris_base_poly','NEGATIVE'),('project_damage_base_poly','NEGATIVE')):
        values = face_values(mesh, name)
        if values is not None:
            mask = values.astype(bool) if kind=='BOOL' else values==0 if kind=='ZERO' else values<0
            break
    if not mask.any() and (obj.get('project_damage_bfrag_of') or obj.get('project_damage_fragment_of')
                           or obj.get('project_damage_baked_fragment')
                           or obj.get('project_damage_baked_effect') == 'boolean'):
        # Boolean separation can have cut-face effect IDs without Fracture Fills provenance.
        from . import effect_groups
        values = face_values(mesh, effect_groups.ATTRIBUTE)
        if values is not None:
            mask = values == effect_groups.KEYS.index('boolean')
        if not mask.any():
            name = mesh.get(effect_groups.OWNED_FACES, {}).get('boolean')
            values = face_values(mesh, name) if name else None
            if values is not None:
                mask = values.astype(bool)
    return mask

def fragment(obj):
    if not is_applied(obj):return False
    if 'project_damage_baked_fragment' in obj:return bool(obj['project_damage_baked_fragment'])
    return not obj.get('project_damage_baked_surface') and (
        any(obj.data.attributes.get(n) is not None for n in
            ('project_damage_physics_inner','project_damage_debris_base_poly','_pd_fill_outer'))
        or (obj.get('project_damage_baked_effect') == 'boolean' and inner_mask(obj).any()))

def batch_objects(context,obj,fresh=False,*,scene_wide=False):
    if obj is None or not is_applied(obj):return []
    batch=obj.get('project_damage_applied_batch');source=obj.get('project_damage_baked_backup')
    token=('BATCH',batch) if batch else ('SOURCE',source.session_uid) if isinstance(source,bpy.types.Object) else ('OBJECT',obj.session_uid)
    key=(context.scene.session_uid,token,len(bpy.data.objects))
    if fresh or key not in _GROUPS:
        found=[]
        for other in context.scene.objects:
            if not is_applied(other):continue
            if (batch and other.get('project_damage_applied_batch')==batch) or (not batch and source is not None and other.get('project_damage_baked_backup')==source) or other==obj:found.append(other)
        _GROUPS[key]=tuple(found)
        if len(_GROUPS)>32:_GROUPS.popitem(last=False)
    layer = context.scene.objects if scene_wide else context.view_layer.objects
    try:return [o for o in _GROUPS[key] if is_applied(o) and o.name in layer]
    except ReferenceError:
        _GROUPS.pop(key,None);return batch_objects(context,obj,True,scene_wide=scene_wide)

def fracture_objects(context,obj,fresh=False,*,scene_wide=False):
    return [other for other in batch_objects(context,obj,fresh,scene_wide=scene_wide) if fragment(other)]

def split_walls(objects):
    walls = [obj for obj in objects if obj.get('project_damage_baked_split_wall')]
    if walls:
        return walls
    legacy = []
    for obj in objects:
        if 'project_damage_baked_split_wall' in obj or not fragment(obj):
            continue
        source = obj.get('project_damage_baked_backup')
        uid = source.get('project_damage_uid') if isinstance(source, bpy.types.Object) else None
        if uid and any(coll.get('project_damage_split_collection') == uid for coll in obj.users_collection):
            legacy.append(obj)
    return [max(legacy, key=lambda obj: len(obj.data.polygons))] if legacy else []

# действие «Apply Sides Material to All Fracture Objects»
class PROJECT_DAMAGE_OT_baked_sides_material(bpy.types.Operator):
    bl_idname='project_damage.baked_sides_material'
    bl_label='Apply Sides Material to All Fracture Objects'
    bl_description='Assign this material only to the stored inner cut faces of every applied fracture piece in this group. Preserve exterior materials, geometry and UVs'
    bl_options={'REGISTER','UNDO'}
    @classmethod
    def poll(cls,context):return context.mode=='OBJECT' and target(context) is not None
    def execute(self,context):
        active=target(context);mat=active.project_damage.baked_sides_material
        if mat is None:
            self.report({'WARNING'},'Choose a Sides Material first');return {'CANCELLED'}
        objects=fracture_objects(context,active,True)
        masks=[(obj,inner_mask(obj)) for obj in objects]
        masks=[(obj,mask) for obj,mask in masks if mask.any()]
        if not masks:
            self.report({'WARNING'},'No stored inner cut faces found in this group');return {'CANCELLED'}
        if any(not obj.is_editable or obj.data.library for obj,_ in masks):
            self.report({'ERROR'},'Make fracture pieces local before assigning materials');return {'CANCELLED'}
        from . import fracture_uv, material_update, mesh_finalization
        with material_update.shader_edit(context):
            for obj,mask in masks:
                material_update.assign(obj, mat, mask, uv_name=fracture_uv.layer_name(obj.data))
                obj.project_damage.baked_sides_material=mat
                mesh_finalization.remove_unused_slots(obj)
        invalidate();self.report({'INFO'},'Assigned sides material to %d fracture objects'%len(masks))
        return {'FINISHED'}

# действие «New Material»
class PROJECT_DAMAGE_OT_baked_new_material(bpy.types.Operator):
    bl_idname='project_damage.baked_new_material'
    bl_label='New Material'
    bl_description='Create a material for an applied mesh that has no material slots'
    bl_options={'REGISTER','UNDO'}
    @classmethod
    def poll(cls,context):
        obj=target(context)
        return obj is not None and not obj.material_slots and obj.library is None and obj.data.library is None
    def execute(self,context):
        obj=target(context)
        if obj.data.users>1:obj.data=obj.data.copy()
        material=bpy.data.materials.new(obj.name+' Material')
        from .blender_compat import enable_nodes
        enable_nodes(material)
        obj.data.materials.append(material)
        invalidate()
        return {'FINISHED'}

class PROJECT_DAMAGE_UL_baked_groups(bpy.types.UIList):
    def draw_item(self,context,layout,data,item,icon,active_data,active_propname,index):
        layout.label(text=item.name,icon='GROUP_VERTEX')

# собираем видимые настройки этого раздела
def draw(layout,context,obj):
    from . import material_controls,material_library,uv_zones,physics_ui,physics_workflow
    layout.label(text=obj.name,icon='MESH_DATA')
    layout.label(text='Geometry applied',icon='CHECKMARK')
    row=layout.row()
    row.scale_y=2.0
    row.operator_context='INVOKE_DEFAULT'
    row.context_pointer_set('project_damage_import_source',obj)
    row.operator('project_damage.update',text='Add Project Damage',icon='ADD').target_name=obj.name
    layout.label(text='Start new effects on this applied mesh.',icon='INFO')
    fragments=fracture_objects(context,obj,scene_wide=True)
    if fragments:
        batch=layout.box()
        material_controls.centered_heading(batch,'All Fracture Objects (%d)'%len(fragments),icon='MOD_EXPLODE')
        material_library.draw_property(batch,obj.project_damage,'baked_sides_material',text='Sides Material')
        batch.operator('project_damage.baked_sides_material',text='Apply Sides Material to All',icon='MATERIAL')
        uv_zones.draw_inside_uv(batch,context,text='Quick UV Map - All Fracture Objects',has_pieces=True)
    box=layout.box()
    material_controls.centered_heading(box,'Materials',icon='MATERIAL')
    slots=used_slots(obj)
    if not slots:
        box.label(text='No material slots used by faces',icon='INFO')
        if not obj.material_slots:box.operator('project_damage.baked_new_material',icon='ADD')
    for index in slots:
        slot=obj.material_slots[index]
        material_library.draw_property(box,slot,'material',text='Slot %d'%(index+1))
    uv=layout.box()
    material_controls.centered_heading(uv,'UV Map',icon='UV')
    row=uv.row(align=True)
    row.operator('project_damage.unwrap_zones',text='Quick UV Map',icon='UV')
    row.operator('project_damage.edit_zone_uv',text='Edit Scale',icon='FULLSCREEN_ENTER')
    uv.label(text=uv_zones.UV_NAME if obj.data.uv_layers.get(uv_zones.UV_NAME) else 'Quick UV Map creates ProjectDamage_Zones',icon='GROUP_UVS')
    groups=layout.box()
    material_controls.centered_heading(groups,'Vertex Groups',icon='GROUP_VERTEX')
    if obj.vertex_groups:
        groups.template_list('PROJECT_DAMAGE_UL_baked_groups','',obj,'vertex_groups',obj.vertex_groups,'active_index',rows=min(5,len(obj.vertex_groups)))
        groups.label(text='PD_ groups identify damage zones.',icon='INFO')
        groups.label(text='Use Object Data to select their vertices.')
    else:groups.label(text='This mesh has no vertex groups.',icon='INFO')
    simulation = physics_ui.section(layout, context, 'simulation', 'Rigid Body Simulation', 'PHYSICS')
    if simulation is not None:
        physics_workflow.draw_quick(simulation, context)
