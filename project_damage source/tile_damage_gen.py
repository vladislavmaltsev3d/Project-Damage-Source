# связываем генератор повреждённых плиток с объектом Blender
# ProjectDamage Broken Tiles generator - GPL-3.0-or-later; see LICENSE.
#
# A tiled wall or floor built from scratch and broken by the native core
# (tile_damage.cpp, build_tile_damage): impact zones knock tiles out (some
# leave pieces stuck on), loosen and tilt others; cracks run out of the zones,
# follow the grout lines and split the tiles they cross; edges and corners are
# chipped; the grout crumbles; the bed under a missing tile shows the adhesive
# ridges. Impact Points are child Empties: move / scale one to move the damage.
import bpy
from bpy.props import BoolProperty, EnumProperty, FloatProperty, IntProperty, PointerProperty, StringProperty

from . import damage_gen_common as G

KEY = 'TILES'
PROP = 'project_damage_tile_gen'


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
    'WALL_15': dict(tile_width=0.15, tile_height=0.15, tile_thickness=0.008, grout_width=0.003, pattern='GRID'),
    'METRO': dict(tile_width=0.2, tile_height=0.1, tile_thickness=0.008, grout_width=0.003, pattern='RUNNING'),
    'WALL_20x30': dict(tile_width=0.2, tile_height=0.3, tile_thickness=0.008, grout_width=0.002, pattern='GRID'),
    'FLOOR_30': dict(tile_width=0.3, tile_height=0.3, tile_thickness=0.01, grout_width=0.004, pattern='GRID'),
    'FLOOR_60': dict(tile_width=0.6, tile_height=0.6, tile_thickness=0.01, grout_width=0.003, pattern='GRID'),
    'MOSAIC': dict(tile_width=0.025, tile_height=0.025, tile_thickness=0.005, grout_width=0.002, pattern='GRID'),
}


def _preset(self, context):
    if self.tile_preset != 'CUSTOM':
        G._building = True
        try:
            for k, v in PRESETS[self.tile_preset].items():
                setattr(self, k, v)
        finally:
            G._building = False
    G.settings_changed(self, context)


