# коллайдеры, активные осколки и отдельное запекание анимации
# SPDX-License-Identifier: GPL-3.0-or-later
"""Guided physics workflow, automatic collider list, and baking for owned shards."""
from hashlib import blake2s

import bpy
from . import ownership

_pending=False
_running=False
PIECE_ROLE='project_damage_physics_piece_role'
PIECE_COLOR='project_damage_physics_previous_color'
ACTIVE_COLOR=(1.0, .24, .05, 1.0)
PASSIVE_COLOR=(.13, .48, .95, 1.0)
_piece_color_views={}
_APPLIED_SETTINGS = 'project_damage_applied_physics_settings'
BAKE_BODY = 'project_damage_physics_bake_body'
_BAKE_MATRIX = 'project_damage_physics_bake_matrix'
_BAKE_PREVIOUS = 'project_damage_physics_previous_action'
_BAKE_SLOT = 'project_damage_physics_previous_slot'
_EFFECT_PHYSICS_FIELDS = (
    'break_separate', 'break_separate_min', 'frame_restructure_angle',
    'frame_restructure_minimum', 'frame_restructure_collisions',
    'hit_through_gap', 'hit_through_remove', 'hit_through_min_piece',
    'cellf_material', 'cellf_drawing', 'cellf_surface_drawing', 'cellf_guidance_drawing',
    'cellf_guidance_amount', 'cellf_guidance_radius', 'cellf_guidance_depth',
    'cellf_guidance_falloff', 'cellf_guidance_distance', 'cellf_knife_width',
)


def _settings_value(value):
    if isinstance(value, bpy.types.ID):
        return value.name_full, value.library.filepath if value.library else ''
    return value


def _settings_values(group, excluded=()):
    return tuple(_settings_value(getattr(group, prop.identifier))
                 for prop in group.bl_rna.properties
                 if prop.identifier != 'rna_type' and prop.identifier not in excluded)


def _settings_signature(context, source):
    from . import baked_materials, damage_physics as dp, inner_rough

    q = dp.quick(context)
    values = (_settings_values(q, {'colliders', 'bake_start_frame', 'bake_end_frame'}),
              tuple((_settings_value(row.object), row.motion) for row in q.colliders))
    if source is not None and not baked_materials.is_applied(source):
        values += (_settings_values(dp.settings(source)),
                   tuple(_settings_value(getattr(source.project_damage, name))
                         for name in _EFFECT_PHYSICS_FIELDS),
                   _settings_values(inner_rough.settings(source, 'pieces')))
    return blake2s(repr(values).encode(), digest_size=16).hexdigest()


def settings_source(context):
    from . import baked_materials
    return baked_materials.target(context) or ownership.context_source(context)


def _settings_key(source):
    if source is None:
        return 'scene'
    batch = source.get('project_damage_applied_batch')
    backup = source.get('project_damage_baked_backup')
    if batch:
        identity = ('batch', batch)
    elif isinstance(backup, bpy.types.Object) and not source.get('project_damage_uid'):
        identity = ('applied', backup.get('project_damage_uid') or backup.name_full)
    else:
        identity = ('object', source.get('project_damage_uid') or source.name_full)
    return blake2s(repr(identity).encode(), digest_size=16).hexdigest()


def settings_need_update(context, source):
    from . import damage_physics as dp
    applied = dp.quick(context).get(_APPLIED_SETTINGS, {})
    return applied.get(_settings_key(source)) != _settings_signature(context, source)


def record_settings_applied(context, sources):
    from . import damage_physics as dp
    q = dp.quick(context)
    if _APPLIED_SETTINGS not in q:
        q[_APPLIED_SETTINGS] = {}
    applied = q[_APPLIED_SETTINGS]
    for source in sources or (None,):
        applied[_settings_key(source)] = _settings_signature(context, source)


def clear_settings_applied(context, source):
    from . import damage_physics as dp
    applied = dp.quick(context).get(_APPLIED_SETTINGS)
    key = _settings_key(source)
    if applied is not None and key in applied:
        del applied[key]


def selected_key(source):
    items=source.project_damage_effects;index=source.project_damage_effect_index
    return items[index].key if 0<=index<len(items) else ''


def active_module(context,source):
    from . import sidebar,damage_physics as dp
    key=selected_key(source)
    if key=='cellf' or (not key and source.project_damage.cellf_enable):return 'cellf'
    if key in sidebar.FRACTURE_KEYS:return 'fracture'
    if key:
                                                                              
        return sidebar.PHYSICS_MODULES.get(key)
    return next((m for m in dp.MODULES if dp.module_enabled(source.project_damage,m)),None)


def supported(context,source):
    return source is not None and active_module(context,source) is not None


