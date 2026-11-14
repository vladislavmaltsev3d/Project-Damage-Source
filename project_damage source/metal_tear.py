# настройки и рисунок для разрыва металлического листа
from . import quality_ui
# SPDX-License-Identifier: GPL-3.0-or-later
# Modified work: new effect added to ProjectDamage (Cornelius Dammrich, GPL-3).
"""Metal Tear Draw (core: metal_tear.cpp, effect stamp 34).

Draw a Grease Pencil stroke on sheet metal: a jagged slit is torn through the
metal along it, opening from a hairline to the full width, and the metal around
it is pushed in with the tear (a smooth bend of the whole sheet, so thin sheets
never thin out or cross themselves), with small beads of buckled metal along
the edges. Two point attributes drive the material: PD_MetalTear Heat (glow at
the hot edges) and PD_MetalTear Scratches (fine scratches around the tear).

build.rebuild_object calls before_core(); fingerprints() calls hash_params();
after effect_groups.tag_mesh it calls after_result()."""
from .blender_compat import enable_nodes
import bpy
import numpy as np

from . import live

STAMP = 34
DEFAULT_MATERIAL = 'ProjectDamage_MetalTear'
HEAT_ATTR = 'PD_MetalTear Heat'
SCRATCH_ATTR = 'PD_MetalTear Scratches'


def _hint(general, low, high):
    """'General \\n Lowest \\n Highest' - two line breaks, so ui_hints keeps it verbatim."""
    return general + '\n' + low + '\n' + high


def _drawing_poll(self, obj):
    return obj.type in {'GREASEPENCIL', 'GPENCIL', 'CURVE'}


