# вносим фоновый результат в сцену и восстанавливаем связи объектов
import math
from pathlib import Path

import bpy
import numpy as np
from mathutils import Matrix


def copy_value(value):
    if isinstance(value, bpy.types.ID):
        return value
    if hasattr(value, 'to_dict'):
        value = value.to_dict()
    if isinstance(value, dict):
        return {key: copy_value(item) for key, item in value.items()}
    if hasattr(value, 'to_list'):
        return value.to_list()
    if isinstance(value, (list, tuple)):
        return [copy_value(item) for item in value]
    return value


def _capture(ob):
    return dict(data=ob.data, groups=[(g.name, g.lock_weight) for g in ob.vertex_groups]
                if ob.type == 'MESH' else [],
                group_index=ob.vertex_groups.active_index if ob.type == 'MESH' else 0,
                parent=ob.parent, parent_type=ob.parent_type, parent_bone=ob.parent_bone,
                inverse=ob.matrix_parent_inverse.copy(), basis=ob.matrix_basis.copy(),
                world=ob.matrix_world.copy(), display_type=ob.display_type,
                hidden=ob.hide_get(), hide_viewport=ob.hide_viewport, hide_render=ob.hide_render,
                props={key: copy_value(value) for key, value in ob.items()},
                owner=getattr(ob, 'project_damage_og_object', None))


def _restore(ob, state):
    if ob.type == 'MESH' and ob.data != state['data']:
        ob.vertex_groups.clear()
        for name, locked in state['groups']:
            ob.vertex_groups.new(name=name).lock_weight = locked
        ob.vertex_groups.active_index = state['group_index']
        ob.data = state['data']
    for key in list(ob.keys()):
        if key not in state['props']:
            del ob[key]
    for key, value in state['props'].items():
        ob[key] = value
    if hasattr(ob, 'project_damage_og_object'):
        ob.project_damage_og_object = state['owner']
    ob.parent = state['parent']
    ob.parent_type = state['parent_type']
    ob.parent_bone = state['parent_bone']
    ob.matrix_parent_inverse = state['inverse']
    ob.matrix_basis = state['basis']
    ob.display_type = state['display_type']
    ob.hide_viewport = state['hide_viewport']
    ob.hide_render = state['hide_render']
    if ob.name in bpy.context.view_layer.objects:
        ob.hide_set(state['hidden'])


def _matrix(value):
    a = np.asarray(value, dtype=float)
    if a.shape != (4, 4) or not np.isfinite(a).all():
        raise ValueError('Invalid background object transform')
    return Matrix(value)


def _validate(changed, removed, loaded, objects):
    names = [row['name'] for row in changed]
    if len(set(names)) != len(names) or set(names) & set(removed):
        raise ValueError('Conflicting background object records')
    known = set(objects) | set(names)
    parents = {}
    for row in changed:
        _matrix(row['matrix'])
        if 'parent_inverse' in row:
            _matrix(row['parent_inverse'])
        if 'basis' in row:
            _matrix(row['basis'])
        parent = row.get('parent')
        if parent and (parent not in known or parent in removed):
            raise ValueError('Background parent is missing: ' + parent)
        parents[row['name']] = parent
        existing = objects.get(row['name'])
        if existing is not None and (not existing.is_editable or existing.mode != 'OBJECT'):
            raise RuntimeError('Finish editing before applying the background result')
        if not row['geometry']:
            if existing is None and row.get('type', 'EMPTY') != 'EMPTY':
                raise ValueError('Unsupported background object type')
            continue
        me = loaded.get(row['mesh'])
        if me is None:
            raise ValueError('Missing background mesh: ' + row['mesh'])
        groups = row['groups']
        if len(set(groups)) != len(groups) or len(row.get('weights', [])) > len(groups):
            raise ValueError('Invalid background vertex groups')
        if len(row['materials']) != len(me.materials):
            raise ValueError('Invalid background material slots')
        for rows in row.get('weights', []):
            for weight, indices in rows:
                ids = np.asarray(indices)
                if (not math.isfinite(weight) or not 0 <= weight <= 1 or ids.ndim != 1
                        or ids.dtype.kind not in 'iu'
                        or (len(ids) and (ids.min() < 0 or ids.max() >= len(me.vertices)))):
                    raise ValueError('Invalid background vertex weights')
    for name in parents:
        seen = set()
        node = name
        while node:
            if node in seen:
                raise ValueError('Cyclic background parent hierarchy')
            seen.add(node)
            if node in parents:
                node = parents[node]
            else:
                ob = objects.get(node)
                node = ob.parent.name if ob is not None and ob.parent else None
    for name in removed:
        ob = objects.get(name)
        if ob is None or not ob.is_editable or ob.mode != 'OBJECT':
            raise ValueError('Cannot remove background object: ' + name)


def _merge(target, values):
    for key, value in values.items():
        if value is None:
            continue
        if isinstance(value, dict) and key in target and hasattr(target[key], 'keys'):
            _merge(target[key], value)
        else:
            target[key] = value


