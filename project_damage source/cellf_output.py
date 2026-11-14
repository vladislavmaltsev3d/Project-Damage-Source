# размещаем осколки Cell Fracture и переключаем их с оригиналом
# SPDX-License-Identifier: GPL-3.0-or-later
"""Cell Fracture pieces as owned objects; geometry only, never adds physics."""
from contextlib import contextmanager

import numpy as np
from . import debris_output,rbf_output


def objects_of(source):return debris_output.objects_of(source,kind='cellf')

def place(context,source,out,factor,source_snapshot=None,uv_layer_name=None,cuts_prepared=False):
    from . import cell_cuts
    if not cuts_prepared:out=cell_cuts.apply(context,source,out,factor)
    volumes=np.asarray(out.get('cellf_piece_volume',()),dtype=np.float64)
    centers=np.asarray(out.get('cellf_piece_center',()),dtype=np.float64).reshape(-1,3)/factor
    if len(centers)!=len(volumes) or not np.isfinite(centers).all() or not np.isfinite(volumes).all():
        raise ValueError('Invalid cell fracture piece centers/volumes')
    ids=np.asarray(out.get('cellf_piece',()),dtype=np.int32)
    if ids.size and (ids.min()<0 or ids.max()>=len(volumes)):
        raise ValueError('Cell fracture piece index outside volume table')
    data={'debris_'+key:out.get('cellf_'+key,())
          for key in ('points','polys','piece','base_poly','uvs','normals')}
    data['debris_source_uvs']=bool(uv_layer_name)
    outputs=debris_output.place_debris(context,source,data,factor,source_snapshot,uv_layer_name,
                                      kind='cellf',centers=centers)
    for obj in outputs:
        piece=int(obj['project_damage_cellf_piece'])
        world=obj.matrix_world.copy();obj.parent=None;obj.matrix_world=world
        obj['project_damage_cellf_volume_cm3']=float(volumes[piece])
    return outputs

focus=rbf_output.focus


SWAP_FRAME = 'project_damage_cellf_swap_frame'
SWAP_ALL = 'project_damage_physics_swap_all'
_SWITCH_ACTION = 'project_damage_cellf_switch_action'
_SWITCH_TRACKS = {'visibility': 'Project Damage · Fracture Visibility',
                  'activation': 'Project Damage · Fracture Activation'}
_SWITCH_CHANNELS = {
    'original': {'hide_viewport': (False, True), 'hide_render': (False, True)},
    'shards': {'hide_viewport': (True, False), 'hide_render': (True, False)},
    'proxy': {'hide_viewport': (True, False), 'hide_render': (True, True)},
    'activation': {'rigid_body.enabled': (False, True)},
}


def _switch_action(source, obj, role):
    import bpy
    from bpy_extras import anim_utils

    prop = f'{_SWITCH_ACTION}_{role}'
    owner = source.get('project_damage_uid')
    action = source.get(prop)
    if isinstance(action, bpy.types.Action) and action.get(_SWITCH_ACTION) == owner:
        return action
    action = bpy.data.actions.new(f'{source.name} · {role.title()} Switch')
    action[_SWITCH_ACTION] = owner
    slot = action.slots.new(id_type='OBJECT', name='Fracture Switch')
    channelbag = anim_utils.action_ensure_channelbag_for_slot(action, slot)
    for path, values in _SWITCH_CHANNELS[role].items():
        curve = channelbag.fcurves.new(path)
        for frame, value in enumerate(values):
            key = curve.keyframe_points.insert(frame, float(value))
            key.interpolation = 'CONSTANT'
        curve.update_autoflags(obj)
    source[prop] = action
    return action


def _switch_track(obj, kind):
    ad = obj.animation_data
    if ad is None:
        return None
    name = _SWITCH_TRACKS[kind]
    return next((track for track in ad.nla_tracks if track.name.startswith(name)
                 and any(strip.action and strip.action.get(_SWITCH_ACTION)
                         for strip in track.strips)), None)