# здесь хранятся значения, которые сохраняются вместе с объектом
class ProjectDamageTileGenSettings(bpy.types.PropertyGroup):
    mode: EnumProperty(name='Mode',default='PATCH',items=(('PATCH','Separate Patch','Independent generated object'),('OBJECT','On Object','Apply to the selected Project Damage surface')),update=_u)
    source: PointerProperty(name='Object',type=bpy.types.Object,poll=G.source_poll,update=_u)
    surface_axis: EnumProperty(name='Surface Side',default='AUTO',items=(('AUTO','Auto','Use the positive side along the thinnest dimension'),('X','+ X','Positive local X'),('NX','− X','Negative local X'),('Y','+ Y','Positive local Y'),('NY','− Y','Negative local Y'),('Z','+ Z','Positive local Z'),('NZ','− Z','Negative local Z')),update=_u)

    live_update: BoolProperty(name='Live Update', default=True, description='Rebuild when a setting or an Impact Point changes')
    report: StringProperty(name='Report', default='')
    seed: _int('Seed', 0, 'Random seed', soft_max=1000, maximum=2 ** 31 - 1)
    tile_preset: EnumProperty(name='Tiles', default='WALL_15', update=_preset, description='Tile size preset',
                              items=(('WALL_15', 'Wall 15 x 15', 'Square wall tiles'), ('METRO', 'Metro 20 x 10', 'Running bond'),
                                     ('WALL_20x30', 'Wall 20 x 30', 'Large wall tiles'), ('FLOOR_30', 'Floor 30 x 30', 'Floor tiles'),
                                     ('FLOOR_60', 'Floor 60 x 60', 'Large floor tiles'), ('MOSAIC', 'Mosaic 2.5 cm', 'Small mosaic'),
                                     ('CUSTOM', 'Custom', 'Own sizes')))
    width: _dist('Width', 1.2, 'Tiled area along X', soft_max=10.0, minimum=0.01)
    height: _dist('Height', 1.0, 'Tiled area along Y', soft_max=10.0, minimum=0.01)
    tile_width: _dist('Tile Width', 0.15, 'Tile size along X', soft_max=1.0, minimum=0.005)
    tile_height: _dist('Tile Height', 0.15, 'Tile size along Y', soft_max=1.0, minimum=0.005)
    tile_thickness: _dist('Thickness', 0.008, 'Tile thickness', soft_max=0.03, minimum=0.0005, precision=4)
    grout_width: _dist('Grout', 0.003, 'Width of the joints', soft_max=0.02, precision=4)
    grout_recess: _dist('Grout Recess', 0.0012, 'The grout sits this far below the tile face', soft_max=0.01, precision=4)
    bevel: _dist('Edge Round', 0.0015, 'Rounded glazed edge of the tiles', soft_max=0.006, precision=4)
    corner_radius: _dist('Corner Radius', 0.001, 'Rounded tile corners', soft_max=0.02, precision=4)
    pattern: EnumProperty(name='Pattern', default='GRID', update=_u,
                          items=(('GRID', 'Grid', 'Straight rows and columns'), ('RUNNING', 'Running Bond', 'Rows offset like bricks'),
                                 ('COLUMNS', 'Offset Columns', 'Columns offset')))
    row_offset: _fac('Offset', 0.5, 'Offset of every other row / column, share of the tile')
    bed_thickness: _dist('Bed', 0.02, 'Wall or floor slab under the tiles (0: none)', soft_max=0.2, precision=3)
    resolution_quality: G.resolution_property(0.004)
    resolution: _dist('Resolution', 0.004, 'Mesh edge length in damaged tiles (intact tiles use a coarser mesh)',
                      soft_max=0.02, minimum=0.0005, precision=4)
    max_verts: _int('Max Vertices', 1500000, 'Upper limit: the resolution is coarsened to stay below it',
                    minimum=1000, soft_max=4000000, maximum=20000000)
    tile_warp: _dist('Uneven', 0.0003, 'Tiles not quite flat and not quite level', soft_max=0.003, precision=4)
    # damage
    impacts: _int('Impacts', 1, 'Random damage zones (Impact Points add more)', soft_max=6, maximum=64)
    impact_radius: _dist('Zone Radius', 0.4, 'Radius of a random damage zone', soft_max=2.0, minimum=0.01)
    damage: _fac('Damage', 0.8, 'Overall strength inside the zones', maximum=2.0, soft_max=1.5)
    missing: _fac('Missing Tiles', 0.65, 'Share of tiles knocked out at the zone centre')
    leftovers: _fac('Leftover Pieces', 0.5, 'Share of knocked-out tiles that leave pieces stuck on')
    loose: _fac('Loose Tiles', 0.35, 'Share of tiles in a zone that came loose and tilted')
    tilt_angle: FloatProperty(name='Tilt', default=7.0, min=0.0, soft_max=30.0, max=80.0, update=_u, description='Tilt of loose tiles, degrees')
    cracks: _int('Cracks', 6, 'Crack paths per zone', soft_max=20, maximum=64)
    crack_length: _dist('Crack Length', 0.6, 'Length of the crack paths', soft_max=3.0)
    follow_grout: _fac('Follow Grout', 0.6, 'How strongly cracks run along the grout lines')
    crack_width: _dist('Crack Width', 0.0015, 'Width of the cracks through tiles', soft_max=0.01, precision=4)
    crack_jag: _fac('Jagged', 0.5, 'Ragged crack edges', maximum=2.0)
    piece_shift: _dist('Piece Shift', 0.0015, 'Pieces of a cracked tile move apart and sink', soft_max=0.01, precision=4)
    piece_tilt: FloatProperty(name='Piece Tilt', default=3.0, min=0.0, soft_max=15.0, max=60.0, update=_u,
                              description='Pieces of a cracked tile tilt, degrees')
    chips: _fac('Chips', 0.35, 'Chipped edges and corners inside the zones', maximum=3.0, soft_max=1.5)
    chip_size: _dist('Chip Size', 0.012, 'Size of the chips', soft_max=0.05, precision=4)
    chip_depth: _fac('Chip Depth', 0.5, 'Depth of shallow glaze chips, share of the thickness', maximum=0.9)
    scatter_chips: _fac('Chips Elsewhere', 0.08, 'Chips on tiles outside the zones', maximum=2.0)
    grout_crumble: _fac('Grout Crumble', 0.6, 'Grout broken out along cracks and around missing tiles')
    adhesive: _fac('Adhesive', 0.6, 'Adhesive left on the bed where tiles are gone (0: bare bed)')
    adhesive_height: _dist('Ridge Height', 0.003, 'Height of the adhesive ridges', soft_max=0.01, precision=4)
    adhesive_spacing: _dist('Ridge Spacing', 0.01, 'Spacing of the notched-trowel ridges', soft_max=0.03, minimum=0.001, precision=4)
    uv_scale: FloatProperty(name='UV Scale', default=1.0, min=0.0001, soft_max=10.0, update=_u,
                            description='UV units per metre; each tile is mapped from its own corner')


