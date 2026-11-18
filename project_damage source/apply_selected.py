# применяем один выбранный эффект и сохраняем остальные доступными для правок
# SPDX-License-Identifier: GPL-3.0-or-later
"""Transactional, isolated evaluation for baking one damage generator."""
import uuid
import bpy
from . import build, ownership, live, mesh_finalization, mesh_states


def owned(uid):
    return [o for o in bpy.data.objects
            if any(o.get(stamp) == uid for stamp in ownership.STAMPS)]


def assign_mesh(obj, mesh, groups):
                                                                              
    scratch = bpy.data.meshes.new('PD group staging')
    obj.data = scratch
    obj.vertex_groups.clear()
    for name in groups:
        obj.vertex_groups.new(name=name)
    obj.data = mesh
    bpy.data.meshes.remove(scratch)


def remove_copy_physics(context,obj):
    """Evaluation/backup copies must not become extra bodies in the simulation."""
    if obj.rigid_body is not None:
        with context.temp_override(object=obj,active_object=obj,selected_objects=[obj],selected_editable_objects=[obj]):
            if bpy.ops.rigidbody.object_remove()!={'FINISHED'}:
                raise RuntimeError('Could not remove physics from temporary copy')



def baked_mesh(context, obj):
    temp = obj.copy()
    temp.data = obj.data.copy()
    original = temp.data
    context.scene.collection.objects.link(temp)
    try:
        remove_copy_physics(context,temp)
        temp.hide_viewport = False
        temp.hide_set(False)
        for mod in list(temp.modifiers):
            if not (mod.name == 'Weighted Normal' and mod.type == 'WEIGHTED_NORMAL'):
                temp.modifiers.remove(mod)
        context.view_layer.update()
        graph = context.evaluated_depsgraph_get()
        return bpy.data.meshes.new_from_object(temp.evaluated_get(graph),
            preserve_all_data_layers=True, depsgraph=graph)
    finally:
        bpy.data.objects.remove(temp, do_unlink=True)
        if original.users == 0:
            bpy.data.meshes.remove(original)


def stage_boolean_fragments(context,source,stage,first,key):
    """Bake Boolean fragments independently, rather than duplicating them in the wall."""
    from . import boolean_break
    if key!='boolean' or not boolean_break.wanted(source):return [],[]
    stage[boolean_break.STAGE_KEY]=False
    try:
        pieces=boolean_break.after_build(context,stage,first)
    finally:stage[boolean_break.STAGE_KEY]=True
    existing=boolean_break.fragments_of(source)
    if len(existing)!=len(pieces) and any(obj.rigid_body is not None for obj in existing):
        raise RuntimeError('Applying Boolean Break changes the number of physical fragments; apply all damage to keep the current pieces')
    return pieces,existing


def commit_boolean_fragments(context,pieces,existing,backup,saved_result):
    """Keep physical fragment object IDs/constraints; backups carry no bodies."""
    from . import boolean_break
    for obj in existing if saved_result is not None else ():
        saved=obj.copy();saved.data=obj.data.copy();context.scene.collection.objects.link(saved)
        remove_copy_physics(context,saved)
        saved[boolean_break.FRAGMENT_STAMP]=backup['project_damage_uid']
        saved['project_damage_fragment_of']=saved_result.name
        saved.project_damage_og_object=backup;saved.hide_set(True);saved.hide_render=True
    pinned=getattr(context.window_manager,'project_damage_pinned_source',None)
    for i,piece in enumerate(pieces):
        target=existing[i] if i<len(existing) else piece
        if target!=piece:
            assign_mesh(target,piece.data.copy(),[group.name for group in piece.vertex_groups])
            target.matrix_world=piece.matrix_world
        ownership.release_loose_piece(context,target)
        mesh_finalization.remove_unused_slots(target)
        target['project_damage_baked_backup']=backup
        target['project_damage_baked_effect']='boolean'
        if target!=piece:bpy.data.objects.remove(piece,do_unlink=True)
    for obj in existing[len(pieces):]:bpy.data.objects.remove(obj,do_unlink=True)
    context.window_manager.project_damage_pinned_source=pinned


