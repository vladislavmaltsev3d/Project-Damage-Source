# настройки генератора расколотого дерева
# ProjectDamage Split Wood generator - GPL-3.0-or-later; see LICENSE.
#
# A board (plank or beam) snapped across the grain and split along it, built
# from scratch by the native core (wood_split.cpp, build_wood_split): long
# pointed splinters stick out of both pieces and interlock, peel away from the
# board and bend sideways; the pieces bend at the break and are pulled apart;
# splits along the grain fork the board. The wood is made of fibre layers
# through the thickness, each breaking on its own. The grain runs along X.
#
# On an Object: with a mesh selected, Add makes "<name> Broken": that object's
# own mesh broken across its grain (build_wood_split_mesh). The object is
# hidden and followed; its UVs and materials are kept, the broken wood gets one
# more material.
import bpy
from bpy.props import BoolProperty, EnumProperty, FloatProperty, IntProperty, PointerProperty, StringProperty

from . import damage_gen_common as G

KEY = 'WOOD'
PROP = 'project_damage_wood_gen'


def _u(self, context):
    G.settings_changed(self, context)


def _dist(name, default, description, soft_max=1.0, minimum=0.0, precision=3):
    return FloatProperty(name=name, default=default, description=description, min=minimum, soft_max=soft_max,
                         subtype='DISTANCE', unit='LENGTH', precision=precision, update=_u)


def _fac(name, default, description, maximum=1.0, minimum=0.0, soft_max=None):
    return FloatProperty(name=name, default=default, description=description, min=minimum, max=maximum,
                         soft_max=soft_max if soft_max is not None else maximum, subtype='FACTOR', update=_u)


def _int(name, default, description, minimum=0, soft_max=20, maximum=10000):
    return IntProperty(name=name, default=default, description=description, min=minimum, max=maximum, soft_max=soft_max, update=_u)


PRESETS = {
    'PLANK': dict(length=1.0, width=0.14, thickness=0.03, layers=4),
    'FLOORBOARD': dict(length=1.2, width=0.12, thickness=0.022, layers=3),
    'BEAM': dict(length=1.4, width=0.12, thickness=0.12, layers=8),
    'STICK': dict(length=0.8, width=0.045, thickness=0.045, layers=4),
}


def _preset(self, context):
    if self.board != 'CUSTOM':
        G._building = True
        try:
            for k, v in PRESETS[self.board].items():
                setattr(self, k, v)
        finally:
            G._building = False
    G.settings_changed(self, context)


