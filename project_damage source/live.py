# очередь обновлений после реального изменения настроек, рисунков или объектов
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
"""Deferred live rebuilds - the Blender equivalent of C4D's Interactive Delay
(cpp/CLAUDE.md, "Deferred rebuild"): a source keeps rebuilding itself in the
background as its settings or a linked breaker change, coalesced behind a
short delay so a slider drag or a viewport-drag breaker move costs one rebuild
at release, not one per intermediate value.

Two empirical facts drive the design: an Object.project_damage / Object.project_damage_breaker
PropertyGroup field edit NEVER lands in depsgraph.updates (Blender reports
depsgraph updates for evaluated-graph state only - object transforms, mesh
geometry - never for a plain PropertyGroup value change), so every scalar
field on ProjectDamageProps and ProjectDamageBreakerProps carries update=touch_prop (props.py),
which marks the relevant source(s) dirty directly; and object TRANSFORM and
mesh GEOMETRY edits DO land, which is what _on_depsgraph catches - a breaker
or breaker-child drag, a source mesh's own edit - neither of which touches a
PropertyGroup field at all.
"""
import time

import bpy
from bpy.app.handlers import persistent

from . import build, live_input_state

                                                                                     
                                                                       
                                                                                   
                                                                            
                                                                        
                                                                     
_dirty = {}
_dirty_context = {}
_event_scene = None
_draw_exit_until = {}
_drawing_source_name = None
_drawing_deferred = False
_drawing_object_name = None


def drawing_started(source, *, drawing=None, defer=None):
    global _drawing_source_name,_drawing_deferred,_drawing_object_name
    from . import guide_timing
    drawing = drawing or bpy.context.object
    guide_timing.start_paint(bpy.context.scene, drawing)
    if defer is None:
        from . import fabric_paint
        defer=fabric_paint.is_drawing(drawing or bpy.context.object,source)
    _drawing_source_name=source.name if source is not None else None
    _drawing_deferred=bool(defer)
    _drawing_object_name=drawing.name if drawing is not None else None
    if source is not None:_draw_exit_until.pop(source.get('project_damage_uid'),None)


GP_DEBOUNCE = 0.4
LIVE_DRAW_FACE_LIMIT = 50000
_last_gp_update = 0.0
_drew_since_build = set()
GP_PAINT_MODES={'PAINT_GREASE_PENCIL','PAINT_GPENCIL'}


def _busy(source=None, *, editable=()):
    if _drawing_deferred and (source is None or source.name==_drawing_source_name):
        return True
    from . import fabric_paint
    if source is not None and source.project_damage.foam_enable:
        settings = source.project_damage_foam
        drawing = settings.drawing
        if settings.placement == 'DRAW' and drawing is not None and drawing.type == 'CURVE':
            editable = (*editable, drawing)
    active = bpy.context.active_object
    if source is not None and active is not None and active.type == 'CURVE' and active.mode == 'EDIT':
        from . import boolean_stack, rebar
        layers = [props for _, props in boolean_stack.layers(source)] if boolean_stack.SERIAL in source else [source.project_damage]
        if any(rebar.drawn_curve(props) == active for props in layers):
            editable = (*editable, active)
    try:objects=bpy.context.view_layer.objects
    except AttributeError:return False
    drawing=False
    for obj in objects:
        if obj is None:continue
        try:mode=obj.mode
        except ReferenceError:continue
        if mode=='OBJECT' or (mode=='EDIT' and obj in editable):continue
        if obj.type in {'GREASEPENCIL','GPENCIL'} and obj.mode in GP_PAINT_MODES:
            drawing=True
            explicit_live=obj.name==_drawing_object_name and not _drawing_deferred
            if not explicit_live and fabric_paint.is_drawing(obj,source):return True
        else:return True
    if drawing:
        from . import background_compute
        if background_compute.interacting(bpy.context.scene):return True
    return False


def finish_drawing(source):
    """Finishing a drawing is navigation, not an implicit geometry update -
    Pending Fabric strokes are built once here. Other drawings already
    update between strokes and retain any running background update."""
    global _drawing_source_name,_drawing_deferred,_drawing_object_name
    from . import guide_timing, paint
    guide_timing.stop_paint()
    paint.finish_surface_projection()
    deferred=_drawing_deferred
    _drawing_source_name=None
    _drawing_deferred=False
    _drawing_object_name=None
    if source is None:return
    uid=source.get('project_damage_uid')
    if uid:
        drew = uid in _drew_since_build or uid in _dirty
        _drew_since_build.discard(uid)
        if drew and source.project_damage.live_update:
            _frame_dirty.pop(uid, None)
            _dirty[uid] = time.monotonic()
            _remember_context(source)
            _ensure_timer()
        else:
            _drop_dirty(uid)
        _draw_exit_until[uid]=time.monotonic()+.5
    if deferred:
        from . import background_compute
        background_compute.cancel(source)


def drawing_exit_suppressed(source):
    uid=source.get('project_damage_uid')
    return time.monotonic() < _draw_exit_until.get(uid,0.)

                                                                       
                                                                          
                                                                       
                                                                            
                                                                         
                                                                            
                                                                             
                                                                        
                                                                            
               
_APPLYING = False
_HISTORY_RESTORING = False
_drawing_frames = {}
_frame_dirty = {}
_changing_frames = set()


def playing_scenes():
    wm = bpy.context.window_manager
    return {window.scene for window in wm.windows if window.screen.is_animation_playing} if wm else set()


def keeps_fragments(source):
    from . import boolean_break
    return boolean_break.wanted(source) or bool(boolean_break.fragments_of(source))


def _drop_dirty(uid):
    _dirty.pop(uid, None)
    _dirty_context.pop(uid, None)
    _frame_dirty.pop(uid, None)


