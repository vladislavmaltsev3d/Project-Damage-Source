# создаём Grease Pencil и читаем штрихи, привязанные к нужной поверхности
# SPDX-License-Identifier: GPL-3.0-or-later
"""Drawn damage inputs. Drawings remain editable; source meshes are never written.

Grease Pencil strokes at the current frame (visible layers only) or evaluated
Curve splines are transformed into source-local centimetres. Surface contours
are planar regions, independently projected onto their nearest source face.
"""
from . import output_collections
from types import SimpleNamespace
import bpy
import numpy as np
from mathutils import Vector
from mathutils.bvhtree import BVHTree

from . import mesh_io, ownership


def drawing_prop(target, level=1):
    if target in {'WELD','GRASS','GRAVEL'}:return target.lower()+'_drawing'
    if target=='FRACT_HELPER':return 'fract_helper_drawing'
    if target=='CLAY':return 'clay_drawing'
    if target=='METALTEAR':return 'metaltear_drawing'
    if target=='PEELING':return 'peeling_drawing'
    if target=='CELLF':return 'cellf_drawing'
    if target=='CELLF_SURFACE':return 'cellf_surface_drawing'
    if target=='CELLF_GUIDANCE':return 'cellf_guidance_drawing'
    if target in {'RBF_CUTS','RBF_IMPACTS'}:
        return 'rbf_cuts_drawing' if target=='RBF_CUTS' else 'rbf_impacts_drawing'
    if target == 'DRAWMASK':
        return 'draw_mask_drawing'
    if target == 'FRACTURE':
        return 'fract_l1_drawing'
    if target == 'EDGE':
        return 'chips_drawing'
    if target == 'CORNER':
        return 'corner_drawing'
    if target == 'BEAD':
        return 'build_drawing'
    return {"REBAR":"rebar_drawing", "BEVEL": "bevel_drawing", "SCRATCH": "scratch_drawing", "BULLET": "bullet_drawing", "RUSTY": "rusty_drawing", "GROUND": "ground_drawing", "BRICK": "brick_drawing", "FISSURE": "fissure_drawing", "GLASS_HOLES": "glass_holes_drawing", "GLASS_IMPACTS": "glass_impacts_drawing", "SURFACE": "surf_drawing", "CONCRETE": "concrete_drawing", "COATING": "coating_drawing", "RUST": "rust_drawing", "WOOD": "wood_drawing", "LATH": "lath_drawing", "WALLPAPER": "wallpaper_drawing", "FABRIC_HOLES": "fabric_holes_drawing", "FABRIC_EDGES": "fabric_edges_drawing"}.get(target, "fract_l%d_drawing" % level)


def drawing_poll(self, obj):
    return obj.type in {'GREASEPENCIL', 'GPENCIL', 'CURVE'}


def read_paths(context, source, drawing, min_points=2, layer_names=None):
    if drawing is None:
        return []
    deps = context.evaluated_depsgraph_get()
    if drawing.type == 'CURVE':
        return mesh_io.read_guide_splines(deps, source, [SimpleNamespace(obj=drawing)], min_points=min_points)
    if drawing.type not in {'GREASEPENCIL', 'GPENCIL'}:
        return []
                                                                       
                                                           
    ev = drawing if drawing.hide_get(view_layer=context.view_layer) else drawing.evaluated_get(deps)
    rel = source.evaluated_get(deps).matrix_world.inverted() @ ev.matrix_world
    paths = []
    for layer in ev.data.layers:
        if layer_names is not None and layer.name not in layer_names:
            continue
        if layer.hide:
            continue
        if hasattr(layer, 'current_frame'):
            frame = layer.current_frame()
            mat = rel @ layer.matrix_local
        else:
            frame = layer.active_frame
            mat = rel
        if frame is None:
            continue
        strokes = frame.drawing.strokes if hasattr(frame, 'drawing') else frame.strokes
        for stroke in strokes:
            if getattr(stroke, 'hide_stroke', False):
                continue
            pts = [tuple(mat @ (p.position if hasattr(p, 'position') else p.co))
                   for p in stroke.points]
            if len(pts) < min_points:
                continue
            arr = np.asarray(pts, dtype=np.float64)
            arr = arr[np.r_[True, np.linalg.norm(np.diff(arr, axis=0), axis=1) > 1e-8]]
            if len(arr) < min_points or not np.isfinite(arr).all():
                continue
            if getattr(stroke, 'cyclic', getattr(stroke, 'use_cyclic', False)):
                arr = np.vstack((arr, arr[0]))
            paths.append(np.ascontiguousarray(arr))
    return paths


                                                                           
                                                                            
                                                                              
CELL_LAYERS = {'GUIDANCE': 'Guidance (Primary)', 'THROUGH': 'Draw Through', 'SURFACE': 'Draw Surface'}
CELL_PROPS = {'GUIDANCE': 'cellf_guidance_drawing', 'THROUGH': 'cellf_drawing', 'SURFACE': 'cellf_surface_drawing'}


def _cell_layer_names(drawing, mode):
    prefix = CELL_LAYERS[mode]
    return [l.name for l in drawing.data.layers if l.name == prefix or l.name.startswith(prefix + ' | ')]


def _cell_role_has_strokes(drawing, mode):
    return any(len(stroke.points) > 1 for name in _cell_layer_names(drawing, mode)
               for frame in drawing.data.layers[name].frames for stroke in frame.drawing.strokes)