def reuse_outputs(context, source, stage_uid, result, final, old_outputs, frozen, frozen_roles, flags, replaced_meshes):
    available = [obj for obj in old_outputs if obj != result]
    uid = source['project_damage_uid']
    from .wall_stack import INSTANCE
    frozen_set = set(frozen)
    adopted = {}
    for obj in frozen + [obj for obj in owned(stage_uid) if obj != final]:
        roles = frozen_roles.get(obj) or tuple(stamp for stamp in ownership.STAMPS if obj.get(stamp) == stage_uid)
        target = next((old for old in available if old.type == obj.type == 'MESH'
                       and old.get(INSTANCE, 0) == obj.get(INSTANCE, 0)
                       and tuple(stamp for stamp in ownership.STAMPS if old.get(stamp) == uid) == roles), None)
        if target is None:continue
        available.remove(target)
        replaced_meshes.append(target.data)
        assign_mesh(target, obj.data, [g.name for g in obj.vertex_groups])
        world = obj.matrix_world.copy()
        target.parent = obj.parent
        target.matrix_world = world
        target.hide_viewport = obj.hide_viewport
        target.hide_render = obj.hide_render
        target.hide_set(obj.hide_get())
        wn = target.modifiers.get('Weighted Normal')
        new_wn = obj.modifiers.get('Weighted Normal')
        if wn and wn.type == 'WEIGHTED_NORMAL' and new_wn is None:
            target.modifiers.remove(wn)
        elif new_wn and new_wn.type == 'WEIGHTED_NORMAL':
            if wn is None:wn = target.modifiers.new('Weighted Normal', 'WEIGHTED_NORMAL')
            if wn.type == 'WEIGHTED_NORMAL':
                for prop in new_wn.bl_rna.properties:
                    if not prop.is_readonly and prop.type not in {'POINTER','COLLECTION'} and prop.identifier != 'name':
                        setattr(wn, prop.identifier, getattr(new_wn, prop.identifier))
        for stamp in ownership.STAMPS:
            if stamp in target:del target[stamp]
            if stamp in obj:target[stamp] = obj[stamp]
        target.project_damage_og_object = obj.project_damage_og_object
        if obj in frozen_set:
            target.project_damage_effects.clear()
            target.project_damage.live_update = False
            for name in flags:setattr(target.project_damage, name, False)
        adopted[obj] = target
    for obj, target in adopted.items():
        for child in list(obj.children):
            world = child.matrix_world.copy()
            child.parent = target
            child.matrix_world = world
    updated_frozen = [adopted.get(obj, obj) for obj in frozen]
    for obj in adopted:bpy.data.objects.remove(obj, do_unlink=True)
    return updated_frozen, available


