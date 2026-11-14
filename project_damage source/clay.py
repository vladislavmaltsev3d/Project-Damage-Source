# передаём форму, рисунок и материал в эффект глины
from . import quality_ui
# SPDX-License-Identifier: GPL-3.0-or-later
# Modified work: new effect added to ProjectDamage (Cornelius Dammrich, GPL-3).
"""Clay: turns the object into hand-modelled clay (core: clay.cpp).

Displaced into the base mesh itself, like Plaster and Concrete Pitting: no
cutter, no separate object, every face stays a base face (UVs and materials
come through). Softened edges, lumps, thumb presses with rims and fingerprint
ridges, finger smears, grain, drying cracks and an optional sag. All sizes are
relative to the object (or a reference object), so the look is size independent.

build.rebuild_object calls before_core(); fingerprints() calls hash_params()
and hash_inputs(); after effect_groups.tag_mesh it calls after_result()
(optional Clay Material on the base faces)."""
import bpy
import numpy as np

from . import live


def _hint(general, low, high):
    """'General \\n Lowest \\n Highest' - two line breaks, so ui_hints keeps it verbatim."""
    return general + '\n' + low + '\n' + high


P = bpy.props
PROPS = {
    'clay_enable': P.BoolProperty(
        name='Clay', default=False, update=live.touch_prop,
        description=_hint('Turns the object into hand-modelled clay: soft edges, lumps, thumb presses and smears',
                          'Off: the object keeps its shape',
                          'On: the whole object is reshaped as clay')),
    'clay_seed': P.IntProperty(
        name='Seed', default=0, min=0, max=100000, update=live.touch_prop,
        description=_hint('Picks a different layout of lumps, presses and smears',
                          'Lowest: the first layout',
                          'Highest: any other value gives a new, repeatable layout')),
    'clay_softness': P.FloatProperty(
        name='Softness', default=60.0, min=0.0, max=100.0, subtype='PERCENTAGE', update=live.touch_prop,
        description=_hint('Rounds hard edges and corners into soft, pillowy clay',
                          'Lowest: edges stay crisp, as if freshly cut',
                          'Highest: every edge is fully rounded, like a soft hand-rolled block')),
    'clay_lumps': P.FloatProperty(
        name='Lumps', default=35.0, min=0.0, max=100.0, subtype='PERCENTAGE', update=live.touch_prop,
        description=_hint('Uneven, hand-formed bumps over the whole surface',
                          'Lowest: smooth, even surfaces',
                          'Highest: wobbly, clearly hand-shaped surfaces')),
    'clay_lump_scale': P.FloatProperty(
        name='Lump Scale', default=1.0, min=0.05, soft_max=5.0, max=50.0, update=live.touch_prop,
        description=_hint('Size of the lumps',
                          'Lowest: a few wide, gentle swells',
                          'Highest: many small, tight bumps')),
    'clay_presses': P.FloatProperty(
        name='Presses', default=30.0, min=0.0, max=100.0, subtype='PERCENTAGE', update=live.touch_prop,
        description=_hint('Thumb and finger presses scattered over the surface',
                          'Lowest: no presses',
                          'Highest: the surface is covered in overlapping presses')),
    'clay_press_size': P.FloatProperty(
        name='Press Size', default=10.0, min=0.5, max=50.0, subtype='PERCENTAGE', update=live.touch_prop,
        description=_hint('Size of a press, as a share of the object size',
                          'Lowest: small fingertip dabs (a giant\'s object)',
                          'Highest: broad palm-sized presses (a tiny figurine)')),
    'clay_press_depth': P.FloatProperty(
        name='Press Depth', default=35.0, min=0.0, max=100.0, subtype='PERCENTAGE', update=live.touch_prop,
        description=_hint('How deep presses and smears go, relative to their size',
                          'Lowest: faint touches that barely mark the surface',
                          'Highest: deep, firm thumb prints')),
    'clay_press_rim': P.FloatProperty(
        name='Press Rim', default=50.0, min=0.0, max=100.0, subtype='PERCENTAGE', update=live.touch_prop,
        description=_hint('Clay pushed up around the edge of each press',
                          'Lowest: clean dents with no raised edge',
                          'Highest: a pronounced ridge of squeezed-out clay')),
    'clay_fingerprints': P.FloatProperty(
        name='Fingerprints', default=50.0, min=0.0, max=100.0, subtype='PERCENTAGE', update=live.touch_prop,
        description=_hint('Fine fingerprint ridges inside the presses (needs a high Resolution)',
                          'Lowest: smooth presses',
                          'Highest: clearly visible swirling ridges')),
    'clay_smears': P.FloatProperty(
        name='Smears', default=20.0, min=0.0, max=100.0, subtype='PERCENTAGE', update=live.touch_prop,
        description=_hint('Dragged finger or tool strokes, with clay piled up at the end',
                          'Lowest: no smears',
                          'Highest: the surface is worked over with many strokes')),
    'clay_smear_length': P.FloatProperty(
        name='Smear Length', default=3.0, min=1.0, max=8.0, update=live.touch_prop,
        description=_hint('Length of a smear compared to its width',
                          'Lowest: short, round dabs',
                          'Highest: long, dragged strokes')),
    'clay_grain': P.FloatProperty(
        name='Grain', default=20.0, min=0.0, max=100.0, subtype='PERCENTAGE', update=live.touch_prop,
        description=_hint('Fine sandy grain of the clay body (needs a high Resolution)',
                          'Lowest: perfectly smooth, polished clay',
                          'Highest: coarse, gritty clay')),
    'clay_grain_scale': P.FloatProperty(
        name='Grain Scale', default=1.0, min=0.1, max=10.0, update=live.touch_prop,
        description=_hint('Size of the grain',
                          'Lowest: soft, blotchy texture',
                          'Highest: very fine grit (hidden if the mesh is too coarse)')),
    'clay_cracks': P.FloatProperty(
        name='Dry Cracks', default=0.0, min=0.0, max=100.0, subtype='PERCENTAGE', update=live.touch_prop,
        description=_hint('Drying cracks in patches of the surface',
                          'Lowest: fresh, wet clay without cracks',
                          'Highest: dried-out clay cracked almost everywhere')),
    'clay_crack_width': P.FloatProperty(
        name='Crack Width', default=50.0, min=0.0, max=100.0, subtype='PERCENTAGE', update=live.touch_prop,
        description=_hint('Width of the drying cracks',
                          'Lowest: hairline cracks',
                          'Highest: wide, open splits')),
    'clay_crack_depth': P.FloatProperty(
        name='Crack Depth', default=50.0, min=0.0, max=100.0, subtype='PERCENTAGE', update=live.touch_prop,
        description=_hint('Depth of the drying cracks',
                          'Lowest: shallow surface crazing',
                          'Highest: deep grooves')),
    'clay_sag': P.FloatProperty(
        name='Sag', default=0.0, min=0.0, max=100.0, subtype='PERCENTAGE', update=live.touch_prop,
        description=_hint('Each piece slumps under its own weight, downward in the world',
                          'Lowest: stands firm',
                          'Highest: soft clay that settles and squashes down')),
    'clay_bulge': P.FloatProperty(
        name='Bulge', default=50.0, min=0.0, max=100.0, subtype='PERCENTAGE', update=live.touch_prop,
        description=_hint('How much the base spreads outward when it sags',
                          'Lowest: drops straight down',
                          'Highest: the base swells out like a settling lump')),
    'clay_resolution': P.FloatProperty(
        name='Resolution', default=140.0, min=20.0, soft_max=400.0, max=2000.0, subtype='PERCENTAGE', update=live.touch_prop,
        description=_hint('Mesh detail: edges across the object size',
                          'Lowest: light, fast mesh; fine grain and fingerprints disappear',
                          'Highest: dense mesh with every fingerprint ridge, slower')),
    'clay_max_tris': P.IntProperty(
        name='Max Triangles', default=600000, min=5000, soft_max=4000000, update=live.touch_prop,
        description=_hint('Triangle budget; Resolution is lowered to fit it',
                          'Lowest: always light, big objects lose fine detail',
                          'Highest: full detail on big objects, more memory and time')),
    'clay_thickness_cap': P.FloatProperty(
        name='Thickness Cap', default=0.45, min=0.0, max=1.0, update=live.touch_prop,
        description=_hint('Limits how deep presses and cracks go, as a share of the local wall thickness',
                          'Lowest (0): off, deep marks may break through thin parts',
                          'Highest: marks stay shallow on thin parts')),
    'clay_relative_scale': P.BoolProperty(
        name='Relative Scale', default=True, update=live.touch_prop,
        description=_hint('Ties every size of the effect to the object size',
                          'Off: sizes follow Clay Scale (fixed cm), so objects of any size get equal-size presses',
                          'On: the same look on any object size')),
    'clay_ref_object': P.PointerProperty(
        name='Reference Object', type=bpy.types.Object, update=live.touch_prop,
        description=_hint('Uses another object\'s size as the scale (Relative Scale)',
                          'Empty: this object\'s own size',
                          'Set: several objects share one clay scale taken from this object')),
    'clay_scale_size': P.FloatProperty(
        name='Clay Scale', default=200.0, min=1.0, soft_max=1000.0, update=live.touch_prop,
        description=_hint('Fixed reference size when Relative Scale is off, in cm (follows Master Scale)',
                          'Lowest: small presses and fine detail',
                          'Highest: large presses and broad lumps')),
    'clay_avoid': P.BoolProperty(
        name='Avoid Selection', default=True, update=live.touch_prop,
        description=_hint('Keeps the Avoid faces untouched',
                          'Off: Avoid faces turn to clay like the rest',
                          'On: Avoid faces keep their original shape')),
    'clay_drawing': P.PointerProperty(name='Drawing', type=bpy.types.Object, poll=lambda self,obj:obj.type in {'CURVE','GREASEPENCIL','GPENCIL'}, update=live.touch_prop),
    'clay_paint_enable': P.BoolProperty(name='Draw Clay Area', default=False, update=live.touch_prop),
    'clay_paint_radius': P.FloatProperty(name='Draw Radius (cm)', default=50.0, min=.01, soft_max=100., update=live.touch_prop, description='Distance around each stroke affected by Clay. Small values make narrow marks; large values deform a wider area with a soft boundary'),
    'clay_group': P.StringProperty(
        name='Vertex Group', default='', update=live.touch_prop,
        description=_hint('Vertex group painting where the clay effect applies',
                          'Weight 0: untouched',
                          'Weight 1: full clay')),
    'clay_material': P.PointerProperty(
        name='Clay Material', type=bpy.types.Material, update=live.touch_prop,
        description=_hint('Optional material for the whole clay body',
                          'Empty: the object keeps its own materials',
                          'Set: every base face uses this material')),
}