def cell_drawing(source, mode, *, reconnect=False):
    drawing = getattr(source.project_damage, CELL_PROPS[mode])
    if mode == 'GUIDANCE':
        return drawing
    if drawing is None and not reconnect:
        return None
    if drawing is not None and (drawing.type != 'GREASEPENCIL'
            or not drawing.get('project_damage_cell_layers')
            or _cell_role_has_strokes(drawing, mode)):
        return drawing
    return next((obj for obj in sorted(source.children, key=lambda obj: obj.name)
                 if obj != drawing and obj.type == 'GREASEPENCIL'
                 and obj.get('project_damage_cell_layers')
                 and obj.get('project_damage_paint_source') == source
                 and _cell_role_has_strokes(obj, mode)), drawing)


def read_cell_paths(context, source, mode):
    drawing = cell_drawing(source, mode)
    if drawing is None:
        return []
    if drawing.type == 'GREASEPENCIL' and drawing.get('project_damage_cell_layers'):
        return read_paths(context, source, drawing, layer_names=_cell_layer_names(drawing, mode))
    return read_paths(context, source, drawing)


def ensure_cell_layer(context, source, mode):
    from . import live
    p = source.project_damage
    drawing = cell_drawing(source, mode, reconnect=True)
    if drawing is not None and drawing.type != 'GREASEPENCIL':
        return drawing
    if drawing is None:
        drawing = next((getattr(p, prop) for prop in CELL_PROPS.values()
                        if getattr(p, prop) is not None and getattr(p, prop).type == 'GREASEPENCIL'), None)
    if drawing is not None and drawing.library is not None:
        raise RuntimeError('Make the linked Grease Pencil object local before adding drawing layers')
    previous = live._APPLYING
    live._APPLYING = True
    try:
        if drawing is None:
            gp = bpy.data.grease_pencils.new(source.name + '_CellFractureDrawing')
            drawing = bpy.data.objects.new(gp.name, gp)
            output_collections.link_object(context,source,drawing)
            drawing.parent = source
            drawing.hide_render = True
        gp = drawing.data
        if not drawing.get('project_damage_cell_layers'):
            old_mode = next((k for k, prop in CELL_PROPS.items() if getattr(p, prop) == drawing), mode)
            for layer in gp.layers:
                layer.name = CELL_LAYERS[old_mode] + ' | ' + layer.name
            drawing['project_damage_cell_layers'] = True
        drawing['project_damage_paint_source'] = source
        setattr(p, CELL_PROPS[mode], drawing)
        for kind, prop in CELL_PROPS.items():
            if getattr(p, prop) is None:
                setattr(p, prop, cell_drawing(source, kind, reconnect=True) or drawing)
            if getattr(p, prop) != drawing:
                continue
            names = _cell_layer_names(drawing, kind)
            if not names:
                layer = gp.layers.new(CELL_LAYERS[kind], set_active=False)
                layer.frames.new(context.scene.frame_current)
        names = _cell_layer_names(drawing, mode)
        layer = gp.layers[names[0]]
        gp.layers.active = layer
        layer.hide = False
        layer.lock = False
        if layer.current_frame() is None:
            layer.frames.new(context.scene.frame_current)
        ink_name = 'ProjectDamage Cell ' + mode + ' Ink'
        ink = next((m for m in gp.materials if m and m.name == ink_name), None)
        if ink is None:
            ink = bpy.data.materials.get(ink_name) or bpy.data.materials.new(ink_name)
            if ink.grease_pencil is None:
                bpy.data.materials.create_gpencil_data(ink)
            ink.grease_pencil.color = {'GUIDANCE': (.25,.9,.25,.85), 'THROUGH': (.05,.5,1.,.85), 'SURFACE': (1.,.4,.08,.85)}[mode]
            gp.materials.append(ink)
        drawing.active_material_index = list(gp.materials).index(ink)
        if p.cellf_curve_type != mode:
            p.cellf_curve_type = mode
    finally:
        live._APPLYING = previous
    return drawing


def cell_layer_changed(props, context):
    """Switch an active drawing's layer without scheduling a geometry build."""
    from . import live
    if live._APPLYING or props.cellf_curve_type not in CELL_PROPS:
        return
    source = props.id_data
    active = getattr(context, 'active_object', None)
    if active is None or active.type != 'GREASEPENCIL':
        return
    if not any(getattr(props, prop) == active for prop in CELL_PROPS.values()):
        return
    target = getattr(props, CELL_PROPS[props.cellf_curve_type])
    if target is None or target == active:
        ensure_cell_layer(context, source, props.cellf_curve_type)


def read_stroke_regions(context, source, drawing, points, polys, factor):
    """Open strokes and dots with a source-facing projection normal."""
    faces = [tuple(int(v) for v in row[:3]) if row[2] == row[3]
             else tuple(int(v) for v in row) for row in polys]
    bvh = BVHTree.FromPolygons([Vector(v) for v in points], faces, all_triangles=False)
    from . import surface_guidance
    guidance=surface_guidance.field(points,polys)
    paths, normals = [], []
    for path in read_paths(context, source, drawing, min_points=1):
        path = np.ascontiguousarray(path * factor, dtype=np.float64)
        hit, normal, face, distance = bvh.find_nearest(Vector(path[0]))
        if hit is None or distance > source.project_damage.surf_paint_snap:
            continue
        normal=surface_guidance.face_normal(guidance,face,normal)
        paths.append(path)
        normals.append(tuple(normal))
    return paths, np.asarray(normals, dtype=np.float64).reshape(-1, 3) if normals else None