def apply(context, source, key, instance=0):
    from . import boolean_stack, damage_generator_assets, wall_stack
    if key in damage_generator_assets.KINDS:return damage_generator_assets.bake_selected(context,source,key)
    from . import sidebar
    result = build.result_of(source)
    stacked_boolean = boolean_stack.SERIAL in source
    index = boolean_stack.chosen_index(source, instance) if key == 'boolean' and stacked_boolean else wall_stack.chosen_index(source, key, instance)
    row = source.project_damage_effects[index] if index is not None else None
    if key not in sidebar.CATALOG or not sidebar.enabled(source, key, row) or source.project_damage.all_off:
        raise RuntimeError('Choose an enabled effect first')
    if key in {'rbf', 'cellf', 'debris'}:
        raise RuntimeError('Fragment/debris outputs depend on the full stack; use Apply All for these generators')
    if key == 'boolean' and stacked_boolean and any(item != row and item.get(boolean_stack.ON)
                                                    for item in boolean_stack.rows(source)):
        from . import boolean_break
        if boolean_break.fragments_of(source):
            raise RuntimeError('Use Apply All to bake the fragments from multiple Boolean Breaks')
    if key == 'hang' and source.project_damage.hang_mode == '1':
        raise RuntimeError('Rebar Holes using Boolean breakers depends on Boolean Break; use Apply All')
    if source.library or result is None or result.library:
        raise RuntimeError('An editable local source and damage result are required')
    from . import input_prep
    if input_prep.settings(source).use_modifiers and any(m.show_viewport for m in source.modifiers):
        raise RuntimeError('Disable Use Modifiers in Mesh Preparation or apply the source modifiers first')
    uid = source['project_damage_uid']
    old_outputs = owned(uid)
    keep_states = mesh_states.save_all(context)
    replaced_meshes = []
    if any(obj!=result and obj.rigid_body is not None for obj in old_outputs):
        raise RuntimeError('Apply the auxiliary physics piece as its own damage source before applying an individual generator')
    before_objects = set(bpy.data.objects)
    before_meshes = set(bpy.data.meshes)
    before_collections = set(bpy.data.collections)
    active = context.view_layer.objects.active
    selected = tuple(context.selected_objects)
    previous = live._APPLYING
    live._APPLYING = True
    stage_uid = uuid.uuid4().hex
    stage = None
    committed = False
    try:
        from . import fracture_stack
        fracture_stack.ensure(source)
        fracture_stack.save(source)
        wall_stack.save(source)
        boolean_stack.save(source)
        stage = source.copy()
        stage.data = source.data.copy()
        context.scene.collection.objects.link(stage)
        remove_copy_physics(context,stage)
        stage['project_damage_uid'] = stage_uid
        stage['project_damage_bfrag_stage'] = True                                                      
        stage.project_damage_og_object = None
        stage.project_damage.live_update = False
        stage.project_damage.background_compute = False
        from . import background_compute
        background_compute.cancel(source)
        flags = {name: getattr(source.project_damage, name) for name in sidebar.ENABLE_PROPS | {'rebar_enable'}}
        solo = {sidebar.CATALOG[key][2]}
        if key == 'buildup':
            solo.add('deposit_enable')
        if key == 'boolean':
            solo.add('rebar_enable')
        for name, value in flags.items():
            setattr(stage.project_damage, name, value if name in solo else False)
        fracture_flags={r.key:bool(r.get(fracture_stack.ON,False)) for r in stage.project_damage_effects if r.key in fracture_stack.FRACTURE_KEYS}
        for r in stage.project_damage_effects:
            if r.key in fracture_stack.FRACTURE_KEYS:r[fracture_stack.ON]=bool(r.key==key and fracture_flags[r.key])
        fracture_stack.flags(stage)
        if key in fracture_stack.FRACTURE_KEYS:fracture_stack.select(stage,key)
        wall_flags = {r[wall_stack.INSTANCE]: bool(r.get(wall_stack.ON)) for r in wall_stack.rows(stage)}
        for item in wall_stack.rows(stage):
            item[wall_stack.ON] = key in wall_stack.KEYS and item[wall_stack.INSTANCE] == instance and wall_flags[instance]
        boolean_flags = {item[boolean_stack.INSTANCE]: bool(item.get(boolean_stack.ON)) for item in boolean_stack.rows(stage)}
        if stacked_boolean:
            stage[boolean_stack.EDIT] = 0
            for item in boolean_stack.rows(stage):
                serial = item[boolean_stack.INSTANCE]
                item[boolean_stack.ON] = key == 'boolean' and serial == instance and boolean_flags[serial]
            if key == 'boolean':
                boolean_stack.select(stage, boolean_stack.by_id(stage, instance))
            boolean_stack.refresh_master(stage)
        first, reports = build.rebuild_object(context, stage)
        if first is None:
            raise RuntimeError('Selected effect failed: ' + '; '.join(str(r[1]) for r in reports))
        boolean_pieces,old_boolean_pieces=stage_boolean_fragments(context,source,stage,first,key)
        base = baked_mesh(context, first)
        assign_mesh(stage, base, [g.name for g in first.vertex_groups])
        frozen = [o for o in owned(stage_uid) if o != first]
        frozen_roles = {obj: tuple(stamp for stamp in ownership.STAMPS if obj.get(stamp) == stage_uid) for obj in frozen}
        for obj in frozen:
            if obj.type == 'MESH':
                assign_mesh(obj, baked_mesh(context, obj), [g.name for g in obj.vertex_groups])
                wn = obj.modifiers.get('Weighted Normal')
                if wn and wn.type == 'WEIGHTED_NORMAL':
                    obj.modifiers.remove(wn)
            world = obj.matrix_world.copy()
            obj.parent = None
            obj.matrix_world = world
            for stamp in ownership.STAMPS:
                if stamp in obj:
                    del obj[stamp]
            obj.project_damage_og_object = None
            obj.project_damage_effects.clear()
            obj.project_damage.live_update = False
            for name in flags:
                setattr(obj.project_damage, name, False)
        for name, value in flags.items():
            setattr(stage.project_damage, name, value)
        setattr(stage.project_damage, sidebar.CATALOG[key][2], False)
        for r in stage.project_damage_effects:
            if r.key in fracture_flags:r[fracture_stack.ON]=fracture_flags[r.key] and r.key!=key
        fracture_stack.flags(stage)
        for item in wall_stack.rows(stage):
            serial = item[wall_stack.INSTANCE]
            item[wall_stack.ON] = wall_flags[serial] and (key not in wall_stack.KEYS or serial != instance)
        if stacked_boolean:
            stage[boolean_stack.EDIT] = 0
            for item in boolean_stack.rows(stage):
                serial = item[boolean_stack.INSTANCE]
                item[boolean_stack.ON] = boolean_flags[serial] and (key != 'boolean' or serial != instance)
            boolean_stack.refresh_master(stage)
        if key == 'buildup':
            stage.project_damage.deposit_enable = False
        if key == 'boolean':
            stage.project_damage.rebar_enable = False
        final, reports = build.rebuild_object(context, stage)
        if final is None:
            raise RuntimeError('Remaining effects failed: ' + '; '.join(str(r[1]) for r in reports))
                                                                                
        final_mesh = baked_mesh(context, final)
        mesh_finalization.preserve_surface_data(result.data,final_mesh)
        if keep_states:
            backup_uid = uuid.uuid4().hex
            backup = source.copy()
            backup.data = source.data.copy()
            context.scene.collection.objects.link(backup)
            remove_copy_physics(context,backup)
            backup.name = source.name + ' — Before Apply ' + sidebar.CATALOG[key][1]
            backup['project_damage_uid'] = backup_uid
            backup.project_damage.live_update = False
            backup.hide_set(True)
            backup.hide_render = True
            saved_result = result.copy()
            saved_result.data = result.data.copy()
            context.scene.collection.objects.link(saved_result)
            remove_copy_physics(context,saved_result)
            saved_result.project_damage_og_object = backup
            if saved_result.parent == source:
                world = saved_result.matrix_world.copy()
                saved_result.parent = backup
                saved_result.matrix_world = world
            for stamp in ownership.STAMPS:
                if saved_result.get(stamp) == uid:
                    saved_result[stamp] = backup_uid
            saved_result.hide_set(True)
            saved_result.hide_render = True
            for obj in old_outputs:
                if obj == result:
                    continue
                for stamp in ownership.STAMPS:
                    if obj.get(stamp) == uid:
                        obj[stamp] = backup_uid
                obj.project_damage_og_object = backup
                world = obj.matrix_world.copy()
                if obj.parent == source:
                    obj.parent = backup
                    obj.matrix_world = world
                obj.hide_set(True)
                obj.hide_render = True
        else:
            backup_uid = uid
            backup = source
            saved_result = None
        mesh_states.remember_initial(source)
        replaced_meshes.extend((source.data, result.data))
        assign_mesh(source, stage.data.copy(), [g.name for g in stage.vertex_groups])
        assign_mesh(result, final_mesh, [g.name for g in final.vertex_groups])
        mesh_finalization.remove_unused_slots(source)
        mesh_finalization.remove_unused_slots(result)
        wn = result.modifiers.get('Weighted Normal')
        if wn and wn.type == 'WEIGHTED_NORMAL':
            result.modifiers.remove(wn)
        for name in solo:
            setattr(source.project_damage, name, False)
        if key in fracture_stack.FRACTURE_KEYS:
            fracture_stack.set_enabled(source,key,False)
        if key == 'boolean' and stacked_boolean:
            boolean_stack.remove(source, index)
        elif instance:
            wall_stack.remove(source, index)
        else:
            for i in reversed(range(len(source.project_damage_effects))):
                if source.project_damage_effects[i].key == key:
                    source.project_damage_effects.remove(i)
        source.project_damage_effect_index = min(source.project_damage_effect_index, max(0, len(source.project_damage_effects)-1))
        if not keep_states:
            frozen, stale_outputs = reuse_outputs(context, source, stage_uid, result, final,
                old_outputs, frozen, frozen_roles, flags, replaced_meshes)
        for obj in owned(stage_uid):
            if obj == final:
                continue
            for stamp in ownership.STAMPS:
                if obj.get(stamp) == stage_uid:
                    obj[stamp] = uid
            obj.project_damage_og_object = source
            world = obj.matrix_world.copy()
            if obj.parent == stage:
                obj.parent = source
                obj.matrix_world = world
        for obj in frozen:
            mesh_finalization.remove_unused_slots(obj)
            obj['project_damage_baked_backup'] = backup
            obj['project_damage_baked_effect'] = key
        for collection in bpy.data.collections:
            for tag in ('project_damage_debris_collection', 'project_damage_rbf_collection', 'project_damage_cellf_collection'):
                if keep_states and collection.get(tag) == uid:
                    collection[tag] = backup_uid
                elif collection.get(tag) == stage_uid:
                    collection[tag] = uid
        if keep_states:
            source['project_damage_last_apply_backup'] = backup
            result['project_damage_last_apply_backup'] = backup
        replaced_meshes.extend(obj.data for obj in old_boolean_pieces if obj.type == 'MESH')
        commit_boolean_fragments(context,boolean_pieces,old_boolean_pieces,backup,saved_result)
        if not keep_states:
            for obj in stale_outputs:
                if obj.type == 'MESH':replaced_meshes.append(obj.data)
                bpy.data.objects.remove(obj, do_unlink=True)
        bpy.data.objects.remove(final, do_unlink=True)
        bpy.data.objects.remove(stage, do_unlink=True)
        stage = None
        committed = True
        mesh_states.discard_unused(replaced_meshes)
        from . import boolean_break
        boolean_break.after_apply_selected(context, source)
    finally:
        if not committed:
            for obj in set(bpy.data.objects) - before_objects:
                bpy.data.objects.remove(obj, do_unlink=True)
        for mesh in set(bpy.data.meshes) - before_meshes:
            if mesh.users == 0:
                bpy.data.meshes.remove(mesh)
        for collection in set(bpy.data.collections) - before_collections:
            if not collection.objects and not collection.children:
                bpy.data.collections.remove(collection)
        for token in (uid, stage_uid):
            live._dirty.pop(token, None)
            live._BREAKER_SIG.pop(token, None)
            build._CACHES.pop(token, None)
        from . import pipes
        pipes._INPUTS.pop(stage_uid,None)
        from . import cables
        cables._INPUTS.pop(stage_uid,None)
        live._APPLYING = previous
        for obj in tuple(context.selected_objects):
            obj.select_set(False)
        for obj in selected:
            try:
                if obj.name in context.view_layer.objects:obj.select_set(True)
            except ReferenceError:
                pass
        try:
            context.view_layer.objects.active = active
        except ReferenceError:
            context.view_layer.objects.active = result


