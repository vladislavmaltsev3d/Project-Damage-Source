# связываем генератор взрывной воронки с настройками и материалами Blender
# ProjectDamage Blast Crater generator - GPL-3.0-or-later; see LICENSE.
#
# A patch of ground with a blast crater, built by the native core
# (blast_crater.cpp, build_blast_crater): a bowl with a ragged outline and a
# rubbly floor, a raised rim, a blanket of thrown-out soil with streaks and
# clods; asphalt or concrete over the ground torn away over the crater, broken
# into plates pushed up at the rim and cracked further out; stones, clods and
# pavement chunks thrown around. Crater Points (child Empties) add craters.
import bpy
import numpy as np
from bpy.props import BoolProperty, EnumProperty, FloatProperty, IntProperty, PointerProperty, StringProperty

from . import damage_gen_common as G

KEY = 'CRATER'
PROP = 'project_damage_crater_gen'
PAVEMENT = {'NONE': 0, 'ASPHALT': 1, 'CONCRETE': 2}


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


# blast sizes: crater, ground patch and the detail that goes with them
PRESETS = {
    'GRENADE': dict(radius=0.35, depth=0.12, rim_height=0.05, width=3.0, height=3.0, resolution=0.015, ground_thickness=0.25,
                    clod_size=0.035, debris_size=0.045, plate_size=0.22, crack_width=0.012, pavement_thickness=0.05,
                    slab_size=1.0, terrain=0.01, terrain_scale=1.0),
    'SHELL': dict(radius=0.9, depth=0.4, rim_height=0.16, width=6.0, height=6.0, resolution=0.03, ground_thickness=0.4,
                  clod_size=0.08, debris_size=0.11, plate_size=0.4, crack_width=0.018, pavement_thickness=0.07,
                  slab_size=1.5, terrain=0.02, terrain_scale=2.0),
    'BOMB': dict(radius=1.6, depth=0.7, rim_height=0.3, width=10.0, height=10.0, resolution=0.05, ground_thickness=0.6,
                 clod_size=0.12, debris_size=0.18, plate_size=0.6, crack_width=0.025, pavement_thickness=0.08,
                 slab_size=2.0, terrain=0.03, terrain_scale=3.0),
    'LARGE': dict(radius=4.5, depth=1.9, rim_height=0.7, width=26.0, height=26.0, resolution=0.12, ground_thickness=1.5,
                  clod_size=0.3, debris_size=0.4, plate_size=1.3, crack_width=0.05, pavement_thickness=0.15,
                  slab_size=3.0, terrain=0.08, terrain_scale=6.0),
}


def _preset(self, context):
    if self.blast != 'CUSTOM':
        G._building = True
        try:
            for k, v in PRESETS[self.blast].items():
                setattr(self, k, v)
            self['_resolution_reference'] = self.resolution
        finally:
            G._building = False
    G.settings_changed(self, context)