def _clear_pending():
    global _drawing_source_name,_drawing_deferred,_drawing_object_name
    from . import guide_timing, paint
    guide_timing.stop_paint()
    paint.finish_surface_projection()
    _drawing_source_name=None
    _drawing_deferred=False
    _drawing_object_name=None
    from . import background_compute
    background_compute.cancel()
    live_input_state.clear()
    from . import ivy, roots
    ivy.clear_pending()
    roots.clear_pending()
    from . import foam, physics_live
    foam.clear_pending()
    physics_live.clear()
    _dirty.clear()
    _dirty_context.clear()
    _drawing_frames.clear()
    _frame_dirty.clear()
    _changing_frames.clear()
    invalidate_index()
    _draw_exit_until.clear()
    _drew_since_build.clear()
    if bpy.app.timers.is_registered(build._stale_flush_timer):
        bpy.app.timers.unregister(build._stale_flush_timer)
    build.stale_mesh_flush()
    _BREAKER_SIG.clear()
    if bpy.app.timers.is_registered(_tick):
        bpy.app.timers.unregister(_tick)
                                                                        
                                                                            
    build._CACHES.clear()
    from . import input_prep
    input_prep.clear_prep_cache()
    from . import incremental
    incremental.clear_outputs()
    from . import material_update
    material_update.clear()


@persistent
def _history_pre(*unused):
    from . import paint
    paint.suspend_surface_projection()
    global _HISTORY_RESTORING
    _HISTORY_RESTORING = True
    if bpy.app.timers.is_registered(_history_resume):
        bpy.app.timers.unregister(_history_resume)
    _clear_pending()


@persistent
def _history_post(*unused):
    global _HISTORY_RESTORING
    _HISTORY_RESTORING = True
                                                                          
                                                                        
    _clear_pending()
    if bpy.app.background:
        # Timers never run without a window: release the guard right away, or
        # every later rebuild in this process is refused.
        _HISTORY_RESTORING = False
        from . import paint
        _seed_drawing_frames()
        paint.resume_surface_projection(bpy.context)
        return
    if not bpy.app.timers.is_registered(_history_resume):
        bpy.app.timers.register(_history_resume, first_interval=0.1)


def _history_resume():
    global _HISTORY_RESTORING
    _clear_pending()
    _HISTORY_RESTORING = False
    from . import paint
    _seed_drawing_frames()
    paint.resume_surface_projection(bpy.context)
                                                                             
    return None


def _breaker_references(br,obj):
    cutter=br.project_damage_breaker.cutter
    while obj is not None:
        if obj==br or obj==cutter:return True
        obj=obj.parent
    return False


def _breaker_ancestor(obj, scan=None):
    """`obj` itself if it is a breaker Empty, else the first breaker Empty
    among its parents, else None. Shared by _relevant_sources (below) and
    _on_depsgraph's transform-only resync check - both need to know whether
    an object sits inside a breaker's own subtree."""
    scan = scan or _scan()
    cache = _EVENT_CACHE
    if cache is not None and scan is _EVENT_SCAN:
        # One depsgraph event can report hundreds of objects (showing a
        # collection, loading a file). The answer per object cannot change
        # inside the event, and the cutter lists are shared by all of them.
        key = obj.as_pointer()
        known = cache['ancestor']
        if key in known:
            return known[key]
        table = cache.get('breaker_table')
        if table is None:
            def entry(br):
                cutter = br.project_damage_breaker.cutter
                return br, br.as_pointer(), cutter.as_pointer() if cutter is not None else 0
            table = cache['breaker_table'] = (
                [entry(br) for br in scan.breakers],
                [entry(cutter) for br in scan.linkers for cutter in _cutters_of(br) if cutter is not None])
        chain = set()
        parent = obj
        while parent is not None:
            chain.add(parent.as_pointer())
            parent = parent.parent
        found = None
        for rows in table:
            for br, pointer, cutter_pointer in rows:
                if pointer in chain or cutter_pointer in chain:
                    found = br
                    break
            if found is not None:
                break
        else:
            found = obj
            while found is not None and not (found.project_damage_breaker.is_breaker):
                found = found.parent
        known[key] = found
        return found
    for br in scan.breakers:
        if _breaker_references(br,obj):return br
    for br in scan.linkers:
        for cutter in _cutters_of(br):
            if _breaker_references(cutter,obj):return cutter
    breaker = obj
    while breaker is not None and not (breaker.project_damage_breaker.is_breaker):
        breaker = breaker.parent
    return breaker


def _cutters_of(source):
    """Boolean cutters of `source`; computed once per depsgraph event."""
    from . import boolean_stack
    cache = _EVENT_CACHE
    if cache is None:
        return boolean_stack.cutter_objects(source)
    key = source.as_pointer()
    cutters = cache['cutters'].get(key)
    if cutters is None:
        cutters = cache['cutters'][key] = boolean_stack.cutter_objects(source)
    return cutters


def _new_event_cache():
    return {'ancestor': {}, 'cutters': {}}