def _convert(d, s):
    d['pattern'] = {'GRID': 0, 'RUNNING': 1, 'COLUMNS': 2}[s.pattern]


def _impact_rows(obj):
    import numpy as np
    rows = []
    for m, e in G.marker_rows(obj, 'IMPACT'):
        sc = m.to_scale()
        rows.append((m.translation.x, m.translation.y, e.empty_display_size * max(abs(sc.x), abs(sc.y), 1e-6),
                     float(e.get('strength', 1.0))))
    return np.ascontiguousarray(np.array(rows, dtype=np.float64).reshape(-1, 4))


def build(context, obj, s):
    c = G.core()
    fn = G.require(c, 'build_tile_damage')
    d = G.params_dict(c, 'tile_damage_defaults', s, convert=_convert)
    if s.mode=='OBJECT':
        from . import damage_gen_surface
        return damage_gen_surface.build(context,obj,s,d,fn,_impact_rows(obj),KIND)
    r = fn(d, _impact_rows(obj))
    if not r['ok']:
        raise RuntimeError('nothing was built')
    parts = ['%d tiles' % r['tiles']]
    for key, label in (('missing', 'missing'), ('cracked', 'cracked'), ('loose', 'loose'), ('chips', 'chips')):
        if r[key]:
            parts.append('%d %s' % (r[key], label))
    return r, ', '.join(parts)


def add_marker(context, obj, mtype):
    s = G.settings_of(obj)
    local = obj.matrix_world.inverted_safe() @ context.scene.cursor.location
    if abs(local.x) > 0.5 * s.width or abs(local.y) > 0.5 * s.height:
        local.x, local.y = 0.0, 0.0
    local.z = s.tile_thickness
    return G.add_marker(context, obj, 'IMPACT', local, s.impact_radius, display='SPHERE', name=obj.name + '_Impact', strength=1.0)


# ---------------------------------------------------------------------------
# materials

def _glaze(nodes, links, out):
    rnd = G.attr_node(nodes, 'tile_random', y=250)
    chip = G.attr_node(nodes, 'tile_chip', y=0)
    crack = G.attr_node(nodes, 'tile_crack', y=-250)
    vary = G.map_range(nodes, links, rnd.outputs['Fac'], 0.0, 1.0, x=-350, y=250)
    base = G.mix_color(nodes, links, vary, (0.62, 0.74, 0.78), (0.70, 0.80, 0.83), x=-150, y=250)
    col = G.mix_color(nodes, links, chip.outputs['Fac'], base, (0.80, 0.74, 0.66), x=50, y=150)
    dirt = G.math_node(nodes, links, 'MULTIPLY', crack.outputs['Fac'], 0.6, x=-150, y=-250)
    col = G.mix_color(nodes, links, dirt, col, (0.25, 0.22, 0.2), x=220, y=100)
    rough = G.map_range(nodes, links, chip.outputs['Fac'], 0.0, 1.0, x=50, y=-100)
    r2 = G.math_node(nodes, links, 'MULTIPLY_ADD', rough, 0.7, x=220, y=-100)
    r2.node.inputs[2].default_value = 0.08
    b = G.principled(nodes, links, out, (0.62, 0.74, 0.78), 0.0, 0.08)
    links.new(col, b.inputs['Base Color'])
    links.new(r2, b.inputs['Roughness'])
    try:
        b.inputs['Coat Weight'].default_value = 0.6
        b.inputs['Coat Roughness'].default_value = 0.03
    except KeyError:
        pass


def _body(nodes, links, out):
    G.principled(nodes, links, out, (0.80, 0.74, 0.66), 0.0, 0.85)


def _grout(nodes, links, out):
    crack = G.attr_node(nodes, 'tile_crack', y=0)
    col = G.mix_color(nodes, links, crack.outputs['Fac'], (0.72, 0.71, 0.68), (0.36, 0.34, 0.31), x=0)
    b = G.principled(nodes, links, out, (0.72, 0.71, 0.68), 0.0, 0.95)
    links.new(col, b.inputs['Base Color'])


def _bed(nodes, links, out):
    tex = G.noise_tex(nodes, 40.0, x=-400)
    col = G.mix_color(nodes, links, tex.outputs['Fac'], (0.42, 0.41, 0.39), (0.55, 0.54, 0.51), x=0)
    b = G.principled(nodes, links, out, (0.5, 0.49, 0.46), 0.0, 0.95)
    links.new(col, b.inputs['Base Color'])


def materials():
    return [G.node_material('PD Tile Glaze', _glaze), G.node_material('PD Tile Body', _body),
            G.node_material('PD Tile Grout', _grout), G.node_material('PD Tile Bed', _bed)]


