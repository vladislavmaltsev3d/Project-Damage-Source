# настраиваем поверхность среза, её сглаживание и закрепление краёв
from . import quality_ui
# SPDX-License-Identifier: GPL-3.0-or-later
"""Boolean Break extras.

- Procedural Break Surface: Smoothness, Detail, Roughness, Sharp Ridges,
  Distortion and a size per noise layer, for every breaker type (Plane,
  Sphere, Strip, Custom cutters) and for the Macro Shape under a Texture
  surface. Surface Style presets and Copy to All Breakers.
- Rebar / Bricks / Planks Axis: an Auto toggle; with Auto off the X, Y and Z
  buttons appear and any of them can be on at once (one layout per axis).

The core takes the new values once it is rebuilt (BreakerSpec.surf_smooth,
BreakRebarParams.axis_mask). With an older core the add-on still works:
Smoothness then only tones down the turbulence and fine layers, and only the
first chosen axis gets a layout.
"""
from heapq import heappop, heappush

import bpy
import numpy as np
from . import live

DISPLACEMENT_MASK = 'project_damage_break_displacement'
RIM_FACES = 'project_damage_boolean_rim_faces'


def compute_displacement_weights(positions_cm, edges, rim, pin_cm):
    """Measure the shortest path from the rim along the cut surface."""
    pairs = np.concatenate((edges, edges[:, ::-1]))
    pairs = pairs[np.argsort(pairs[:, 0])]
    offsets = np.concatenate(([0], np.cumsum(np.bincount(pairs[:, 0], minlength=len(positions_cm)))))
    lengths = np.linalg.norm(positions_cm[pairs[:, 0]] - positions_cm[pairs[:, 1]], axis=1)
    distances = np.full(len(positions_cm), np.inf)
    distances[rim] = 0
    queue = [(0.0, int(vertex)) for vertex in rim]
    limit = 2 * pin_cm
    while queue:
        distance, vertex = heappop(queue)
        if distance > distances[vertex]:
            continue
        for i in range(offsets[vertex], offsets[vertex + 1]):
            neighbor = int(pairs[i, 1])
            candidate = distance + lengths[i]
            if candidate < min(limit, distances[neighbor]):
                distances[neighbor] = candidate
                heappush(queue, (candidate, neighbor))
    t = np.clip((distances - pin_cm) / pin_cm, 0, 1)
    return np.asarray(t * t * (3 - 2 * t), dtype=np.float32)


def _smooth_cut_positions(positions, edges, triangles, weights, strength):
    """Taubin smoothing with a fixed rim and local rollback at unsafe triangles."""
    original = positions.astype(np.float64)
    coords = original.copy()
    start, end = np.concatenate((edges, edges[:, ::-1])).T
    lengths = np.linalg.norm(original[start] - original[end], axis=1)
    edge_weight = 1 / np.maximum(lengths, max(np.median(lengths) * 0.1, 1e-12))
    degree = np.bincount(start, weights=edge_weight, minlength=len(coords))
    degree[degree == 0] = 1
    extra = 12 * max(strength - 1.0, 0.0)
    steps = [0.45, -0.47] * 8 + [0.35] * int(extra)
    if extra % 1:
        steps.append(0.35 * (extra % 1))
    for step in steps:
        mean = np.column_stack([
            np.bincount(start, weights=edge_weight * axis[end], minlength=len(coords))
            for axis in coords.T
        ]) / degree[:, None]
        coords += (step * weights[:, None]) * (mean - coords)

    delta = min(strength, 1.0) * (coords - original)
    a, b, c = triangles.T
    normal = np.cross(original[b] - original[a], original[c] - original[a])
    area_sq = np.einsum('ij,ij->i', normal, normal)
    for _ in range(10):
        candidate = (original + delta).astype(positions.dtype)
        check = candidate.astype(np.float64)
        changed = np.cross(check[b] - check[a], check[c] - check[a])
        reversed_or_flat = np.einsum('ij,ij->i', normal, changed) < 0.1 * area_sq
        if not reversed_or_flat.any():
            return candidate
        delta[np.unique(triangles[reversed_or_flat])] *= 0.5
    return positions


