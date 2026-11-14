# металлический пробой с точками удара и направлением вылета
# ProjectDamage Torn Metal generator - GPL-3.0-or-later; see LICENSE.
#
# Bent and torn sheet metal built from scratch by the native core
# (metal_tear.cpp, build_metal_tear): blast holes with petals curled away from
# the blast, shrapnel punctures, torn slits, a ragged torn border with bites,
# dents, corrugation and a whole-sheet bend. Blast Points are child Empties:
# move one to move its hole, scale it to change its size.
# The Petals setting controls the curl direction at every hole.
#
# On an Object: with a mesh selected, Add makes "<name> Torn": the same damage
# cut into that object's own mesh (build_metal_tear_mesh). The object is
# hidden and followed (edit it and the result updates); its UVs and materials
# are kept, the inner skin and the torn edges get two more materials.
import bpy
import numpy as np
from bpy.props import BoolProperty, EnumProperty, FloatProperty, IntProperty, PointerProperty, StringProperty

from . import damage_gen_common as G

KEY = 'METAL'
PROP = 'project_damage_metal_gen'
_blast_inputs = {}


def _u(self, context):
    G.settings_changed(self, context)


def _dist(name, default, description, soft_max=1.0, minimum=0.0, step=1, precision=3):
    return FloatProperty(name=name, default=default, description=description, min=minimum, soft_max=soft_max,
                         subtype='DISTANCE', unit='LENGTH', step=step, precision=precision, update=_u)


def _fac(name, default, description, maximum=1.0, minimum=0.0, soft_max=None):
    return FloatProperty(name=name, default=default, description=description, min=minimum, max=maximum,
                         soft_max=soft_max if soft_max is not None else maximum, subtype='FACTOR', update=_u)


def _int(name, default, description, minimum=0, soft_max=20, maximum=10000):
    return IntProperty(name=name, default=default, description=description, min=minimum, max=maximum, soft_max=soft_max, update=_u)