REGION_SNAP_REL = 0.05      # snap distance floor: share of the mesh diagonal
REGION_NORMAL_DOT = 0.5     # neighbouring samples must agree this well


def _object_scale(source):
    try:
        sc = source.matrix_world.to_scale()
        value = (abs(sc.x) + abs(sc.y) + abs(sc.z)) / 3.0
    except (AttributeError, TypeError):
        return 1.0
    return value if value > 1e-9 else 1.0


def _snap_hit(bvh, guidance, co, snap, reference, *, planar=False):
    """Closest surface point of `co` within `snap` whose normal agrees with
    `reference` (the previous accepted sample's normal), or, for the first
    sample, faces the stroke point. On a thin or double-layer cloth the
    plain closest point can land on the opposite layer; the candidates in
    range are searched for the one on the same side. None when no
    candidate qualifies."""
    from . import surface_guidance
    hit, normal, face, distance = bvh.find_nearest(co)
    if hit is None or distance > snap:
        return None
    normal = surface_guidance.face_normal(guidance, face, normal)

    def good(h, n):
        if reference is not None:
            return n.dot(reference) >= REGION_NORMAL_DOT
        return (co - h).dot(n) >= -1e-9
    if good(hit, normal):
        return hit, normal, distance
    best = None
    search = snap if planar else min(snap, distance * 1.5 + 1e-4 * snap + 1e-6)
    for h, n, f, d in bvh.find_nearest_range(co, search):
        if h is None:
            continue
        n = surface_guidance.face_normal(guidance, f, n)
        if good(h, n) and (best is None or d < best[2]):
            best = (h, n, d)
    if best is None and reference is None:
        return hit, normal, distance          # nothing faces the stroke: keep the closest
    return best


def _glass_contour_axis(loop):
    _, spans, basis = np.linalg.svd(loop - loop.mean(axis=0), full_matrices=False)
    if spans[1] <= max(1e-6, spans[0] * 1e-4) or spans[2] > 0.35 * spans[1]:
        return None
    return Vector(basis[-1])


def _glass_contour_reference(bvh, guidance, loop, axis, snap):
    from . import surface_guidance
    best = None
    for co in loop:
        hit, normal, face, distance = bvh.find_nearest(Vector(co), snap)
        if hit is None:
            continue
        normal = surface_guidance.face_normal(guidance, face, normal)
        if abs(normal.dot(axis)) >= 0.75 and (best is None or distance < best[0]):
            best = (distance, normal)
    if best is not None:
        return best[1]
    # A stroke can start and end just outside a pane, where its rim is closer.
    # Try both orientations of the drawn plane to find the nearest broad face.
    for co in loop:
        point = Vector(co)
        for reference in (axis, -axis):
            match = _snap_hit(bvh, guidance, point, snap, reference, planar=True)
            if match is None:
                continue
            _, normal, distance = match
            if abs(normal.dot(axis)) >= 0.75 and (best is None or distance < best[0]):
                best = (distance, normal)
    return best[1] if best is not None else None


def read_regions(context, source, drawing, points, polys, factor, reports=None, label='Contour', *, glass=False):
    """Closed contours in source-local cm, with source-facing region normals.

    Each sample is projected onto the surface (closest point within the
    Projection Distance, never less than REGION_SNAP_REL of the mesh size,
    divided by the object scale). Its normal is compared with the
    NEIGHBOURING accepted sample, not the first one, so a contour that
    follows a curved garment is kept; samples that cannot be projected or
    that jump to the opposite layer are dropped one by one. A contour is
    rejected only when fewer than 3 samples remain; `reports` (a list)
    receives one line per dropped contour saying why."""
    if drawing is None:
        return [], None
    faces = [tuple(int(v) for v in row[:3]) if row[2] == row[3]
             else tuple(int(v) for v in row) for row in polys]
    bvh = BVHTree.FromPolygons([Vector(v) for v in points], faces, all_triangles=False)
    from . import surface_guidance
    guidance=surface_guidance.field(points,polys)
    pts = np.asarray(points, dtype=np.float64)
    diag = float(np.linalg.norm(pts.max(axis=0) - pts.min(axis=0))) if len(pts) else 0.0
    snap = max(source.project_damage.surf_paint_snap / _object_scale(source), REGION_SNAP_REL * diag)
    regions, normals = [], []

    def why(index, text):
        if reports is not None:
            reports.append(({'WARNING'}, '%s %d: %s' % (label, index + 1, text)))
    for index, path in enumerate(read_paths(context, source, drawing)):
        if len(path) < 3:
            why(index, 'skipped, fewer than 3 points')
            continue
        closed = np.linalg.norm(path[-1] - path[0]) < 1e-6
        if not closed and not source.project_damage.surf_close_strokes:
            why(index, 'skipped, the stroke is open (close it or enable Close Strokes)')
            continue
        loop = path[:-1] if closed else path
        if len(loop) < 3:
            why(index, 'skipped, fewer than 3 points')
            continue
        loop = np.ascontiguousarray(loop * factor, dtype=np.float64)
        kept, kept_n = [], []
        far = flipped = 0
        reference = None
        axis = _glass_contour_axis(loop) if glass else None
        if axis is not None:
            reference = _glass_contour_reference(bvh, guidance, loop, axis, snap)
            if reference is None:
                why(index, 'skipped, no glass face aligned with the contour within Projection Distance')
                continue
        for v in loop:
            co = Vector(v)
            got = _snap_hit(bvh, guidance, co, snap, reference, planar=axis is not None)
            if got is None:
                plain = bvh.find_nearest(co)
                if plain[0] is None or plain[3] > snap:
                    far += 1
                else:
                    flipped += 1
                continue
            hit, normal, _d = got
            if axis is not None:
                # Retain the silhouette where the contour crosses the pane edge.
                # The cutter is clipped to the glass by the core.
                hit = co - reference * (co - hit).dot(reference)
                normal = reference
            else:
                reference = normal
            kept.append((hit.x, hit.y, hit.z))
            kept_n.append(normal)
        if len(kept) < 3:
            why(index, 'dropped, only %d of %d points lie on the surface (%d farther than %.3g cm, '
                       '%d on the opposite side)' % (len(kept), len(loop), far, snap, flipped))
            continue
        if far or flipped:
            why(index, 'used %d of %d points (%d farther than %.3g cm, %d on the opposite side dropped)'
                % (len(kept), len(loop), far, snap, flipped))
        mean = Vector((0.0, 0.0, 0.0))
        for n in kept_n:
            mean += n
        normal = mean.normalized() if mean.length > 1e-9 else kept_n[0]
        regions.append(np.ascontiguousarray(kept, dtype=np.float64))
        normals.append(tuple(normal))
    return regions, np.asarray(normals, dtype=np.float64).reshape(-1, 3) if normals else None