def compute_cut_surface(positions, edges, triangles, rim, basis_cm, pin_cm, smoothness):
    weights = np.ones(len(positions), dtype=np.float32)
    weights[rim] = 0
    if pin_cm > 0 and rim.size:
        weights = compute_displacement_weights(positions @ basis_cm.T, edges, rim, pin_cm)
    smoothed = None
    if smoothness > 0 and edges.size and np.any(weights > 0):
        smoothed = _smooth_cut_positions(positions, edges, triangles, weights, smoothness)
    return weights, smoothed


def _rim_faces(mesh, ids, cuts, totals):
    """The core leaves secondary rim chips unstamped. Find chip patches touching the cut."""
    from . import effect_groups
    chips = effect_groups._face_values(mesh, 'chips_P', bool)
    rim = np.zeros(len(ids), dtype=bool)
    if chips is None:
        return rim
    loop_edges = np.empty(len(mesh.loops), dtype=np.int32)
    mesh.loops.foreach_get('edge_index', loop_edges)
    cut_edges = set(loop_edges[np.repeat(cuts, totals)].tolist())
    offsets = np.concatenate(([0], np.cumsum(totals)))
    edge_faces = {}
    pending = []
    for face in np.flatnonzero(chips & (ids == 0)):
        edges = loop_edges[offsets[face]:offsets[face + 1]].tolist()
        for edge in edges:
            edge_faces.setdefault(edge, []).append(face)
        if cut_edges.intersection(edges):
            pending.append(face)
            rim[face] = True
    while pending:
        face = pending.pop()
        for edge in loop_edges[offsets[face]:offsets[face + 1]]:
            for neighbor in edge_faces.pop(edge, ()):
                if not rim[neighbor]:
                    rim[neighbor] = True
                    pending.append(neighbor)
    return rim


def finish_cut_surface_steps(mesh, matrix_world, unit_factor, pin_cm, smoothness, *,
                             write_mask=True, rim_damage=False, owner=None):
    from . import boolean_stack, effect_groups

    ids = effect_groups._face_values(mesh, effect_groups.ATTRIBUTE, np.int32)
    if ids is None:
        return
    cuts = ids == effect_groups.stamp_of('boolean')
    if owner is not None:
        owners = effect_groups._face_values(mesh, boolean_stack.FACE_OWNER, np.int32)
        if owners is None:
            return
        cuts &= owners == owner
    if not cuts.any():
        return

    totals = np.empty(len(mesh.polygons), dtype=np.int32)
    mesh.polygons.foreach_get('loop_total', totals)
    regions = [(cuts, smoothness)]
    if write_mask and rim_damage:
        if owner is not None:
            rim = effect_groups._face_values(mesh, RIM_FACES, bool)
            if rim is not None:
                rim &= owners == owner
        else:
            rim = _rim_faces(mesh, ids, cuts, totals)
            if rim.any():
                attr = mesh.attributes.get(RIM_FACES) or mesh.attributes.new(RIM_FACES, 'BOOLEAN', 'FACE')
                attr.data.foreach_set('value', rim)
        if rim is not None and rim.any():
            regions.append((rim, 0.0))
    for faces, strength in regions:
        yield from _finish_region_steps(
            mesh, faces, totals, matrix_world, unit_factor, pin_cm, strength, write_mask)