# здесь хранятся значения, которые сохраняются вместе с объектом
class ProjectDamageWoodGenSettings(bpy.types.PropertyGroup):
    live_update: BoolProperty(name='Live Update', default=True, description='Rebuild when a setting changes')
    report: StringProperty(name='Report', default='')
    mode: EnumProperty(name='Mode', default='BOARD', update=_u,
                       items=(('BOARD', 'New Board', 'A broken board built from scratch', 'MESH_CUBE', 0),
                              ('OBJECT', 'On an Object', 'Another object broken across its grain (it is hidden and followed)', 'OBJECT_DATA', 1)))
    source: PointerProperty(name='Object', type=bpy.types.Object, poll=G.source_poll, update=_u,
                            description='The object to break (its scale is applied, its placement is not)')
    hide_source: BoolProperty(name='Hide Object', default=True, update=_u, description='Hide the original in the viewport and in renders')
    grain_axis: EnumProperty(name='Grain', default='AUTO', update=_u, description='Which way the grain runs in the object',
                             items=(('AUTO', 'Longest Side', 'Along the longest side of the object'),
                                    ('X', 'X', 'Along the object\'s X'), ('Y', 'Y', 'Along the object\'s Y'), ('Z', 'Z', 'Along the object\'s Z')))
    seed: _int('Seed', 0, 'Random seed', soft_max=1000, maximum=2 ** 31 - 1)
    board: EnumProperty(name='Board', default='PLANK', update=_preset, description='Board size preset',
                        items=(('PLANK', 'Plank', '1 m x 14 cm x 3 cm'), ('FLOORBOARD', 'Floorboard', '1.2 m x 12 cm x 22 mm'),
                               ('BEAM', 'Beam', '1.4 m x 12 x 12 cm'), ('STICK', 'Batten', '80 cm x 45 x 45 mm'),
                               ('CUSTOM', 'Custom', 'Own size')))
    length: _dist('Length', 1.0, 'Board length, along the grain (X)', soft_max=5.0, minimum=0.01)
    width: _dist('Width', 0.14, 'Board width (Y)', soft_max=0.5, minimum=0.002)
    thickness: _dist('Thickness', 0.03, 'Board thickness (Z)', soft_max=0.3, minimum=0.001)
    layers: _int('Fibre Layers', 4, 'Layers through the thickness; each breaks on its own, so a broken end steps in ragged layers',
                 minimum=1, soft_max=12, maximum=32)
    resolution_quality: G.resolution_property(0.0015)
    resolution: _dist('Resolution', 0.0015, 'Mesh edge across the grain. Thin splinters need a fine resolution',
                      soft_max=0.01, minimum=0.0002, precision=4)
    grain_stretch: FloatProperty(name='Grain Stretch', default=4.0, min=1.0, max=20.0, update=_u,
                                 description='The mesh edge along the grain is Resolution times this')
    max_verts: _int('Max Vertices', 800000, 'Upper limit: the resolution is coarsened to stay below it',
                    minimum=1000, soft_max=3000000, maximum=20000000)
    bow: _dist('Bow', 0.0, 'The whole board bowed (height at the middle)', soft_max=0.2, minimum=-1.0)
    # break
    break_mode: EnumProperty(name='Break', default='SNAPPED', update=_u,
                             items=(('NONE', 'None', 'Not broken across: only splits along the grain'),
                                    ('SNAPPED', 'Snapped in Two', 'Two pieces with interlocking splinters'),
                                    ('END', 'Broken End', 'One piece with a splintered end')))
    break_pos: FloatProperty(name='Position', default=0.0, min=-0.4, max=0.4, subtype='FACTOR', update=_u,
                             description='Where the board broke, share of the length from the middle')
    break_angle: FloatProperty(name='Bend', default=18.0, soft_min=-60.0, soft_max=60.0, min=-120.0, max=120.0, update=_u,
                               description='Degrees the pieces bend at the break (negative: up)')
    break_gap: _dist('Gap', 0.04, 'The two pieces pulled apart along the grain', soft_max=0.3)
    break_jag: _dist('Ragged Line', 0.012, 'How uneven the base line of the break is', soft_max=0.1)
    # splinters
    splinter_width_min: _dist('Width Min', 0.003, 'Splinter width', soft_max=0.05, minimum=0.0002, precision=4)
    splinter_width_max: _dist('Width Max', 0.014, 'Splinter width', soft_max=0.05, minimum=0.0002, precision=4)
    splinter_length_min: _dist('Length Min', 0.015, 'Length of the short splinters', soft_max=0.3)
    splinter_length_max: _dist('Length Max', 0.28, 'Length of the long splinters', soft_max=1.0)
    long_share: _fac('Long Splinters', 0.3, 'Share of long splinters')
    tip_shape: _fac('Sharp Tips', 0.7, '0 blunt, 1 needle-sharp splinter tips')
    splinter_lift: _fac('Peel', 0.22, 'Splinters peel away from the board: the top layer up, the bottom layer down', maximum=3.0, soft_max=1.5)
    splinter_sway: _fac('Sway', 0.15, 'Splinters bend sideways', maximum=2.0, soft_max=1.0)
    splinter_gap: _dist('Crack Width', 0.0012, 'Cracks between neighbouring splinters', soft_max=0.01, precision=4)
    crack_depth: _dist('Crack Depth', 0.06, 'How far those cracks run back into the board', soft_max=0.5)
    layer_shift: _fac('Layer Shift', 0.6, '0: all layers break alike, 1: every layer breaks on its own')
    # splits
    splits: _int('Splits', 1, 'Splits along the grain: the board forks', soft_max=4, maximum=16)
    split_length: _fac('Split Length', 0.35, 'Share of the piece length the split runs back', maximum=0.95)
    split_open: _dist('Opening', 0.035, 'How far the split opens at the end', soft_max=0.3)
    split_jag: _fac('Torn Fibres', 0.5, 'Fibres torn along the split', maximum=2.0)
    grain_rough: _fac('Fibre Teeth', 0.4, 'Fibre teeth on torn edges along the grain', maximum=2.0)
    uv_scale: FloatProperty(name='UV Scale', default=1.0, min=0.0001, soft_max=10.0, update=_u,
                            description='UV units per metre (U along the grain)')