class _Scan:
    """One pass over bpy.data.objects, shared by every lookup of one depsgraph
    event. Sources without a uid were never built, so _mark_dirty would skip
    them anyway; the lookups below only need the objects that can matter."""
    __slots__=('sources','breakers','linkers','planks','cables','owned','unlinked_results','tracked','_refs')
    def __init__(self):
        from . import boolean_stack
        self.sources=[];self.breakers=[];self.linkers=[];self.owned={};self.unlinked_results={};self._refs=None
        self.planks=[];self.cables=[]
        for ob in bpy.data.objects:
            if ob.get('pd_plank_surface'):self.planks.append(ob)
            if ob.type in {'CURVE','EMPTY'}:
                cable=getattr(ob,'project_damage_cable_gen',None)
                if cable is not None and cable.enabled:self.cables.append(ob)
            if ob.project_damage_breaker.is_breaker:self.breakers.append(ob)
            if ob.type!='MESH':continue
            if ob.get('project_damage_uid') is not None:self.sources.append(ob)
            if boolean_stack.SERIAL in ob or len(ob.project_damage.breakers) or ob.project_damage.break_objects is not None:
                self.linkers.append(ob)
            stamp = ob.get(build.RESULT_STAMP)
            if stamp:
                owner = ob.project_damage_og_object
                if owner is not None:
                    self.owned.setdefault(owner.as_pointer(), ob)
                else:
                    self.unlinked_results.setdefault(stamp, ob)
        self.tracked={ob.session_uid for group in (self.sources, self.breakers, self.linkers,
            self.planks, self.cables, self.owned.values(), self.unlinked_results.values()) for ob in group}

    def result(self, source):
        return self.owned.get(source.as_pointer()) or self.unlinked_results.get(source.get('project_damage_uid'))

    def users(self,data):
        return (obj for obj in bpy.data.user_map(subset=(data,), value_types={'OBJECT'}).get(data, ())
                if obj.data==data)
    def referencing(self,obj):
        """Built sources that name `obj` in a drawing, an Empty or a collection."""
        if self._refs is None:
            from .new_effects import DRAWINGS
            refs=self._refs={}
            def add(target,source):
                if target is not None:refs.setdefault(target.as_pointer(),[]).append(source)
            def add_all(coll,source,skip_self=False):
                if coll is None:return
                for member in coll.all_objects:
                    if not(skip_self and member==source):add(member,source)
            for source in self.sources:
                p=source.project_damage
                from . import boolean_stack, fracture_stack, wall_stack
                for target in fracture_stack.references(source):add(target,source)
                for target in wall_stack.references(source):add(target,source)
                for target in boolean_stack.references(source):add(target,source)
                if p.wallhole_enable:add_all(source.project_damage_wallhole.sites,source)
                for coll in (p.fract_prox_collection,p.fract_shard_collection,p.rbf_impact_collection,p.cellf_prox_collection):
                    add_all(coll,source,True)
                for target in (p.surf_drawing,p.concrete_drawing,p.rust_drawing,p.coating_drawing,p.fract_l1_drawing,p.fract_l2_drawing,p.fract_l3_drawing):
                    add(target,source)
                if source.get(build.RESULT_STAMP):continue
                for field in DRAWINGS:
                    if not field.startswith('cellf_') or p.cellf_enable:
                        add(getattr(p,field),source)
                from . import damage_proximity
                for target in damage_proximity.references(p):add(target,source)
                if p.cellf_enable:add(p.cellf_cut_plane,source)
                if p.paper_generator_enable:add(source.project_damage_paper.drawing,source)
                if p.foam_enable:add(source.project_damage_foam.drawing,source)
                if p.wallpaper_enable:
                    from .wallpaper_controls import finders
                    for finder in finders(p):add(finder,source)
                if p.glass_fill_hole and (p.glass_drawn_enable or p.glass_hit_enable):
                    add(p.glass_fill_origin, source)
                for coll in (p.wallpaper_flap_collection,p.hit_collection,p.glass_hit_collection,p.blast_collection,p.metal_collection,p.metal_crush_collection):
                    add_all(coll,source)
                if p.deform_enable:
                    add(p.deform_origin,source)
                    add_all(p.deform_collection,source);add_all(p.deform_protection,source)
        return self._refs.get(obj.as_pointer(),())


_EVENT_SCAN=None
_EVENT_CACHE=None
_INDEX=None
_INDEX_COUNT=-1
_INPUT_DATA_TYPES=(bpy.types.Mesh,bpy.types.Curve)+tuple(
    getattr(bpy.types,name) for name in ('GreasePencil','GreasePencilv3') if hasattr(bpy.types,name))


def invalidate_index():
    global _INDEX, _INDEX_COUNT
    _INDEX=None
    _INDEX_COUNT=-1
    forget_frame_static()


def event_relevant(depsgraph):
    data_types=(bpy.types.Object,bpy.types.Collection,bpy.types.Scene)+_INPUT_DATA_TYPES
    return any(isinstance(u.id,data_types) and
               (u.is_updated_transform or u.is_updated_geometry) for u in depsgraph.updates)


def _index_changed(update):
    if not update.is_updated_geometry:
        return False
    obj=update.id.original
    if isinstance(obj,(bpy.types.Collection,bpy.types.Scene)):
        return True
    if not isinstance(obj,bpy.types.Object):
        return False
    return (_INDEX is not None and obj.session_uid in _INDEX.tracked
            or obj.get('project_damage_uid') is not None or bool(obj.get(build.RESULT_STAMP))
            or bool(obj.get('pd_plank_surface')) or obj.project_damage_breaker.is_breaker
            or (obj.type=='MESH' and (len(obj.project_damage.breakers) or obj.project_damage.break_objects is not None)))


def _scan():
    global _INDEX, _INDEX_COUNT
    if _EVENT_SCAN is not None:return _EVENT_SCAN
    if _INDEX is None or _INDEX_COUNT!=len(bpy.data.objects):
        _INDEX=_Scan();_INDEX_COUNT=len(bpy.data.objects)
    return _INDEX


def _remember_context(source,scene=None,layer=None):
    uid=source.get('project_damage_uid')
    if not uid:return
    scene=scene or _event_scene or bpy.context.scene
    if scene is None or source.name not in scene.objects:
        scene=next((s for s in source.users_scene),None)
    if scene is None:return
    if layer is None:
        current=bpy.context.view_layer
        layer=current if current is not None and scene.view_layers.get(current.name)==current else None
    if layer is None or source.name not in layer.objects:
        layer=next((v for v in scene.view_layers if source.name in v.objects),None)
    if layer is not None:_dirty_context[uid]=(scene,layer.name)


def _relevant_sources(obj, scan=None, *, include_self=True):
    """Yield sources affected by changes to this object or its breaker."""
    from . import boolean_stack
    scan = scan or _scan()
    if include_self and obj.type == 'MESH':
        yield obj
    for source in scan.referencing(obj):
        if include_self or source != obj:
            yield source
    breaker = _breaker_ancestor(obj, scan)
    for candidate in scan.linkers:
        if candidate.get('project_damage_uid') is None or (not include_self and candidate == obj):
            continue
        is_result = candidate.get(build.RESULT_STAMP)
        if any(cutter == breaker or (not is_result and _breaker_references(cutter, obj))
               for cutter in _cutters_of(candidate)):
            yield candidate