def collect(context, source, points, polys, factor):
    """Return fracture paths/labels and Surface contours/normals plus reports."""
    p = source.project_damage
    guides, levels, regions, normals, reports = [], [], [], [], []
    if not p.all_off and (p.fract_enable or p.fract_through_enable) and p.fract_mode == 'PAINT':
        paths = read_paths(context, source, p.fract_l1_drawing)
        guides.extend(np.ascontiguousarray(path * factor) for path in paths)
        levels.extend([0] * len(paths))
        if not guides:
            reports.append(({'WARNING'}, "Paint Fracture: no strokes at this frame; no cracks generated"))
    if not p.all_off and p.surf_enable and p.surf_mode == 'PAINT' and p.surf_amount > 0:
                                                                              
        faces = [tuple(int(v) for v in row[:3]) if row[2] == row[3]
                 else tuple(int(v) for v in row) for row in polys]
        bvh = BVHTree.FromPolygons([Vector(v) for v in points], faces, all_triangles=False)
        for path in read_paths(context, source, p.surf_drawing):
            if len(path) < 3:
                continue
            closed = np.linalg.norm(path[-1] - path[0]) < 1e-6
            if not closed and not p.surf_close_strokes:
                reports.append(({'WARNING'}, "Paint Surface: open contour skipped; close it or enable Close Strokes"))
                continue
            loop = path[:-1] if closed else path
            if len(loop) < 3:
                continue
            loop = np.ascontiguousarray(loop * factor)
                                                                        
                                                                            
            hit, normal, face, distance = bvh.find_nearest(Vector(loop[0]))
            if hit is None or distance > p.surf_paint_snap:
                reports.append(({'WARNING'}, "Paint Surface: contour is farther than Projection Distance from the source"))
                continue
                                                                             
                                                          
            samples = [bvh.find_nearest(Vector(v)) for v in loop]
            if any(h is None or d > p.surf_paint_snap or normal.dot(n) < 0.5
                   for h, n, face, d in samples):
                reports.append(({'WARNING'}, "Paint Surface: use a separate local contour on each face; this contour spans distant/opposite surfaces"))
                continue
            regions.append(loop)
            normals.append(tuple(normal))
        if not regions:
            reports.append(({'WARNING'}, "Paint Surface: no usable closed contours; no craters generated"))
    ns = np.asarray(normals, dtype=np.float64).reshape(-1, 3) if normals else None
    return guides, levels, regions, ns, reports


def source_for(context):
    obj = ownership.context_source(context)
    if obj is None:
        return None
    owner = obj.get("project_damage_paint_source")
    if isinstance(owner, bpy.types.Object):
        return owner
    if obj.type == 'MESH' and not obj.get("project_damage_result_of"):
        return obj
    return None


def _activate(context, obj):
    if obj.type in {'GREASEPENCIL','GPENCIL'} and obj.get('project_damage_paint_source'):
        from . import viewport_helpers
        viewport_helpers.activate(context,obj)
    if context.object and context.object.mode != 'OBJECT':
        if context.object.type in {'GREASEPENCIL','GPENCIL','CURVE'}:
            from . import live
            live.finish_drawing(ownership.source_of(context.object))
        bpy.ops.object.mode_set(mode='OBJECT')
    for ob in context.selected_objects:
        ob.select_set(False)
    obj.hide_set(False)
    obj.select_set(True)
    context.view_layer.objects.active = obj


DRAW_BRUSH_DIAMETER = 0.004406


def _set_draw_brush_diameter(context):
    settings = context.scene.tool_settings
    paint = settings.gpencil_paint
    brush = paint.brush
                                                                           
                                                                            
    unified = getattr(paint, 'unified_paint_settings', None)
    if unified is None:
        unified = getattr(settings, 'unified_paint_settings', None)
    for owner in (brush, unified):
        if owner is not None:
            owner.use_locked_size = 'SCENE'
            owner.unprojected_size = DRAW_BRUSH_DIAMETER


_surface_session = None
_surface_history = None