# здесь хранятся значения, которые сохраняются вместе с объектом
class ProjectDamageMetalGenSettings(bpy.types.PropertyGroup):
    live_update: BoolProperty(name='Live Update', default=True, description='Rebuild when a setting or a Blast Point changes')
    report: StringProperty(name='Report', default='')
    mode: EnumProperty(name='Mode', default='SHEET', update=_u,
                       items=(('SHEET', 'New Sheet', 'A torn sheet built from scratch', 'MESH_PLANE', 0),
                              ('OBJECT', 'On an Object', 'The damage cut into another object\'s mesh (it is hidden and followed)', 'OBJECT_DATA', 1)))
    source: PointerProperty(name='Object', type=bpy.types.Object, poll=G.source_poll, update=_u,
                            description='The object to damage (its scale is applied, its placement is not)')
    hide_source: BoolProperty(name='Hide Object', default=True, update=_u, description='Hide the original in the viewport and in renders')
    seed: _int('Seed', 0, 'Random seed', soft_max=1000, maximum=2 ** 31 - 1)
    # sheet
    width: _dist('Width', 1.2, 'Sheet size along X', soft_max=5.0, minimum=0.001)
    height: _dist('Height', 0.8, 'Sheet size along Y', soft_max=5.0, minimum=0.001)
    thickness: _dist('Thickness', 0.002, 'Sheet thickness (0: a single surface)', soft_max=0.02, precision=4)
    resolution_quality: G.resolution_property(0.006)
    resolution: _dist('Resolution', 0.006, 'Edge length of the mesh. Smaller gives finer torn edges and petals',
                      soft_max=0.05, minimum=0.0005, precision=4)
    max_verts: _int('Max Vertices', 600000, 'Upper limit: the resolution is coarsened to stay below it',
                    minimum=1000, soft_max=2000000, maximum=20000000)
    corrugation: _dist('Corrugation', 0.0, 'Corrugated sheet: height of the waves (0: flat sheet)', soft_max=0.08, precision=4)
    corrugation_period: _dist('Wave Length', 0.076, 'Distance between the corrugation crests', soft_max=0.3, minimum=0.002)
    corrugation_box: BoolProperty(name='Box Profile', default=False, description='Trapezoid corrugation instead of a sine wave', update=_u)
    bend: FloatProperty(name='Bend', default=0.0, description='Bends the whole sheet: curvature (1 / radius). Negative bends the other way',
                        soft_min=-5.0, soft_max=5.0, update=_u)
    bend_angle: FloatProperty(name='Bend Direction', default=0.0, description='Direction of the bend in the sheet',
                              subtype='ANGLE', update=_u)
    warp: _dist('Warp', 0.003, 'Low waviness of the whole sheet', soft_max=0.05, precision=4)
    warp_scale: _dist('Warp Size', 0.6, 'Size of the waves of Warp', soft_max=3.0, minimum=0.01)
    # blasts
    blasts: _int('Blast Holes', 1, 'Random blast holes (Blast Points add more)', soft_max=6, maximum=64)
    blast_size_min: _dist('Size Min', 0.12, 'Petal length: radius of the torn star', soft_max=1.0, minimum=0.002)
    blast_size_max: _dist('Size Max', 0.2, 'Petal length: radius of the torn star', soft_max=1.0, minimum=0.002)
    blast_side: EnumProperty(name='Petals', default='FRONT', update=_u,
                             items=(('FRONT', 'Out of the Front', 'Curl petals along the surface normal for automatic holes and Blast Points'),
                                    ('BACK', 'Out of the Back', 'Curl petals against the surface normal for automatic holes and Blast Points')))
    petals_min: _int('Petals Min', 5, 'Fewest petals per hole', minimum=3, soft_max=16, maximum=24)
    petals_max: _int('Petals Max', 8, 'Most petals per hole', minimum=3, soft_max=16, maximum=24)
    hole_size: _fac('Hole Size', 0.3, 'Blown-out centre, share of the petal length', maximum=0.9)
    petal_gap: _fac('Petal Gap', 0.35, 'How far the petals were torn apart at the hole')
    petal_curl: _fac('Curl', 0.6, 'How far the petals curl back: 0 flat, 0.5 standing up, 1 rolled back onto the sheet', maximum=1.5, soft_max=1.2)
    curl_random: _fac('Curl Random', 0.35, 'Each petal curls its own amount')
    petal_twist: _fac('Twist', 0.25, 'Petals twist about their length')
    petal_crumple: _fac('Petal Folds', 0.4, 'Folds along the petals and crumpled tips', maximum=2.0, soft_max=1.0)
    slit_random: _fac('Irregular', 0.35, 'Uneven petal lengths, angles and curved tears')
    jagged: _fac('Jagged', 0.5, 'Ragged torn edges', maximum=2.0, soft_max=1.0)
    bulge: _fac('Bulge', 0.18, 'The sheet around a hole bulges away from the blast (height, share of the petal length)', maximum=1.0, soft_max=0.5)
    bulge_radius: FloatProperty(name='Bulge Reach', default=2.2, min=1.05, soft_max=5.0, update=_u,
                                description='How far the bulge reaches, times the petal length')
    punctures: _int('Punctures', 0, 'Small shrapnel holes with 3-5 petals', soft_max=60, maximum=2048)
    puncture_size_min: _dist('Size Min', 0.008, 'Puncture petal length', soft_max=0.1, minimum=0.001)
    puncture_size_max: _dist('Size Max', 0.025, 'Puncture petal length', soft_max=0.1, minimum=0.001)
    # tears
    tears: _int('Torn Slits', 2, 'Long torn slits with bent lips', soft_max=10, maximum=256)
    tear_length_min: _dist('Length Min', 0.12, 'Slit length', soft_max=1.0, minimum=0.005)
    tear_length_max: _dist('Length Max', 0.35, 'Slit length', soft_max=1.0, minimum=0.005)
    tear_width: _dist('Opening', 0.02, 'Widest opening of a slit', soft_max=0.1)
    tear_open: _fac('Lip Bend', 0.6, 'How far the lips of a slit are bent out', maximum=3.0, soft_max=1.5)
    # dents
    dents: _int('Dents', 5, 'Random dents', soft_max=30, maximum=512)
    dent_size_min: _dist('Size Min', 0.05, 'Dent radius', soft_max=0.5, minimum=0.002)
    dent_size_max: _dist('Size Max', 0.18, 'Dent radius', soft_max=0.5, minimum=0.002)
    dent_depth: _fac('Depth', 0.18, 'Dent depth, share of its radius', maximum=1.0, soft_max=0.5)
    dent_sharpness: _fac('Sharpness', 0.5, '0 soft bowl, 1 creased cone')
    dent_creases: _fac('Creases', 0.5, 'Radial buckles in the dent walls', maximum=2.0, soft_max=1.0)
    dent_back: _fac('From Behind', 0.25, 'Share of dents pushed from the back')
    # border
    edge_tear: _dist('Torn Border', 0.02, 'Depth of the ragged torn border (0: clean cut sheet)', soft_max=0.1, precision=4)
    edge_tear_scale: _dist('Border Lobes', 0.06, 'Size of the lobes of the torn border', soft_max=0.3, minimum=0.001)
    edge_bites: _int('Bites', 2, 'Big torn-out notches in the border', soft_max=10, maximum=64)
    bite_size: _dist('Bite Size', 0.12, 'Radius of the torn-out notches', soft_max=0.5, minimum=0.002)
    edge_curl: _dist('Border Bend', 0.03, 'The torn border is bent out of plane (height)', soft_max=0.2, minimum=-1.0)
    edge_curl_width: _dist('Bend Width', 0.08, 'How far from the border the bend reaches', soft_max=0.5, minimum=0.001)
    # detail
    crumple: _fac('Crumple', 0.5, 'Buckles and crumple around the damage', maximum=3.0, soft_max=1.5)
    crumple_size: _dist('Crumple Size', 0.03, 'Size of the buckles', soft_max=0.2, minimum=0.001)
    min_piece: FloatProperty(name='Min Piece', default=0.0004, min=0.0, soft_max=0.01, precision=5, update=_u,
                             description='Loose bits smaller than this area are dropped')
    torn_band: _dist('Torn Mask Width', 0.02, 'Reach of the metal_torn attribute from torn edges', soft_max=0.2, minimum=0.0001)
    uv_scale: FloatProperty(name='UV Scale', default=1.0, min=0.0001, soft_max=10.0, update=_u,
                            description='UV units per metre of the flat sheet')