# помечаем объект как изменённый, но не запускаем расчёт здесь
def _mark_dirty(obj, scan=None, *, include_self=True, scene=None, geometry_edit=False):
    """`obj` just changed (a prop edit or a depsgraph transform/geometry
    update) - dirty every source it is relevant to, subject to the actual
    opt-in gate: live_update on, and a result object already exists (the
    manual first build happened)."""
    if _HISTORY_RESTORING:
        return
    from . import background_compute as bg
    scan = scan or _scan()
    touched = False
    timeline = _event_scene is not None and (
        _event_scene.session_uid in _changing_frames or (not geometry_edit and _event_scene in playing_scenes()))
    for source in dict.fromkeys(_relevant_sources(obj, scan, include_self=include_self)):
        if timeline and keeps_fragments(source):
            continue
        bg.invalidate(source)
        if drawing_exit_suppressed(source):continue
        if not source.project_damage.live_update:
            continue
                                                                             
                                                                               
                                                         
        uid = source.get("project_damage_uid")
        if uid is None:
            continue
        if scan.result(source) is None:
            continue
        _frame_dirty.pop(uid, None)
        _dirty[uid] = time.monotonic()
        _remember_context(source,scene)
        touched = True
    if touched:
        _ensure_timer()


def touch_prop(self, context):
    """Shared `update=` callback for every ProjectDamageProps/ProjectDamageBreakerProps field
    (props.py) - the fallback the probe above requires, since a
    PropertyGroup field edit never reaches _on_depsgraph on its own."""
    if _APPLYING or _HISTORY_RESTORING:
        return
    obj = self.id_data
    if obj is not None:
        invalidate_index()
        from . import background_compute
        scan = _scan()
        for source in dict.fromkeys(_relevant_sources(obj, scan)):
            _draw_exit_until.pop(source.get('project_damage_uid'),None)
            background_compute.cancel(source)
        _mark_dirty(obj, scan, scene=context.scene)


@persistent
def _on_depsgraph(scene, depsgraph):
    global _EVENT_SCAN, _EVENT_CACHE, _event_scene
    if _APPLYING or _HISTORY_RESTORING:
        return
    exiting=bool(_drawing_source_name and bpy.context.mode not in GP_PAINT_MODES)
    if not exiting and not event_relevant(depsgraph):return
    if any(_index_changed(update) for update in depsgraph.updates):
        invalidate_index()
    scan = _scan()
    if not exiting and not scan.sources:return
    previous_scan,previous_scene,previous_cache = _EVENT_SCAN,_event_scene,_EVENT_CACHE
    _EVENT_SCAN = scan
    _EVENT_CACHE = _new_event_cache()
    _event_scene=scene
    try:
        _handle_depsgraph(scene, depsgraph)
        from . import physics_live
        physics_live.observe(scene, depsgraph, scan.sources)
    finally:
        _EVENT_SCAN = previous_scan
        _EVENT_CACHE = previous_cache
        _event_scene = previous_scene


_swapped = {}


def _material_swap(obj):
    """True when this source update only put other materials into its existing
    slots. The outputs get the same swap on the next timer tick; no rebuild."""
    if obj.get('project_damage_uid') is None:
        return False
    from . import incremental
    uid = obj.session_uid
    recent = _swapped.get(uid)
    if recent is not None and time.monotonic() - recent[0] < .25 and recent[1] == incremental.slot_signature(obj):
        return True
    try:
        swaps = incremental.material_swaps(obj)
    except (ReferenceError, RuntimeError):
        return False
    if not swaps or build.result_of(obj) is None:
        return False
    from . import material_controls, material_uv
    explicit = {m.session_uid for m in map(material_uv.base_material, material_controls.explicit_materials(obj.project_damage)) if m is not None}
    if any(old in explicit for old, _ in swaps):
        return False
    incremental.remember_slots(obj)
    _swapped[uid] = (time.monotonic(), incremental.slot_signature(obj))
    while len(_swapped) > 32:
        del _swapped[next(iter(_swapped))]
    name = obj.name
    rows = [(old, material.name) for old, material in swaps]
    bpy.app.timers.register(lambda: _apply_material_swap(name, rows), first_interval=0.0)
    return True


def _apply_material_swap(name, rows):
    from . import boolean_break, incremental, material_update, material_uv
    source = bpy.data.objects.get(name)
    if source is None or _HISTORY_RESTORING:
        return None
    result = build.result_of(source)
    if result is None:
        return None
    outputs = [result, *boolean_break.fragments_of(source), build.coating_of(source), build.deposits_of(source)]
    known = incremental.body_known(source)
    try:
        with material_update.shader_edit(bpy.context):
            for out in dict.fromkeys(o for o in outputs if o is not None and o.type == 'MESH' and o.is_editable and o.mode == 'OBJECT'):
                for index, slot in enumerate(out.material_slots):
                    current = slot.material
                    if current is None:
                        continue
                    base = material_uv.base_material(current)
                    for old, new_name in rows:
                        material = bpy.data.materials.get(new_name)
                        if material is None:
                            continue
                        if current.session_uid == old:
                            material_target = material
                        elif base is not None and base.session_uid == old:
                            material_target = material_uv.for_object(material, out)
                        else:
                            continue
                        if out.data.users > 1 and slot.link == 'DATA':
                            out.data = out.data.copy()
                        out.material_slots[index].material = material_target
                        break
            if known:
                incremental.remember_body(bpy.context, source, result)
    except Exception:
        import traceback
        traceback.print_exc()
        _mark_dirty(source)
    return None