def _finish_region_steps(mesh, cuts, totals, matrix_world, unit_factor, pin_cm, smoothness, write_mask):
    from . import background_compute

    cut_corners = np.repeat(cuts, totals)
    corners = np.empty(len(mesh.loops), dtype=np.int32)
    mesh.loops.foreach_get('vertex_index', corners)
    cut_vertices = np.unique(corners[cut_corners])
    rim = np.intersect1d(cut_vertices, corners[~cut_corners])
    rim = np.searchsorted(cut_vertices, rim)

    if (pin_cm > 0 and rim.size) or smoothness > 0:
        loop_edges = np.empty(len(mesh.loops), dtype=np.int32)
        mesh.loops.foreach_get('edge_index', loop_edges)
        edges = np.empty(len(mesh.edges) * 2, dtype=np.int32)
        mesh.edges.foreach_get('vertices', edges)
        cut_edges = edges.reshape(-1, 2)[np.unique(loop_edges[cut_corners])]
        cut_edges = np.searchsorted(cut_vertices, cut_edges)
        coords = np.empty(len(mesh.vertices) * 3, dtype=np.float32)
        mesh.vertices.foreach_get('co', coords)
        basis_cm = np.asarray(matrix_world.to_3x3(), dtype=np.float64) * unit_factor
        triangles = None
        if smoothness > 0:
            mesh.calc_loop_triangles()
            tri_faces = np.empty(len(mesh.loop_triangles), dtype=np.int32)
            tri_corners = np.empty(len(mesh.loop_triangles) * 3, dtype=np.int32)
            mesh.loop_triangles.foreach_get('polygon_index', tri_faces)
            mesh.loop_triangles.foreach_get('vertices', tri_corners)
            triangles = np.searchsorted(cut_vertices, tri_corners.reshape(-1, 3)[cuts[tri_faces]])
        points = coords.reshape(-1, 3)
        weights, smoothed = yield from background_compute.task(
            'Finishing Boolean Break surface', compute_cut_surface,
            points[cut_vertices], cut_edges, triangles, rim, basis_cm, pin_cm, smoothness)
        if smoothed is not None:
            points[cut_vertices] = smoothed
            mesh.vertices.foreach_set('co', coords)
            mesh.update()
    else:
        weights = np.ones(len(cut_vertices), dtype=np.float32)
        weights[rim] = 0

    if not write_mask:
        return
    attr = mesh.attributes.get(DISPLACEMENT_MASK)
    if attr is not None and (attr.domain != 'POINT' or attr.data_type != 'FLOAT'):
        mesh.attributes.remove(attr)
        attr = None
    values = np.zeros(len(mesh.vertices), dtype=np.float32)
    if attr is None:
        attr = mesh.attributes.new(DISPLACEMENT_MASK, 'FLOAT', 'POINT')
    else:
        attr.data.foreach_get('value', values)
    values[cut_vertices] = weights
    attr.data.foreach_set('value', values)

                                                                             
                    

                                                  
SPEC_FIELDS = (
    ('surf_smooth', 'surf_smooth', 1.0),
    ('surf_detail', 'surf_detail', 1.0),
    ('surf_roughness', 'surf_roughness', 1.0),
    ('surf_crease', 'surf_crease', 1.0),
    ('surf_warp', 'surf_warp', 1.0),
    ('macro_scale', 'macro_scale', 100.0),
    ('turb_scale', 'turb_scale', 100.0),
    ('fine_scale', 'fine_scale', 100.0),
)

# Wood looks: only a core that has them gets them, 0 means they do nothing.
WOOD_SPEC_FIELDS = (('surf_aniso', 'surf_aniso', 1.0), ('surf_terrace', 'surf_terrace', 1.0))
AXIS_NUMBER = {'X': 0, 'Y': 1, 'Z': 2}

                                                                            
COPY_FIELDS = ('strength', 'macro_h', 'turb_h', 'fine_h', 'noise_scale', 'rim_amp', 'rim_scale',
               'smooth_slope', 'smooth_width') + tuple(prop for _f, prop, _d in SPEC_FIELDS) + (
                   'surf_aniso', 'surf_aniso_axis', 'surf_terrace')
SHARED_FIELDS = COPY_FIELDS + ('surf_mode', 'tex_path', 'tex_scale', 'tex_strength', 'tex_edge',
                               'tex_exit', 'tex_macro', 'tex_offset_x', 'tex_offset_y', 'tex_offset_z')

_DEFAULTS = dict(strength=100.0, macro_h=8.0, turb_h=7.0, fine_h=3.0, noise_scale=100.0,
                 surf_smooth=0.0, surf_detail=4.0, surf_roughness=0.5, surf_crease=1.0, surf_warp=0.0,
                 macro_scale=100.0, turb_scale=100.0, fine_scale=100.0,
                 surf_aniso=0.0, surf_aniso_axis='X', surf_terrace=0)

