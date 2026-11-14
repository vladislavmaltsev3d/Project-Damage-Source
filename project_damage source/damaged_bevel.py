# параметры повреждённой фаски, рисование и заполнение для физики
from . import quality_ui
# SPDX-License-Identifier: GPL-3.0-or-later
"""Damaged Edge Bevel: worn concrete edges in one click.

Every sharp convex edge (or the edges drawn along with grease pencil) gets a
slightly rounded chamfer, and spaced stretches of each edge are broken, with
spall chips around them. Core: damaged_bevel.cpp (ProjectDamageParams
.damaged_bevel, build_worn(damaged_bevel_strokes=...)).

Controls:
  Smoothness     how soft the chamfer and the breakage are
  Damage         how deep and wide the breakage is
  Damage Amount  how chewed a damaged stretch is (chunks and chips per length)
  Damage Factor  share of every edge's length that is damaged, as spaced
                 stretches (1 = the whole edge, 0.5 = half, 0 = none)
  Placement Random  0 = stretches evenly spaced and alike, 1 = clustered,
                 with very uneven lengths, gaps and depths (the share stays)
  Smooth Damage Surface  rounds the broken surfaces
  Profile Shape  like the Bevel modifier's: 0 = the chamfer as set by
                 Smoothness, 1 = bulging out toward the old sharp edge
Presets: Damaged (the default worn edge), Light Wear, Heavy, Chipped,
Weathered, Clean (wide chamfer), Narrow (thin chamfer). Clean and Narrow set
Damage Factor to 0.

Pieces for Physics: with it on, every build also returns the broken-off
volume (build_worn "bevel_segments_points/_polys"); store_segments() keeps it
for the session, Fill Segments (fill_segments) turns it into one closed fill
object per damaged stretch, and the add-on's Fracture and Quick Physics
(damage_physics.py) break and drop them.

Hook points (see INTEGRATION.md): register_props() before ProjectDamageProps
is registered, TILE / CATEGORY into sidebar.py, PANEL into
panels._EFFECT_PANELS, fill_params() in params.fill_params, collect() in
new_effects.collect, hash_params() in build.fingerprints, write_attributes()
after the build, MATERIALS in material_controls.material_fields,
store_segments() after the build, and in damage_physics.py: 'dbevel' in
MODULES / LABELS / EFFECT_IDS / ENABLE_PROP and fill_damage() handing
'dbevel' to fill_segments().
"""
from . import ui_image_hints
import struct

import bpy

from . import live