def _follow_source_outputs(source, *, timeline=False):
    from . import ownership,boolean_break
    uid=source.get('project_damage_uid')
    if not uid:return
    animated_fragments = timeline and keeps_fragments(source)
    # острова физики имеют собственные центры; перемещаем только цельные результаты
    stamps=set(ownership.STAMPS)-{'project_damage_debris_of','project_damage_rbf_of','project_damage_cellf_of','project_damage_wallhole_of'}
    cache=_EVENT_CACHE
    if cache is not None:
        # index the stamped outputs once per event instead of walking every
        # object of the file for every moved source
        owned=cache.get('outputs')
        if owned is None:
            owned=cache['outputs']={}
            for out in bpy.data.objects:
                if out.type!='MESH':continue
                for stamp in stamps:
                    value=out.get(stamp)
                    if value:
                        rows=owned.setdefault(value,[])
                        if out not in rows:rows.append(out)
        candidates=owned.get(uid,())
    else:
        candidates=[out for out in bpy.data.objects
                    if out.type=='MESH' and any(out.get(stamp)==uid for stamp in stamps)]
    for out in candidates:
        if out is not source:
            if animated_fragments and out.rigid_body is not None:
                continue
            if out.matrix_world!=source.matrix_world:out.matrix_world=source.matrix_world
    boolean_break.follow_source(source, timeline=timeline)


def _handle_depsgraph(scene, depsgraph):
    """Catches what touch_prop cannot: a breaker or breaker-child TRANSFORM
    (a viewport drag moves the Object, not a PropertyGroup field) and a
    source's own mesh GEOMETRY edit. @persistent so the handler survives a
    .blend load - Blender drops non-persistent handlers there.

    _place()'s own mesh remove + material relink forces a depsgraph RELATIONS
    rebuild, which re-reports every object in the scene with
    is_updated_shading (not transform, not geometry) on the evaluation right
    after a rebuild - by which point _tick has already returned and
    _APPLYING is back down. Without the flag filter below, that single
    self-report would re-dirty every live source in the scene, forever: one
    edit anywhere would storm the whole scene into rebuilding on a loop. The
    two flags this handler exists to react to are exactly
    is_updated_transform (a move) and is_updated_geometry (a mesh edit), so
    filtering on them is not a narrowing - it is what "relevant" already
    meant.
    """
    if _APPLYING or _HISTORY_RESTORING:
        return
    scan = _scan()
    timeline = scene.session_uid in _changing_frames or scene in playing_scenes()
    if not timeline:
        # a real edit (new keyframe in a drawing, relinked input): re-check on the next frame change
        forget_frame_static()
    global _drawing_source_name
    if _drawing_source_name and bpy.context.mode not in {'PAINT_GREASE_PENCIL','PAINT_GPENCIL'}:
        source=bpy.data.objects.get(_drawing_source_name)
        finish_drawing(source)
    moving={u.id.original for u in depsgraph.updates if u.is_updated_transform
            and isinstance(u.id.original,bpy.types.Object) and u.id.original.mode=='OBJECT'}
    from . import ivy, roots
    ivy.dependencies_changed(scene, depsgraph)
    roots.dependencies_changed(scene, depsgraph)
    from . import foam
    foam.dependencies_changed(scene,depsgraph)
    from . import material_update
    ignored = material_update.consume_rna_notifications(depsgraph)
    global _last_gp_update
    for update in depsgraph.updates:
        if update.id.original.session_uid in ignored:
            continue
        if not (update.is_updated_transform or update.is_updated_geometry):
            continue
        obj = update.id
        if not isinstance(obj, bpy.types.Object):
            if not isinstance(obj,_INPUT_DATA_TYPES):
                continue
                                                                           
            for candidate in scan.users(obj.original):
                if candidate.type in {'GREASEPENCIL', 'GPENCIL', 'CURVE', 'MESH'}:
                    if candidate.type=='MESH' and not (candidate.get('project_damage_uid') is not None
                            or scan.referencing(candidate) or _breaker_ancestor(candidate, scan)):
                        continue
                    if candidate.type=='MESH' and foam.shader_only_update(candidate,depsgraph):continue
                    if candidate.type=='MESH' and _material_swap(candidate):continue
                    if candidate.type=='MESH':
                        if candidate in moving or candidate.get(build.RESULT_STAMP):continue
                        if not live_input_state.changed(candidate,'body-mesh',local=True):continue
                    if candidate.type in live_input_state.DRAWING_TYPES:
                        if not _scan().referencing(candidate) or not live_input_state.changed(candidate, 'body'):
                            continue
                        _last_gp_update = time.monotonic()
                    _mark_dirty(candidate, geometry_edit=candidate.type == 'MESH')
            continue
                                                                      
                                                                            
                                                                        
                                                                      
                                                                       
        obj = obj.original
        breaker = _breaker_ancestor(obj) if obj.type == 'MESH' else None
        if obj in moving and obj.type=='MESH' and breaker is None:
            if obj.get(build.RESULT_STAMP):
                source=obj.project_damage_og_object
                if obj==bpy.context.active_object and source is not None:
                    if not (timeline and obj.rigid_body is not None and keeps_fragments(source)):
                        source.matrix_world=obj.matrix_world
                        _follow_source_outputs(source, timeline=timeline)
                continue
            if obj.get('project_damage_uid') is not None:
                _follow_source_outputs(obj, timeline=timeline)
                continue
            from .ownership import STAMPS
            if any(obj.get(stamp) for stamp in STAMPS):continue
        if update.is_updated_geometry and obj.type == 'MESH' and _material_swap(obj):
            continue
        if obj.type in live_input_state.CONTROL_TYPES:
            if not scan.referencing(obj) and _breaker_ancestor(obj, scan) is None:
                continue
            if not live_input_state.changed(obj, 'body'):
                continue
            if obj.type in live_input_state.DRAWING_TYPES:
                _last_gp_update = time.monotonic()
        if foam.shader_only_update(obj,depsgraph):continue
        if obj.get('project_damage_fill_piece_of') or obj.get('project_damage_split_piece_of') or obj.get('project_damage_fragment_of'):
            continue                                                                  
                                                                            
                                                                            
                                                                            
                                                                          
                                                                            
                                                                            
                                                                      
                                                                            
                                                                          
                                                                            
                                                               
        if obj.type == 'MESH' and breaker is not None:
            evaluated = obj.modifiers or obj.constraints or obj.data.shape_keys
            if not evaluated and not live_input_state.changed(obj, 'body'):
                continue
            geometry_edit = (update.is_updated_geometry and not update.is_updated_transform
                             and live_input_state.changed(obj, 'body-mesh', local=True))
            _mark_dirty(obj, geometry_edit=geometry_edit)
            continue
        if obj.type=='MESH':
            # готовый результат не является исходным мешем для новой сборки
            if obj.get(build.RESULT_STAMP):
                if update.is_updated_transform and obj.mode=='OBJECT' and obj==bpy.context.active_object:
                    source=obj.project_damage_og_object
                    if source is not None and source.matrix_world!=obj.matrix_world:
                        source.matrix_world=obj.matrix_world
                        _follow_source_outputs(source, timeline=timeline)
                _mark_dirty(obj,include_self=False)
                continue
            if obj.get('project_damage_uid') is not None:
                if update.is_updated_transform and obj.mode=='OBJECT':
                    _follow_source_outputs(obj, timeline=timeline)
                if update.is_updated_geometry and live_input_state.changed(obj,'body-mesh',local=True):
                    _mark_dirty(obj, geometry_edit=True)
                elif update.is_updated_transform:
                    _mark_dirty(obj,include_self=False)
                continue
            # An unrelated mesh can get a geometry notification during ordinary Blender work.
            if not scan.referencing(obj):continue
            if update.is_updated_geometry and not update.is_updated_transform:
                if not live_input_state.changed(obj,'body-mesh',local=True):continue
        _mark_dirty(obj)
    _sync_breaker_lists()