def targets(context,source=None):
    from . import damage_physics as dp,cellf_output,debris_output,baked_materials
    source=source or baked_materials.target(context) or ownership.context_source(context)
    if source is None:return dp._quick_targets(context)
    if baked_materials.is_applied(source):
        colliders = {row.object for row in dp.quick(context).colliders}
        fragments = baked_materials.fracture_objects(context, source)
        colliders.update(baked_materials.split_walls(fragments))
        return [obj for obj in fragments if obj not in colliders]
    uid=source.get('project_damage_uid')
    objects=dp.pieces_of(source)+cellf_output.objects_of(source)+debris_output.objects_of(source,'rbf')
    objects += [o for o in bpy.data.objects if uid and o.get(dp.SPLIT_STAMP)==uid]
    objects += [o for o in bpy.data.objects if uid and o.get('project_damage_bfrag_of')==uid]
    objects += [o for o in bpy.data.objects if o.get('project_damage_fragment_of') and ownership.source_of(o)==source]
    return list(dict.fromkeys(o for o in objects if o.type=='MESH' and o.name in context.view_layer.objects and not o.get('project_damage_split_wall')))


def owned_piece_scope(context, source=None):
    from . import baked_materials,damage_physics as dp
    source=source or baked_materials.target(context) or ownership.context_source(context)
    if source is None:return []
    colliders={row.object for row in dp.quick(context).colliders if row.object}
    return [obj for obj in targets(context,source) if obj not in colliders]


def motion_groups(context, selected, whole_group=False):
    owned=owned_piece_scope(context)
    if whole_group or any(obj.get(PIECE_ROLE) for obj in owned):
        return ([obj for obj in owned if obj.get(PIECE_ROLE)!='PASSIVE'],
                [obj for obj in owned if obj.get(PIECE_ROLE)=='PASSIVE'])
    return selected,[]


def _set_piece_color(obj, color):
    if color is None:
        original=obj.get(PIECE_COLOR)
        if original is not None:
            obj.color=original
            del obj[PIECE_COLOR]
        return
    if PIECE_COLOR not in obj:
        obj[PIECE_COLOR]=tuple(obj.color)
    obj.color=color


def _piece_colors_in_view(context, show):
    area=context.area
    if area is None or area.type!='VIEW_3D':return
    shading=area.spaces.active.shading
    key=(context.window.as_pointer(),area.as_pointer())
    if show and shading.type=='SOLID':
        _piece_color_views.setdefault(key,shading.color_type)
        shading.color_type='OBJECT'
    elif not show:
        original=_piece_color_views.pop(key,None)
        if original and shading.color_type=='OBJECT':shading.color_type=original


# действие «Set as Destructive»
class PROJECT_DAMAGE_OT_static_pieces(bpy.types.Operator):
    bl_idname='project_damage.static_pieces'
    bl_label='Set as Destructive'
    bl_options={'UNDO','INTERNAL'}
    clear:bpy.props.BoolProperty(default=False,options={'SKIP_SAVE'})

    @classmethod
    def poll(cls,context):
        return context.mode=='OBJECT'

    @classmethod
    def description(cls,context,properties):
        if properties.clear:
            return 'Clear the active/passive assignment and restore the original object colors'
        return 'Make selected shards active and keep the remaining shards in this Project Damage group as passive colliders'

    def execute(self,context):
        from . import inner_rough
        objects=owned_piece_scope(context)
        selected={obj.parent if obj.get(inner_rough.DETAIL_STAMP) and obj.parent else obj
                  for obj in context.selected_objects}
        destructive=selected.intersection(objects)
        if not objects or (not self.clear and not destructive):
            self.report({'WARNING'},'Select the shards you want to simulate')
            return {'CANCELLED'}
        marked=[obj for obj in objects if obj.get(PIECE_ROLE)]
        if self.clear and not marked:
            self.report({'INFO'},'No static particle assignment to clear')
            return {'CANCELLED'}
        previous={}
        for obj in objects:
            detail=inner_rough.detail_of(obj)
            previous[obj]=(obj.get(PIECE_ROLE),tuple(obj.color),obj.get(PIECE_COLOR))
            if detail is not None:
                previous[detail]=(None,tuple(detail.color),detail.get(PIECE_COLOR))
            if self.clear:
                if PIECE_ROLE in obj:del obj[PIECE_ROLE]
                color=None
            else:
                active=obj in destructive
                obj[PIECE_ROLE]='ACTIVE' if active else 'PASSIVE'
                color=ACTIVE_COLOR if active else PASSIVE_COLOR
            _set_piece_color(obj,color)
            if detail is not None:_set_piece_color(detail,color)
        if not self.clear or any(obj.rigid_body is not None for obj in marked):
            status=bpy.ops.project_damage.quick_physics(whole_group=True)
            if 'FINISHED' not in status:
                for obj,(role,color,original) in previous.items():
                    if role is not None:obj[PIECE_ROLE]=role
                    elif PIECE_ROLE in obj:del obj[PIECE_ROLE]
                    if original is not None:obj[PIECE_COLOR]=original
                    elif PIECE_COLOR in obj:del obj[PIECE_COLOR]
                    obj.color=color
                return status
        if not self.clear:
            _piece_colors_in_view(context,True)
        elif not any(obj.get(PIECE_ROLE) for obj in context.view_layer.objects):
            _piece_colors_in_view(context,False)
        return {'FINISHED'}


def _schedule(self,context):
    global _pending
    if _running:return
    _pending=True
    if not bpy.app.timers.is_registered(_sync_tick):bpy.app.timers.register(_sync_tick,first_interval=.1)


