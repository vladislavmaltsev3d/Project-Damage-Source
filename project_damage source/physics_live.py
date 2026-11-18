"""Live edits of the cutting tools used by the Physics tab."""
import time
from dataclasses import dataclass

import bpy
import numpy as np

from . import background_compute as bg, build, live, paint

JOB = 'physics_preview'
_pending = {}
_samples = {}
_selection = {}
_manual_fractures = {}
_frames = {}


@dataclass
class Edit:
    source: object
    scene: object
    view_layer: str
    module: str
    frame: int
    time: float
    baseline: bool = False


def inputs(source):
    from . import damage_physics as dp
    p = source.project_damage
    return tuple(dict.fromkeys(obj for obj in (
        *(getattr(p, prop, None) for prop in paint.CELL_PROPS.values()),
        p.cellf_cut_plane, dp.settings(source).cut_drawing,
    ) if obj is not None))


def _module(context, source):
    from . import damage_physics as dp, physics_workflow
    module = physics_workflow.active_module(context, source)
    return module if module in dp.MODULES else None


def _sample(context, source):
    from . import cell_cut_plane, damage_physics as dp, incremental
    previous = live._APPLYING
    live._APPLYING = True
    try:
        editable = inputs(source)
        for obj in editable:
            if obj.type == 'CURVE' and obj.mode == 'EDIT':
                obj.update_from_editmode()
        plane = cell_cut_plane.mesh_arrays(context, source)
        paths = dp._layer_paths(context, source)
    finally:
        live._APPLYING = previous
    # Read in source space so moving the whole setup keeps the same cuts.
    def coordinates(array):
        rounded = np.around(array, 5)
        rounded[rounded == 0] = 0.
        return rounded

    p = source.project_damage
    guidance = paths['GUIDANCE'] if p.cellf_guidance_amount > 0 else ()
    guide_settings = tuple(getattr(p, 'cellf_guidance_' + key) for key in
        ('amount', 'radius', 'depth', 'falloff', 'distance')) if guidance else ()
    # A blank guide, an empty drawing and an unassigned drawing all mean no cut.
    has_cuts = bool(plane is not None or guidance or paths['THROUGH'] or paths['SURFACE'])
    data = ([coordinates(path) for path in guidance], guide_settings,
            [[coordinates(path) for path in paths[role]] for role in ('THROUGH', 'SURFACE')],
            (coordinates(plane[0]), plane[1]) if plane is not None else None)
    return incremental.digest(data), has_cuts


def _capture(context, source, module):
    try:
        _samples[source.session_uid, module] = (context.scene.frame_current, _sample(context, source))
    except (ValueError, RuntimeError, ReferenceError):
        pass


def prime(context, source, *, remember_selection=True):
    """Save the initial tools and selected fragments before switching objects."""
    module = _module(context, source)
    if module is None:
        return
    if source.session_uid not in _pending and not bg.pending(source, JOB):
        _capture(context, source, module)
    if remember_selection:
        from . import boolean_break, damage_physics as dp
        uid = source.get('project_damage_uid')
        fills = set(dp.fills_of(source, module))
        chosen = tuple(o.name for o in context.selected_objects if o in fills or (
            module == 'boolean' and o.get(boolean_break.FRAGMENT_STAMP) == uid))
        if chosen:
            _selection[source.session_uid, module] = chosen


def manual(context, source, module, chosen):
    """A manual fracture supersedes any queued preview for the same source."""
    uid = source.session_uid
    _pending.pop(uid, None)
    bg.cancel(source, JOB)
    if chosen:
        _selection[uid, module] = tuple(obj.name for obj in chosen)
    else:
        _selection.pop((uid, module), None)
    _capture(context, source, module)


def fracture_finished(context, source, module, target_names, fragments_only, *, notify=True):
    """Pause live edits once the requested pieces exist in the user's scene."""
    uid = source.session_uid
    _manual_fractures[uid, module] = (target_names, fragments_only)
    previous = live._APPLYING
    live._APPLYING = True
    try:
        source.project_damage.live_update = False
    finally:
        live._APPLYING = previous
    live._drop_dirty(source.get('project_damage_uid'))
    live._drew_since_build.discard(source.get('project_damage_uid'))
    _pending.pop(uid, None)
    bg.cancel(source, JOB)
    live._redraw_ui()
    if notify and not bpy.app.background and context.window is not None:
        _show_fracture_notice(context, source)