def _convert(d, s):
    d['break_mode'] = {'NONE': 0, 'SNAPPED': 1, 'END': 2}[s.break_mode]


def build(context, obj, s):
    c = G.core()
    d = G.params_dict(c, 'wood_split_defaults', s, convert=_convert)
    if s.mode == 'OBJECT':
        fn = G.require(c, 'build_wood_split_mesh')
        src = G.source_mesh(context, s.source)
        G.track_source(obj, s.source, s.hide_source)
        axis = {'AUTO': -1, 'X': 0, 'Y': 1, 'Z': 2}[s.grain_axis]
        r = fn(d, src['points'], src['face_sizes'], src['face_verts'], src['face_mat'], src['loop_uvs'], src['face_smooth'],
               axis, src['n_mats'], 40.0)
        if not r['ok']:
            raise RuntimeError('nothing is left of the object')
        r['_materials'] = (src['materials'] or [None]) + [materials()[1]]
        r['_uv_name'] = src['uv_name']
    else:
        G.track_source(obj, None, False)
        fn = G.require(c, 'build_wood_split')
        r = fn(d)
        if not r['ok']:
            raise RuntimeError('nothing was built')
    parts = ['%d piece%s' % (r['pieces'], '' if r['pieces'] == 1 else 's')]
    if r['splinters']:
        parts.append('%d splinters' % r['splinters'])
    if r['splits']:
        parts.append('%d split%s' % (r['splits'], '' if r['splits'] == 1 else 's'))
    return r, ', '.join(parts)


# ---------------------------------------------------------------------------
# materials: aged wood surface, fresh broken wood; grain along U

def _grain(nodes, links, x=-900, y=0, scale=6.0):
    tc = nodes.new('ShaderNodeTexCoord')
    tc.location = (x - 400, y)
    mp = nodes.new('ShaderNodeMapping')
    mp.location = (x - 200, y)
    mp.inputs['Scale'].default_value = (1.0, 18.0, 18.0)
    links.new(tc.outputs['Object'], mp.inputs['Vector'])
    w = nodes.new('ShaderNodeTexWave')
    w.location = (x, y)
    w.wave_type = 'BANDS'
    try:
        w.bands_direction = 'Y'
    except (AttributeError, TypeError):
        pass
    w.inputs['Scale'].default_value = scale
    w.inputs['Distortion'].default_value = 6.0
    try:
        w.inputs['Detail'].default_value = 3.0
    except KeyError:
        pass
    links.new(mp.outputs['Vector'], w.inputs['Vector'])
    return w.outputs['Fac']


def _aged(nodes, links, out):
    g = _grain(nodes, links)
    col = G.mix_color(nodes, links, g, (0.19, 0.12, 0.07), (0.33, 0.22, 0.13), x=-100)
    brk = G.attr_node(nodes, 'wood_break', y=250)
    col = G.mix_color(nodes, links, brk.outputs['Fac'], col, (0.55, 0.42, 0.27), x=120, y=100)
    b = G.principled(nodes, links, out, (0.3, 0.2, 0.12), 0.0, 0.75)
    links.new(col, b.inputs['Base Color'])


def _fresh(nodes, links, out):
    g = _grain(nodes, links, scale=10.0)
    col = G.mix_color(nodes, links, g, (0.62, 0.45, 0.26), (0.78, 0.62, 0.40), x=-100)
    b = G.principled(nodes, links, out, (0.75, 0.58, 0.36), 0.0, 0.85)
    links.new(col, b.inputs['Base Color'])


def materials():
    return [G.node_material('PD Wood Aged', _aged), G.node_material('PD Wood Fresh', _fresh)]