STYLES = {
    'DEFAULT': ("Default", "The original break surface", {}),
    'CONCRETE': ("Rough Concrete", "Coarse, gritty break with sharp ridges",
                 dict(macro_h=9.0, turb_h=8.0, fine_h=4.5, surf_detail=5.0, surf_roughness=0.55, surf_crease=1.0,
                      fine_scale=80.0)),
    'STONE': ("Smooth Stone", "Rounded, worn surface without grit",
              dict(macro_h=10.0, turb_h=5.0, fine_h=1.5, surf_smooth=0.6, surf_detail=3.0, surf_roughness=0.4,
                   surf_crease=0.3, macro_scale=130.0)),
    'ROCK': ("Jagged Rock", "Deep, sharp, layered break",
             dict(macro_h=14.0, turb_h=11.0, fine_h=4.0, surf_detail=6.0, surf_roughness=0.6, surf_crease=1.0,
                  surf_warp=0.4, turb_scale=80.0)),
    'PLASTER': ("Soft Plaster", "Shallow, soft break for plaster and render",
                dict(macro_h=4.0, turb_h=3.0, fine_h=1.0, surf_smooth=0.4, surf_detail=3.0, surf_crease=0.5)),
    'SHATTERED': ("Shattered", "Many small sharp facets",
                  dict(macro_h=5.0, turb_h=9.0, fine_h=5.0, noise_scale=60.0, surf_detail=7.0, surf_roughness=0.65,
                       surf_crease=1.0, fine_scale=70.0)),
    'ERODED': ("Eroded", "Large flowing shapes, washed out",
               dict(macro_h=12.0, turb_h=6.0, fine_h=1.0, surf_smooth=0.5, surf_crease=0.2, surf_warp=1.0,
                    macro_scale=150.0)),
    'WOOD_SPLINTERED': ("Wood - Splintered Fibers", "Torn across the grain: ragged fibres stretched along the Grain Axis, "
                        "sharp creases and fine fibrous detail. Set Grain Axis to match the wood",
                        dict(macro_h=8.0, turb_h=26.0, fine_h=12.0, noise_scale=60.0, surf_detail=6.0, surf_roughness=0.6,
                             surf_crease=1.0, surf_warp=0.15, macro_scale=130.0, turb_scale=50.0, fine_scale=35.0,
                             surf_aniso=0.92, surf_terrace=0)),
    'WOOD_PLYWOOD': ("Wood - Plywood Layers", "Stepped plywood break: flat shelves ply by ply with moderate roughness "
                     "and a slight stretch along the Grain Axis",
                     dict(macro_h=20.0, turb_h=16.0, fine_h=3.0, noise_scale=100.0, surf_detail=4.0, surf_roughness=0.45,
                          surf_crease=0.8, macro_scale=45.0, turb_scale=35.0, fine_scale=30.0,
                          surf_aniso=0.75, surf_terrace=5)),
}


def _pct(name, description, default=100.0):
    return bpy.props.FloatProperty(name=name, default=default, subtype='PERCENTAGE', min=25.0, max=400.0,
                                   update=live.touch_prop, description=description)