P = bpy.props
PROPS = {
    'metaltear_enable': P.BoolProperty(
        name='Metal Tear Draw', default=False, update=live.touch_prop,
        description=_hint('Tears a jagged slit through sheet metal along your Grease Pencil strokes',
                          'Off: the metal is intact',
                          'On: every stroke becomes a torn, pushed-in slit')),
    'metaltear_drawing': P.PointerProperty(
        name='Drawing', type=bpy.types.Object, poll=_drawing_poll, update=live.touch_prop,
        description=_hint('Grease Pencil (or curve) object whose strokes are the tears',
                          'Empty: nothing is torn',
                          'Set: each stroke on the current frame tears one slit, from its first point to its last')),
    'metaltear_seed': P.IntProperty(
        name='Seed', default=0, min=0, max=100000, update=live.touch_prop,
        description=_hint('Picks a different shape of the torn edges and beads',
                          'Lowest: the first variant',
                          'Highest: any other value gives a new, repeatable variant')),
    'metaltear_width': P.FloatProperty(
        name='Width', default=0.6, min=0.01, soft_max=10.0, update=live.touch_prop,
        description=_hint('Widest opening of the slit, in cm',
                          'Lowest: a hairline crack',
                          'Highest: a wide, gaping tear')),
    'metaltear_taper': P.FloatProperty(
        name='Taper', default=70.0, min=0.0, max=100.0, subtype='PERCENTAGE', update=live.touch_prop,
        description=_hint('How the slit opens along the stroke (draw it from the start of the tear to its widest end)',
                          'Lowest: the same width all along',
                          'Highest: starts as a hairline and opens toward the end')),
    'metaltear_jagged': P.FloatProperty(
        name='Jagged Edges', default=50.0, min=0.0, max=100.0, subtype='PERCENTAGE', update=live.touch_prop,
        description=_hint('Torn notches along the slit edges',
                          'Lowest: clean, smooth edges like a laser cut',
                          'Highest: ragged, torn edges')),
    'metaltear_meander': P.FloatProperty(
        name='Meander', default=30.0, min=0.0, max=100.0, subtype='PERCENTAGE', update=live.touch_prop,
        description=_hint('How much the slit wanders away from your stroke line',
                          'Lowest: follows the stroke exactly',
                          'Highest: wobbles from side to side')),
    'metaltear_cut_depth': P.FloatProperty(
        name='Cut Depth', default=1.0, min=0.01, soft_max=10.0, update=live.touch_prop,
        description=_hint('How deep the slit cuts, in cm; it goes right through any sheet thinner than this',
                          'Lowest: a shallow gouge in thick metal',
                          'Highest: cuts through thick plate')),
    'metaltear_dent_depth': P.FloatProperty(
        name='Push Depth', default=0.8, min=0.0, soft_max=5.0, update=live.touch_prop,
        description=_hint('How far the metal around the tear is pushed in, in cm',
                          'Lowest: the sheet stays flat',
                          'Highest: a deep trough bent in along the tear')),
    'metaltear_dent_width': P.FloatProperty(
        name='Push Width', default=2.5, min=0.05, soft_max=20.0, update=live.touch_prop,
        description=_hint('How far to the sides the push reaches, in cm',
                          'Lowest: only a narrow strip bends, sharply',
                          'Highest: a wide, gentle dent around the tear')),
    'metaltear_lip_curl': P.FloatProperty(
        name='Lip Curl', default=50.0, min=0.0, max=100.0, subtype='PERCENTAGE', update=live.touch_prop,
        description=_hint('How sharply the torn lips bend in right at the edge',
                          'Lowest: one soft, even dent',
                          'Highest: the edges fold in sharply, like a punched-through sheet')),
    'metaltear_blisters': P.FloatProperty(
        name='Beads', default=40.0, min=0.0, max=100.0, subtype='PERCENTAGE', update=live.touch_prop,
        description=_hint('Small beads of buckled or molten metal along the edges',
                          'Lowest: clean edges',
                          'Highest: edges lined with beads')),
    'metaltear_blister_size': P.FloatProperty(
        name='Bead Size', default=0.25, min=0.01, soft_max=2.0, update=live.touch_prop,
        description=_hint('Size of the beads, in cm',
                          'Lowest: fine grit along the edge',
                          'Highest: large blobs')),
    'metaltear_heat_spread': P.FloatProperty(
        name='Heat Spread', default=0.8, min=0.0, soft_max=10.0, update=live.touch_prop,
        description=_hint('How far the PD_MetalTear Heat attribute reaches from the edges, in cm (use it for the orange glow)',
                          'Lowest (0): no heat attribute',
                          'Highest: a wide heat-tinted zone around the tear')),
    'metaltear_scratches': P.FloatProperty(
        name='Scratches', default=40.0, min=0.0, max=100.0, subtype='PERCENTAGE', update=live.touch_prop,
        description=_hint('Fine scratches around the tear, written to the PD_MetalTear Scratches attribute for the material',
                          'Lowest: no scratches',
                          'Highest: densely scratched metal around the tear')),
    'metaltear_detail': P.FloatProperty(
        name='Resolution', default=1.5, min=0.01, soft_max=2.0, update=live.touch_prop,
        description=_hint('Triangle size around the tears, in cm (the rest of the mesh stays light)',
                          'Lowest: crisp edges and smooth bends, heavier mesh',
                          'Highest: light mesh, faceted bends')),
    'metaltear_max_tris': P.IntProperty(
        name='Max Triangles', default=1500000, min=20000, soft_max=8000000, update=live.touch_prop,
        description=_hint('Triangle budget for the refinement around the tears',
                          'Lowest: always light, long strokes lose detail',
                          'Highest: full detail on long strokes, more memory and time')),
    'metaltear_material': P.PointerProperty(
        name='Edge Material', type=bpy.types.Material, update=live.touch_prop,
        description=_hint('Material of the torn edge faces inside the slit',
                          'Empty: the default ProjectDamage_MetalTear material',
                          'Set: this material; the sheet keeps its own materials')),
}

FIELDS = ('seed', 'taper', 'jagged', 'meander', 'lip_curl', 'blisters', 'max_tris')
LENGTHS = ('width', 'cut_depth', 'dent_depth', 'dent_width', 'blister_size', 'detail')
_HASHED = FIELDS + LENGTHS + ('enable',)


def wanted(p):
    return bool(getattr(p, 'metaltear_enable', False) and not p.all_off and p.metaltear_drawing is not None)


def _cm_to_local(source, p):
    """cm (Master Scale applied) -> the source's local core units."""
    from .params import _clamp_scale
    s = source.matrix_world.to_scale()
    mean = max(1e-9, (abs(s.x) + abs(s.y) + abs(s.z)) / 3.0)
    return _clamp_scale(p.global_scale / 100.0) / mean


def _paths(context, source, p):
    from . import paint
    return paint.read_paths(context, source, p.metaltear_drawing, min_points=2)