def drawing_surfaces(context,drawing):
    from . import structure_drawing
    structure_source=structure_drawing.source_for_drawing(drawing)
    if structure_source is not None:return [structure_source]
    from .build import result_of
    source=ownership.source_of(drawing)
    if source is None:return []
    def visible(obj):
        return (isinstance(obj,bpy.types.Object) and obj.type=='MESH'
                and obj.name in context.view_layer.objects and obj.visible_get(view_layer=context.view_layer)
                and not obj.project_damage_breaker.is_breaker
                and not obj.get('project_damage_proximity_owner'))
    if drawing.get('project_damage_cell_layers') and source.project_damage.cellf_enable:
        from . import cellf_output
        shards=[obj for obj in cellf_output.objects_of(source) if visible(obj)]
        if shards:return shards
    active=context.active_object
    if visible(active) and ownership.source_of(active)==source:
        return [active]
    stored=drawing.get('project_damage_paint_surface')
    if visible(stored) and ownership.source_of(stored)==source:return [stored]
    for obj in (result_of(source),source):
        if visible(obj):return [obj]
    return [obj for obj in context.view_layer.objects
            if visible(obj) and ownership.source_of(obj)==source]


def finish_surface_projection():
    global _surface_session
    session=_surface_session;_surface_session=None
    if session is None:return
    scene,selected_only,drawing,in_front,surfaces=session
    try:scene.tool_settings.use_gpencil_project_only_selected=selected_only
    except (ReferenceError,RuntimeError):pass
    try:drawing.show_in_front=in_front
    except (ReferenceError,RuntimeError):pass
    for surface in surfaces:
        try:
            if surface!=bpy.context.active_object:surface.select_set(False)
        except (ReferenceError,RuntimeError):pass
    from . import structure_drawing
    structure_drawing.restore_projection()


def suspend_surface_projection():
    global _surface_history
    _surface_history=None
    if _surface_session is not None:
        scene,selected_only,drawing,in_front,_surfaces=_surface_session
        try:_surface_history=(scene.name,drawing.name,selected_only,in_front)
        except ReferenceError:pass
    finish_surface_projection()


def resume_surface_projection(context):
    global _surface_history
    history=_surface_history;_surface_history=None
    drawing=context.active_object
    if (drawing is None or drawing.type!='GREASEPENCIL'
            or drawing.mode!='PAINT_GREASE_PENCIL'
            or not drawing.get('project_damage_paint_source')):return
    from . import structure_drawing
    structure_source=structure_drawing.source_for_drawing(drawing)
    if structure_source is not None:structure_drawing.prepare_projection(structure_source)
    surfaces=drawing_surfaces(context,drawing)
    if not surfaces:return
    if history is not None and history[:2]==(context.scene.name,drawing.name):
        context.scene.tool_settings.use_gpencil_project_only_selected=history[2]
        drawing.show_in_front=history[3]
    for obj in tuple(context.selected_objects):
        if obj!=drawing:obj.select_set(False)
    start_surface_projection(context,drawing,surfaces)
    from . import live
    live.drawing_started(ownership.source_of(drawing),drawing=drawing)


def start_surface_projection(context,drawing,surfaces):
    global _surface_session
    finish_surface_projection()
    if not surfaces:raise RuntimeError('Make the Project Damage target mesh visible before drawing')
    settings=context.scene.tool_settings
    _surface_session=(context.scene,settings.use_gpencil_project_only_selected,drawing,drawing.show_in_front,tuple(surfaces))
    settings.use_gpencil_project_only_selected=True
    for surface in surfaces:surface.select_set(True)
    source=ownership.source_of(drawing)
    surface_knives=drawing.get('project_damage_cell_layers') and source and source.project_damage.cellf_enable
    drawing.show_in_front=not surface_knives
    drawing['project_damage_paint_surface']=surfaces[0]


def refresh_cell_knife_projection(context, source, shards):
    global _surface_session
    drawing = context.active_object
    if (context.mode != 'PAINT_GREASE_PENCIL' or drawing is None
            or not drawing.get('project_damage_cell_layers')
            or ownership.source_of(drawing) != source):
        return
    layer = drawing.data.layers.active
    if layer is None or not any(layer.name in _cell_layer_names(drawing, mode) for mode in ('THROUGH', 'SURFACE')):
        return
    surfaces = tuple(obj for obj in shards if obj.name in context.view_layer.objects
                     and obj.visible_get(view_layer=context.view_layer))
    if not surfaces:
        return
    if _surface_session is None or _surface_session[2] != drawing:
        start_surface_projection(context, drawing, surfaces)
        return
    scene, selected_only, obj, in_front, _previous = _surface_session
    for shard in surfaces:
        if not shard.select_get():
            shard.select_set(True)
    _surface_session = (scene, selected_only, obj, in_front, surfaces)