KEY = 'dbevel'                                                                     
CORE_BLOCK = 'damaged_bevel'                                               
STROKES_INPUT = 'damaged_bevel_strokes'                       
DRAWING_PROP = 'bevel_drawing'                                                                        
PAINT_TARGET = 'BEVEL'
TILE = (KEY, 'Damaged Edge Bevel', 'dbevel_enable', 'MOD_BEVEL')
TILE_LABEL = 'Damaged Bevel'
CATEGORY = 'general'
MATERIALS = [('bevel_material', 'Chamfer'), ('wear_material', 'Broken Concrete')]
ATTRIBUTES = ('edge_wear', 'bevel_damage')                                     


                                                                          
FIELDS = (
    ('enable', 'bool', False, False, 'Enable', 'Worn concrete edges: soft chamfer with broken stretches', {}),
    ('seed', 'int', 0, False, 'Seed', 'Another layout of the damaged stretches, chunks and chips', {'min': 0}),
    ('avoid', 'bool', True, False, 'Respect Avoid', 'Skip edges that touch an Avoid face', {}),
    ('detail', 'float', 0.3, False, 'Resolution', 'Profile segments and stations along the edge (x Global Detail)', {'min': 0.05, 'max': 4.0}),
           
    ('angle', 'float', 40.0, False, 'Edge Angle', 'Only edges whose faces turn by more than this (degrees)', {'min': 1.0, 'max': 179.0}),
    ('drawn_only', 'bool', False, False, 'Drawn Edges Only', 'An empty drawing means no edges instead of every sharp edge', {}),
    ('paint_radius', 'float', 4.0, True, 'Paint Radius', 'How far an edge may be from a stroke to be picked (cm)', {'min': 0.01, 'soft_max': 30.0}),
    ('paint_align', 'float', 40.0, False, 'Paint Align', 'Max angle between a stroke and an edge it picks (degrees)', {'min': 0.0, 'max': 90.0}),
    ('paint_cover', 'float', 0.3, False, 'Paint Cover', 'Share of an edge the strokes must run along to pick it', {'min': 0.0, 'max': 1.0, 'subtype': 'FACTOR'}),
    ('max_turn', 'float', 35.0, False, 'Max Turn', 'Edges meeting at less than this angle form one run (degrees)', {'min': 0.0, 'max': 89.0}),
           
    ('width', 'float', 1.0, True, 'Bevel Width', 'Chamfer size along each face (cm). 0 = sharp edge, damage only', {'min': 0.0, 'soft_max': 6.0}),
    ('smoothness', 'float', 0.35, False, 'Smoothness', 'How soft the chamfer and the breakage are: 0 crisp chamfer, 1 fully rounded and worn', {'min': 0.0, 'max': 1.0, 'subtype': 'FACTOR'}),
    ('profile_shape', 'float', 0.0, False, 'Profile Shape', 'Shape of the chamfer, like the Bevel modifier Profile Shape: 0 = the chamfer cut in (straight to round by Smoothness), 1 = it bulges out toward the old sharp edge, values between blend', {'min': 0.0, 'max': 1.0, 'subtype': 'FACTOR'}),
    ('uneven', 'float', 0.15, False, 'Uneven', 'How much the chamfer width wanders along the edge. 0 = clean, machined', {'min': 0.0, 'max': 1.0, 'subtype': 'FACTOR'}),
    ('max_share', 'float', 0.45, False, 'Max Share', 'Chamfer and damage never take more than this share of the local thickness', {'min': 0.01, 'max': 0.99, 'subtype': 'FACTOR'}),
            
    ('damage', 'float', 0.5, False, 'Damage', 'How deep and wide the breakage is (x Damage Depth)', {'min': 0.0, 'max': 1.0, 'subtype': 'FACTOR'}),
    ('damage_amount', 'float', 0.5, False, 'Damage Amount', 'How chewed a damaged stretch is: more, shorter chunks and more chips', {'min': 0.0, 'max': 1.0, 'subtype': 'FACTOR'}),
    ('damage_factor', 'float', 0.5, False, 'Damage Factor', 'Share of every edge that is damaged, in spaced stretches: 1 = the whole edge, 0.5 = half of it, 0 = none', {'min': 0.0, 'max': 1.0, 'subtype': 'FACTOR'}),
    ('damage_depth', 'float', 4.0, True, 'Damage Depth', 'Breakage depth at Damage 1, from the old sharp edge (cm)', {'min': 0.0, 'soft_max': 15.0}),
    ('patch_length', 'float', 15.0, True, 'Stretch Length', 'Average length of one damaged stretch (cm). Shorter = more, smaller stretches', {'min': 0.1, 'soft_max': 100.0}),
    ('chip_scatter', 'float', 0.5, False, 'Loose Chips', 'Small chips just outside the damaged stretches', {'min': 0.0, 'max': 1.0, 'subtype': 'FACTOR'}),
    ('jagged', 'float', 0.6, False, 'Jagged', 'How torn the break surface and its border are (Smoothness lowers it)', {'min': 0.0, 'max': 1.0, 'subtype': 'FACTOR'}),
    ('placement_random', 'float', 0.75, False, 'Placement Random', 'Where the damaged stretches sit: 0 = evenly spaced and alike, 1 = clustered in groups with long clean runs between, very uneven lengths and depths. The damaged share stays Damage Factor', {'min': 0.0, 'max': 1.0, 'subtype': 'FACTOR'}),
    ('smooth_damage', 'bool', False, False, 'Smooth Damage Surface', 'Smooth the broken surfaces: fewer tears, soft steps between chunks, rounded chips', {}),
    ('smooth_damage_amount', 'float', 0.6, False, 'Smooth Amount', 'How much Smooth Damage Surface rounds the breakage', {'min': 0.0, 'max': 1.0, 'subtype': 'FACTOR'}),
             
    ('export_segments', 'bool', False, False, 'Pieces for Physics', 'Also build the broken-off pieces so Fill Segments can make fill objects for Fracture and Quick Physics (a second Boolean per build)', {}),
           
    ('mask_width', 'float', 1.5, True, 'Mask Width', 'Fade of the edge_wear / bevel_damage attributes onto the faces (cm)', {'min': 0.0, 'soft_max': 10.0}),
    ('mask_noise', 'float', 0.6, False, 'Mask Noise', 'Noisy, scuffed border of the masks', {'min': 0.0, 'max': 1.0, 'subtype': 'FACTOR'}),
    ('mask_scale', 'float', 3.0, True, 'Mask Noise Scale', 'Size of the mask border noise (cm)', {'min': 0.01, 'soft_max': 20.0}),
    ('up_x', 'float', 0.0, False, 'Up X', 'Object-space up: picks the top face of a run', {}),
    ('up_y', 'float', 0.0, False, 'Up Y', 'Object-space up: picks the top face of a run', {}),
    ('up_z', 'float', 1.0, False, 'Up Z', 'Object-space up: picks the top face of a run', {}),
)
FIELD_NAMES = tuple(f[0] for f in FIELDS)

                                                                              
PRESETS = {
    'DAMAGED': dict(width=1.0, smoothness=0.35, uneven=0.15, damage=0.5, damage_amount=0.5, damage_factor=0.5,
                    damage_depth=4.0, patch_length=15.0, chip_scatter=0.5, jagged=0.6,
                    placement_random=0.75, smooth_damage=False, smooth_damage_amount=0.6),
    'CLEAN': dict(width=3.0, smoothness=0.08, uneven=0.0, damage_factor=0.0),
    'NARROW': dict(width=0.5, smoothness=0.12, uneven=0.0, damage_factor=0.0),
    'LIGHT': dict(width=0.8, smoothness=0.4, uneven=0.12, damage=0.3, damage_amount=0.35, damage_factor=0.25,
                  damage_depth=4.0, patch_length=8.0, chip_scatter=0.35, jagged=0.5,
                  placement_random=0.85, smooth_damage=False, smooth_damage_amount=0.6),
    'HEAVY': dict(width=1.2, smoothness=0.3, uneven=0.2, damage=0.85, damage_amount=0.75, damage_factor=0.75,
                  damage_depth=5.0, patch_length=25.0, chip_scatter=0.8, jagged=0.75,
                  placement_random=0.7, smooth_damage=False, smooth_damage_amount=0.6),
    'CHIPPED': dict(width=0.6, smoothness=0.2, uneven=0.1, damage=0.3, damage_amount=0.9, damage_factor=0.55,
                    damage_depth=4.0, patch_length=5.0, chip_scatter=1.0, jagged=0.8,
                    placement_random=0.9, smooth_damage=False, smooth_damage_amount=0.6),
    'WEATHERED': dict(width=1.5, smoothness=0.75, uneven=0.25, damage=0.6, damage_amount=0.4, damage_factor=0.55,
                      damage_depth=4.0, patch_length=18.0, chip_scatter=0.3, jagged=0.3,
                      placement_random=0.8, smooth_damage=True, smooth_damage_amount=0.7),
}
                                                                  
                                                                          
PRESET_ITEMS = (
    ('DAMAGED', 'Damaged', 'Worn concrete: small soft chamfer, half of each edge broken in scattered stretches', 'NONE', 0),
    ('LIGHT', 'Light Wear', 'A quarter of each edge nicked: short, shallow stretches far apart', 'NONE', 4),
    ('HEAVY', 'Heavy', 'Deep chunks broken out of three quarters of each edge', 'NONE', 5),
    ('CHIPPED', 'Chipped', 'Many small chips and nicks along a thin chamfer', 'NONE', 6),
    ('WEATHERED', 'Weathered', 'Soft, rounded erosion on a wide worn chamfer (Smooth Damage Surface on)', 'NONE', 7),
    ('CLEAN', 'Clean', 'Wide, crisp chamfer with no damage', 'NONE', 1),
    ('NARROW', 'Narrow', 'Thin chamfer with no damage', 'NONE', 2),
    ('CUSTOM', 'Custom', 'Edited after picking a preset', 'NONE', 3),
)
                                                                              