def _sync_tick():
    global _pending
    if not _pending:return None
    if not hasattr(bpy.data,'objects') or not getattr(bpy.context,'view_layer',None):return .2
    from . import live
    if live._APPLYING or live._HISTORY_RESTORING:return .2
    _pending=False
    try:sync_colliders(bpy.context)
    except Exception as exc:print('[Project Damage] Collider list:',exc)
    return None


class ProjectDamageCollider(bpy.types.PropertyGroup):
    previous_object:bpy.props.PointerProperty(type=bpy.types.Object,options={'HIDDEN'})
    object:bpy.props.PointerProperty(name='Collider',type=bpy.types.Object,poll=lambda self,obj:obj.type=='MESH',update=_schedule,
        description='Pick a scene mesh with the eyedropper. It automatically becomes a passive collider without changing the selected damage object')
    motion:bpy.props.EnumProperty(name='Motion',items=(('AUTO','Auto','Use animation and constraints to detect a moving collider'),('STATIC','Static','Fixed obstacle'),('MOVING','Animated','Follow animation and push the shards')),default='AUTO',update=_schedule)


def rigid_cache_is_baked(context):
    world = context.scene.rigidbody_world
    return world is not None and world.point_cache.is_baked


def baked_action(obj):
    action = obj.animation_data.action if obj.animation_data else None
    if action is not None and (action.get('project_damage_physics_bake')
                               or (_BAKE_MATRIX in obj and action.name.endswith('_PhysicsBake'))):
        return action
    return None


def sync_colliders(context):
    global _running
    from . import damage_physics as dp,live
    if rigid_cache_is_baked(context):
        return
    q=dp.quick(context);previous=live._APPLYING;live._APPLYING=True;_running=True
    try:
        active={row.object for row in q.colliders if row.object}
        for row in q.colliders:
            old=row.previous_object
            if old and old!=row.object and old not in active and old.get('project_damage_list_collider_created'):
                if old.rigid_body:
                    with context.temp_override(object=old,active_object=old,selected_objects=[old],selected_editable_objects=[old]):bpy.ops.rigidbody.objects_remove()
                del old['project_damage_list_collider_created']
            row.previous_object=row.object
        valid=[row for row in q.colliders if row.object and row.object.name in context.view_layer.objects]
        if not valid:return
        dp._ensure_world(context)
        for row in valid:
            obj=row.object
            if obj.rigid_body and obj.rigid_body.type=='ACTIVE':
                                                                            
                if obj in targets(context):continue
            if obj.rigid_body is None:obj['project_damage_list_collider_created']=True
            dp._add_bodies(context,[obj],'PASSIVE')
            rb=obj.rigid_body;rb.collision_shape=q.collider_shape;rb.friction=q.friction;rb.restitution=q.bounce
            rb.kinematic=row.motion=='MOVING' or (row.motion=='AUTO' and dp._animated(obj))
            rb.use_margin=True;rb.collision_margin=.0005/(context.scene.unit_settings.scale_length or 1.)
    finally:_running=False;live._APPLYING=previous


def preset_changed(self,context):
    if self.material_preset=='CUSTOM':return
    self.density,self.friction,self.bounce={'CONCRETE':(2400.,.65,.05),'WOOD':(650.,.5,.15)}[self.material_preset]
    _schedule(self,context)


class PROJECT_DAMAGE_OT_collider_list(bpy.types.Operator):
    bl_idname='project_damage.collider_list';bl_label='Edit Colliders';bl_options={'UNDO','INTERNAL'}
    action:bpy.props.EnumProperty(items=(('ADD','Add','Add a collider slot'),('REMOVE','Remove','Remove this slot; remove only rigid bodies created by this list')))
    index:bpy.props.IntProperty(default=-1)
    def execute(self,context):
        from . import damage_physics as dp
        q=dp.quick(context)
        if self.action=='ADD':q.colliders.add()
        elif 0<=self.index<len(q.colliders):
            obj=q.colliders[self.index].object;q.colliders.remove(self.index)
            if obj and not any(row.object==obj for row in q.colliders) and obj.get('project_damage_list_collider_created'):
                if obj.rigid_body:
                    with context.temp_override(object=obj,active_object=obj,selected_objects=[obj],selected_editable_objects=[obj]):bpy.ops.rigidbody.objects_remove()
                del obj['project_damage_list_collider_created']
        _schedule(q,context);return {'FINISHED'}


def _body_snapshot(obj):
    from . import cellf_output

    body = obj.rigid_body
    values = {prop.identifier: getattr(body, prop.identifier) for prop in body.bl_rna.properties
              if not prop.is_readonly and prop.type in {'BOOLEAN', 'INT', 'FLOAT', 'ENUM'}
              and not getattr(prop, 'is_array', False) and prop.identifier != 'type'}
    track = cellf_output._switch_track(obj, 'activation')
    if track is not None and len(track.strips) == 1:
        values['activation_frame'] = int(round(track.strips[0].frame_start)) + 1
    return values