BREAKER_PROPS = {
    'surf_smooth': bpy.props.FloatProperty(
        name='Smoothness', default=0.0, min=0.0, max=1.0, subtype='FACTOR', update=live.touch_prop,
        description='Softens the break surface: fewer and weaker fine octaves, rounded instead of sharp ridges. '
                    '0 = the original surface'),
    'surf_detail': bpy.props.FloatProperty(
        name='Detail', default=4.0, min=1.0, max=8.0, update=live.touch_prop,
        description='Octaves of the fine layer: more detail adds smaller and smaller grain (4 = the original)'),
    'surf_roughness': bpy.props.FloatProperty(
        name='Roughness', default=0.5, min=0.05, max=0.95, subtype='FACTOR', update=live.touch_prop,
        description='How strong each finer octave is compared to the one before (0.5 = the original)'),
    'surf_crease': bpy.props.FloatProperty(
        name='Sharp Ridges', default=1.0, min=0.0, max=1.0, subtype='FACTOR', update=live.touch_prop,
        description='Turbulence layer: 1 = sharp ridges and creases (the original), 0 = rounded bumps'),
    'surf_warp': bpy.props.FloatProperty(
        name='Distortion', default=0.0, min=0.0, soft_max=2.0, max=4.0, update=live.touch_prop,
        description='Twists the noise into flowing, less regular shapes'),
    'surf_aniso': bpy.props.FloatProperty(
        name='Grain Stretch', default=0.0, min=0.0, max=1.0, subtype='FACTOR', update=live.touch_prop,
        description='Draws the noise out along the Grain Axis into long fibres, for wood broken across the grain. '
                    '0 = no stretch, the surface is exactly as without this control'),
    'surf_aniso_axis': bpy.props.EnumProperty(
        name='Grain Axis', default='X', update=live.touch_prop,
        items=(('X', 'X', "The breaker's local X axis", 'NONE', 0), ('Y', 'Y', "The breaker's local Y axis. "
                'A Plane or Strip has its surface in X and Z, so Y acts as Z there', 'NONE', 1),
               ('Z', 'Z', "The breaker's local Z axis", 'NONE', 2)),
        description="The breaker-local direction the fibres run along. Rotate the breaker to aim it. Used with Grain Stretch"),
    'surf_terrace': bpy.props.IntProperty(
        name='Terraces', default=0, min=0, max=24, update=live.touch_prop,
        description='Cuts the break surface into about this many flat steps, like the plies of a plywood break. '
                    'Tall noise layers make the steps show; needs enough Resolution to resolve them. '
                    '0 = no steps, the surface is exactly as without this control'),
    'macro_scale': _pct('Macro Scale', 'Size of the large shapes, on top of Noise Scale'),
    'turb_scale': _pct('Turbulence Scale', 'Size of the ridges and creases, on top of Noise Scale'),
    'fine_scale': _pct('Fine Scale', 'Size of the fine grain, on top of Noise Scale'),
}


def core_supports_surface():
    try:
        from . import core_loader
        return hasattr(core_loader.load().BreakerSpec(), 'surf_smooth')
    except Exception:
        return False


def core_supports_axes():
    try:
        from . import core_loader
        return hasattr(core_loader.load().Params().break_rebar, 'axis_mask')
    except Exception:
        return False


def fill_spec(s, bp):
    """Breaker props -> core BreakerSpec (after params.fill_breakers set the
    heights). An older core has none of the new fields: Smoothness then tones
    down the turbulence and fine layers instead."""
    if hasattr(s, 'surf_smooth'):
        for field, prop, div in SPEC_FIELDS:
            setattr(s, field, float(getattr(bp, prop)) / div)
        if hasattr(s, 'surf_aniso'):
            for field, prop, div in WOOD_SPEC_FIELDS:
                setattr(s, field, float(getattr(bp, prop)) / div)
            s.surf_aniso_axis = AXIS_NUMBER[bp.surf_aniso_axis]
        return
    sm = min(1.0, max(0.0, float(bp.surf_smooth)))
    if sm > 0.0:
        s.turb_h = s.turb_h * (1.0 - 0.5 * sm)
        s.fine_h = s.fine_h * (1.0 - sm) * (1.0 - sm)


def hash_spec(h, spec, hash_value):
    for field, _prop, _div in SPEC_FIELDS:
        if hasattr(spec, field):
            hash_value(h, getattr(spec, field))
    for field in ('surf_aniso', 'surf_aniso_axis', 'surf_terrace'):
        if hasattr(spec, field):
            hash_value(h, getattr(spec, field))


def _rows(src):
    p = src.project_damage
    return [row.obj for row in p.breakers if row.obj is not None and row.obj != src]


def active_cutter(p):
    return p.breakers[min(p.breaker_index, len(p.breakers) - 1)].obj if p.breakers else None