CORE_PRESET = {'DAMAGED': 1, 'CLEAN': 2, 'NARROW': 3, 'LIGHT': 4, 'HEAVY': 5, 'CHIPPED': 6, 'WEATHERED': 7}
_SHAPE_FIELDS = set().union(*PRESETS.values())
_LOADING = False


def _prop(name):
    return KEY + '_' + name


def _apply_preset(self, context):
    global _LOADING
    if _LOADING or self.dbevel_preset == 'CUSTOM':
        live.touch_prop(self, context)
        return
    _LOADING = True
    try:
        for name, value in PRESETS[self.dbevel_preset].items():
            setattr(self, _prop(name), value)
    finally:
        _LOADING = False
    live.touch_prop(self, context)


def _shape_touch(self, context):
                                                                               
    global _LOADING
    if not _LOADING and self.dbevel_preset != 'CUSTOM':
        _LOADING = True
        try:
            self.dbevel_preset = 'CUSTOM'
        finally:
            _LOADING = False
    if not _LOADING:
        live.touch_prop(self, context)


def _make_prop(name, kind, default, is_length, label, description, extra):
    update = _shape_touch if name in _SHAPE_FIELDS else live.touch_prop
    text = label + (' (cm)' if is_length else '')
    if kind == 'bool':
        return bpy.props.BoolProperty(name=text, default=default, description=description, update=update)
    if kind == 'int':
        return bpy.props.IntProperty(name=text, default=default, description=description, update=update, **extra)
    return bpy.props.FloatProperty(name=text, default=default, description=description, update=update, **extra)