# готовим данные этого эффекта перед расчётом геометрии
def before_core(context, source, params, snap, prep, points, polys, factor, reports):
    p = source.project_damage
    block = getattr(params, 'metal_tear', None)
    if not wanted(p):
        if block is not None:
            block.enable = False
        if getattr(p, 'metaltear_enable', False) and p.metaltear_drawing is None and not p.all_off:
            reports.append(({'WARNING'}, 'Metal Tear Draw: no drawing linked; click New Drawing and draw the tears'))
        return
    if block is None:
        reports.append(({'WARNING'}, 'Metal Tear Draw: rebuild the core and restart Blender to use it'))
        return
    paths = _paths(context, source, p)
    if not paths:
        reports.append(({'WARNING'}, 'Metal Tear Draw: no strokes on this frame; nothing is torn'))
        block.enable = False
        return
    for name in FIELDS:
        setattr(block, name, getattr(p, 'metaltear_' + name))
    k = _cm_to_local(source, p)
    for name in LENGTHS:
        setattr(block, name, float(getattr(p, 'metaltear_' + name)) * k)
    xyz = np.ascontiguousarray(np.concatenate([path * factor for path in paths]).ravel(), dtype=np.float64)
    counts = np.ascontiguousarray([len(path) for path in paths], dtype=np.int32)
    block.set_strokes(xyz, counts)
    block.enable = True


def hash_params(h, params):
    block = getattr(params, 'metal_tear', None)
    if block is None or not block.enable:
        return
    h.update(b'metal_tear')
    h.update(repr(tuple(getattr(block, name) for name in _HASHED)).encode())


def hash_inputs(h, context, source):
    """The strokes themselves (fingerprints() must rebuild when they change)."""
    p = source.project_damage
    if not wanted(p):
        return
    for path in _paths(context, source, p):
        h.update(np.ascontiguousarray(path, dtype=np.float64).tobytes())


                                                                             
                                                    

def _segments(paths):
    a = np.concatenate([path[:-1] for path in paths])
    b = np.concatenate([path[1:] for path in paths])
    return a, b


def _dist_to_segments(pts, a, b, chunk=4096):
    """Distance of every point to the nearest segment (a[i], b[i])."""
    out = np.full(len(pts), np.inf)
    ab = b - a
    l2 = np.maximum((ab * ab).sum(1), 1e-30)
    for s in range(0, len(a), 64):
        A, AB, L2 = a[s:s + 64], ab[s:s + 64], l2[s:s + 64]
        for i in range(0, len(pts), chunk):
            P = pts[i:i + chunk, None, :]
            t = np.clip(((P - A[None]) * AB[None]).sum(2) / L2[None], 0.0, 1.0)
            d = np.linalg.norm(P - (A[None] + AB[None] * t[..., None]), axis=2).min(1)
            out[i:i + chunk] = np.minimum(out[i:i + chunk], d)
    return out


def _write_point_attr(mesh, name, values):
    attr = mesh.attributes.get(name)
    if attr is not None and (attr.domain != 'POINT' or attr.data_type != 'FLOAT'):
        mesh.attributes.remove(attr)
        attr = None
    if attr is None:
        attr = mesh.attributes.new(name, 'FLOAT', 'POINT')
    attr.data.foreach_set('value', np.ascontiguousarray(values, dtype=np.float32))