# действие «Apply Selected»
class PROJECT_DAMAGE_OT_apply_selected(bpy.types.Operator):
    bl_idname = 'project_damage.apply_selected'
    bl_label = 'Apply Selected'
    bl_description = 'Bake only this effect into the source, remove it from the live list and recompute the others. Hidden intermediate copies are kept only with Save All Mesh States in Preferences'
    bl_options = {'REGISTER', 'UNDO'}
    effect: bpy.props.StringProperty()
    instance: bpy.props.IntProperty(options={'HIDDEN', 'SKIP_SAVE'})

    @classmethod
    def poll(cls, context):
        source = ownership.context_source(context)
        return (context.mode == 'OBJECT' and not live._APPLYING and not live._HISTORY_RESTORING
                and source is not None and source.type == 'MESH' and build.result_of(source) is not None)

    def execute(self, context):
        source = ownership.context_source(context)
        rows = source.project_damage_effects
        key = self.effect or (rows[min(source.project_damage_effect_index, len(rows)-1)].key if rows else '')
        if key == 'ivy':
            from . import ivy, background_compute
            try:
                background_compute.cancel(source,method='grow_ivy')
                with background_compute.synchronous():
                    if bpy.ops.project_damage.grow_ivy()!={'FINISHED'}:
                        raise RuntimeError('Ivy failed; nothing was applied')
                mesh_finalization.remove_unused_slots(ivy.apply_ivy(source))
            except Exception as exc:
                self.report({'ERROR'},str(exc));return {'CANCELLED'}
            return {'FINISHED'}
        if key == 'roots':
            from . import roots, background_compute
            try:
                background_compute.cancel(source,method='grow_roots')
                with background_compute.synchronous():
                    if bpy.ops.project_damage.grow_roots()!={'FINISHED'}:
                        raise RuntimeError('Roots failed; nothing was applied')
                mesh_finalization.remove_unused_slots(roots.apply_roots(source))
            except Exception as exc:
                self.report({'ERROR'},str(exc));return {'CANCELLED'}
            return {'FINISHED'}
        try:
            from . import boolean_stack, wall_stack
            if key == 'boolean' and boolean_stack.SERIAL in source:
                index = boolean_stack.chosen_index(source, self.instance)
                instance = rows[index].get(boolean_stack.INSTANCE, 0) if index is not None else 0
            else:
                index = wall_stack.chosen_index(source, key, self.instance)
                instance = rows[index].get(wall_stack.INSTANCE, 0) if index is not None else 0
            apply(context, source, key, instance)
        except Exception as exc:
            self.report({'ERROR'}, 'Apply Selected: ' + str(exc))
            return {'CANCELLED'}
        message = 'Selected effect applied; others recomputed'
        if mesh_states.save_all(context):message += '; previous setup kept hidden'
        self.report({'INFO'}, message)
        return {'FINISHED'}