def register_props(cls):
    """Add the dbevel_* properties to ProjectDamageProps (before it is registered)."""
    ann = cls.__dict__.get('__annotations__')
    if ann is None:
        ann = {}
        cls.__annotations__ = ann
    for name, kind, default, is_length, label, description, extra in FIELDS:
        ann[_prop(name)] = _make_prop(name, kind, default, is_length, label, description, extra)
    ann['dbevel_preset'] = bpy.props.EnumProperty(
        name='Preset', items=PRESET_ITEMS, default='DAMAGED', update=_apply_preset,
        description='One click looks from the references. Editing a shape setting switches to Custom')


def core_supported(core):
    try:
        return hasattr(core.Params(), CORE_BLOCK)
    except Exception:
        return False


def fill_params(p, props, gscale, gdetail, all_off):
    """Copy the props into params.damaged_bevel. Lengths follow Global Scale,
    Resolution follows Global Detail, like every other asset."""
    if not hasattr(p, CORE_BLOCK):
        return
    block = getattr(p, CORE_BLOCK)
    for name, kind, default, is_length, *_ in FIELDS:
        if not hasattr(block, name):
            continue                                                                     
        value = getattr(props, _prop(name))
        if name == 'enable':
            value = bool(value and not all_off)
        elif is_length:
            value *= gscale
        elif name == 'detail':
            value *= gdetail
        setattr(block, name, value)
    block.drawn_only = getattr(props, DRAWING_PROP) is not None


def collect(context, source, factor, inputs, reports):
    """Strokes for build_worn(damaged_bevel_strokes=...). Called from
    new_effects.collect; an empty list means every sharp edge."""
    p = source.project_damage
    if p.all_off or not p.dbevel_enable:
        return
    from . import paint
    import numpy as np
    paths = [np.ascontiguousarray(path * factor, dtype=np.float64)
             for path in paint.read_paths(context, source, getattr(p, DRAWING_PROP), min_points=2)]
    inputs[STROKES_INPUT] = paths
    if not paths and getattr(p,DRAWING_PROP) is not None:
        reports.append(({'WARNING'}, 'Damaged Edge Bevel: the assigned drawing has no strokes at this frame'))


