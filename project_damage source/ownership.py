# определяем, какие источник, результат и помощники принадлежат одному эффекту
# SPDX-License-Identifier: GPL-3.0-or-later
"""Per-result original pointers, selection workflow and legacy scene migration."""
from .blender_compat import uses_nodes
import bpy
from bpy.app.handlers import persistent

STAMPS = ('project_damage_result_of', 'project_damage_deposits_of', 'project_damage_coating_of', 'project_damage_rebar_of','project_damage_lath_of','project_damage_bricks_of')


STAMPS += ('project_damage_debris_of','project_damage_beads_of','project_damage_piles_of')
STAMPS += ('project_damage_rbf_of',)
STAMPS += ('project_damage_cellf_of',)
STAMPS += ('project_damage_wallhole_of',)
STAMPS += ('project_damage_weld_of','project_damage_grass_of','project_damage_gravel_of')
STAMPS += ('project_damage_foam_of','project_damage_concrete_rebar_of','project_damage_paper_generator_of')

                                                                           
                                                                       
LOOSE_STAMPS = ('project_damage_fill_of', 'project_damage_fill_piece_of',
                'project_damage_split_piece_of', 'project_damage_fragment_of')

STAMPS += tuple('project_damage_'+key+'_of' for key in ('gen_metal','gen_wood','gen_tiles','gen_erosion','gen_crater'))

def is_internal_key(name):
    return name.startswith(('project_damage', 'edgy'))


def source_of(obj):
    if obj is None:
        return None
                                                                             
    original = getattr(obj, 'project_damage_og_object', None)
    if original is not None and original is not obj:
        return original if original.type == 'MESH' else None
    plane_owner = obj.get('project_damage_cell_plane_source')
    if isinstance(plane_owner, bpy.types.Object):
        return plane_owner
    proximity_owner = obj.get('project_damage_proximity_owner')
    if isinstance(proximity_owner,bpy.types.Object):
        return proximity_owner
    drawing_owner = obj.get('project_damage_paint_source')
    if isinstance(drawing_owner, bpy.types.Object):
        return drawing_owner
                                                                              
                                                                                        
    fragment = obj.get('project_damage_fragment_of')
    if isinstance(fragment, str) and hasattr(bpy.data, 'objects'):
        result = bpy.data.objects.get(fragment)
        if result is not None and result is not obj and result.get('project_damage_result_of'):
            return source_of(result)
    uid = next((obj.get(key) for key in STAMPS + LOOSE_STAMPS
                if key != 'project_damage_fragment_of' and obj.get(key)), None)
    if uid:
        if not hasattr(bpy.data, 'objects'):
            return None
        return next((o for o in bpy.data.objects if o.type == 'MESH'
                     and not any(o.get(k) for k in STAMPS + LOOSE_STAMPS)
                     and o.get('project_damage_uid') == uid), None)
    return obj


def pinned_source(context):
    obj=getattr(context.window_manager,'project_damage_pinned_source',None)
    try:
        obj=source_of(obj)                                                                
        if obj is not None and obj.type=='MESH' and obj.name in context.scene.objects:
            return obj
    except ReferenceError:
        pass
    return None


def context_source(context):
    """UI/operator target only. Never change source_of or live ID ownership."""
    imported=getattr(context,'project_damage_import_source',None)
    if isinstance(imported,bpy.types.Object) and imported.type=='MESH':return imported
    return pinned_source(context) or source_of(getattr(context,'active_object',None))


def initialization_source(context):
    """Explicit Init on a loose piece targets that mesh, even if its old owner is pinned."""
    active=getattr(context,'active_object',None)
    from .baked_materials import is_applied
    if is_applied(active):return active
    if (active is not None and active.type=='MESH' and any(active.get(key) for key in LOOSE_STAMPS)
            and not any(active.get(key) for key in STAMPS)):
        return active
    return context_source(context)


def release_loose_piece(context,obj):
    """Retire generator ownership only; keep geometry, shaders, UVs and rigid bodies."""
    if not any(obj.get(key) for key in LOOSE_STAMPS):return None
    keys=LOOSE_STAMPS+('project_damage_uid','project_damage_baked_backup','project_damage_fill_kind',
                     'project_damage_fill_kinds','project_damage_piece_fill','project_damage_split_wall')
    def copy_value(value):
        if hasattr(value,'to_dict'):return value.to_dict()
        if hasattr(value,'to_list'):return value.to_list()
        return value
    state={'keys':keys,'values':{key:copy_value(obj[key]) for key in keys if key in obj},
           'original':obj.project_damage_og_object,'parent':obj.parent,'world':obj.matrix_world.copy(),
           'pin':getattr(context.window_manager,'project_damage_pinned_source',None)}
    original=source_of(obj)
    if original is not None and original!=obj:
        obj['project_damage_baked_backup']=original
    old_uids={obj.get(key) for key in LOOSE_STAMPS if obj.get(key)}
    if original is not None and original!=obj:old_uids.add(original.get('project_damage_uid'))
    if obj.get('project_damage_uid') in old_uids and 'project_damage_uid' in obj:
        del obj['project_damage_uid']
    obj.project_damage_og_object=None
    for key in LOOSE_STAMPS:
        if key in obj:del obj[key]
                                                                              
    for key in ('project_damage_fill_kind','project_damage_fill_kinds','project_damage_piece_fill',
                'project_damage_split_wall'):
        if key in obj:del obj[key]
    if original is not None and obj.parent==original:
        world=obj.matrix_world.copy();obj.parent=None;obj.matrix_world=world
    if pinned_source(context)==original:
        context.window_manager.project_damage_pinned_source=obj
    return state