def _start_drawing(context,obj,*,defer_preview=False):
    from . import live
    previous=live._APPLYING
    live._APPLYING=True
    try:
        surfaces=drawing_surfaces(context,obj)
        _activate(context,obj)
        start_surface_projection(context,obj,surfaces)
        context.scene.tool_settings.gpencil_stroke_placement_view3d='SURFACE'
        if hasattr(context.scene.tool_settings,'gpencil_surface_offset'):
            source=ownership.source_of(obj)
            surface_knives=obj.get('project_damage_cell_layers') and source and source.project_damage.cellf_enable
            context.scene.tool_settings.gpencil_surface_offset=0.0002 if surface_knives else 0.0
        if not bpy.app.background:
            area=context.area if context.area and context.area.type=='VIEW_3D' else next((a for a in context.screen.areas if a.type=='VIEW_3D'),None)
            if area is None:raise RuntimeError('Open a 3D View to start Grease Pencil drawing')
            region=next(r for r in area.regions if r.type=='WINDOW')
            context.view_layer.update()
            with context.temp_override(area=area,region=region,object=obj,active_object=obj,selected_objects=[obj]+surfaces,selected_editable_objects=[obj]+surfaces):
                bpy.ops.object.mode_set(mode='PAINT_GREASE_PENCIL')
                bpy.ops.wm.tool_set_by_id(name='builtin.brush')
                _set_draw_brush_diameter(context)
            area.spaces.active.show_region_ui=True
        else:_set_draw_brush_diameter(context)
        live.drawing_started(ownership.source_of(obj),drawing=obj,defer=defer_preview)
    except Exception:
        finish_surface_projection()
        raise
    finally:
        live._APPLYING=previous


class PROJECT_DAMAGE_OT_paint_new(bpy.types.Operator):
    bl_idname = "project_damage.paint_new"
    bl_label = "New Drawing"
    bl_description = "Create a Grease Pencil drawing on this Project Damage mesh only; objects in front are ignored"
    bl_options = {'REGISTER', 'UNDO'}
    target: bpy.props.StringProperty(default="FRACTURE")
    level: bpy.props.IntProperty(default=1, min=1, max=3)

    @classmethod
    def poll(cls, context):
        return source_for(context) is not None

    def execute(self, context):
        return new_drawing(context,self.target,self.level)


def new_drawing(context,target,level=1):
    if bpy.app.version < (4, 3, 0):
        return {'CANCELLED'}
    source = source_for(context)
    if target in {'CELLF', 'CELLF_SURFACE', 'CELLF_GUIDANCE'}:
        mode = {'CELLF': 'THROUGH', 'CELLF_SURFACE': 'SURFACE', 'CELLF_GUIDANCE': 'GUIDANCE'}[target]
        drawing = ensure_cell_layer(context, source, mode)
        _start_drawing(context, drawing) if drawing.type == 'GREASEPENCIL' else _activate(context, drawing)
        return {'FINISHED'}
    name = source.name + ("_" + target.title() + "Paint" if target != 'FRACTURE' else "_FracturePaint")
    gp = bpy.data.grease_pencils.new(name)
    layer = gp.layers.new('Damage Mask' if target == 'DRAWMASK' else "Damage Contours" if target in {'PEELING','SURFACE', 'CONCRETE', 'COATING', 'RUST', 'WOOD', 'LATH', 'BRICK', 'WALLPAPER', 'FABRIC_HOLES', 'FABRIC_EDGES'} else "Crack Paths", set_active=True)
    layer.frames.new(context.scene.frame_current)
    obj = bpy.data.objects.new(name, gp)
    output_collections.link_object(context,source,obj)
    obj.parent = source
    obj["project_damage_paint_source"] = source
    obj.hide_render = True
    mat = bpy.data.materials.new(name + "_Ink")
    bpy.data.materials.create_gpencil_data(mat)
    mat.grease_pencil.color = (1.0, 0.15, 0.02, 0.85) if target == 'SURFACE' else (0.05, 0.5, 1.0, 0.85)
    if target == 'DRAWMASK':
        mat.grease_pencil.color = (0.1, 0.85, 0.2, 0.85)
    if target=='CELLF_GUIDANCE':mat.grease_pencil.color=(.25,.9,.25,.85)
    gp.materials.append(mat)
    setattr(source.project_damage, drawing_prop(target, level), obj)
    from . import fabric_paint
    if target in fabric_paint.TARGETS:fabric_paint.start(context,obj)
    else:_start_drawing(context,obj)
    return {'FINISHED'}

class PROJECT_DAMAGE_OT_paint_draw(bpy.types.Operator):
    bl_idname = "project_damage.paint_draw"
    bl_label = "Draw / Edit"
    bl_description = "Draw on this Project Damage mesh only, ignoring foreground objects; linked Curves remain editable"
    bl_options = {'REGISTER', 'UNDO'}
    target: bpy.props.StringProperty(default="FRACTURE")
    level: bpy.props.IntProperty(default=1)

    def execute(self, context):
        return draw_existing(context,self.target,self.level)


def draw_existing(context,target,level=1):
    source = source_for(context)
    if source is None:
        return {'CANCELLED'}
    if target in {'CELLF', 'CELLF_SURFACE', 'CELLF_GUIDANCE'}:
        mode = {'CELLF': 'THROUGH', 'CELLF_SURFACE': 'SURFACE', 'CELLF_GUIDANCE': 'GUIDANCE'}[target]
        drawing = ensure_cell_layer(context, source, mode)
        _start_drawing(context, drawing) if drawing.type == 'GREASEPENCIL' else _activate(context, drawing)
        return {'FINISHED'}
    drawing = getattr(source.project_damage, drawing_prop(target, level))
    if drawing is None:
        return {'CANCELLED'}
    drawing["project_damage_paint_source"] = source
    if drawing.type == 'GREASEPENCIL':
        from . import fabric_paint
        if target in fabric_paint.TARGETS:fabric_paint.start(context,drawing)
        else:_start_drawing(context,drawing)
    else:
        _activate(context, drawing)
    return {'FINISHED'}