def _petal_side(settings):
    return 1 if settings.blast_side == 'FRONT' else -1


def _convert(d, s):
    d['bend_angle'] = s.bend_angle * 57.29577951308232
    d['blast_side'] = _petal_side(s)
    d['corrugation_shape'] = 1 if s.corrugation_box else 0


def blast_rows(obj, settings):
    on_object = settings.mode == 'OBJECT'
    dimensions = 3 if on_object else 2
    side = _petal_side(settings)
    rows = []
    for matrix, empty in G.marker_rows(obj, 'BLAST'):
        position = tuple(matrix.translation)[:dimensions]
        scale = matrix.to_scale()
        radius = empty.empty_display_size * max(1e-6, *(abs(scale[i]) for i in range(dimensions)))
        rows.append((*position, radius, side, int(empty.get('petals', 0))))
    return np.asarray(rows, dtype=np.float64).reshape(-1, dimensions + 3)


def remember_blast_inputs(obj, rows):
    _blast_inputs[obj.session_uid] = rows


def blast_inputs_changed(obj):
    rows = blast_rows(obj, G.settings_of(obj))
    previous = _blast_inputs.get(obj.session_uid)
    if previous is not None and previous.shape == rows.shape:
        if np.allclose(previous, rows, rtol=1e-6, atol=1e-6):
            return False
    remember_blast_inputs(obj, rows)
    return True