def _set_switch_track(source, obj, role, frame):
    kind = 'activation' if role == 'activation' else 'visibility'
    action = _switch_action(source, obj, role)
    ad = obj.animation_data_create()
    if not ad.use_nla and not ad.nla_tracks:
        ad.use_nla = True
    previous = _switch_track(obj, kind)
    if previous is not None and len(previous.strips) == 1:
        strip = previous.strips[0]
        if strip.action == action and strip.frame_start == frame - 1:
            return
    track = ad.nla_tracks.new()
    track.name = _SWITCH_TRACKS[kind]
    strip = track.strips.new(_SWITCH_TRACKS[kind], frame - 1, action)
    strip.action_slot = action.slots[0]
    strip.extrapolation = 'HOLD'
    if previous is not None:
        ad.nla_tracks.remove(previous)
        track.name = _SWITCH_TRACKS[kind]


def clear_activation(obj):
    track = _switch_track(obj, 'activation')
    if track is not None:
        obj.animation_data.nla_tracks.remove(track)


@contextmanager
def editable_shards(context, objects):
    """Blender's rigid-body operators require a visible active object."""
    layer = context.view_layer
    previous = []
    try:
        for obj in objects:
            track = _switch_track(obj, 'visibility')
            if track is None or obj.name not in layer.objects:
                continue
            hidden = obj.hide_get(view_layer=layer)
            if obj.hide_viewport or hidden:
                previous.append((obj, track, track.mute, obj.hide_viewport, hidden))
                track.mute = True
                obj.hide_viewport = False
                obj.hide_set(False, view_layer=layer)
        if previous:
            layer.update()
        yield
    finally:
        for obj, track, mute, viewport, hidden in previous:
            track.mute = mute
            obj.hide_viewport = viewport
            obj.hide_set(hidden, view_layer=layer)
        if previous:
            layer.update()


def sync_activation(source, obj):
    frame = source.get(SWAP_FRAME)
    body = obj.rigid_body
    if frame is None or not body or body.type != 'ACTIVE':
        clear_activation(obj)
        return
    _set_switch_track(source, obj, 'activation', frame)


def _set_visibility(context, source, obj, role, frame, view_layers):
    _set_switch_track(source, obj, role, frame)
    for view_layer in (() if role == 'proxy' else view_layers):
        if obj.name in view_layer.objects and obj.hide_get(view_layer=view_layer):
            obj.hide_set(False, view_layer=view_layer)
    state = int(context.scene.frame_current >= frame)
    viewport = _SWITCH_CHANNELS[role]['hide_viewport'][state]
    render = _SWITCH_CHANNELS[role]['hide_render'][state]
    if obj.hide_viewport != viewport:
        obj.hide_viewport = viewport
    if obj.hide_render != render:
        obj.hide_render = render


def _physics_objects(context, source):
    from . import damage_physics as dp, ownership, physics_workflow

    shards = physics_workflow.owned_piece_scope(context, source)
    shard_set = set(shards)
    colliders = {item.object for item in dp.quick(context).colliders if item.object}
    uid = source.get('project_damage_uid')
    stamps = (*ownership.STAMPS, dp.SPLIT_STAMP)
    bodies = []
    for obj in context.scene.objects:
        if obj == source or obj.type != 'MESH' or obj in colliders or obj in shard_set:
            continue
        fill = uid and obj.get(dp.FILL_STAMP) == uid
        owned = uid and any(obj.get(stamp) == uid for stamp in stamps)
        if not (fill or owned):
            continue
        if _switch_track(obj, 'visibility') is None and (
                obj.hide_render or obj.hide_viewport or
                (obj.name in context.view_layer.objects and obj.hide_get(view_layer=context.view_layer))):
            continue
        if fill and obj.get(dp.FILL_KIND) in dp.ADDITIVE:
            shards.append(obj)
        else:
            bodies.append(obj)
    return shards, bodies