FIELDS = ('seed', 'avoid', 'softness', 'lumps', 'lump_scale', 'presses', 'press_size', 'press_depth',
          'press_rim', 'fingerprints', 'smears', 'smear_length', 'grain', 'grain_scale', 'cracks',
          'crack_width', 'crack_depth', 'sag', 'bulge', 'resolution', 'max_tris', 'thickness_cap')
_HASHED = FIELDS + ('enable', 'ref_size', 'grav_x', 'grav_y', 'grav_z')


def wanted(p):
    return bool(getattr(p, 'clay_enable', False) and not p.all_off)


def _mean_scale(source):
    s = source.matrix_world.to_scale()
    return max(1e-9, (abs(s.x) + abs(s.y) + abs(s.z)) / 3.0)


def _ref_size(source, p, factor):
    if p.clay_relative_scale:
        ref = p.clay_ref_object
        return 0.0 if ref is None else max(ref.dimensions) / _mean_scale(source) * factor
    from .params import _clamp_scale
    return p.clay_scale_size * _clamp_scale(p.global_scale / 100.0) / _mean_scale(source)


def _gravity(source):
    """World down (-Z) in the source's object space."""
    from mathutils import Vector
    down = source.matrix_world.to_3x3().inverted_safe() @ Vector((0.0, 0.0, -1.0))
    return down.normalized() if down.length > 1e-12 else Vector((0.0, 0.0, -1.0))