def build(context, obj, s):
    c = G.core()
    d = G.params_dict(c, 'metal_tear_defaults', s, convert=_convert)
    if s.mode == 'OBJECT':
        fn = G.require(c, 'build_metal_tear_mesh')
        src = G.source_mesh(context, s.source)
        G.track_source(obj, s.source, s.hide_source)
        points = blast_rows(obj, s)
        r = fn(d, src['points'], src['face_sizes'], src['face_verts'], src['face_mat'], src['loop_uvs'], src['face_smooth'],
               points, src['n_mats'], 40.0)
        if not r['ok']:
            raise RuntimeError('nothing is left of the object')
        mats = materials()
        r['_materials'] = (src['materials'] or [None]) + [mats[1], mats[2]]
        r['_uv_name'] = src['uv_name']
    else:
        G.track_source(obj, None, False)
        fn = G.require(c, 'build_metal_tear')
        points = blast_rows(obj, s)
        r = fn(d, points)
        if not r['ok']:
            raise RuntimeError('nothing is left of the sheet')
    remember_blast_inputs(obj, points)
    parts = []
    for key, label in (('blasts', 'blast'), ('punctures', 'puncture'), ('petals', 'petal'), ('tears', 'slit'), ('dents', 'dent')):
        if r[key]:
            parts.append('%d %s%s' % (r[key], label, '' if r[key] == 1 else 's'))
    return r, ', '.join(parts)


def add_marker(context, obj, mtype):
    s = G.settings_of(obj)
    r = 0.5 * (s.blast_size_min + s.blast_size_max)
    if s.mode == 'OBJECT':
        # Place the marker at the cursor when it is near the object.
        local = obj.matrix_world.inverted_safe() @ context.scene.cursor.location
        if s.source is not None:
            half = [0.5 * x for x in s.source.dimensions]
            if any(abs(local[i]) > half[i] + 2 * r for i in range(3)):
                local = local * 0.0
                local.z = half[2] + 0.5 * r
        return G.add_marker(context, obj, 'BLAST', local, r, display='SPHERE', name=obj.name + '_Blast', petals=0)
    # Offset the display sphere slightly from the sheet.
    z = -0.5 * r * _petal_side(s)
    loc = context.scene.cursor.location
    local = obj.matrix_world.inverted_safe() @ loc
    if abs(local.x) > 0.5 * s.width or abs(local.y) > 0.5 * s.height:
        local.x, local.y = 0.0, 0.0
    local.z = z
    return G.add_marker(context, obj, 'BLAST', local, r, display='SPHERE', name=obj.name + '_Blast', petals=0)


# ---------------------------------------------------------------------------
# materials: painted face, bare back, torn edge walls; scorch and torn masks

def _paint(nodes, links, out):
    tex = G.noise_tex(nodes, 6.0, x=-900, y=-250)
    scorch = G.attr_node(nodes, 'metal_scorch', y=200)
    torn = G.attr_node(nodes, 'metal_torn', y=-50)
    bend = G.attr_node(nodes, 'metal_bend', y=-300)
    # paint chipped off near torn edges and on sharp bends
    chip = G.math_node(nodes, links, 'MAXIMUM', torn.outputs['Fac'], bend.outputs['Fac'], x=-400, y=-150)
    chip = G.math_node(nodes, links, 'MULTIPLY', chip, tex.outputs['Fac'], x=-250, y=-150)
    chip = G.map_range(nodes, links, chip, 0.25, 0.4, x=-100, y=-150)
    col = G.mix_color(nodes, links, chip, (0.13, 0.22, 0.15), (0.32, 0.31, 0.30), x=-50, y=50)
    soot = G.map_range(nodes, links, scorch.outputs['Fac'], 0.1, 0.9, x=-300, y=250)
    col = G.mix_color(nodes, links, soot, col, (0.015, 0.013, 0.012), x=120, y=100)
    b = G.principled(nodes, links, out, (0.13, 0.22, 0.15), 0.0, 0.55)
    links.new(col, b.inputs['Base Color'])
    links.new(chip, b.inputs['Metallic'])