# здесь хранятся значения, которые сохраняются вместе с объектом
class ProjectDamageCraterGenSettings(bpy.types.PropertyGroup):
    mode: EnumProperty(name='Mode',default='PATCH',items=(('PATCH','Separate Patch','Independent generated object'),('OBJECT','On Object','Apply to the selected Project Damage surface')),update=_u)
    source: PointerProperty(name='Object',type=bpy.types.Object,poll=G.source_poll,update=_u)
    surface_axis: EnumProperty(name='Surface Side',default='AUTO',items=(('AUTO','Auto','Use the positive side along the thinnest dimension'),('X','+ X','Positive local X'),('NX','− X','Negative local X'),('Y','+ Y','Positive local Y'),('NY','− Y','Negative local Y'),('Z','+ Z','Positive local Z'),('NZ','− Z','Negative local Z')),update=_u)

    live_update: BoolProperty(name='Live Update', default=True, description='Rebuild when a setting or a Crater Point changes')
    report: StringProperty(name='Report', default='')
    seed: _int('Seed', 0, 'Random seed', soft_max=1000, maximum=2 ** 31 - 1)
    blast: EnumProperty(name='Blast', default='BOMB', update=_preset, description='Size preset: crater, ground patch and detail',
                        items=(('GRENADE', 'Grenade', 'A small pit, 3 m patch'), ('SHELL', 'Shell', 'A shell crater, 6 m patch'),
                               ('BOMB', 'Bomb', 'A bomb crater, 10 m patch'), ('LARGE', 'Large Bomb', 'A large crater, 26 m patch'),
                               ('CUSTOM', 'Custom', 'Own sizes')))
    # ground
    width: _dist('Width', 10.0, 'Ground patch along X', soft_max=50.0, minimum=0.1)
    height: _dist('Depth', 10.0, 'Ground patch along Y', soft_max=50.0, minimum=0.1)
    ground_thickness: _dist('Thickness', 0.6, 'Closed ground slab below the surface (0: the top surface only)', soft_max=3.0)
    resolution_quality: G.resolution_property(0.05)
    resolution: _dist('Resolution', 0.05, 'Mesh edge of the ground (the broken pavement zone is finer)', soft_max=0.3, minimum=0.002, precision=4)
    max_verts: _int('Max Vertices', 1500000, 'Upper limit: the resolution is coarsened to stay below it', minimum=1000, soft_max=4000000,
                    maximum=20000000)
    terrain: _dist('Terrain', 0.03, 'Gentle unevenness of the ground', soft_max=0.5, precision=3)
    terrain_scale: _dist('Terrain Scale', 3.0, 'Size of the terrain undulation', soft_max=20.0, minimum=0.01)
    # crater
    main_crater: BoolProperty(name='Main Crater', default=True, update=_u, description='A crater at the object origin (Crater Points add more)')
    radius: _dist('Radius', 1.6, 'Crater radius', soft_max=10.0)
    depth: _dist('Crater Depth', 0.7, 'Depth of the bowl', soft_max=5.0)
    floor_flat: _fac('Flat Floor', 0.25, 'Flat bottom, share of the radius', maximum=0.9)
    rim_height: _dist('Rim', 0.3, 'Height of the raised rim', soft_max=2.0)
    ejecta: _fac('Blanket Reach', 0.6, 'Reach of the thrown-out soil beyond the rim, share of the radius', maximum=5.0, soft_max=2.0)
    rays: _fac('Streaks', 0.5, 'Streaks of thrown-out soil (and debris along them)', maximum=2.0)
    ray_count: _int('Streak Count', 9, 'Number of streaks', soft_max=24, maximum=64)
    asymmetry: _fac('Asymmetry', 0.25, 'Oblique blast: rim and blanket stronger downrange', maximum=0.95)
    direction: FloatProperty(name='Direction', default=30.0, min=-360.0, max=360.0, update=_u, description='Downrange, degrees from +X')
    irregularity: _fac('Irregular', 0.5, 'Ragged outline and rubbly floor', maximum=2.0)
    clods: _fac('Clods', 0.5, 'Lumpy, clodded soil on the blanket', maximum=2.0)
    clod_size: _dist('Clod Size', 0.12, 'Size of the clods', soft_max=0.5)
    # pavement
    pavement: EnumProperty(name='Surface', default='ASPHALT', update=_u, description='What covers the ground',
                           items=(('NONE', 'Bare Ground', 'Soil only'), ('ASPHALT', 'Asphalt', 'An asphalt layer'),
                                  ('CONCRETE', 'Concrete Slabs', 'Concrete slabs with joints')))
    pavement_thickness: _dist('Layer', 0.08, 'Thickness of the asphalt or slabs', soft_max=0.4, precision=3)
    hole_size: _fac('Hole', 1.12, 'The pavement is torn away out to this share of the radius', maximum=3.0, minimum=0.3)
    plate_size: _dist('Plate Size', 0.6, 'Broken plates at the rim (larger further out)', soft_max=3.0, minimum=0.02)
    crack_width: _dist('Crack Width', 0.025, 'Width of the cracks', soft_max=0.1, precision=4)
    crack_reach: _fac('Broken Zone', 2.6, 'Reach of the broken plates, share of the radius', maximum=8.0, minimum=0.5)
    radial_cracks: _int('Long Cracks', 7, 'Long cracks running out of the broken zone', soft_max=20, maximum=64)
    crack_length: _fac('Crack Length', 2.5, 'Length of the long cracks, share of the radius', maximum=10.0)
    plate_tilt: FloatProperty(name='Plate Tilt', default=12.0, min=0.0, max=60.0, update=_u,
                              description='Degrees: plates at the rim are pushed up and tilted')
    slab_size: _dist('Slab Size', 2.0, 'Concrete slab size (joints)', soft_max=6.0, minimum=0.1)
    # debris
    debris: _int('Debris', 250, 'Pieces thrown around', soft_max=1000, maximum=20000)
    debris_size: _dist('Debris Size', 0.18, 'Size of the largest pieces', soft_max=1.0)
    debris_spread: _fac('Spread', 0.8, 'How far it is thrown, share of the radius', maximum=5.0)
    debris_inside: _fac('Fallen Back', 0.2, 'Share that fell back into the crater')
    scorch: _fac('Scorch', 0.8, 'Burnt ground around the crater (the crater_scorch attribute)', maximum=2.0)
    uv_scale: FloatProperty(name='UV Scale', default=1.0, min=0.0001, soft_max=10.0, update=_u, description='UV units per metre')