def hash_params(h, params):
    """Hash params.damaged_bevel into the damage fingerprint (the nested block
    is not in build._DAMAGE_FIELDS). Strokes are hashed by new_effects.hash_inputs."""
    block = getattr(params, CORE_BLOCK, None)
    h.update(b'damaged_bevel')
    if block is None:
        h.update(b'\0')
        return
    for name in FIELD_NAMES:
        value = getattr(block, name, 0.0)
        h.update(struct.pack('<d', float(value)))


def write_attributes(mesh, out):
    """bevel_damage (and edge_wear when build.py does not already write it) as
    point float attributes project_damage_<name>."""
    for key in ATTRIBUTES:
        values = out.get(key, ())
        if len(values) and len(values) == len(mesh.vertices):
            name = 'project_damage_' + key
            attr = mesh.attributes.get(name) or mesh.attributes.new(name, 'FLOAT', 'POINT')
            attr.data.foreach_set('value', values)


                                                                             
                                                                               

                                                                              
                                                                                
_SEGMENTS = {}


def store_segments(source, out, factor):
    """After the build (build.py): keep the broken-off volume for Fill Segments."""
    uid = source.get('project_damage_uid')
    if uid is None:
        return
    pts = out.get('bevel_segments_points')
    tris = out.get('bevel_segments_polys')
    if pts is None or tris is None:
        _SEGMENTS.pop(uid, None)
        return
    import numpy as np
    _SEGMENTS[uid] = (np.asarray(pts, dtype=np.float64).reshape(-1, 3) / factor,
                      np.asarray(tris, dtype=np.int32).reshape(-1, 3))


def physics_ready():
    """damage_physics.py is installed and knows 'dbevel' (apply_damaged_bevel.py patched it)."""
    try:
        from . import damage_physics
    except ImportError:
        return None
    return damage_physics if KEY in getattr(damage_physics, 'MODULES', ()) else None


def _fills_of(dp, source):
    try:
        return dp.fills_of(source, KEY)
    except TypeError:                                                             
        return [o for o in dp.fills_of(source) if o.get(dp.FILL_KIND) == KEY]


def fill_segments(context, source):
    """One closed object per damaged stretch that fills its break exactly and
    looks like the chamfered edge: the source intersected with the broken-off
    volume of the last build. Front faces keep the source's materials and UVs,
    the faces against the break get the inner material. damage_physics
    .fill_damage hands 'dbevel' here. Returns (objects, reports)."""
    import bmesh
    import numpy as np
    from . import damage_physics as dp, ownership
    p = source.project_damage
    if not p.dbevel_enable:
        return [], [({'ERROR'}, 'Fill Segments: enable Damaged Edge Bevel on this object first')]
    if p.all_off:
        return [], [({'ERROR'}, 'Fill Segments: the damage is switched off (All Off)')]
    if not p.dbevel_export_segments:
        return [], [({'ERROR'}, 'Fill Segments: turn on Pieces for Physics, then press Update')]
    data = _SEGMENTS.get(source.get('project_damage_uid'))
    if data is None:
        return [], [({'ERROR'}, 'Fill Segments: press Update to build the pieces first')]
    pts, tris = data
    if not len(tris):
        return [], [({'WARNING'}, 'Fill Segments: no broken stretches to fill (Damage or Damage Factor is 0)')]
    reports = []
    for obj in _fills_of(dp, source):
        dp._remove(obj)
    coll = dp._collection(context, source, dp.FILL_COLLECTION, '_DamageFill')
    seg = bpy.data.meshes.new('_pd_dbevel_segments')
    seg.from_pydata(pts.tolist(), [], tris.tolist())
    seg.update()
    temp = dp._temp_object(context, '_pd_dbevel_segments', seg, source)
    try:
        base = source.data.copy()
        base.materials.clear()
        for slot in source.material_slots:
            base.materials.append(slot.material)
        if not len(base.materials):
            base.materials.append(None)
        base.materials.append(dp._inner_material(source))
        fill, solver = dp._boolean(context, base, source, [temp], operation='INTERSECT')
    finally:
        dp._remove(temp)
    from .params import unit_factor
    factor = unit_factor(context)
    g = dp._gscale(p)
    min_vol = max(1e-9, getattr(p, 'debris_min_volume', 0.05) * g ** 3) / factor ** 3
    bm = bmesh.new()
    bm.from_mesh(fill)
    if solver != 'MANIFOLD':
        dp._split_pinches(bm)
    out, dropped = [], 0
    for part in sorted(dp._islands(bm), key=len, reverse=True):
        pb = dp._bm_part(bm, part)
        if abs(pb.calc_volume()) < min_vol or not dp._closed(pb):
            dropped += 1
            pb.free()
            continue
        mesh = bpy.data.meshes.new('segment')
        pb.to_mesh(mesh)
        pb.free()
        for m in fill.materials:
            mesh.materials.append(m)
        obj, _ = dp._make_object('%s_Segment_%03d' % (source.name, len(out) + 1), mesh, coll, source)
        mesh.name = obj.name
        obj[dp.FILL_STAMP] = source.get('project_damage_uid')
        ownership.bind(source, obj)                                               
        obj[dp.FILL_KIND] = KEY
        out.append(obj)
    bm.free()
    bpy.data.meshes.remove(fill)
    if not out:
        return [], reports + [({'WARNING'}, 'Fill Segments: the broken pieces are smaller than Damage Debris Min Volume')]
    reports.append(({'INFO'}, 'Fill Segments: %d fill objects in %s%s' % (
        len(out), coll.name, (' (%d dust parts left out)' % dropped) if dropped else '')))
    return out, reports