def add_object(op, context):
    obj, src = G.add_on_object(context, KIND, 'Broken')
    if src is not None:
        s = G.settings_of(obj)
        G._building = True
        try:
            s.mode = 'OBJECT'
            s.source = src
            s.board = 'CUSTOM'
            # splinters to the size of the cross-section (the defaults suit a 14 x 3 cm plank)
            dims = sorted(src.dimensions)
            cross = dims[1] if dims[1] > 0 else 0.14
            k = max(0.3, min(3.0, cross / 0.14))
            for name in ('splinter_width_min', 'splinter_width_max', 'splinter_length_min', 'splinter_length_max',
                         'crack_depth', 'break_jag', 'break_gap', 'resolution'):
                setattr(s, name, getattr(s, name) * k)
            s.layers = max(2, min(8, int(round(dims[0] / 0.008))))   # about one layer per 8 mm of thickness
        finally:
            G._building = False
    return obj


# собираем видимые настройки этого раздела
def draw(layout, s, context, obj):
    on_obj = s.mode == 'OBJECT'
    col = G.box(layout, 'Object' if on_obj else 'Board', 'OBJECT_DATA' if on_obj else 'MESH_CUBE')
    col.prop(s, 'mode', text='')
    if on_obj:
        row = col.row(align=True)
        row.prop(s, 'source', text='')
        row.prop(s, 'hide_source', text='', icon='HIDE_ON' if s.hide_source else 'HIDE_OFF')
        col.prop(s, 'grain_axis')
    else:
        col.prop(s, 'board', text='')
        col.prop(s, 'length')
        row = col.row(align=True)
        row.prop(s, 'width')
        row.prop(s, 'thickness')
    col.prop(s, 'layers')
    col.prop(s, 'resolution_quality')
    col.prop(s, 'seed')
    if not on_obj:
        col.prop(s, 'bow')

    col = G.box(layout, 'Break', 'MOD_PHYSICS')
    col.prop(s, 'break_mode', text='')
    if s.break_mode != 'NONE':
        col.prop(s, 'break_pos')
        row = col.row(align=True)
        row.prop(s, 'break_angle')
        if s.break_mode == 'SNAPPED':
            row.prop(s, 'break_gap')
        col.prop(s, 'break_jag')

        col = G.box(layout, 'Splinters', 'IPO_SINE')
        row = col.row(align=True)
        row.prop(s, 'splinter_length_min')
        row.prop(s, 'splinter_length_max')
        row = col.row(align=True)
        row.prop(s, 'splinter_width_min')
        row.prop(s, 'splinter_width_max')
        col.prop(s, 'long_share')
        col.prop(s, 'tip_shape')
        row = col.row(align=True)
        row.prop(s, 'splinter_lift')
        row.prop(s, 'splinter_sway')
        row = col.row(align=True)
        row.prop(s, 'splinter_gap')
        row.prop(s, 'crack_depth')
        col.prop(s, 'layer_shift')

    if not on_obj:
        col = G.box(layout, 'Splits Along the Grain', 'MOD_EDGESPLIT')
        col.prop(s, 'splits')
        if s.splits:
            col.prop(s, 'split_length')
            col.prop(s, 'split_open')
            col.prop(s, 'split_jag')

    col = G.box(layout, 'Detail', 'MOD_NOISE')
    col.prop(s, 'grain_rough')
    col.prop(s, 'grain_stretch')
    col.prop(s, 'uv_scale')
    col.prop(s, 'max_verts')


KIND = G.Kind(KEY, 'Split Wood', 'MOD_BUILD', PROP, build, draw, materials,
              description='A board snapped across the grain and split along it, with long splinters. '
                          'With a mesh selected: that object is broken',
              smooth_angle=30.0, add_object=add_object)


# подключаем свойства и действия этого модуля к Blender
def register():
    bpy.utils.register_class(ProjectDamageWoodGenSettings)
    setattr(bpy.types.Object, PROP, PointerProperty(type=ProjectDamageWoodGenSettings))
    G.register_kind(KIND)


# убираем подключения модуля при выключении аддона
def unregister():
    G.KINDS.pop(KEY, None)
    if hasattr(bpy.types.Object, PROP):
        delattr(bpy.types.Object, PROP)
    if ProjectDamageWoodGenSettings.is_registered:
        bpy.utils.unregister_class(ProjectDamageWoodGenSettings)