def _show_fracture_notice(context, source):
    window, scene = context.window, context.scene
    origin = context.area if context.area and context.area.type == 'VIEW_3D' else None

    def show():
        context = bpy.context
        try:
            if (not any(current == window for current in context.window_manager.windows) or window.scene != scene
                    or not bg.valid_id(source) or source.project_damage.live_update):
                return None
            areas = [area for area in window.screen.areas if area.type == 'VIEW_3D']
            area = origin if origin in areas else max(areas, key=lambda a: a.width * a.height, default=None)
            region = next((r for r in area.regions if r.type == 'WINDOW'), None) if area else None
            if region is not None:
                def draw(menu, _context):
                    menu.layout.label(text='Live update is disabled, hit update to refresh result')
                with context.temp_override(window=window, area=area, region=region):
                    context.window_manager.popup_menu(draw, title='Project Damage', icon='INFO')
        except (ReferenceError, RuntimeError):
            pass
        return None

    bpy.app.timers.register(show, first_interval=0.0)


def refresh(context, source):
    """Use Update in Physics to repeat the last explicit fracture."""
    if source.project_damage.live_update or context.window_manager.project_damage_ui_section != 'PHYSICS':
        return None
    module = _module(context, source)
    request = _manual_fractures.get((source.session_uid, module))
    if request is None:
        return None
    names, fragments_only = request
    if names:
        from . import boolean_break, damage_physics as dp
        uid = source.get('project_damage_uid')
        stamp = boolean_break.FRAGMENT_STAMP if fragments_only else dp.FILL_STAMP
        targets = [obj for name in names if (obj := context.view_layer.objects.get(name))
                   is not None and obj.get(stamp) == uid]
        if not targets:
            raise RuntimeError('The fracture inputs were removed. Prepare a fill or select fragments to fracture again.')
    else:
        targets = []
    with context.temp_override(selected_objects=targets, project_damage_import_source=source):
        return bpy.ops.project_damage.physics_fracture('INVOKE_DEFAULT',
            module=module, fragments_only=fragments_only, notify_live_disabled=False)


def _schedule(context, scene, source, module, *, baseline=False):
    if module is None or not source.get('project_damage_uid') or not source.project_damage.live_update:
        return
    layer = context.view_layer if context.scene == scene else scene.view_layers[0]
    _pending[source.session_uid] = Edit(source, scene, layer.name, module, scene.frame_current,
                                        time.monotonic(), baseline)
    if not bpy.app.timers.is_registered(_tick):
        bpy.app.timers.register(_tick, first_interval=.06)


def changed(props, context):
    if live._APPLYING or live._HISTORY_RESTORING:
        return
    source = props.id_data
    if source is None or context.scene is None:
        return
    if source.project_damage.cellf_enable:
        live.touch_prop(props, context)
    _schedule(context, context.scene, source, _module(context, source))


def frame_changed(scene):
    _frames[scene.session_uid] = (scene.frame_current, time.monotonic())
    # Edits queued on a different frame must not publish on the new one.
    for uid, edit in tuple(_pending.items()):
        if edit.scene == scene and edit.frame != scene.frame_current:
            del _pending[uid]


def _playing(scene):
    return any(w.scene == scene and w.screen.is_animation_playing for w in bpy.context.window_manager.windows)


def observe(scene, depsgraph, sources):
    """Listen to helper changes only. Sampling and geometry run outside the depsgraph."""
    from . import guide_timing, live_input_state
    ids = {u.id.original for u in depsgraph.updates if u.is_updated_geometry or u.is_updated_transform}
    if not ids or _playing(scene):
        return
    context = bpy.context
    last_frame, last_at = _frames.get(scene.session_uid, (None, 0.))
    timeline = last_frame == scene.frame_current and time.monotonic() - last_at < .18
    for source in sources:
        if not source.project_damage.live_update or source.name not in scene.objects:
            continue
        helpers = inputs(source)
        if not any((o in ids or o.data in ids) and
                   (not guide_timing.static(o) or live_input_state.changed(o, 'physics-guide'))
                   for o in helpers):
            continue
        module = _module(context, source)
        current = _pending.get(source.session_uid)
        prior_frame, _ = _samples.get((source.session_uid, module), (None, None))
        baseline = timeline and prior_frame != scene.frame_current and (current is None or current.baseline)
        _schedule(context, scene, source, module, baseline=baseline)