def draw_physics(layout, context, source, on):
    p = source.project_damage
    box = layout.box()
    box.enabled = on
    from . import effect_layout
    effect_layout.heading(box, 'Pieces for Physics')
    row = box.row(align=True)
    row.scale_y = 1.3
    row.prop(p, 'dbevel_export_segments', text='Pieces for Physics', toggle=True, icon='MOD_EXPLODE')
    if not p.dbevel_export_segments:
        return
    dp = physics_ready()
    if dp is None or not hasattr(source, 'project_damage_physics'):
        box.label(text='Needs Pieces for Physics (damage_physics.py)', icon='ERROR')
        box.label(text='Run apply_damaged_bevel.py after installing it')
        return
    s = dp.settings(source)
    if source.get('project_damage_uid') not in _SEGMENTS:
        box.label(text='Press Update to build the pieces', icon='INFO')
    row = box.row(align=True)
    row.scale_y = 1.5
    row.operator('project_damage.physics_fill', text='Fill Segments', icon='MOD_SOLIDIFY').module = KEY
    col = box.column(align=True)
    col.prop(s, 'material', text='Fracture Type')
    if s.material == 'WOOD':
        col.row(align=True).prop(s, 'grain', expand=True)
    col.prop(s, 'piece_size')
    col.prop(s, 'seed')
    col.prop(s, 'hide_fill')
    if hasattr(s, 'cut_mode'):
        col = box.column(align=True)
        row = col.row(align=True)
        row.scale_y = 1.3
        row.prop(s, 'cut_mode', expand=True)
        if s.cut_mode != 'AUTO':
            col.prop(s, 'cut_drawing', text='Lines')
            col.prop(s, 'cut_gap')
            row = box.row(align=True)
            drawing = context.mode != 'OBJECT' and context.object is not None and context.object == s.cut_drawing
            if drawing:
                row.operator('project_damage.physics_lines_done', icon='CHECKMARK')
            else:
                row.operator('project_damage.physics_lines_new', icon='GREASEPENCIL',
                             text='Draw Lines' if s.cut_drawing is None else 'Draw More Lines')
    row = box.row(align=True)
    row.scale_y = 1.5
    row.operator('project_damage.physics_fracture', icon='MOD_EXPLODE').module = KEY
    count = len(_fills_of(dp, source))
    box.label(text=('%d fill objects' % count) if count else 'One closed piece per broken stretch', icon='INFO')
    if hasattr(dp, 'draw_quick_physics'):
        dp.draw_quick_physics(box, context)


                                                                             
       
def _panel_base():
    from . import panels
    return panels._ProjectDamagePanel