# действие «Calculate Hit Releases»
class PROJECT_DAMAGE_OT_calculate_hit_releases(bpy.types.Operator):
    bl_idname = 'project_damage.calculate_hit_releases'
    bl_label = 'Calculate Hit Releases'
    bl_description = ('Calculate when held shards are released by Hit Area, moving colliders or Chain Reaction. '
                      'Updates release keys for playback')
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return context.mode == 'OBJECT'

    def execute(self, context):
        from . import damage_physics as dp, live

        if rigid_cache_is_baked(context):
            self.report({'WARNING'}, 'Remove Baked Cache before recalculating hit releases')
            return {'CANCELLED'}
        q = dp.quick(context)
        pieces = [obj for obj in owned_piece_scope(context) if obj.get(dp.HOLD_STAMP)
                  and obj.rigid_body and obj.rigid_body.type == 'ACTIVE']
        if not pieces:
            self.report({'WARNING'}, 'Choose Only Hit Pieces and press Enable / Update Physics first')
            return {'CANCELLED'}
        sync_colliders(context)
        if q.hit_area is None and not dp._moving_colliders(context):
            self.report({'WARNING'}, 'Add a Hit Area or an animated collider to release shards')
        cache = context.scene.rigidbody_world.point_cache
        previous = live._APPLYING
        live._APPLYING = True
        try:
            hits = dp.plan_hits(context, pieces, cache.frame_start, cache.frame_end, dp._hit_margin(context, q))
        finally:
            live._APPLYING = previous
        self.report({'INFO'}, f'Hit releases updated · {len(hits)} shards directly reached')
        return {'FINISHED'}


# действие «Quick Bake»
class PROJECT_DAMAGE_OT_quick_bake(bpy.types.Operator):
    """Bake the active shards to transform keyframes from Bake Start through Bake End"""
    bl_idname='project_damage.quick_bake';bl_label='Quick Bake';bl_options={'REGISTER','UNDO'}
    @classmethod
    def poll(cls,context):return context.mode=='OBJECT' and any(o.rigid_body and o.rigid_body.type=='ACTIVE' for o in targets(context))
    def execute(self,context):
        from . import damage_physics as dp,live
        objects=[o for o in targets(context) if o.rigid_body and o.rigid_body.type=='ACTIVE'];q=dp.quick(context)
        start, end = q.bake_start_frame, q.bake_end_frame
        if end < start:
            self.report({'ERROR'}, 'Bake End must be at or after Bake Start')
            return {'CANCELLED'}
        if not objects:return {'CANCELLED'}
        if rigid_cache_is_baked(context):
            self.report({'WARNING'}, 'Remove Baked Cache before baking shard keyframes')
            return {'CANCELLED'}
        selected=list(context.selected_objects);active=context.view_layer.objects.active;previous=live._APPLYING;live._APPLYING=True
        hidden={o:o.hide_get() for o in objects}
        try:
            for o in selected:o.select_set(False)
            for o in objects:o.hide_set(False);o.select_set(True)
            context.view_layer.objects.active=objects[0]
            world=context.scene.rigidbody_world
            scene=context.scene;original_frame=scene.frame_current
            sim_start=min(world.point_cache.frame_start if world else q.start_frame, start)
            scene.frame_end=max(scene.frame_end,end)
            if world:
                cache = world.point_cache
                cache.frame_end = max(cache.frame_end, end)
                with context.temp_override(point_cache=cache):
                    if bpy.ops.ptcache.free_bake.poll():
                        bpy.ops.ptcache.free_bake()
                cache.frame_start=sim_start
                held = [obj for obj in objects if q.hold == 'HIT' and obj.get(dp.HOLD_STAMP)]
                if held:
                    dp.plan_hits(context, held, sim_start, end, dp._hit_margin(context, q))
                                                                           
                                                                           
                                                        
            samples=[]
            original_matrices={obj:obj.matrix_basis.copy() for obj in objects}
            original_bodies={obj:_body_snapshot(obj) for obj in objects}
            for frame in range(sim_start,end+1):
                scene.frame_set(frame)
                if frame>=start:
                    dg=context.evaluated_depsgraph_get()
                    samples.append([obj.evaluated_get(dg).matrix_world.copy() for obj in objects])
            from . import cellf_output
            for obj in objects:
                cellf_output.clear_activation(obj)
            with cellf_output.editable_shards(context, objects):
                with context.temp_override(object=objects[0], active_object=objects[0],
                                           selected_objects=objects, selected_editable_objects=objects):
                    bpy.ops.rigidbody.objects_remove()
            for i,obj in enumerate(objects):
                if obj.animation_data and obj.animation_data.action:
                    obj.animation_data.action.use_fake_user=True
                obj[_BAKE_MATRIX] = [x for row in original_matrices[obj] for x in row]
                obj[BAKE_BODY] = original_bodies[obj]
                if obj.animation_data and obj.animation_data.action:
                    obj[_BAKE_PREVIOUS] = obj.animation_data.action
                    obj[_BAKE_SLOT] = obj.animation_data.action_slot.identifier if obj.animation_data.action_slot else ''
                obj.animation_data_create();obj.animation_data.action=bpy.data.actions.new(obj.name+'_PhysicsBake')
                obj.animation_data.action['project_damage_physics_bake'] = True
                for frame,matrices in zip(range(start,end+1),samples):
                    matrix=matrices[i]
                    if obj.parent:matrix=obj.matrix_parent_inverse.inverted()@obj.parent.matrix_world.inverted()@matrix
                    obj.location=matrix.to_translation()
                    if obj.rotation_mode=='QUATERNION':
                        rotation=matrix.to_quaternion()
                        if obj.rotation_quaternion.dot(rotation)<0:rotation=-rotation
                        obj.rotation_quaternion=rotation;rotation_path='rotation_quaternion'
                    elif obj.rotation_mode=='AXIS_ANGLE':
                        axis,angle=matrix.to_quaternion().to_axis_angle();obj.rotation_axis_angle=(angle,*axis);rotation_path='rotation_axis_angle'
                    else:
                        obj.rotation_euler=matrix.to_euler(obj.rotation_mode,obj.rotation_euler);rotation_path='rotation_euler'
                    obj.keyframe_insert(data_path='location',frame=frame,group='Physics Bake')
                    obj.keyframe_insert(data_path=rotation_path,frame=frame,group='Physics Bake')
                from bpy_extras import anim_utils
                channels=anim_utils.action_get_channelbag_for_slot(obj.animation_data.action,obj.animation_data.action_slot)
                if channels:
                    for curve in channels.fcurves:
                        for point in curve.keyframe_points:point.interpolation='LINEAR'
            scene.frame_set(original_frame)
            self.report({'INFO'},'Baked %d shards to frame %d'%(len(objects),end));return {'FINISHED'}
        finally:
            for o in objects:o.select_set(False);o.hide_set(hidden[o])
            for o in selected:
                if o.name in context.view_layer.objects:o.select_set(True)
            context.view_layer.objects.active=active;live._APPLYING=previous