def _bare(nodes, links, out):
    scorch = G.attr_node(nodes, 'metal_scorch', y=100)
    soot = G.map_range(nodes, links, scorch.outputs['Fac'], 0.1, 0.9, x=-300, y=100)
    col = G.mix_color(nodes, links, soot, (0.36, 0.35, 0.33), (0.03, 0.028, 0.026), x=0, y=50)
    b = G.principled(nodes, links, out, (0.36, 0.35, 0.33), 1.0, 0.45)
    links.new(col, b.inputs['Base Color'])


def _edge(nodes, links, out):
    G.principled(nodes, links, out, (0.6, 0.59, 0.57), 1.0, 0.3)


def materials():
    return [G.node_material('PD Metal Paint', _paint), G.node_material('PD Metal Bare', _bare),
            G.node_material('PD Metal Torn Edge', _edge)]


# ---------------------------------------------------------------------------
# panel

def add_object(op, context):
    obj, src = G.add_on_object(context, KIND, 'Torn')
    if src is not None:
        s = G.settings_of(obj)
        G._building = True
        try:
            s.mode = 'OBJECT'
            s.source = src
            # small objects get smaller damage
            big = max(src.dimensions)
            k = min(1.0, big / 0.8) if big > 0 else 1.0
            if k < 1.0:
                for name in ('blast_size_min', 'blast_size_max', 'puncture_size_min', 'puncture_size_max', 'tear_length_min',
                             'tear_length_max', 'tear_width', 'dent_size_min', 'dent_size_max', 'crumple_size', 'torn_band'):
                    setattr(s, name, getattr(s, name) * k)
                s.resolution = max(s.resolution * k, 0.0005)
                s.min_piece = s.min_piece * k * k
        finally:
            G._building = False
    return obj