# собираем видимые настройки этого раздела
def draw(layout, context):
    from . import ownership, effect_layout, material_controls
    source = ownership.context_source(context)
    p = source.project_damage
    layout.use_property_split = False
    layout.use_property_decorate = False
    on = p.dbevel_enable and not p.all_off

    box = layout.box()
    box.enabled = on
    effect_layout.heading(box, 'Look')
    material_controls.inline(box,p,'bevel_material',text='Chamfer')
    grid = box.grid_flow(row_major=True, columns=4, even_columns=True, align=True)
    grid.scale_y = 1.3
    for ident, *_ in PRESET_ITEMS:
        grid.prop_enum(p, 'dbevel_preset', ident)
    col = box.column(align=True)
    col.prop(p, 'dbevel_width', text='Bevel Width')
    ui_image_hints.prop(col,p, 'dbevel_smoothness', text='Smoothness', slider=True)
    if hasattr(p, 'dbevel_profile_shape'):
        col.prop(p, 'dbevel_profile_shape', text='Profile Shape', slider=True)

    box = layout.box()
    box.enabled = on
    effect_layout.heading(box, 'Damage')
    material_controls.inline(box,p,'wear_material',text='Broken Concrete')
    col = box.column(align=True)
    col.prop(p, 'dbevel_damage', text='Damage', slider=True)
    col.prop(p, 'dbevel_damage_amount', text='Damage Amount', slider=True)
    col.prop(p, 'dbevel_damage_factor', text='Damage Factor', slider=True)
    col.prop(p, 'dbevel_placement_random', text='Placement Random', slider=True)
    col = box.column(align=True)
    col.prop(p, 'dbevel_smooth_damage', text='Smooth Damage Surface')
    sub = col.column(align=True)
    sub.active = p.dbevel_smooth_damage
    sub.prop(p, 'dbevel_smooth_damage_amount', text='Smooth Amount', slider=True)
    col = box.column(align=True)
    col.prop(p, 'dbevel_damage_depth', text='Damage Depth')
    col.prop(p, 'dbevel_patch_length', text='Stretch Length')
    col.prop(p, 'dbevel_chip_scatter', text='Loose Chips', slider=True)
    col.prop(p, 'dbevel_jagged', text='Jagged', slider=True)
    col.prop(p, 'dbevel_uneven', text='Uneven Chamfer', slider=True)

    box = layout.box()
    box.enabled = on
    effect_layout.heading(box, 'Edges')
    col = box.column(align=True)
    col.prop(p, 'dbevel_angle', text='Edge Angle')
    from .paint import drawing_row
    drawing_row(box, p, PAINT_TARGET)
    col = box.column(align=True)
    col.label(text='Draw a contour or stroke over the edges to wear.', icon='INFO')
    if getattr(p, DRAWING_PROP) is not None:
        col.prop(p, 'dbevel_paint_radius', text='Paint Radius (cm)')
    col.prop(p, 'dbevel_max_turn', text='Max Turn')

    box = layout.box()
    box.enabled = on
    effect_layout.heading(box, 'Advanced')
    col = box.column(align=True)
    col.prop(p, 'dbevel_seed', text='Seed')
    pass
    quality_ui.draw(col, p, 'dbevel_detail', text='Resolution')
    col.prop(p, 'dbevel_max_share', text='Max Share', slider=True)
    col.prop(p, 'dbevel_mask_width', text='Mask Width (cm)')
    col.prop(p, 'dbevel_mask_noise', text='Mask Noise', slider=True)
    col.prop(p, 'dbevel_mask_scale', text='Mask Noise Scale (cm)')
    row = col.row(align=True)
    row.prop(p, 'dbevel_up_x', text='Up X')
    row.prop(p, 'dbevel_up_y', text='Y')
    row.prop(p, 'dbevel_up_z', text='Z')

                                                  


def make_panel():
    """The effect panel class. Append it to panels._EFFECT_PANELS (the sidebar
    finds an effect's settings there by effect_module) before panels registers
    its _CLASSES."""
    base = _panel_base()

    class OBJECT_PT_project_damage_dbevel(base):
        bl_idname = 'OBJECT_PT_project_damage_dbevel'
        bl_parent_id = 'OBJECT_PT_project_damage'
        bl_label = 'Damaged Edge Bevel'
        effect_module = KEY

        def draw(self, context):
            draw(self.layout, context)

    return OBJECT_PT_project_damage_dbevel