_BREAKER_SIG = {}                                                                         


def _breaker_signature(source):
    from .new_effects import DRAWINGS
    p = source.project_damage
    from . import damage_proximity
    from . import boolean_stack, fracture_stack
    return (tuple(sorted(o.name for o in fracture_stack.references(source))),tuple(o.name for o in damage_proximity.references(p)),
            boolean_stack.cutter_signature(source),
            tuple(ob.name if ob else None for ob in [p.surf_drawing, p.concrete_drawing, p.rust_drawing, p.coating_drawing] +
                  [getattr(p, field) for field in DRAWINGS if not field.startswith('fract_')
                   and (not field.startswith('cellf_') or p.cellf_enable)] +
                  [p.cellf_cut_plane if p.cellf_enable else None,
                   source.project_damage_paper.drawing if p.paper_generator_enable else None,
                   source.project_damage_foam.drawing if p.foam_enable else None,
                   p.wallpaper_finder if p.wallpaper_enable else None,
                   p.glass_fill_origin if p.glass_fill_hole and (p.glass_drawn_enable or p.glass_hit_enable) else None]),
            tuple(sorted(obj.name for obj in p.hit_collection.all_objects)) if p.hit_collection else (),
            tuple(sorted(obj.name for obj in p.glass_hit_collection.all_objects)) if p.glass_hit_collection else (),
            tuple(sorted(obj.name for obj in p.wallpaper_finders.all_objects)) if p.wallpaper_finders else (),
            tuple(sorted(obj.name for obj in p.wallpaper_flap_collection.all_objects)) if p.wallpaper_flap_collection else (),
            tuple(sorted(obj.name for obj in p.blast_collection.all_objects)) if p.blast_collection else (),
            tuple(sorted(o.name for o in p.metal_collection.all_objects)) if p.metal_collection else (),
            tuple(sorted(o.name for o in p.metal_crush_collection.all_objects)) if p.metal_crush_collection else (),
            tuple(sorted(o.name for o in p.rbf_impact_collection.all_objects)) if p.rbf_impact_collection else (),
            tuple(sorted(o.name for o in p.cellf_prox_collection.all_objects)) if p.cellf_prox_collection else (),
            tuple(sorted(o.name for o in source.project_damage_wallhole.sites.all_objects)) if source.project_damage_wallhole.sites else ())


def _sync_breaker_lists():
    """DELETING a breaker Empty clears every PointerProperty that referenced
    it with no update callback and no transform/geometry update on any
    surviving id, so neither entry point above sees it - the result kept
    showing the break of a breaker that no longer existed. Compare each live source's link list with what it was on the
    previous evaluation and dirty the ones that changed. Sources never built
    have no uid and are skipped, exactly like _mark_dirty skips them."""
    for source in _scan().sources:
        if not source.project_damage.live_update:
            continue
        uid = source.get("project_damage_uid")
        sig = _breaker_signature(source)
        if _BREAKER_SIG.get(uid) != sig:
            if uid in _BREAKER_SIG:
                _mark_dirty(source)
            _BREAKER_SIG[uid] = sig


def _frame_drawings(source):
    from . import fracture_stack
    from .new_effects import DRAWINGS

    p = source.project_damage
    if p.all_off:
        return ()
    drawings = [getattr(p, field) for field in DRAWINGS
                if not field.startswith('cellf_') or p.cellf_enable]
    drawings.extend((p.surf_drawing, p.coating_drawing, p.rust_drawing, p.concrete_drawing))
    drawings.extend(fracture_stack.references(source))
    from . import boolean_stack, wall_stack
    drawings.extend(wall_stack.references(source))
    drawings.extend(boolean_stack.references(source))
    if p.paper_generator_enable:
        drawings.append(source.project_damage_paper.drawing)
    if p.foam_enable:
        drawings.append(source.project_damage_foam.drawing)

    return tuple(drawing for drawing in dict.fromkeys(drawings)
                 if drawing is not None and drawing.type in live_input_state.DRAWING_TYPES)


_frame_static = {}
_FRAME_STATIC_SECONDS = 2.0


def forget_frame_static(uid=None):
    """Drop the 'drawings are static' answer after anything that can change it."""
    if uid is None:
        _frame_static.clear()
    else:
        _frame_static.pop(uid, None)


def _timeline_static(source, uid, now):
    """True when no drawing of `source` can show another keyframe on a frame
    change. Cached briefly; edits clear it at once (forget_frame_static) and
    the expiry, spread over time, covers anything that bypasses those hooks."""
    hit = _frame_static.get(uid)
    if hit is not None and hit[0] > now:
        return hit[1]
    from . import guide_timing
    try:
        static = all(guide_timing.static(drawing) for drawing in _frame_drawings(source))
    except (ReferenceError, AttributeError):
        static = False
    import random
    _frame_static[uid] = (now + _FRAME_STATIC_SECONDS * (1.0 + random.random()), static)
    return static