def _copy_surface(reference, targets, fields=SHARED_FIELDS):
    bp = reference.project_damage_breaker
    values = [(name, getattr(bp, name)) for name in fields]
    changed = set()
    previous = live._APPLYING
    live._APPLYING = True
    try:
        for cutter in targets:
            if cutter == reference or not cutter.is_editable:
                continue
            settings = cutter.project_damage_breaker
            for name, value in values:
                if getattr(settings, name) != value:
                    setattr(settings, name, value)
                    changed.add(cutter)
    finally:
        live._APPLYING = previous
    return changed


def _share_toggle(p, context):
    if not live._HISTORY_RESTORING and not live._APPLYING and p.break_share_surface:
        reference = active_cutter(p) or next(iter(_rows(p.id_data)), None)
        if reference is not None:
            for cutter in _copy_surface(reference, _rows(p.id_data)):
                live.touch_prop(cutter.project_damage_breaker, context)
    live.touch_prop(p, context)


def share_surface_edit(bp, context):
    if live._APPLYING or live._HISTORY_RESTORING:
        return
    reference = bp.id_data
    changed = set()
    from . import boolean_stack
    for source in live._scan().linkers:
        layers = [boolean_stack.layer_props(source, row) for row in boolean_stack.rows(source)]
        for props in layers or (source.project_damage,):
            if not props.break_share_surface:
                continue
            cutters = [row.obj for row in props.breakers if row.obj is not None and row.obj != source]
            if reference in cutters:
                changed.update(_copy_surface(reference, cutters))
    live.touch_prop(bp, context)
    for cutter in changed:
        live.touch_prop(cutter.project_damage_breaker, context)


def cutter_link_changed(row, context):
    source, cutter = row.id_data, row.obj
    if not live._HISTORY_RESTORING and not live._APPLYING and cutter is not None:
        p = source.project_damage
        if p.break_share_surface:
            reference = active_cutter(p)
            if reference is None or reference == cutter:
                reference = next((other for other in _rows(source) if other != cutter), None)
            if reference is not None:
                _copy_surface(reference, (cutter,))
    live.touch_prop(row, context)


def _shown_breaker(context):
    from . import ownership
    src = ownership.context_source(context)
    if src is None:
        return None, None
    bp_obj = getattr(context, 'project_damage_breaker_obj', None)
    if bp_obj is not None:
        return src, bp_obj
    if src.project_damage_breaker.is_breaker:
        return src, src
    p = src.project_damage
    return src, active_cutter(p)


# действие «Surface Style»
class PROJECT_DAMAGE_OT_break_surface_style(bpy.types.Operator):
    """Set the procedural break surface of this breaker to a style"""
    bl_idname = 'project_damage.break_surface_style'
    bl_label = 'Surface Style'
    bl_options = {'REGISTER', 'UNDO'}

    style: bpy.props.EnumProperty(name='Style', items=[(k, v[0], v[1]) for k, v in STYLES.items()])

    @classmethod
    def poll(cls, context):
        return _shown_breaker(context)[1] is not None

    def execute(self, context):
        _src, br = _shown_breaker(context)
        bp = br.project_damage_breaker
        values = dict(_DEFAULTS)
        values.pop('strength')
        values.update(STYLES[self.style][2])
        previous = live._APPLYING
        live._APPLYING = True
        try:
            for k, v in values.items():
                setattr(bp, k, v)
        finally:
            live._APPLYING = previous
        share_surface_edit(bp, context)
        return {'FINISHED'}


# действие «Copy Surface to All Breakers»
class PROJECT_DAMAGE_OT_break_surface_copy(bpy.types.Operator):
    """Copy this breaker's break surface (procedural settings, Strength, Smooth Slope) to every other breaker of the object"""
    bl_idname = 'project_damage.break_surface_copy'
    bl_label = 'Copy Surface to All Breakers'
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        src, br = _shown_breaker(context)
        return br is not None and src is not None and br is not src and len(_rows(src)) > 1

    def execute(self, context):
        src, br = _shown_breaker(context)
        n = 0
        for other in _copy_surface(br, _rows(src), COPY_FIELDS):
            live.touch_prop(other.project_damage_breaker, context)
            n += 1
        self.report({'INFO'}, 'Break surface copied to %d breaker%s' % (n, '' if n == 1 else 's'))
        return {'FINISHED'}