def _select(context,objects):
    for obj in context.selected_objects:obj.select_set(False)
    for obj in objects:obj.hide_set(False);obj.select_set(True)
    if objects:context.view_layer.objects.active=objects[0]


class PROJECT_DAMAGE_OT_select_all_fractures(bpy.types.Operator):
    """Select the visible, selectable fracture pieces that belong to the current Project Damage object"""
    bl_idname='project_damage.select_all_fractures'
    bl_label='Select All Fractures'
    bl_options={'REGISTER','UNDO'}

    @classmethod
    def poll(cls,context):
        return context.mode=='OBJECT' and settings_source(context) is not None

    def execute(self,context):
        pieces=[obj for obj in owned_piece_scope(context) if obj.name in context.view_layer.objects
                and obj.visible_get(view_layer=context.view_layer) and not obj.hide_select]
        if not pieces:
            self.report({'INFO'}, 'No selectable fracture pieces for this object')
            return {'CANCELLED'}
        _select(context,pieces)
        return {'FINISHED'}


class PROJECT_DAMAGE_OT_quick_physics_menu(bpy.types.Operator):
    """Fill the object's damage, then draw your cuts or generate fracture pieces automatically and enable physics"""
    bl_idname='project_damage.quick_physics_menu';bl_label='Quick Physics'
    def invoke(self,context,event):return context.window_manager.invoke_popup(self,width=330)
    def draw(self,context):
        col=self.layout.column(align=True);col.scale_y=1.5
        col.operator('project_damage.quick_physics_prepare',text='Start Painting',icon='GREASEPENCIL').mode='PAINT'
        col.operator('project_damage.quick_physics_prepare',text='After Fracture',icon='MOD_EXPLODE').mode='AUTO'
    def execute(self,context):return {'FINISHED'}


# действие «Prepare Quick Physics»
class PROJECT_DAMAGE_OT_quick_physics_prepare(bpy.types.Operator):
    bl_idname='project_damage.quick_physics_prepare';bl_label='Prepare Quick Physics';bl_options={'REGISTER','UNDO'}
    mode:bpy.props.EnumProperty(items=(('PAINT','Start Painting','Fill all damage, then draw cuts; press Fracture & Physics when ready'),('AUTO','After Fracture','Fill all damage, fracture automatically and add rigid bodies')),default='AUTO')
    def execute(self,context):
        from . import damage_physics as dp,live,build,paint,cellf_output
        source=ownership.context_source(context)
        if source is None:return {'CANCELLED'}
        module=active_module(context,source)
        if module is None:self.report({'ERROR'},'Choose a damage effect with physics support');return {'CANCELLED'}
        if context.mode!='OBJECT':bpy.ops.object.mode_set(mode='OBJECT')
        previous=live._APPLYING;live._APPLYING=True
        try:
            source['project_damage_quick_physics_module']=module
            if module=='cellf':
                if self.mode=='PAINT':
                    source['project_damage_quick_physics_painting']=True
                    drawing=paint.ensure_cell_layer(context,source,'THROUGH');paint._start_drawing(context,drawing);return {'FINISHED'}
                result,reports=build.rebuild_object(context,source)
                for flags,msg in reports:self.report(flags,msg)
                pieces=cellf_output.objects_of(source)
            else:
                if build.result_of(source) is None:
                    result,reports=build.rebuild_object(context,source)
                    for flags,msg in reports:self.report(flags,msg)
                fills,reports=dp.fill_damage(context,source,module,all_damage=True)
                for flags,msg in reports:self.report(flags,msg)
                if not fills:return {'CANCELLED'}
                settings=dp.settings(source)
                if self.mode=='PAINT':
                    settings.cut_mode='DRAWN';source['project_damage_quick_physics_painting']=True
                    drawing=paint.ensure_cell_layer(context,source,'THROUGH');paint._start_drawing(context,drawing)
                    self.report({'INFO'},'Draw the cuts, then press Fracture & Physics');return {'FINISHED'}
                settings.cut_mode='AUTO'
                pieces,reports=dp.fracture(context,source,module,fills=fills)
                for flags,msg in reports:self.report(flags,msg)
            if not pieces:return {'CANCELLED'}
            _select(context,pieces)
            return bpy.ops.project_damage.quick_physics()
        finally:live._APPLYING=previous