def drawing_frames(source):
    from . import guide_timing
    frames = []
    for drawing in _frame_drawings(source):
        layers = []
        held = guide_timing.uses_held_frame(drawing)
        for layer in drawing.data.layers:
            if not layer.hide:
                frame = guide_timing.frame(layer, held)
                layers.append((layer.name, frame.frame_number if frame is not None else None))
        frames.append((drawing.session_uid, tuple(layers)))
    return tuple(frames)


def _drawing_geometry(source, *, remember=False):
    state = []
    for drawing in _frame_drawings(source):
        signature = live_input_state.signature(drawing)
        if remember:
            live_input_state.remember(drawing, 'body', value=signature)
        state.append((drawing.session_uid, signature))
    return tuple(state)


def remember_drawing_frames(source, frames):
    uid = source.get('project_damage_uid')
    if uid is not None:
        forget_frame_static(uid)
        live_input_state.remember(source,'body-mesh',local=True)
        _drawing_frames[uid] = frames, _drawing_geometry(source, remember=True)
        from . import boolean_stack
        for cutter in boolean_stack.cutter_objects(source):
            if cutter.type == 'MESH':
                live_input_state.remember(cutter, 'body-mesh', local=True)
                live_input_state.remember(cutter, 'body')


def _seed_drawing_frames():
    global _APPLYING
    scene = getattr(bpy.context, 'scene', None)
    if scene is None or not hasattr(bpy.data, 'objects'):
        return
    from . import guide_timing, physics_live
    previous = _APPLYING
    _APPLYING = True
    try:
        for drawing in scene.objects:
            if guide_timing.owned_guide(drawing):
                guide_timing.migrate(drawing)
                live_input_state.remember(drawing)
    finally:
        _APPLYING = previous
    for source in scene.objects:
        if source.type == 'MESH' and source.get('project_damage_uid') is not None:
            remember_drawing_frames(source, drawing_frames(source))
            if physics_live.inputs(source):
                physics_live.prime(bpy.context, source, remember_selection=False)
        elif source.type == 'EMPTY':
            live_input_state.remember(source)


@persistent
def _begin_input_event(*_args):
    live_input_state.begin_event()


@persistent
def _frame_pre(scene, *_args):
    live_input_state.begin_event()
    _changing_frames.add(scene.session_uid)


@persistent
def _on_frame(scene, depsgraph=None):
    _changing_frames.discard(scene.session_uid)
    if _HISTORY_RESTORING or _APPLYING:
        return
    from . import background_compute as bg, physics_live
    physics_live.frame_changed(scene)

    scan = None
    sources = _scan().sources
    if not sources:
        return
    # one name set per frame instead of a by-name lookup in scene.objects per source
    in_scene = set(scene.objects.keys())
    now = time.monotonic()
    for source in sources:
        if source.name not in in_scene:continue
        uid = source.get('project_damage_uid')
        if not source.project_damage.live_update or (uid in _dirty and uid not in _frame_dirty):
            continue
        # A source whose drawings hold a single static keyframe cannot change
        # with the timeline: skip it without re-reading its effect stacks.
        if uid in _drawing_frames and uid not in _frame_dirty and _timeline_static(source, uid, now):
            continue
        frames = drawing_frames(source)
        if uid not in _drawing_frames:
            remember_drawing_frames(source, frames)
        built_frames, built_geometry = _drawing_frames[uid]
        if frames == built_frames:
            if uid in _frame_dirty:
                _drop_dirty(uid)
            continue
        if keeps_fragments(source):
            if uid in _frame_dirty:
                _drop_dirty(uid)
            continue
        if _frame_dirty.get(uid) == frames or drawing_exit_suppressed(source):
            continue
        if _drawing_geometry(source) == built_geometry:
            _drawing_frames[uid] = frames, built_geometry
            if uid in _frame_dirty:
                _drop_dirty(uid)
            continue
        if scan is None:
            scan = _scan()
        if scan.result(source) is None:
            continue
        bg.invalidate(source)
        _frame_dirty[uid] = frames
        _dirty[uid] = time.monotonic()
        _remember_context(source, scene)
        _ensure_timer()


def _object_by_uid(uid):
                                                                            
                                                                         
                                                                            
    for obj in bpy.data.objects:
        if obj.get("project_damage_uid") == uid:
            return obj
    return None