def _breaker_mode(br):
    try:
        from . import breaker as breaker_mod
        return breaker_mod.effective_mode(br)
    except Exception:
        return 0


def draw_procedural(layout, br, show_copy=False, show_strength=True):
    """The procedural settings of one breaker, for the Break Surface box."""
    bp = br.project_damage_breaker
    col = layout.column()
    col.use_property_split = False
    col.use_property_decorate = False
    row = col.row(align=True)
    row.context_pointer_set('project_damage_breaker_obj', br)
    from . import ui_image_hints
    ui_image_hints.button(col.row(align=True), 'break_surface_styles')
    styles=row.grid_flow(row_major=True,columns=3,even_columns=True,even_rows=True,align=True)
    styles.scale_y=1.5
    for key,(label,hint,values) in STYLES.items():
        styles.operator('project_damage.break_surface_style',text=label).style=key
    if show_copy:
        row.operator('project_damage.break_surface_copy', text='', icon='DUPLICATE')
    if show_strength:
        col.prop(bp, 'strength')
    col.prop(bp, 'noise_scale')
    col.prop(bp, 'surf_smooth')
    quality_ui.draw(col, bp, 'surf_detail')
    col.prop(bp, 'surf_roughness')
    col.prop(bp, 'surf_crease')
    col.prop(bp, 'surf_warp')
    draw_wood_fields(col, bp)
    from . import effect_layout
    sub = effect_layout.section(col,'Noise Layers').column(align=True)
    sub.prop(bp, 'macro_h')
    sub.prop(bp, 'macro_scale')
    sub.prop(bp, 'turb_h')
    sub.prop(bp, 'turb_scale')
    sub.prop(bp, 'fine_h')
    sub.prop(bp, 'fine_scale')
    sheet = _breaker_mode(br) in (0, 2)
    edge = effect_layout.section(col,'Edge Break-up').column(align=True)
    edge.active = sheet
    edge.prop(bp, 'rim_amp')
    edge.prop(bp, 'rim_scale')
    if not core_supports_surface():
        col.label(text='Rebuild the core for all of these (see the notes)', icon='ERROR')


def draw_wood_fields(layout, bp):
    """Grain and terraces: the controls the wood surface styles use."""
    layout.prop(bp, 'surf_aniso')
    row = layout.row(align=True)
    row.active = bp.surf_aniso > 0.0
    row.prop(bp, 'surf_aniso_axis', expand=True)
    layout.prop(bp, 'surf_terrace')


def draw_noise_extra(layout, bp):
    """The new fields under Surface Noise in the breaker Empty's own panel."""
    for k in ('surf_smooth', 'surf_detail', 'surf_roughness', 'surf_crease', 'surf_warp',
              'macro_scale', 'turb_scale', 'fine_scale'):
        layout.prop(bp, k)
    draw_wood_fields(layout, bp)


                                                                             
                                                                        
                                                                
                                                                           

def _guess_axis(self):
    """The axis Auto would pick for a grid: the thinnest side of the object."""
    obj = self.id_data
    try:
        d = list(obj.dimensions)
        return 1 << min(range(3), key=lambda i: d[i])
    except Exception:
        return 1


def _get_axes(self):
    b = int(self.rebar_axes_bits)
    if b < 0:
        return 0
    if b:
        return b
    a = self.rebar_axis
    return 1 << (int(a) - 1) if a in ('1', '2', '3') else _guess_axis(self)


def _first(b):
    return str((b & -b).bit_length())


def _set_axes(self, value):
    value = int(value) & 7
    # Zero in legacy scenes means to read rebar_axis.
    self.rebar_axes_bits = value or -1
    if value and self.rebar_axis != '0':
        self.rebar_axis = _first(value)


def _get_bit(self, i):
    return bool(_get_axes(self) & (1 << i))


def _set_bit(self, i, on):
    b = _get_axes(self)
    _set_axes(self, (b | (1 << i)) if on else (b & ~(1 << i)))                           


def _get_auto(self):
    return self.rebar_axis == '0'