def _convert(d, s):
    d['pavement'] = PAVEMENT[s.pavement]


def _crater_rows(obj):
    rows = []
    for m, e in G.marker_rows(obj, 'CRATER'):
        sc = m.to_scale()
        rows.append((m.translation.x, m.translation.y, e.empty_display_size * max(abs(sc.x), abs(sc.y), 1e-6), float(e.get('depth', 1.0))))
    return np.ascontiguousarray(np.array(rows, dtype=np.float64).reshape(-1, 4))


def build(context, obj, s):
    c = G.core()
    fn = G.require(c, 'build_blast_crater')
    d = G.params_dict(c, 'blast_crater_defaults', s, convert=_convert)
    if s.mode=='OBJECT':
        from . import damage_gen_surface
        return damage_gen_surface.build(context,obj,s,d,fn,_crater_rows(obj),KIND)
    r = fn(d, _crater_rows(obj))
    if not r['ok']:
        raise RuntimeError('nothing was built')
    parts = ['%d crater%s' % (r['craters'], '' if r['craters'] == 1 else 's')]
    if r['plates']:
        parts.append('%d plates' % r['plates'])
    if r['debris']:
        parts.append('%d debris' % r['debris'])
    return r, ', '.join(parts)


def add_marker(context, obj, mtype):
    s = G.settings_of(obj)
    local = obj.matrix_world.inverted_safe() @ context.scene.cursor.location
    if abs(local.x) > 0.5 * s.width or abs(local.y) > 0.5 * s.height:
        local.x, local.y = 0.3 * s.width, 0.2 * s.height
    local.z = 0.0
    return G.add_marker(context, obj, 'CRATER', local, max(0.6 * s.radius, 0.1), display='CIRCLE', name=obj.name + '_Crater', depth=1.0)


# ---------------------------------------------------------------------------
# materials

def _soil(nodes, links, out):
    scorch = G.attr_node(nodes, 'crater_scorch', y=250)
    ej = G.attr_node(nodes, 'crater_ejecta', y=0)
    tex = G.noise_tex(nodes, 6.0, x=-600, y=500)
    base = G.mix_color(nodes, links, tex.outputs['Fac'], (0.24, 0.19, 0.13), (0.33, 0.27, 0.19), x=-300, y=450)
    fresh = G.mix_color(nodes, links, ej.outputs['Fac'], base, (0.42, 0.35, 0.26), x=-100, y=350)
    burn = G.map_range(nodes, links, scorch.outputs['Fac'], 0.0, 1.0, x=-300, y=150)
    col = G.mix_color(nodes, links, burn, fresh, (0.05, 0.045, 0.04), x=150, y=250)
    b = G.principled(nodes, links, out, (0.3, 0.25, 0.18), 0.0, 0.95)
    links.new(col, b.inputs['Base Color'])


def _soil_body(nodes, links, out):
    tex = G.noise_tex(nodes, 10.0, x=-400)
    col = G.mix_color(nodes, links, tex.outputs['Fac'], (0.30, 0.24, 0.17), (0.40, 0.33, 0.24), x=0)
    b = G.principled(nodes, links, out, (0.35, 0.28, 0.2), 0.0, 0.95)
    links.new(col, b.inputs['Base Color'])


def _asphalt(nodes, links, out):
    scorch = G.attr_node(nodes, 'crater_scorch', y=250)
    tex = G.noise_tex(nodes, 80.0, x=-600, y=0)
    base = G.mix_color(nodes, links, tex.outputs['Fac'], (0.07, 0.07, 0.07), (0.16, 0.16, 0.155), x=-300, y=50)
    col = G.mix_color(nodes, links, scorch.outputs['Fac'], base, (0.025, 0.022, 0.02), x=0, y=150)
    b = G.principled(nodes, links, out, (0.1, 0.1, 0.1), 0.0, 0.85)
    links.new(col, b.inputs['Base Color'])


def _asphalt_edge(nodes, links, out):
    tex = G.noise_tex(nodes, 120.0, x=-400)
    col = G.mix_color(nodes, links, tex.outputs['Fac'], (0.10, 0.10, 0.10), (0.30, 0.29, 0.27), x=0)
    b = G.principled(nodes, links, out, (0.18, 0.18, 0.17), 0.0, 0.9)
    links.new(col, b.inputs['Base Color'])