def _targets(context, source, module):
    from . import boolean_break, damage_physics as dp
    fills = dp.fills_of(source, module)
    pieces = {}
    for obj in dp.pieces_of(source):
        pieces.setdefault(obj.get('project_damage_piece_fill'), []).append(obj)
    fragments = boolean_break.fragments_of(source) if module == 'boolean' else ()
    explicit = _selection.get((source.session_uid, module), ())
    selected = {o.name for o in context.selected_objects}
    if explicit:
        selected.update(explicit)
    candidates = [o for o in (*fills, *fragments) if o.name in context.view_layer.objects]
    # Explicit selection first, then the last pieces made for this effect.
    chosen = [o for o in candidates if o.name in selected]
    if not chosen:
        chosen = [o for o in candidates if o.name in pieces]
    if not chosen:
        chosen = [o for o in candidates if o in fills]
    return [o for o in chosen if o.mode == 'OBJECT' and not any(
        piece.rigid_body or piece.animation_data or piece.mode != 'OBJECT' for piece in pieces.get(o.name, ()))]


def projection_pieces(context, source):
    from . import boolean_break, damage_physics as dp
    module = _module(context, source)
    if module is None:
        return []
    targets = dp.fills_of(source, module)
    if module == 'boolean':
        targets.extend(boolean_break.fragments_of(source))
    names = {obj.name for obj in targets}
    return [obj for obj in dp.pieces_of(source) if obj.get('project_damage_piece_fill') in names]


def _current(source, scene, frame, module, sample):
    return (bg.valid_id(source) and source.project_damage.live_update and not source.project_damage.all_off
            and scene.frame_current == frame and not _playing(scene) and _samples.get((source.session_uid, module)) == (frame, sample))


def _run(context, edit):
    from . import damage_physics as dp, python_jobs
    source, module = edit.source, edit.module
    key = source.session_uid, module
    sample = _sample(context, source)
    previous = _samples.get(key)
    _samples[key] = (edit.frame, sample)
    if edit.baseline or (previous is not None and previous[1] == sample):
        return
    if not sample[1] and (previous is None or not previous[1][1]):
        return
    targets = _targets(context, source, module)
    if not targets:
        return
    # Check the real inputs again at publication, including stroke data that
    # Blender's mesh-only transaction check cannot see.
    names = {obj.name for obj in targets}

    def ready():
        if not _current(source, edit.scene, edit.frame, module, sample) or _sample(bpy.context, source) != sample:
            return False
        return not any(obj.get('project_damage_piece_fill') in names
                       and (obj.rigid_body or obj.animation_data or obj.mode != 'OBJECT')
                       for obj in dp.pieces_of(source))

    if python_jobs.queue_preview(context, source, module, targets, ready):
        return
    bg.cancel(source, JOB)
    pieces, reports = dp.fracture(context, source, module, fills=targets, live_preview=True)
    paint.refresh_physics_projection(context, source, pieces)
    for flags, message in reports:
        if flags & {'ERROR', 'WARNING'}:
            print('Project Damage live fracture:', message)
    live._redraw_ui()


def _tick():
    if live._APPLYING or live._HISTORY_RESTORING:
        return .08
    now = time.monotonic()
    for uid, edit in tuple(_pending.items()):
        source, scene = edit.source, edit.scene
        if (not bg.valid_id(source) or not bg.valid_id(scene) or not source.project_damage.live_update
                or source.project_damage.all_off or edit.frame != scene.frame_current):
            _pending.pop(uid, None)
            continue
        if now - edit.time < max(.12, source.project_damage.defer_ms / 1000.) or _playing(scene):
            continue
        layer = scene.view_layers.get(edit.view_layer)
        if layer is None or source.name not in layer.objects:
            _pending.pop(uid, None)
            continue
        with bpy.context.temp_override(scene=scene, view_layer=layer):
            if bg.interacting(scene) or live._busy(source, editable=inputs(source)) or bg.pending(source, 'python_operation'):
                continue
            _pending.pop(uid, None)
            try:
                _run(bpy.context, edit)
            except (ReferenceError, RuntimeError, ValueError) as exc:
                print('Project Damage live fracture:', exc)
    return .08 if _pending else None


def clear():
    if bpy.app.timers.is_registered(_tick):
        bpy.app.timers.unregister(_tick)
    _pending.clear()
    _samples.clear()
    _selection.clear()
    _manual_fractures.clear()
    _frames.clear()