def restore_loose_piece(context,obj,state):
    """A failed Init must not leave a physics piece detached from its workflow."""
    if state is None:return
    for key in state['keys']:
        if key in obj:del obj[key]
    for key,value in state['values'].items():obj[key]=value
    obj.project_damage_og_object=state['original']
    obj.parent=state['parent'];obj.matrix_world=state['world']
    context.window_manager.project_damage_pinned_source=state['pin']


class PROJECT_DAMAGE_OT_pin_settings(bpy.types.Operator):
    bl_idname='project_damage.pin_settings'
    bl_label='Pin Settings Object'
    bl_description='Pin this source in the sidebar while selecting other objects; click again to unpin'
    @classmethod
    def poll(cls,context):
        ob=context_source(context)
        return pinned_source(context) is not None or (ob is not None and ob.type=='MESH' and not ob.project_damage_breaker.is_breaker)
    def execute(self,context):
        context.window_manager.project_damage_pinned_source=None if pinned_source(context) else context_source(context)
        for window in context.window_manager.windows:
            for area in window.screen.areas:area.tag_redraw()
        return {'FINISHED'}


def bind(source, result):
    result.project_damage_og_object = source


def can_focus_result(context, source):
    """Passive updates must not take selection from an owned drawing.

    Also protect OBJECT mode between GP creation and entering Draw mode, or
    when a user deliberately selects a guide to reposition it.
    """
    active = context.view_layer.objects.active
    return (context.mode == 'OBJECT' and active is not None
            and active.mode == 'OBJECT' and active.type == 'MESH'
            and not any(active.get(key) for key in LOOSE_STAMPS)
            and not active.get('project_damage_cell_plane_source')
            and source_of(active) == source)


def focus_result(context, source, result, *, hide_original=True):
    """Do not steal selection during unrelated background/live updates."""
    if result is None or not can_focus_result(context, source):
        return
    if result.name not in context.view_layer.objects:
        return
    for obj in tuple(context.selected_objects):
        obj.select_set(False)
    source.select_set(False)
    if hide_original:
        source.hide_set(True)
        source.hide_render = True
    result.hide_set(False)
    result.hide_render = False
    result.select_set(True)
    context.view_layer.objects.active = result


def _copy_group(raw, target):
    """ID-property storage preserves numeric enum values and nested links."""
    for key, value in raw.items():
        if key not in target.bl_rna.properties:
            continue
        prop = target.bl_rna.properties[key]
        if prop.type == 'COLLECTION':
            coll = getattr(target, key)
            coll.clear()
            for entry in value:
                _copy_group(entry, coll.add())
        elif prop.type == 'POINTER' and not isinstance(value, bpy.types.ID) and hasattr(value, 'items'):
            _copy_group(value, getattr(target, key))
        elif prop.type == 'POINTER':
            setattr(target, key, value)
        else:
            try:
                target[key] = value
            except (TypeError, ValueError):
                pass


@persistent
def migrate_scene(_unused=None):
                                                                                   
    old_prefix = 'edgy'
    for obj in bpy.data.objects:
        if obj.library is not None and obj.override_library is None:
            continue
        if obj.get('project_damage_migrated_v1'):
            continue
        for suffix in ('', '_breaker', '_input'):
            raw = obj.get(old_prefix + suffix)
            if raw is not None and hasattr(raw, 'items'):
                _copy_group(raw, getattr(obj, 'project_damage' + suffix))
        for key in list(obj.keys()):
            if key.startswith(old_prefix + '_'):
                new_key = 'project_damage' + key[len(old_prefix):]
                if new_key not in obj:
                    obj[new_key] = obj[key]
        if obj.type == 'MESH':
            for attr in obj.data.attributes:
                if attr.name.startswith(old_prefix + '_'):
                    name = 'project_damage' + attr.name[len(old_prefix):]
                    if obj.data.attributes.get(name) is None:
                        attr.name = name
        obj['project_damage_migrated_v1'] = True
    for obj in bpy.data.objects:
        if any(obj.get(key) for key in STAMPS):
            source = source_of(obj)
            if source is not None:
                bind(source, obj)
    for material in bpy.data.materials:
        if material.library is not None and material.override_library is None:
            continue
        for key in list(material.keys()):
            if key.startswith(old_prefix + '_'):
                material['project_damage' + key[len(old_prefix):]] = material[key]
        if uses_nodes(material):
            for node in material.node_tree.nodes:
                if node.type == 'ATTRIBUTE' and node.attribute_name.startswith(old_prefix + '_'):
                    node.attribute_name = 'project_damage' + node.attribute_name[len(old_prefix):]