def _set_auto(self, on):
    b = _get_axes(self)
    self.rebar_axes_bits = b or -1
    self.rebar_axis = '0' if on else _first(b or _guess_axis(self))


AXES_ITEMS = (('X', 'X', '', 1), ('Y', 'Y', '', 2), ('Z', 'Z', '', 4))

PROPS = {
    'rebar_axes_bits': bpy.props.IntProperty(default=0, min=-1, max=7, options={'HIDDEN'}),
    'rebar_axis_auto': bpy.props.BoolProperty(
        name='Auto', get=_get_auto, set=_set_auto, update=live.touch_prop,
        description='Grid: the thinnest side is the wall normal. Cage: the longest side is the column axis. '
                    'Off: pick one or more axes by hand'),
    'rebar_axes': bpy.props.EnumProperty(
        name='Axes', items=AXES_ITEMS, options={'ENUM_FLAG'}, get=_get_axes, set=_set_axes,
        update=live.touch_prop, description='The axes that get a layout'),
    'rebar_axis_x': bpy.props.BoolProperty(
        name='X', get=lambda self: _get_bit(self, 0), set=lambda self, v: _set_bit(self, 0, v),
        update=live.touch_prop, description='A layout for the X axis (several axes can be on)'),
    'rebar_axis_y': bpy.props.BoolProperty(
        name='Y', get=lambda self: _get_bit(self, 1), set=lambda self, v: _set_bit(self, 1, v),
        update=live.touch_prop, description='A layout for the Y axis (several axes can be on)'),
    'rebar_axis_z': bpy.props.BoolProperty(
        name='Z', get=lambda self: _get_bit(self, 2), set=lambda self, v: _set_bit(self, 2, v),
        update=live.touch_prop, description='A layout for the Z axis (several axes can be on)'),
}


def axes_of(p):
    """None for Auto, else the chosen axes as local indices, e.g. (0, 1)."""
    if p.rebar_axis == '0':
        return None
    b = _get_axes(p)
    return tuple(i for i in range(3) if b & (1 << i))


def fill_rebar_axes(br, props):
    """props -> core BreakRebarParams.axis / axis_mask. A single axis is the
    old axis choice exactly (mask 0)."""
    axes = axes_of(props)
    if not axes:
        if axes == ():
            br.enable = False
        br.axis = 0
        mask = 0
    else:
        br.axis = axes[0] + 1
        mask = sum(1 << a for a in axes) if len(axes) > 1 else 0
    if hasattr(br, 'axis_mask'):
        br.axis_mask = mask


def draw_rebar_axes(layout, p):
    row = layout.row(align=True)
    row.label(text='Axis')
    row.prop(p, 'rebar_axis_auto', toggle=True)
    if not p.rebar_axis_auto:
        for k in 'xyz':
            row.prop(p, 'rebar_axis_' + k, toggle=True)
        axes = axes_of(p)
        if not axes:
            layout.label(text='Assign at least one axis', icon='INFO')
        elif len(axes) > 1 and not core_supports_axes():
            layout.label(text='Rebuild the core for several axes (now: %s only)' % 'XYZ'[axes[0]], icon='ERROR')


                                                                             

def add_properties(project_damage_props, breaker_props):
    for k, v in PROPS.items():
        project_damage_props.__annotations__[k] = v
    for k, v in BREAKER_PROPS.items():
        breaker_props.__annotations__[k] = v
    project_damage_props.__annotations__['break_share_surface'] = bpy.props.BoolProperty(
        name='Use on All Cutters', default=False, update=_share_toggle,
        description='Copy the active cutter’s Break Surface settings to every linked cutter and keep them in sync while enabled')
    for name in SHARED_FIELDS:
        breaker_props.__annotations__[name].keywords['update'] = share_surface_edit


_CLASSES = (PROJECT_DAMAGE_OT_break_surface_style, PROJECT_DAMAGE_OT_break_surface_copy)


# подключаем свойства и действия этого модуля к Blender
def register():
    for cls in _CLASSES:
        bpy.utils.register_class(cls)


# убираем подключения модуля при выключении аддона
def unregister():
    for cls in reversed(_CLASSES):
        bpy.utils.unregister_class(cls)