def sync_switch(context, source, shards=(), *, all_layers=False):
    frame = source.get(SWAP_FRAME)
    if frame is None:
        return
    bodies = []
    if source.get(SWAP_ALL):
        if source.get('project_damage_bfrag_stage'):
            return
        shards, bodies = _physics_objects(context, source)
    if not shards:
        track = _switch_track(source, 'visibility')
        if track is not None:
            source.animation_data.nla_tracks.remove(track)
            source.hide_viewport = False
            source.hide_render = True
            if source.name in context.view_layer.objects:
                source.hide_set(True, view_layer=context.view_layer)
        for obj in bodies:
            track = _switch_track(obj, 'visibility')
            if track is not None:
                obj.animation_data.nla_tracks.remove(track)
                obj.hide_viewport = obj.hide_render = False
        return
    from . import inner_rough

    view_layers = tuple(context.scene.view_layers) if all_layers else (context.view_layer,)
    _set_visibility(context, source, source, 'original', frame, view_layers)
    for obj in bodies:
        if obj.is_editable and not _has_visibility_conflict(obj):
            _set_visibility(context, source, obj, 'shards', frame, ())
    for obj in shards:
        detail = inner_rough.detail_of(obj) if obj.get('project_damage_rough_proxy') else None
        _set_visibility(context, source, obj, 'proxy' if detail else 'shards', frame, view_layers)
        if detail is not None:
            _set_visibility(context, source, detail, 'shards', frame, view_layers)
        if obj.rigid_body is not None:
            sync_activation(source, obj)


def _collection_available(layer, source):
    collection = layer.collection
    if layer.exclude or layer.hide_viewport or collection.hide_viewport or collection.hide_render:
        return False
    return (source.name in collection.objects
            or any(_collection_available(child, source) for child in layer.children))


def _has_visibility_conflict(obj):
    from bpy_extras import anim_utils

    ad = obj.animation_data
    if ad is None:
        return False
    paths = {'hide_viewport', 'hide_render'}
    if any(curve.data_path in paths and not curve.mute for curve in ad.drivers):
        return True
    if not ad.use_nla and len(ad.nla_tracks):
        return True
    if ad.action and ad.action_slot:
        channels = anim_utils.action_get_channelbag_for_slot(ad.action, ad.action_slot)
        return channels is not None and any(curve.data_path in paths and not curve.mute
                                           for curve in channels.fcurves)
    return False


def replace_with_original(context, source, *, whole_source=False):
    from . import inner_rough, live

    whole_source = whole_source or bool(source.get(SWAP_ALL))
    if whole_source:
        shards, bodies = _physics_objects(context, source)
    else:
        shards = [obj for obj in objects_of(source) if obj.name in context.scene.objects]
        bodies = []
    if not shards:
        raise ValueError('Create fracture pieces or detach geometry first')
    details = [detail for obj in shards if obj.get('project_damage_rough_proxy')
               if (detail := inner_rough.detail_of(obj)) is not None]
    for obj in [source, *bodies, *shards, *details]:
        if not obj.is_editable:
            raise ValueError(f'{obj.name} must be editable to key its visibility')
        if _has_visibility_conflict(obj):
            raise ValueError(f'{obj.name} has direct visibility animation or disabled NLA tracks')
    scene = context.scene
    frame, subframe = scene.frame_current, scene.frame_subframe
    active = context.view_layer.objects.active
    previous = live._APPLYING
    live._APPLYING = True
    try:
        if not _collection_available(context.view_layer.layer_collection, source):
            if source.name not in scene.collection.objects:
                scene.collection.objects.link(source)
        source[SWAP_FRAME] = frame
        if whole_source:
            source[SWAP_ALL] = True
        sync_switch(context, source, shards, all_layers=True)
        scene.frame_set(frame, subframe=subframe)
        if active == source or context.view_layer.objects.active is None:
            visible = next((obj for obj in shards if obj.visible_get(view_layer=context.view_layer)), None)
            if visible is not None:
                visible.select_set(True)
                context.view_layer.objects.active = visible
        return frame, len(shards)
    finally:
        live._APPLYING = previous