# действие «Edit Original»
class PROJECT_DAMAGE_OT_select_original(bpy.types.Operator):
    bl_idname = 'project_damage.select_original'
    bl_label = 'Edit Original'
    bl_options = {'REGISTER', 'UNDO'}

    def execute(self, context):
        source = context_source(context)
        if source is None:
            return {'CANCELLED'}
        if context.object and context.object.mode != 'OBJECT':
            bpy.ops.object.mode_set(mode='OBJECT')
        from .build import result_of
        result = result_of(source)
        for obj in tuple(context.selected_objects): obj.select_set(False)
        if result is not None: result.hide_set(True)
        source.hide_set(False)
        source.select_set(True)
        context.view_layer.objects.active = source
        return {'FINISHED'}


# действие «Edit Deformable»
class PROJECT_DAMAGE_OT_select_result(bpy.types.Operator):
    bl_idname = 'project_damage.select_result'
    bl_label = 'Edit Deformable'
    bl_options = {'REGISTER', 'UNDO'}

    def execute(self, context):
        source = context_source(context)
        if source is None: return {'CANCELLED'}
        if context.object and context.object.mode != 'OBJECT':
            bpy.ops.object.mode_set(mode='OBJECT')
        from .build import result_of
        result = result_of(source)
        if result is None: return {'CANCELLED'}
                                                                       
        context.view_layer.objects.active = source
        focus_result(context, source, result)
        return {'FINISHED'}


# действие «Delete Damaged Object»
class PROJECT_DAMAGE_OT_delete_result(bpy.types.Operator):
    bl_idname = 'project_damage.delete_result'
    bl_label = 'Delete Damaged Object'
    bl_description = 'Delete the damaged result and return to the original mesh; retain effect settings, drawings and cutters. Undo restores the deleted object'
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        from . import build, live
        source = context_source(context)
        if (source is None or context.mode != 'OBJECT' or live._APPLYING
                or live._HISTORY_RESTORING or not source.is_editable
                or source.name not in context.view_layer.objects):
            return False
        result = build.result_of(source)
        return result is not None and result != source and result.is_editable

    def execute(self, context):
        from . import background_compute, build, live, runtime_lookup
        if not self.poll(context):
            return {'CANCELLED'}
        source = context_source(context)
        result = build.result_of(source)
        background_compute.cancel(source)
        uid = source.get('project_damage_uid')
        live._dirty.pop(uid, None)
        live._dirty_context.pop(uid, None)
        live._drew_since_build.discard(uid)
        previous = live._APPLYING
        live._APPLYING = True
        try:
            for child in tuple(result.children):
                world = child.matrix_world.copy()
                child.parent = None
                child.matrix_world = world
            mesh = result.data
            bpy.data.objects.remove(result, do_unlink=True)
            if mesh.users == 0:
                bpy.data.meshes.remove(mesh)
            runtime_lookup.clear()
            live.invalidate_index()
            source.hide_viewport = False
            source.hide_render = False
            source.hide_set(False)
            source.hide_select = False
            for obj in tuple(context.selected_objects):
                obj.select_set(False)
            source.select_set(True)
            context.view_layer.objects.active = source
        finally:
            live._APPLYING = previous
        for window in context.window_manager.windows:
            for area in window.screen.areas:
                area.tag_redraw()
        return {'FINISHED'}


def _migrate_once():
    migrate_scene()
    return None


# подключаем свойства и действия этого модуля к Blender
def register():
    bpy.types.WindowManager.project_damage_pinned_source=bpy.props.PointerProperty(name='Pinned Damage Object',type=bpy.types.Object)
    bpy.types.Object.project_damage_og_object = bpy.props.PointerProperty(
        name='Original', type=bpy.types.Object, description='Original source owned by this damage result')
    for cls in (PROJECT_DAMAGE_OT_pin_settings, PROJECT_DAMAGE_OT_select_original, PROJECT_DAMAGE_OT_select_result, PROJECT_DAMAGE_OT_delete_result):
        bpy.utils.register_class(cls)
    bpy.app.handlers.load_post.append(migrate_scene)
                                                                      
                                                                           
    if hasattr(bpy.data, 'objects'):
        migrate_scene()
    else:
        bpy.app.timers.register(_migrate_once, first_interval=0.01)


# убираем подключения модуля при выключении аддона
def unregister():
    if bpy.app.timers.is_registered(_migrate_once):
        bpy.app.timers.unregister(_migrate_once)
    if migrate_scene in bpy.app.handlers.load_post:
        bpy.app.handlers.load_post.remove(migrate_scene)
    for cls in (PROJECT_DAMAGE_OT_delete_result, PROJECT_DAMAGE_OT_select_result, PROJECT_DAMAGE_OT_select_original, PROJECT_DAMAGE_OT_pin_settings):
        bpy.utils.unregister_class(cls)
    del bpy.types.Object.project_damage_og_object
    del bpy.types.WindowManager.project_damage_pinned_source