def _stone(nodes, links, out):
    tex = G.noise_tex(nodes, 30.0, x=-400)
    col = G.mix_color(nodes, links, tex.outputs['Fac'], (0.36, 0.34, 0.31), (0.52, 0.50, 0.46), x=0)
    b = G.principled(nodes, links, out, (0.45, 0.43, 0.4), 0.0, 0.8)
    links.new(col, b.inputs['Base Color'])


def materials():
    return [G.node_material('PD Crater Soil', _soil), G.node_material('PD Crater Soil Body', _soil_body),
            G.node_material('PD Crater Pavement', _asphalt), G.node_material('PD Crater Pavement Edge', _asphalt_edge),
            G.node_material('PD Crater Stone', _stone)]


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
    col = G.box(layout, 'Ground', 'MESH_PLANE')
    col.prop(s, 'blast', text='')
    row = col.row(align=True)
    if s.mode!='OBJECT':
        row.prop(s, 'width');row.prop(s, 'height')
    else:row.label(text='Size follows the target mesh',icon='INFO')
    col.prop(s, 'ground_thickness')
    row = col.row(align=True)
    row.prop(s, 'terrain')
    row.prop(s, 'terrain_scale', text='Scale')
    col.prop(s, 'resolution_quality')
    col.prop(s, 'seed')

    col = G.box(layout, 'Crater', 'MOD_EXPLODE')
    row = col.row(align=True)
    row.prop(s, 'main_crater', text='Main', toggle=True)
    row.operator('project_damage.gen_add_marker', text='Crater Point', icon='ADD').marker = 'CRATER'
    n = len(G.marker_rows(obj, 'CRATER'))
    if n:
        col.label(text='%d Crater Point%s: move / scale the Empties' % (n, '' if n == 1 else 's'), icon='EMPTY_DATA')
    row = col.row(align=True)
    row.prop(s, 'radius')
    row.prop(s, 'depth', text='Depth')
    row = col.row(align=True)
    row.prop(s, 'rim_height')
    row.prop(s, 'floor_flat')
    col.prop(s, 'irregularity')
    row = col.row(align=True)
    row.prop(s, 'asymmetry')
    row.prop(s, 'direction', text='Dir')

    col = G.box(layout, 'Thrown-out Soil', 'FORCE_TURBULENCE')
    col.prop(s, 'ejecta')
    row = col.row(align=True)
    row.prop(s, 'rays')
    row.prop(s, 'ray_count', text='Count')
    row = col.row(align=True)
    row.prop(s, 'clods')
    row.prop(s, 'clod_size', text='Size')
    col.prop(s, 'scorch')

    col = G.box(layout, 'Surface', 'MOD_REMESH')
    col.prop(s, 'pavement', text='')
    if s.pavement != 'NONE':
        row = col.row(align=True)
        row.prop(s, 'pavement_thickness')
        row.prop(s, 'hole_size')
        if s.pavement == 'CONCRETE':
            col.prop(s, 'slab_size')
        row = col.row(align=True)
        row.prop(s, 'plate_size')
        row.prop(s, 'crack_reach', text='Zone')
        row = col.row(align=True)
        row.prop(s, 'crack_width')
        row.prop(s, 'plate_tilt', text='Tilt')
        row = col.row(align=True)
        row.prop(s, 'radial_cracks')
        row.prop(s, 'crack_length', text='Length')

    col = G.box(layout, 'Debris', 'PHYSICS')
    row = col.row(align=True)
    row.prop(s, 'debris')
    row.prop(s, 'debris_size', text='Size')
    row = col.row(align=True)
    row.prop(s, 'debris_spread')
    row.prop(s, 'debris_inside')
    col.prop(s, 'uv_scale')
    col.prop(s, 'max_verts')


KIND = G.Kind(KEY, 'Blast Crater', 'FORCE_FORCE', PROP, build, draw, materials,
              description='Ground with a blast crater: bowl, rim, thrown-out soil, broken asphalt or concrete, debris',
              smooth_angle=40.0, add_object=add_object)
KIND.add_marker = add_marker


# подключаем свойства и действия этого модуля к Blender
def register():
    bpy.utils.register_class(ProjectDamageCraterGenSettings)
    setattr(bpy.types.Object, PROP, PointerProperty(type=ProjectDamageCraterGenSettings))
    G.register_kind(KIND)


# убираем подключения модуля при выключении аддона
def unregister():
    G.KINDS.pop(KEY, None)
    if hasattr(bpy.types.Object, PROP):
        delattr(bpy.types.Object, PROP)
    if ProjectDamageCraterGenSettings.is_registered:
        bpy.utils.unregister_class(ProjectDamageCraterGenSettings)