# действие «Delete Drawing»
class PROJECT_DAMAGE_OT_paint_delete(bpy.types.Operator):
    bl_idname = 'project_damage.paint_delete'
    bl_label = 'Delete Drawing'
    bl_description = 'Delete the linked drawing object from the scene, not just unlink it. Shared drawing links will also be cleared. Undo restores it'
    bl_options = {'REGISTER', 'UNDO'}
    target: bpy.props.StringProperty(default='FRACTURE')
    level: bpy.props.IntProperty(default=1, min=1, max=3)

    def invoke(self, context, event):
        return context.window_manager.invoke_confirm(self, event)

    def execute(self, context):
        source = source_for(context)
        if source is None:
            return {'CANCELLED'}
        prop = drawing_prop(self.target, self.level)
        drawing = getattr(source.project_damage, prop, None)
        if drawing is None or drawing.type not in {'GREASEPENCIL', 'GPENCIL', 'CURVE'}:
            return {'CANCELLED'}
        if drawing.library is not None:
            self.report({'WARNING'}, 'Linked-library drawings cannot be deleted here')
            return {'CANCELLED'}
        if self.target in {'CELLF', 'CELLF_SURFACE', 'CELLF_GUIDANCE'} and drawing.get('project_damage_cell_layers'):
            mode = {'CELLF': 'THROUGH', 'CELLF_SURFACE': 'SURFACE', 'CELLF_GUIDANCE': 'GUIDANCE'}[self.target]
            drawings = [drawing]
            recovered = cell_drawing(source, mode)
            if recovered is not None and recovered != drawing and recovered.library is None:
                drawings.append(recovered)
            for obj in drawings:
                for name in _cell_layer_names(obj, mode):
                    obj.data.layers.remove(obj.data.layers[name])
            setattr(source.project_damage, prop, None)
            from . import live
            live.touch_prop(source.project_damage, context)
            return {'FINISHED'}
        if context.object == drawing:
            from .build import result_of
            result = result_of(source)
            _activate(context, result if result is not None else source)
                                                                             
                                                                          
        for obj in list(bpy.data.objects):
            p = getattr(obj, 'project_damage', None)
            if p is None:
                continue
            for field in p.bl_rna.properties:
                if field.identifier.endswith('_drawing') and getattr(p, field.identifier) == drawing:
                    setattr(p, field.identifier, None)
        bpy.data.objects.remove(drawing, do_unlink=True)
        return {'FINISHED'}


# действие «Return to Deformable»
class PROJECT_DAMAGE_OT_paint_return(bpy.types.Operator):
    bl_idname = "project_damage.paint_return"
    bl_label = "Return to Deformable"
    bl_description = "Finish drawing and return to the source's ProjectDamage settings"
    bl_options = {'REGISTER', 'UNDO'}
    update: bpy.props.BoolProperty(default=False)

    def execute(self, context):
        source = source_for(context)
        if source is None:
            return {'CANCELLED'}
        from . import live
        live.finish_drawing(source)
        from .build import result_of
        result = result_of(source)
        _activate(context, result if result is not None else source)
        if result is not None:
            source.hide_set(True)
        return {'FINISHED'}


def drawing_row(layout, p, target, level=1):
    prop = drawing_prop(target, level)
    picker = layout.row(align=True)
    picker.prop(p, prop, text="Drawing")
    remove = picker.row(align=True)
    remove.enabled = getattr(p, prop) is not None
    op = remove.operator('project_damage.paint_delete', text='', icon='TRASH')
    op.target, op.level = target, level
    row = layout.row(align=True)
    fabric=target in {'FABRIC_HOLES','FABRIC_EDGES'}
    for action,text in (('new','New Drawing'),('draw','Draw / Edit')):
        sub=row.row(align=True)
        sub.enabled=action!='draw' or getattr(p,prop) is not None
        op=sub.operator('project_damage.'+('fabric_paint_' if fabric else 'paint_')+action,text=text)
        op.target=target
        if not fabric:op.level=level


# действие «Update Peeling»
class PROJECT_DAMAGE_OT_peeling_update(bpy.types.Operator):
    bl_idname='project_damage.peeling_update'
    bl_label='Update Peeling'
    bl_description='Finish the current stroke and rebuild Peeling from the drawing'
    bl_options={'REGISTER','UNDO'}

    @classmethod
    def poll(cls,context):return source_for(context) is not None

    def execute(self,context):
        from . import live,build
        source=source_for(context)
        live.finish_drawing(source)
        _activate(context,build.result_of(source) or source)
        previous=live._APPLYING;live._APPLYING=True
        try:result,reports=build.rebuild_object(context,source)
        finally:live._APPLYING=previous
        for kind,message in reports:self.report(kind,message)
        return {'FINISHED'} if result is not None else {'CANCELLED'}


# раздел «Paint Inputs»
class VIEW3D_PT_project_damage_paint(bpy.types.Panel):
    bl_label = "Paint Inputs"
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "⚒ Project Damage"
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        return source_for(context) is not None

    def draw(self, context):
        source = source_for(context)
        layout = self.layout
        layout.label(text="Source: " + source.name)
        layout.label(text="Draw on the surface; closed loops for Surface.")
        layout.operator("project_damage.paint_return", text="Finish Drawing", icon='CHECKMARK')
        p = source.project_damage
        layout.prop(p, "fract_mode")
        if p.fract_mode == 'PAINT':
            drawing_row(layout, p, 'FRACTURE', 1)
            layout.prop(p, 'fract_l2_amount', text='Level 2 Amount')
            layout.prop(p, 'fract_l3_amount', text='Level 3 Amount')
        layout.prop(p, "surf_mode")
        if p.surf_mode == 'PAINT':
            drawing_row(layout, p, 'SURFACE')
        if p.coating_enable and p.coating_zone_enable:
            drawing_row(layout, p, 'COATING')
        layout.prop(p, "concrete_mode")
        if p.concrete_mode == '1':
            drawing_row(layout, p, 'CONCRETE')