# обрабатываем накопленные изменения, когда Blender готов к обновлению
def _tick():
    """Rebuilds every dirty source whose age has cleared its OWN
    Interactive Delay. Returns 0.1 while anything is still dirty (or just got
    rebuilt and could go dirty again next event), None once the dirty set is
    empty - which unregisters this timer until _ensure_timer fires it again.

    Re-check Live Update and result ownership before every deferred build.
    Undo/Redo clears pending work and suspends this callback entirely.

    Rebuild in the scene and view layer captured when the source became dirty.
    """
    global _APPLYING
    if _HISTORY_RESTORING:
        _dirty.clear()
        _dirty_context.clear()
        _frame_dirty.clear()
        return None
    for uid in tuple(_dirty_context):
        if uid not in _dirty:_dirty_context.pop(uid,None)
    now = time.monotonic()
    if _dirty and now - _last_gp_update < GP_DEBOUNCE:
        # keep everything dirty; remember who needs the rebuild at finish
        _drew_since_build.update(_dirty)
        return 0.1
    playing = playing_scenes() if _dirty else set()
    for uid in list(_dirty):
        source = _object_by_uid(uid)
        if source is None:
            _drop_dirty(uid)
            continue
        result = build.result_of(source)
        if not source.project_damage.live_update or result is None:
            _drop_dirty(uid)
            continue
        if uid not in _dirty_context:_remember_context(source)
        target_context=_dirty_context.get(uid)
        if target_context is None:
            _drop_dirty(uid)
            continue
        scene,layer_name=target_context
        if scene in playing and (uid in _frame_dirty or keeps_fragments(source)):
            continue
        try:
            layer=scene.view_layers.get(layer_name)
            available=layer is not None and source.name in layer.objects
        except ReferenceError:available=False
        if not available:
            _drop_dirty(uid)
            continue
        with bpy.context.temp_override(scene=scene,view_layer=layer):
            busy=_busy(source)
        if busy:
            _drew_since_build.add(uid)
            continue
        if source.mode != 'OBJECT' or (result is not None and result.mode != 'OBJECT'):
            # rebuild_object reads obj.data (read_mesh), the AUTHORED mesh -
            # but edit mode does not flush bmesh edits into it until the user
            # leaves edit mode, so a rebuild now would run against the
            # PRE-EDIT mesh and produce the previous result for nothing.
            # Leave the uid dirty rather than popping it: leaving edit mode
            # fires its own geometry update (_on_depsgraph), which re-stamps
            # a fresh timestamp and lets the deferred rebuild land correctly.
            continue
        if now - _dirty[uid] < source.project_damage.defer_ms / 1000.0:
            continue
        if (uid in _drew_since_build and len(source.data.polygons) > LIVE_DRAW_FACE_LIMIT
                and now - _last_gp_update < 2.0 * GP_DEBOUNCE):
            # heavy mesh, strokes still arriving: wait for finish/Update
            continue
        _drew_since_build.discard(uid)
        _APPLYING = True
        failed = None
        try:
            with bpy.context.temp_override(scene=scene,view_layer=layer):
                _res, reports = build.rebuild_object(bpy.context, source)
                                                                        
                                                                        
                                                                  
                                                                      
                                                                           
                                                              
            for report_type, msg in reports:
                if 'ERROR' in report_type:
                    print("ProjectDamage live update: %s" % msg)
        except Exception as exc:
            # a failing rebuild must not repeat every tick: switch Live Update
            # off for this object (still under _APPLYING, so touch_prop is quiet)
            failed = exc
            import traceback
            traceback.print_exc()
            print("ProjectDamage live update: rebuild failed for %r: %r - Live Update switched off "
                  "for this object; press Update to retry" % (source.name, exc))
            try:
                source.project_damage.live_update = False
            except (AttributeError, ReferenceError, RuntimeError):
                pass
        finally:
            _APPLYING = False
        _drop_dirty(uid)
        _redraw_ui()
    return 0.1 if _dirty else None


def _redraw_ui():
    """The Properties editor keeps drawing the result's modifier panel; tag it
    (and the viewports) after every live placement."""
    try:
        for window in bpy.context.window_manager.windows:
            for area in window.screen.areas:
                if area.type in {'PROPERTIES', 'VIEW_3D'}:
                    area.tag_redraw()
    except (AttributeError, ReferenceError, RuntimeError):
        pass


def _ensure_timer():
    if _HISTORY_RESTORING:
        return
    if not bpy.app.timers.is_registered(_tick):
        bpy.app.timers.register(_tick)


# подключаем свойства и действия этого модуля к Blender
def register():
    invalidate_index()
    if not bpy.app.background and not bpy.app.timers.is_registered(_seed_drawing_frames):
        bpy.app.timers.register(_seed_drawing_frames, first_interval=0.0)
    for name, callback in (('undo_pre', _history_pre), ('redo_pre', _history_pre),
                           ('undo_post', _history_post), ('redo_post', _history_post),
                           ('load_pre', _history_pre), ('load_post', _history_post)):
        handlers = getattr(bpy.app.handlers, name)
        if callback not in handlers:
            handlers.append(callback)
                                                                             
                                                               
    if _on_frame not in bpy.app.handlers.frame_change_post:
        bpy.app.handlers.frame_change_post.append(_on_frame)
    if _on_depsgraph not in bpy.app.handlers.depsgraph_update_post:
        bpy.app.handlers.depsgraph_update_post.append(_on_depsgraph)
    if _begin_input_event not in bpy.app.handlers.depsgraph_update_pre:
        bpy.app.handlers.depsgraph_update_pre.append(_begin_input_event)
    if _frame_pre not in bpy.app.handlers.frame_change_pre:
        bpy.app.handlers.frame_change_pre.append(_frame_pre)


# убираем подключения модуля при выключении аддона
def unregister():
    from . import guide_timing
    guide_timing.stop_paint()
    from . import physics_live
    physics_live.clear()
    for name, callback in (('undo_pre', _history_pre), ('redo_pre', _history_pre),
                           ('undo_post', _history_post), ('redo_post', _history_post),
                           ('load_pre', _history_pre), ('load_post', _history_post)):
        handlers = getattr(bpy.app.handlers, name)
        if callback in handlers:
            handlers.remove(callback)
    if bpy.app.timers.is_registered(_history_resume):
        bpy.app.timers.unregister(_history_resume)
    if bpy.app.timers.is_registered(_seed_drawing_frames):
        bpy.app.timers.unregister(_seed_drawing_frames)
                                                                      
                                                                            
                                                     
    if _on_frame in bpy.app.handlers.frame_change_post:
        bpy.app.handlers.frame_change_post.remove(_on_frame)
    if _on_depsgraph in bpy.app.handlers.depsgraph_update_post:
        bpy.app.handlers.depsgraph_update_post.remove(_on_depsgraph)
    if _begin_input_event in bpy.app.handlers.depsgraph_update_pre:
        bpy.app.handlers.depsgraph_update_pre.remove(_begin_input_event)
    if _frame_pre in bpy.app.handlers.frame_change_pre:
        bpy.app.handlers.frame_change_pre.remove(_frame_pre)
    if bpy.app.timers.is_registered(_tick):
        bpy.app.timers.unregister(_tick)
    _dirty.clear()
    _dirty_context.clear()
    _drawing_frames.clear()
    _changing_frames.clear()
    _frame_dirty.clear()
    invalidate_index()
    global _APPLYING, _HISTORY_RESTORING
    _APPLYING = False
    _HISTORY_RESTORING = False
    _BREAKER_SIG.clear()
    _drew_since_build.clear()
    live_input_state.clear()
    build._CACHES.clear()