# готовим данные этого эффекта перед расчётом геометрии
def before_core(context, source, params, snap, prep, points, polys, factor, reports):
    p = source.project_damage
    block = getattr(params, 'clay', None)
    if not wanted(p):
        if block is not None:
            block.enable = False
            block.set_zone_weights(np.zeros(0))
        return
    if block is None:
        reports.append(({'WARNING'}, 'Clay: rebuild the core and restart Blender to use it'))
        return
    for name in FIELDS:
        setattr(block, name, getattr(p, 'clay_' + name))
    block.enable = True
    block.ref_size = float(_ref_size(source, p, factor))
    g = _gravity(source)
    block.grav_x, block.grav_y, block.grav_z = g.x, g.y, g.z
    weights = np.zeros(0)
    if p.clay_group:
        vg = source.vertex_groups.get(p.clay_group)
        if vg is None:
            reports.append(({'WARNING'}, "Clay: vertex group '%s' not found; clay everywhere" % p.clay_group))
        else:
            if snap is None:
                from .mesh_io import read_mask_weights
                w = read_mask_weights(source.data, vg.index)
            else:
                w = prep.point_values(snap.groups[vg.index])
            w = np.clip(np.asarray(w, dtype=np.float64), 0.0, 1.0)
            if len(w) == len(points):
                weights = np.ascontiguousarray(w)
            else:
                reports.append(({'WARNING'}, 'Clay: the vertex group does not match the prepared mesh; clay everywhere'))
    block.set_zone_weights(weights)
    block.paint_only = p.clay_paint_enable
    block.paint_radius = p.clay_paint_radius * max(.001, p.global_scale / 100.)
    from . import paint
    paths = paint.read_paths(context, source, p.clay_drawing, min_points=2) if p.clay_paint_enable else []
    block.paint_segments = [float(v) for path in paths for a,b in zip(path[:-1],path[1:]) for point in (a,b) for v in point*factor]