def add_object(op, context):
    obj,src=G.add_on_object(context,KIND,'Surface')
    if src is not None:
        from . import damage_gen_surface
        s=G.settings_of(obj);s.mode='OBJECT';s.source=src
        damage_gen_surface.fit(obj,s,src)
    return obj


# собираем видимые настройки этого раздела
def draw(layout, s, context, obj):
    if s.mode=='OBJECT':
        col=G.box(layout,'Target Surface','OBJECT_DATA');col.prop(s,'surface_axis')
    col = G.box(layout, 'Tiles', 'MESH_GRID')
    col.prop(s, 'tile_preset', text='')
    row = col.row(align=True)
    if s.mode!='OBJECT':
        row.prop(s, 'width');row.prop(s, 'height')
    else:row.label(text='Size follows the target mesh',icon='INFO')
    row = col.row(align=True)
    row.prop(s, 'tile_width', text='Tile X')
    row.prop(s, 'tile_height', text='Tile Y')
    row = col.row(align=True)
    row.prop(s, 'tile_thickness')
    row.prop(s, 'grout_width')
    row = col.row(align=True)
    row.prop(s, 'pattern', text='')
    if s.pattern != 'GRID':
        row.prop(s, 'row_offset')
    row = col.row(align=True)
    row.prop(s, 'bevel')
    row.prop(s, 'corner_radius', text='Corner')
    row = col.row(align=True)
    row.prop(s, 'grout_recess', text='Recess')
    row.prop(s, 'bed_thickness')
    col.prop(s, 'tile_warp')
    col.prop(s, 'resolution_quality')
    col.prop(s, 'seed')

    col = G.box(layout, 'Damage Zones', 'MOD_EXPLODE')
    row = col.row(align=True)
    row.prop(s, 'impacts')
    row.operator('project_damage.gen_add_marker', text='Impact Point', icon='ADD').marker = 'IMPACT'
    n = len(G.marker_rows(obj, 'IMPACT'))
    if n:
        col.label(text='%d Impact Point%s: move / scale the Empties' % (n, '' if n == 1 else 's'), icon='EMPTY_DATA')
    col.prop(s, 'impact_radius')
    col.prop(s, 'damage')
    col.prop(s, 'missing')
    col.prop(s, 'leftovers')
    row = col.row(align=True)
    row.prop(s, 'loose')
    row.prop(s, 'tilt_angle')

    col = G.box(layout, 'Cracks', 'MOD_PHYSICS')
    row = col.row(align=True)
    row.prop(s, 'cracks')
    row.prop(s, 'crack_length', text='Length')
    col.prop(s, 'follow_grout')
    row = col.row(align=True)
    row.prop(s, 'crack_width', text='Width')
    row.prop(s, 'crack_jag')
    row = col.row(align=True)
    row.prop(s, 'piece_shift', text='Shift')
    row.prop(s, 'piece_tilt', text='Tilt')

    col = G.box(layout, 'Chips', 'MOD_BEVEL')
    row = col.row(align=True)
    row.prop(s, 'chips')
    row.prop(s, 'chip_size', text='Size')
    col.prop(s, 'chip_depth')
    col.prop(s, 'scatter_chips')

    col = G.box(layout, 'Grout and Bed', 'MOD_SOLIDIFY')
    col.prop(s, 'grout_crumble')
    col.prop(s, 'adhesive')
    if s.adhesive > 0:
        row = col.row(align=True)
        row.prop(s, 'adhesive_height', text='Height')
        row.prop(s, 'adhesive_spacing', text='Spacing')
    col.prop(s, 'uv_scale')
    col.prop(s, 'max_verts')


KIND = G.Kind(KEY, 'Broken Tiles', 'MESH_GRID', PROP, build, draw, materials,
              description='A tiled wall or floor: knocked-out, cracked, chipped and loose tiles, crumbled grout',
              smooth_angle=35.0, add_object=add_object)
KIND.add_marker = add_marker


# подключаем свойства и действия этого модуля к Blender
def register():
    bpy.utils.register_class(ProjectDamageTileGenSettings)
    setattr(bpy.types.Object, PROP, PointerProperty(type=ProjectDamageTileGenSettings))
    G.register_kind(KIND)


# убираем подключения модуля при выключении аддона
def unregister():
    G.KINDS.pop(KEY, None)
    if hasattr(bpy.types.Object, PROP):
        delattr(bpy.types.Object, PROP)
    if ProjectDamageTileGenSettings.is_registered:
        bpy.utils.unregister_class(ProjectDamageTileGenSettings)