_CLASSES = (PROJECT_DAMAGE_OT_paint_new, PROJECT_DAMAGE_OT_paint_draw, PROJECT_DAMAGE_OT_paint_delete, PROJECT_DAMAGE_OT_paint_return, PROJECT_DAMAGE_OT_peeling_update, VIEW3D_PT_project_damage_paint)


# подключаем свойства и действия этого модуля к Blender
def register():
    for cls in _CLASSES[:-1]:
        bpy.utils.register_class(cls)
    from . import fabric_paint
    fabric_paint.register()


# убираем подключения модуля при выключении аддона
def unregister():
    global _surface_history
    _surface_history=None
    finish_surface_projection()
    from . import fabric_paint
    fabric_paint.unregister()
    for cls in reversed(_CLASSES[:-1]):
        bpy.utils.unregister_class(cls)


DRAW_FIRST = {'concrete':'CONCRETE','concrete_v2':'CONCRETE',
              'lath':'LATH','lath_v2':'LATH','lath_riven':'LATH','lath_framing':'LATH',
              'brick':'BRICK','brick_v2':'BRICK'}


def begin_new_effect(context,source,key):
    target=DRAW_FIRST.get(key)
    if target is None:return
    from . import live
    previous=live._APPLYING;live._APPLYING=True
    try:setattr(source.project_damage,target.lower()+'_mode','1')
    finally:live._APPLYING=previous
    drawing=getattr(source.project_damage,drawing_prop(target))
    if drawing is None:new_drawing(context,target)
    else:draw_existing(context,target)
    drawing=getattr(source.project_damage,drawing_prop(target))
    if drawing is not None:drawing['project_damage_draw_hint']='Draw damage on the mesh in this viewport. Use Return to Deformable to finish.'


def _drawing_family(key):
    return {'surface':'surf','edge':'chips','buildup':'build','beads':'build',
            'fabric_solid':'fabric','concrete_v2':'concrete','pipes':'concrete',
            'cables':'concrete','lath_v2':'lath','lath_riven':'lath',
            'lath_framing':'lath','brick_v2':'brick','glass_hit':'glass',
            'glass_drawn':'glass'}.get(key,key)


def effect_drawings(source,key):
    from . import fracture_stack
    if key in fracture_stack.FRACTURE_KEYS:
        fracture_stack.save(source)
        row=fracture_stack.row_for(source,key)
        return {v for k,v in row.get(fracture_stack.VALUES,{}).items()
                if 'drawing' in k and isinstance(v,bpy.types.Object)} if row else set()
    prefix=_drawing_family(key)+'_'
    group=source.project_damage
    drawings={getattr(group,p.identifier) for p in group.bl_rna.properties
              if p.type=='POINTER' and p.identifier.startswith(prefix) and 'drawing' in p.identifier}
    extra={'foam':('project_damage_foam','drawing'),'ivy':('project_damage_ivy','guide'),'paper_generator':('project_damage_paper','drawing')}.get(key)
    if extra:drawings.add(getattr(getattr(source,extra[0]),extra[1]))
    return {d for d in drawings if d is not None and d.type in {'GREASEPENCIL','GPENCIL'}}


def remove_effect_drawing(source,key,drawings=None):
    from . import live,fracture_stack
    if any(_drawing_family(row.key)==_drawing_family(key) for row in source.project_damage_effects):return
    drawings=effect_drawings(source,key) if drawings is None else drawings
    previous=live._APPLYING;live._APPLYING=True
    try:
        p=source.project_damage
        if key in fracture_stack.FRACTURE_KEYS:
            if source.get(fracture_stack.EDIT)==key:
                for r in fracture_stack.fields(p):
                    if r.type=='POINTER' and 'drawing' in r.identifier:setattr(p,r.identifier,None)
                source[fracture_stack.EDIT]=''
                other=next((r.key for r in source.project_damage_effects if r.key in fracture_stack.FRACTURE_KEYS),None)
                if other:fracture_stack.select(source,other)
        else:
            for r in p.bl_rna.properties:
                if r.type=='POINTER' and r.identifier.startswith(_drawing_family(key)+'_') and 'drawing' in r.identifier:
                    setattr(p,r.identifier,None)
            extra={'foam':('project_damage_foam','drawing'),'ivy':('project_damage_ivy','guide'),'paper_generator':('project_damage_paper','drawing')}.get(key)
            if extra:setattr(getattr(source,extra[0]),extra[1],None)
        for drawing in drawings:
            # пользовательский или общий рисунок не удаляем
            if drawing.library or drawing.get('project_damage_paint_source')!=source:continue
            users=bpy.data.user_map(subset={drawing}).get(drawing,set())
            if any(not isinstance(user,bpy.types.Collection) for user in users):continue
            if bpy.context.object==drawing and drawing.mode!='OBJECT':
                bpy.ops.object.mode_set(mode='OBJECT')
            data=drawing.data;materials=list(data.materials)
            bpy.data.objects.remove(drawing,do_unlink=True)
            if data.users==0:bpy.data.grease_pencils.remove(data)
            for mat in materials:
                if mat and mat.users==0:bpy.data.materials.remove(mat)
    finally:live._APPLYING=previous
    live.invalidate_index()