# собираем видимые настройки этого раздела
def draw(layout, s, context, obj):
    on_obj = s.mode == 'OBJECT'
    col = G.box(layout, 'Object' if on_obj else 'Sheet', 'OBJECT_DATA' if on_obj else 'MESH_PLANE')
    col.prop(s, 'mode', text='')
    if on_obj:
        row = col.row(align=True)
        row.prop(s, 'source', text='')
        row.prop(s, 'hide_source', text='', icon='HIDE_ON' if s.hide_source else 'HIDE_OFF')
        col.prop(s, 'thickness')
        col.prop(s, 'resolution_quality')
        col.prop(s, 'seed')
    else:
        row = col.row(align=True)
        row.prop(s, 'width')
        row.prop(s, 'height')
        col.prop(s, 'thickness')
        col.prop(s, 'resolution_quality')
        col.prop(s, 'seed')
    if not on_obj:
        col.separator()
    if not on_obj:
        col.prop(s, 'corrugation')
    if not on_obj and s.corrugation > 0:
        row = col.row(align=True)
        row.prop(s, 'corrugation_period')
        row.prop(s, 'corrugation_box', text='', icon='MOD_WAVE')
    if not on_obj:
        row = col.row(align=True)
        row.prop(s, 'bend')
        if s.bend:
            row.prop(s, 'bend_angle', text='')
        row = col.row(align=True)
        row.prop(s, 'warp')
        row.prop(s, 'warp_scale', text='Size')

    col = G.box(layout, 'Blast Holes', 'MOD_EXPLODE')
    row = col.row(align=True)
    row.prop(s, 'blasts')
    row.operator('project_damage.gen_add_marker', text='Blast Point', icon='ADD').marker = 'BLAST'
    n = len(G.marker_rows(obj, 'BLAST'))
    if n:
        col.label(text='%d Blast Point%s: move / scale the Empties' % (n, '' if n == 1 else 's'), icon='EMPTY_DATA')
    col.prop(s, 'blast_side', text='')
    row = col.row(align=True)
    row.prop(s, 'blast_size_min')
    row.prop(s, 'blast_size_max')
    row = col.row(align=True)
    row.prop(s, 'petals_min')
    row.prop(s, 'petals_max')
    col.prop(s, 'hole_size')
    col.prop(s, 'petal_gap')
    col.prop(s, 'petal_curl')
    col.prop(s, 'curl_random')
    col.prop(s, 'petal_twist')
    col.prop(s, 'petal_crumple')
    col.prop(s, 'slit_random')
    col.prop(s, 'jagged')
    row = col.row(align=True)
    row.prop(s, 'bulge')
    row.prop(s, 'bulge_radius', text='Reach')
    col.separator()
    col.prop(s, 'punctures')
    if s.punctures:
        row = col.row(align=True)
        row.prop(s, 'puncture_size_min')
        row.prop(s, 'puncture_size_max')

    col = G.box(layout, 'Torn Slits', 'MOD_SIMPLEDEFORM')
    col.prop(s, 'tears')
    if s.tears:
        row = col.row(align=True)
        row.prop(s, 'tear_length_min')
        row.prop(s, 'tear_length_max')
        col.prop(s, 'tear_width')
        col.prop(s, 'tear_open')

    col = G.box(layout, 'Dents', 'SPHERECURVE')
    col.prop(s, 'dents')
    if s.dents:
        row = col.row(align=True)
        row.prop(s, 'dent_size_min')
        row.prop(s, 'dent_size_max')
        col.prop(s, 'dent_depth')
        col.prop(s, 'dent_sharpness')
        col.prop(s, 'dent_creases')
        col.prop(s, 'dent_back')

    if not on_obj:
        col = G.box(layout, 'Torn Border', 'MOD_EDGESPLIT')
        row = col.row(align=True)
        row.prop(s, 'edge_tear')
        row.prop(s, 'edge_tear_scale', text='Lobes')
        row = col.row(align=True)
        row.prop(s, 'edge_bites')
        row.prop(s, 'bite_size', text='Size')
        row = col.row(align=True)
        row.prop(s, 'edge_curl')
        row.prop(s, 'edge_curl_width', text='Width')

    col = G.box(layout, 'Detail', 'MOD_NOISE')
    row = col.row(align=True)
    row.prop(s, 'crumple')
    row.prop(s, 'crumple_size', text='Size')
    col.prop(s, 'min_piece')
    col.prop(s, 'torn_band')
    col.prop(s, 'uv_scale')
    col.prop(s, 'max_verts')


KIND = G.Kind(KEY, 'Torn Metal', 'MOD_EXPLODE', PROP, build, draw, materials,
              description='Bent and torn sheet metal: blast holes with curled petals, punctures, torn slits, dents. '
                          'With a mesh selected: the damage is cut into that object',
              smooth_angle=40.0, add_object=add_object)
KIND.add_marker = add_marker


# подключаем свойства и действия этого модуля к Blender
def register():
    bpy.utils.register_class(ProjectDamageMetalGenSettings)
    setattr(bpy.types.Object, PROP, PointerProperty(type=ProjectDamageMetalGenSettings))
    G.register_kind(KIND)


# убираем подключения модуля при выключении аддона
def unregister():
    _blast_inputs.clear()
    G.KINDS.pop(KEY, None)
    if hasattr(bpy.types.Object, PROP):
        delattr(bpy.types.Object, PROP)
    if ProjectDamageMetalGenSettings.is_registered:
        bpy.utils.unregister_class(ProjectDamageMetalGenSettings)