def _apply_row(ob, row, objects):
    if 'parent' in row:
        ob.parent = objects.get(row['parent'])
        ob.parent_type = row.get('parent_type', 'OBJECT')
        ob.parent_bone = row.get('parent_bone', '')
        ob.matrix_parent_inverse = _matrix(row.get('parent_inverse', Matrix.Identity(4)))
    if 'basis' in row:
        ob.matrix_basis = _matrix(row['basis'])
    else:
        ob.matrix_world = _matrix(row['matrix'])
    ob.hide_render = row['hide_render']
    ob.hide_viewport = row.get('hide_viewport', False)
    ob.display_type = row.get('display_type', ob.display_type)
    if ob.name in bpy.context.view_layer.objects:
        ob.hide_set(row['hidden'])
    _merge(ob, row['props'])
    for key in row.get('props_removed', []):
        if key in ob:
            del ob[key]


def _data_ids():
    return {item for prop in bpy.data.bl_rna.properties if prop.type == 'COLLECTION'
            for item in getattr(bpy.data, prop.identifier) if isinstance(item, bpy.types.ID)}


def _remove_unused_imports(before):
    added = _data_ids() - before
    if not added:
        return
    dependencies = {item: [] for item in added}
    keep = set()
    for item, users in bpy.data.user_map(subset=added).items():
        for user in users:
            if user in added:
                dependencies[user].append(item)
            else:
                keep.add(item)
    pending = list(keep)
    while pending:
        for item in dependencies[pending.pop()]:
            if item not in keep:
                keep.add(item)
                pending.append(item)
    for item in added:
        if item.use_fake_user:
            item.use_fake_user = False
    unused = added - keep
    if unused:
        bpy.data.batch_remove(ids=unused)


def publish(source, answer, snapshot, key_func):
    from . import background_compute as bg, ownership
    objects = {}
    for name, key in snapshot.items():
        ob = bpy.data.objects.get(name)
        if ob is None or key_func(ob) != key:
            raise RuntimeError('Object changed during background operation; result discarded: ' + name)
        objects[name] = ob
    changed = answer['changed']
    removed = answer['removed']
    before = _data_ids()
    loaded = {}
    saved = {}
    created = []
    collections = []
    collection_map = {}
    success = False
    old_meshes = set()
    try:
        names = list(dict.fromkeys(row['mesh'] for row in changed if row['geometry']))
        if names:
            file = Path(answer['output_file'])
            if not file.is_file():
                raise ValueError('Background mesh file is missing')
            with bpy.data.libraries.load(str(file), link=False) as (src, dst):
                if any(name not in src.meshes for name in names):
                    raise ValueError('Background mesh file is incomplete')
                dst.meshes = list(names)
            loaded = dict(zip(names, dst.meshes))
            for me in loaded.values():
                me.use_fake_user = False
        _validate(changed, removed, loaded, objects)
        for row in changed:
            ob = objects.get(row['name'])
            if ob is not None:
                saved[ob] = _capture(ob)
            me = loaded.get(row['mesh']) if row['geometry'] else None
            if me is not None:
                if me.users:
                    me = me.copy()
                bg.stage_mesh(me)
                for i, name in enumerate(row['materials']):
                    mat = bpy.data.materials.get(name) if name else None
                    if mat is not None:
                        me.materials[i] = mat
            if ob is None:
                ob = bpy.data.objects.new(row['name'], me)
                created.append(ob)
                objects[row['name']] = ob
                names = [n for n in row['collections'] if n != 'Collection'] or [source.name + ' Physics']
                for name in names:
                    coll = collection_map.get(name) or bpy.context.scene.collection.children.get(name)
                    if coll is None:
                        coll = bpy.data.collections.new(name)
                        collections.append(coll)
                        bpy.context.scene.collection.children.link(coll)
                    collection_map[name] = coll
                    coll.objects.link(ob)
            elif me is not None:
                old_meshes.add(ob.data)
                ob.data = me
            if me is not None:
                ob.vertex_groups.clear()
                for name in row['groups']:
                    ob.vertex_groups.new(name=name)
                for group, locked in zip(ob.vertex_groups, row.get('group_locks', [])):
                    group.lock_weight = locked
                ob.vertex_groups.active_index = row.get('group_index', 0)
                for index, rows in enumerate(row.get('weights', [])):
                    for weight, indices in rows:
                        ob.vertex_groups[index].add(np.asarray(indices).tolist(), weight, 'REPLACE')
        pending = {row['name']: row for row in changed}
        while pending:
            ready = [row for row in pending.values() if row.get('parent') not in pending]
            if not ready:
                raise ValueError('Cyclic background hierarchy')
            for row in ready:
                ob = objects[row['name']]
                _apply_row(ob, row, objects)
                if ob != source and row['name'] in answer['outputs']:
                    ownership.bind(source, ob)
                del pending[row['name']]
        if removed:
            old_meshes.update(objects[name].data for name in removed if objects[name].type == 'MESH')
            bpy.data.batch_remove(ids=[objects[name] for name in removed])
        success = True
    except Exception:
        for ob, state in saved.items():
            if bg.valid_id(ob):
                _restore(ob, state)
        for ob in reversed(created):
            if bg.valid_id(ob):
                bpy.data.objects.remove(ob, do_unlink=True)
        for coll in reversed(collections):
            if bg.valid_id(coll):
                bpy.data.collections.remove(coll)
        raise
    finally:
        if success:
            for me in old_meshes:
                if bg.valid_id(me) and me.users == 0:
                    bpy.data.meshes.remove(me)
        _remove_unused_imports(before)
    for flags, message in answer['reports']:
        print('Project Damage background:', message, flush=True)
    return {'FINISHED'}