def hash_params(h, params):
    block = getattr(params, 'clay', None)
    if block is None or not block.enable:
        return
    h.update(b'clay-draw-v1')
    h.update(repr((block.paint_only, block.paint_radius, block.paint_segments)).encode())
    h.update(repr(tuple(getattr(block, name) for name in _HASHED)).encode())


def hash_inputs(h, source):
    p = source.project_damage
    if not wanted(p):
        return
    if p.clay_relative_scale and p.clay_ref_object is not None:
        h.update(repr(tuple(p.clay_ref_object.dimensions)).encode())
    if p.clay_group:
        vg = source.vertex_groups.get(p.clay_group)
        if vg is not None:
            from .mesh_io import read_mask_weights
            h.update(np.asarray(read_mask_weights(source.data, vg.index), dtype=np.float64).tobytes())


def after_result(context, source, mesh):
    """Clay Material on every base face (effect id 0). Damage faces keep theirs."""
    p = source.project_damage
    if not wanted(p) or p.clay_material is None or len(mesh.polygons) == 0:
        return
    mat = p.clay_material
    slot = next((i for i, m in enumerate(mesh.materials) if m == mat), None)
    if slot is None:
        mesh.materials.append(mat)
        slot = len(mesh.materials) - 1
    ids = np.zeros(len(mesh.polygons), dtype=np.int32)
    attr = mesh.attributes.get('project_damage_effect_id')
    if attr is not None and attr.domain == 'FACE':
        attr.data.foreach_get('value', ids)
    index = np.empty(len(mesh.polygons), dtype=np.int32)
    mesh.polygons.foreach_get('material_index', index)
    index[ids == 0] = slot
    mesh.polygons.foreach_set('material_index', index)
    mesh.update()


# показываем настройки выбранного эффекта
def draw_settings(layout, context):
    from . import ownership, effect_layout
    src = ownership.context_source(context)
    if src is None:
        return
    p = src.project_damage
    layout.use_property_split = False
    layout.use_property_decorate = False

    box = effect_layout.section(layout, 'Form')
    col = box.column(align=True)
    col.prop(p, 'clay_softness')
    col.prop(p, 'clay_lumps')
    col.prop(p, 'clay_lump_scale')
    col.prop(p, 'clay_seed')
    col = box.column(align=True)
    col.prop(p, 'clay_sag')
    if p.clay_sag > 0.0:
        col.prop(p, 'clay_bulge')

    box = effect_layout.section(layout, 'Hand Marks')
    col = box.column(align=True)
    col.prop(p, 'clay_presses')
    col.prop(p, 'clay_press_size')
    col.prop(p, 'clay_press_depth')
    col.prop(p, 'clay_press_rim')
    col.prop(p, 'clay_fingerprints')
    col = box.column(align=True)
    col.prop(p, 'clay_smears')
    col.prop(p, 'clay_smear_length')

    box = effect_layout.section(layout, 'Surface')
    col = box.column(align=True)
    col.prop(p, 'clay_grain')
    col.prop(p, 'clay_grain_scale')
    col = box.column(align=True)
    col.prop(p, 'clay_cracks')
    if p.clay_cracks > 0.0:
        col.prop(p, 'clay_crack_width')
        col.prop(p, 'clay_crack_depth')

    box = effect_layout.section(layout, 'Scale & Quality')
    col = box.column(align=True)
    quality_ui.draw(col, p, 'clay_resolution')
    col.prop(p, 'clay_max_tris')
    col.prop(p, 'clay_thickness_cap')
    col = box.column(align=True)
    col.prop(p, 'clay_relative_scale')
    if p.clay_relative_scale:
        col.prop(p, 'clay_ref_object')
    else:
        col.prop(p, 'clay_scale_size')

    box = effect_layout.section(layout, 'Clay Area & Material')
    box.prop(p, 'clay_paint_enable', toggle=True)
    if p.clay_paint_enable:
        from . import paint
        paint.drawing_row(box, p, 'CLAY')
        box.prop(p, 'clay_paint_radius')
    from . import material_controls
    material_controls.inline(box,p,'clay_material',text='Clay Material')


# подключаем свойства и действия этого модуля к Blender
def register():
    pass


# убираем подключения модуля при выключении аддона
def unregister():
    pass