# действие «Fracture & Physics»
class PROJECT_DAMAGE_OT_finish_physics_paint(bpy.types.Operator):
    """Fracture the prepared fills with the configured drawings and Cut Plane, then enable physics"""
    bl_idname='project_damage.finish_physics_paint';bl_label='Fracture & Physics';bl_options={'REGISTER','UNDO'}
    def execute(self,context):
        from . import damage_physics as dp,live,build,cellf_output
        source=ownership.context_source(context)
        if source is None:return {'CANCELLED'}
        if context.mode!='OBJECT':bpy.ops.object.mode_set(mode='OBJECT')
        previous=live._APPLYING;live._APPLYING=True
        try:
            module=source.get('project_damage_quick_physics_module') or active_module(context,source)
            if module=='cellf':
                _,reports=build.rebuild_object(context,source);pieces=cellf_output.objects_of(source)
            else:pieces,reports=dp.fracture(context,source,module)
            for flags,msg in reports:self.report(flags,msg)
            if not pieces:return {'CANCELLED'}
            source['project_damage_quick_physics_painting']=False
            _select(context,pieces)
            return bpy.ops.project_damage.quick_physics()
        finally:live._APPLYING=previous



def clear_bake(context, *, restore_bodies=True):
    from mathutils import Matrix
    from . import cellf_output, damage_physics as dp

    scene = context.scene
    world = scene.rigidbody_world
    if world is not None:
        cache = world.point_cache
        with context.temp_override(point_cache=cache):
            if bpy.ops.ptcache.free_bake.poll():
                bpy.ops.ptcache.free_bake()
        if cache.is_baked:
            raise RuntimeError('Could not clear the rigid-body cache')

    baked = {}
    for obj in targets(context):
        action = baked_action(obj)
        if action is not None:
            baked[obj] = action
    missing = [obj for obj in baked if restore_bodies and obj.rigid_body is None]
    if missing:
        world = dp._ensure_world(context) if world is None else world
        dp._add_bodies(context, missing, 'ACTIVE')
        if any(obj.rigid_body is None for obj in missing):
            raise RuntimeError('Could not restore a rigid body on every baked shard')

    restored = set(missing)
    quick = dp.quick(context)
    for obj, action in baked.items():
        ad = obj.animation_data
        ad.action = obj.get(_BAKE_PREVIOUS)
        if ad.action:
            slot = ad.action.slots.get(obj.get(_BAKE_SLOT, ''))
            if slot is not None:
                ad.action_slot = slot
        values = obj.get(_BAKE_MATRIX)
        if values is not None and len(values) == 16:
            obj.matrix_basis = Matrix([values[i:i+4] for i in range(0, 16, 4)])

        if obj in restored:
            saved = obj.get(BAKE_BODY)
            if saved is None:
                dp._configure_active_body(context, obj, quick)
                if ad.action and any(curve.data_path == 'rigid_body.kinematic' for curve in dp._fcurves(ad.action)):
                    obj[dp.HOLD_STAMP] = True
            else:
                for name, value in saved.items():
                    if name != 'activation_frame':
                        setattr(obj.rigid_body, name, value)

            owner = obj.get('project_damage_baked_backup') or ownership.source_of(obj)
            frame = saved.get('activation_frame') if saved is not None else None
            if owner is not None and frame is not None:
                cellf_output._set_switch_track(owner, obj, 'activation', frame)
            elif (saved is None and owner is not None and
                  (obj.get('project_damage_cellf_of') or cellf_output._switch_track(obj, 'visibility'))):
                cellf_output.sync_activation(owner, obj)

        for key in (_BAKE_MATRIX, _BAKE_PREVIOUS, _BAKE_SLOT, BAKE_BODY):
            if key in obj:
                del obj[key]
    for action in set(baked.values()):
        if action.users == 0 and action.get('project_damage_physics_bake'):
            bpy.data.actions.remove(action)
    if world is not None:
        scene.frame_set(world.point_cache.frame_start)
    return len(baked)