def after_result(context, source, mesh):
    """build.rebuild_object, right after effect_groups.tag_mesh(res_mesh, ...).
    Positions are the source's local space (Blender units), like the paths."""
    p = source.project_damage
    if not wanted(p) or len(mesh.vertices) == 0:
        return
    paths = _paths(context, source, p)
    if not paths:
        return
    from .params import unit_factor
    k = _cm_to_local(source, p) / unit_factor(context)                                                    
    co = np.empty(len(mesh.vertices) * 3, dtype=np.float64)
    mesh.vertices.foreach_get('co', co)
    co = co.reshape(-1, 3)
    a, b = _segments(paths)
    spread = p.metaltear_heat_spread * k
    reach = max(spread * 4.0, (p.metaltear_dent_width + p.metaltear_width) * k * 2.0)
    lo = np.minimum(a.min(0), b.min(0)) - reach
    hi = np.maximum(a.max(0), b.max(0)) + reach
    near = np.flatnonzero(((co >= lo) & (co <= hi)).all(1))
    heat = np.zeros(len(co), dtype=np.float32)
    scratch = np.zeros(len(co), dtype=np.float32)
    if len(near):
        d = _dist_to_segments(co[near], a, b)
        if spread > 0.0:
            half = p.metaltear_width * k * 0.5
            heat[near] = np.exp(-np.maximum(d - half, 0.0) / spread)
        amount = p.metaltear_scratches / 100.0
        if amount > 0.0:
                                                                                 
            rng = np.random.default_rng(p.metaltear_seed + 7)
            seg_len = np.linalg.norm(b - a, axis=1)
            total = float(seg_len.sum())
            zone = max(p.metaltear_dent_width * 3.0, 6.0) * k
            n = int(amount * 120 * max(1.0, total / max(zone, 1e-9)))
            pick = rng.choice(len(a), size=n, p=seg_len / max(total, 1e-30))
            t = rng.random(n)
            base = a[pick] + (b[pick] - a[pick]) * t[:, None]
            dirs = (b[pick] - a[pick]) / np.maximum(seg_len[pick, None], 1e-30)
            jitter = rng.normal(size=(n, 3))
            off = jitter / np.maximum(np.linalg.norm(jitter, axis=1, keepdims=True), 1e-30) * (rng.random(n) * zone)[:, None]
            sdir = dirs + rng.normal(scale=0.6, size=(n, 3))
            sdir /= np.maximum(np.linalg.norm(sdir, axis=1, keepdims=True), 1e-30)
            length = (0.3 + rng.random(n)) * zone * 0.5
            s0 = base + off - sdir * length[:, None] * 0.5
            s1 = base + off + sdir * length[:, None] * 0.5
            width = 0.04 * k
            ds = _dist_to_segments(co[near], s0, s1)
            scratch[near] = np.clip(1.0 - ds / width, 0.0, 1.0)
    _write_point_attr(mesh, HEAT_ATTR, heat)
    _write_point_attr(mesh, SCRATCH_ATTR, scratch)
    mesh.update()


# показываем настройки выбранного эффекта
def draw_settings(layout, context):
    from . import ownership, effect_layout, paint
    src = ownership.context_source(context)
    if src is None:
        return
    p = src.project_damage
    layout.use_property_split = False
    layout.use_property_decorate = False

    box = effect_layout.section(layout, 'Draw')
    paint.drawing_row(box, p, 'METALTEAR')
    col = box.column(align=True)
    col.prop(p, 'metaltear_seed')

    box = effect_layout.section(layout, 'Slit')
    col = box.column(align=True)
    col.prop(p, 'metaltear_width')
    col.prop(p, 'metaltear_taper')
    col.prop(p, 'metaltear_jagged')
    col.prop(p, 'metaltear_meander')
    col.prop(p, 'metaltear_cut_depth')

    box = effect_layout.section(layout, 'Bent Metal')
    col = box.column(align=True)
    col.prop(p, 'metaltear_dent_depth')
    col.prop(p, 'metaltear_dent_width')
    col.prop(p, 'metaltear_lip_curl')
    col = box.column(align=True)
    col.prop(p, 'metaltear_blisters')
    col.prop(p, 'metaltear_blister_size')

    box = effect_layout.section(layout, 'Material Masks')
    col = box.column(align=True)
    col.prop(p, 'metaltear_heat_spread')
    col.prop(p, 'metaltear_scratches')
    from . import material_controls
    material_controls.inline(col,p,'metaltear_material',text='Torn Edges')

    box = effect_layout.section(layout, 'Quality')
    col = box.column(align=True)
    quality_ui.draw(col, p, 'metaltear_detail')
    col.prop(p, 'metaltear_max_tris')


# подключаем свойства и действия этого модуля к Blender
def register():
    pass


# убираем подключения модуля при выключении аддона
def unregister():
    pass


def default_material():
    mat=bpy.data.materials.get(DEFAULT_MATERIAL)
    if mat is not None:return mat
    mat=bpy.data.materials.new(DEFAULT_MATERIAL);enable_nodes(mat)
    mat.diffuse_color=(.075,.065,.055,1.)
    nt=mat.node_tree;bs=nt.nodes.get('Principled BSDF')
    bs.inputs['Base Color'].default_value=mat.diffuse_color;bs.inputs['Metallic'].default_value=.8
    bs.inputs['Roughness'].default_value=.45
    attr=nt.nodes.new('ShaderNodeAttribute');attr.attribute_name=HEAT_ATTR
    ramp=nt.nodes.new('ShaderNodeValToRGB');ramp.color_ramp.elements[0].color=(0,0,0,1)
    ramp.color_ramp.elements[1].color=(1.,.18,.005,1.)
    nt.links.new(attr.outputs['Fac'],ramp.inputs['Fac']);nt.links.new(ramp.outputs['Color'],bs.inputs['Emission Color'])
    bs.inputs['Emission Strength'].default_value=2.
    return mat