# действие «Remove Baked Cache»
class PROJECT_DAMAGE_OT_clear_physics_bake(bpy.types.Operator):
    bl_idname='project_damage.clear_physics_bake'
    bl_label='Remove Baked Cache'
    bl_description='Clear the rigid-body cache and restore the shard simulation so it can be baked again'
    bl_options={'REGISTER','UNDO'}

    @classmethod
    def poll(cls, context):
        return context.mode == 'OBJECT'

    def execute(self, context):
        from . import live

        previous = live._APPLYING
        live._APPLYING = True
        try:
            count = clear_bake(context)
        except RuntimeError as exc:
            self.report({'ERROR'}, str(exc))
            return {'CANCELLED'}
        finally:
            live._APPLYING = previous
        self.report({'INFO'}, 'Cleared rigid-body cache and restored %d baked shards' % count)
        return {'FINISHED'}

class PROJECT_DAMAGE_OT_hit_area_add(bpy.types.Operator):
    bl_idname='project_damage.hit_area_add'
    bl_label='Add Hit Area'
    bl_description='Create a sphere Empty at the 3D cursor. Move, scale or animate it, then calculate Hit Releases to preview which shards it releases'
    bl_options={'REGISTER','UNDO'}
    def execute(self,context):
        from . import damage_physics as dp
        q=dp.quick(context)
        obj=bpy.data.objects.new('Project Damage Hit Area',None)
        context.scene.collection.objects.link(obj)
        obj.location=context.scene.cursor.location
        obj.empty_display_type='SPHERE';obj.empty_display_size=.25/(context.scene.unit_settings.scale_length or 1.)
        obj.show_in_front=True;obj.color=(1.,.35,.05,1.)
        q.hit_area=obj
        return {'FINISHED'}

# действие «Replace with Original»
class PROJECT_DAMAGE_OT_cellf_replace_original(bpy.types.Operator):
    bl_idname = 'project_damage.cellf_replace_original'
    bl_label = 'Replace with Original'
    bl_description = ('Show the intact original through the previous frame, then reveal the damage and '
                      'enable the fracture pieces at the current frame')
    bl_options = {'REGISTER', 'UNDO'}
    whole_source: bpy.props.BoolProperty(options={'HIDDEN', 'SKIP_SAVE'})

    @classmethod
    def poll(cls, context):
        source = ownership.context_source(context)
        return context.mode == 'OBJECT' and source is not None and bool(source.get('project_damage_uid'))

    def execute(self, context):
        from . import cellf_output
        source = ownership.context_source(context)
        try:
            frame, count = cellf_output.replace_with_original(
                context, source, whole_source=self.whole_source or not source.project_damage.cellf_enable)
        except (RuntimeError, ValueError) as exc:
            self.report({'ERROR'}, str(exc))
            return {'CANCELLED'}
        world = context.scene.rigidbody_world
        if world is not None and world.point_cache.is_baked:
            self.report({'WARNING'}, 'Visibility keyed. Re-bake the existing physics cache to change its timing')
        else:
            self.report({'INFO'}, f'Original through frame {frame - 1} · {count} shards from frame {frame}')
        return {'FINISHED'}


def draw_replace_original(layout, source, *, whole_source):
    from . import cellf_output, material_controls
    box = layout.box()
    material_controls.centered_heading(box, 'Replace with Original', icon='TIME')
    row = box.row(align=True)
    row.scale_y = 1.5
    row.operator('project_damage.cellf_replace_original', text='Set Current Frame', icon='KEY_HLT').whole_source = whole_source
    frame = source.get(cellf_output.SWAP_FRAME)
    if frame is not None:
        box.label(text=f'Original through {frame - 1} · Fracture from {frame}', icon='INFO')


def draw_quick(layout,context):
    from . import damage_physics as dp,material_controls
    q=dp.quick(context);box=layout.box();box.use_property_split=False
    material_controls.centered_heading(box,'Quick Physics',icon='PHYSICS')
    row=box.row(align=True);row.scale_y=1.5
    row.operator('project_damage.select_all_fractures',icon='RESTRICT_SELECT_OFF')
    row=box.row(align=True);row.scale_y=1.5
    for value,label in (('CONCRETE','Concrete'),('WOOD','Wood'),('CUSTOM','Custom')):
        row.prop_enum(q,'material_preset',value,text=label)
    col=box.column(align=True);col.prop(q,'density');col.prop(q,'friction');col.prop(q,'bounce')
    col.prop(q,'hold');col.prop(q,'start_frame');col.prop(q,'end_frame')
    if q.hold=='HIT':
        col.prop(q, 'chain_reaction', toggle=True, icon='LINKED')
        col.prop(q,'hit_margin')
        row=col.row(align=True);row.prop(q,'hit_area',text='Hit Area')
        row.operator('project_damage.hit_area_add',text='',icon='ADD')
        row=col.row(align=True);row.scale_y=1.5
        row.operator('project_damage.calculate_hit_releases', icon='TIME')
    col.prop(q,'shape')
    row = col.row(align=True)
    row.prop(q, 'wall_collider')
    row.operator('project_damage.refresh_self_collider', text='', icon='FILE_REFRESH')
    static=box.box();material_controls.centered_heading(static,'Static Particles',icon='PHYSICS')
    static.label(text='Select the shards you want to move',icon='INFO')
    row=static.row(align=True)
    row.operator('project_damage.static_pieces',text='Set as Destructive',icon='RESTRICT_SELECT_OFF')
    row.operator('project_damage.static_pieces',text='',icon='LOOP_BACK').clear=True
    static.label(text='Orange: active · Blue: passive (Solid view)')
    cb=box.box();material_controls.centered_heading(cb,'Colliders',icon='MOD_PHYSICS')
    for i,item in enumerate(q.colliders):
        row=cb.row(align=True);row.prop(item,'object',text='');row.prop(item,'motion',text='')
        op=row.operator('project_damage.collider_list',text='',icon='X');op.action='REMOVE';op.index=i
    cb.operator('project_damage.collider_list',text='Add Collider',icon='ADD').action='ADD'
    cb.prop(q,'collider_shape',text='Shape',expand=True)
    cb.label(text='Assigned automatically; keep the damage object selected',icon='INFO')
    row=box.row(align=True);row.scale_y=1.5
    update=row.row(align=True)
    pending=settings_need_update(context, settings_source(context))
    update.alert=pending
    update.operator('project_damage.quick_physics',text='Enable / Update Physics',
                    icon='FILE_REFRESH' if pending else 'PHYSICS')
    row.operator('project_damage.quick_physics_clear',text='',icon='X')
    row=box.row(align=True)
    row.operator('project_damage.quick_bake',text='Quick Bake to End Frame',icon='ACTION')
    row.operator('project_damage.clear_physics_bake',text='',icon='TRASH')
    row=box.row(align=True)
    row.prop(q,'bake_start_frame')
    row.prop(q,'bake_end_frame')
    source=ownership.context_source(context)
    if source and source.get('project_damage_quick_physics_painting'):
        box.operator('project_damage.finish_physics_paint',icon='CHECKMARK')


# действие «Remove Generated Physics Objects»
class PROJECT_DAMAGE_OT_remove_physics_output(bpy.types.Operator):
    """Remove only this damage object's generated fills or physics fracture pieces, including their detail children. Cell Fracture assets stay unchanged. Undo restores them"""
    bl_idname='project_damage.remove_physics_output';bl_label='Remove Generated Physics Objects';bl_options={'REGISTER','UNDO'}
    kind:bpy.props.EnumProperty(items=(('FILL','Remove Fill Damage','Delete generated fill meshes for this effect'),('PIECES','Remove Fracture Pieces','Delete the physics fragments of this source and detail children; keep the Cell Fracture asset')),default='FILL')
    module:bpy.props.StringProperty(default='')
    def execute(self,context):
        from . import damage_physics as dp,inner_rough,live,build,cellf_output
        source=ownership.context_source(context)
        if source is None:return {'CANCELLED'}
        if context.mode!='OBJECT':bpy.ops.object.mode_set(mode='OBJECT')
        objects=dp.fills_of(source,self.module or None) if self.kind=='FILL' else dp.pieces_of(source)
        previous=live._APPLYING;live._APPLYING=True
        try:
            inner_rough._PENDING.pop((source.name,'pieces'),None)
            count=len(objects)
            for obj in list(objects):dp._remove(obj)
            if source.get(cellf_output.SWAP_ALL):cellf_output.sync_switch(context,source)
            if context.view_layer.objects.active is None:
                target=build.result_of(source) or source
                if target.name in context.view_layer.objects:context.view_layer.objects.active=target
            source['project_damage_quick_physics_painting']=False
            self.report({'INFO'},'Removed %d %s'%(count,'fill objects' if self.kind=='FILL' else 'fracture pieces'))
            return {'FINISHED'}
        finally:live._APPLYING=previous


def remove_button(row,source,kind,module=''):
    from . import damage_physics as dp
    objects=dp.fills_of(source,module or None) if kind=='FILL' else dp.pieces_of(source)
    if objects:
        op=row.operator('project_damage.remove_physics_output',text='',icon='TRASH');op.kind=kind;op.module=module


CLASSES=(PROJECT_DAMAGE_OT_cellf_replace_original,PROJECT_DAMAGE_OT_static_pieces,PROJECT_DAMAGE_OT_clear_physics_bake,PROJECT_DAMAGE_OT_hit_area_add,PROJECT_DAMAGE_OT_remove_physics_output,PROJECT_DAMAGE_OT_collider_list,PROJECT_DAMAGE_OT_calculate_hit_releases,PROJECT_DAMAGE_OT_select_all_fractures,PROJECT_DAMAGE_OT_quick_bake,PROJECT_DAMAGE_OT_quick_physics_menu,
         PROJECT_DAMAGE_OT_quick_physics_prepare,PROJECT_DAMAGE_OT_finish_physics_paint)

def stop():
    global _pending
    _pending=False
    _piece_color_views.clear()
    if bpy.app.timers.is_registered(_sync_tick):bpy.app.timers.unregister(_sync_tick)
