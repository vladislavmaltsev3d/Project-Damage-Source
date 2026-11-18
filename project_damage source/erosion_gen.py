# настройки и материалы Weathered Stone
# ProjectDamage Erosion generator - GPL-3.0-or-later; see LICENSE.
#
# Stone or concrete worn by weather, built by the native core (erosion.cpp,
# build_erosion) in a voxel volume: rounded worn edges (concave corners stay),
# chunks broken off the edges, granular surface, bedding grooves, pits,
# honeycomb hollows and exposed aggregate. The shape is a generated block,
# column, boulder or ashlar wall, or any closed mesh (the Source): with a mesh
# selected, Add makes an eroded copy of it that follows edits of the source.
# Wear Zones are child Empties: move / scale one to weather that spot more.
import bpy
import numpy as np
from bpy.props import BoolProperty, EnumProperty, FloatProperty, FloatVectorProperty, IntProperty, PointerProperty, StringProperty
from mathutils import Matrix

from . import damage_gen_common as G

KEY = 'EROSION'
PROP = 'project_damage_erosion_gen'
SHAPES = {'SOURCE': 0, 'BLOCK': 1, 'COLUMN': 2, 'BOULDER': 3, 'WALL': 4}


def _u(self, context):
    G.settings_changed(self, context)


def _dist(name, default, description, soft_max=1.0, minimum=0.0, precision=3):
    return FloatProperty(name=name, default=default, description=description, min=minimum, soft_max=soft_max,
                         subtype='DISTANCE', unit='LENGTH', precision=precision, update=_u)


def _fac(name, default, description, maximum=1.0, minimum=0.0, soft_max=None):
    return FloatProperty(name=name, default=default, description=description, min=minimum, max=maximum,
                         soft_max=soft_max if soft_max is not None else maximum, subtype='FACTOR', update=_u)


# stone types: only the weathering settings, the shape stays
PRESETS = {
    'STONE': dict(edge_round=0.025, edge_loss=0.5, edge_loss_size=0.05, roughness=0.0015, rough_scale=0.015, strata=0.0,
                  pits=0.5, pit_size=0.006, pit_depth=0.5, cavities=0.0, aggregate=0.0),
    'SANDSTONE': dict(edge_round=0.035, edge_loss=0.35, edge_loss_size=0.05, roughness=0.0012, rough_scale=0.01, strata=0.004,
                      strata_spacing=0.04, pits=0.4, pit_size=0.006, pit_depth=0.6, cavities=0.7, cavity_size=0.1,
                      cavity_depth=0.7, aggregate=0.0),
    'LIMESTONE': dict(edge_round=0.02, edge_loss=0.3, edge_loss_size=0.04, roughness=0.001, rough_scale=0.02, strata=0.0,
                      pits=0.9, pit_size=0.005, pit_depth=0.8, cavities=0.25, cavity_size=0.04, cavity_depth=0.6, aggregate=0.0),
    'GRANITE': dict(edge_round=0.012, edge_loss=0.35, edge_loss_size=0.06, roughness=0.002, rough_scale=0.008, strata=0.0,
                    pits=0.15, pit_size=0.005, pit_depth=0.4, cavities=0.0, aggregate=0.0),
    'CONCRETE': dict(edge_round=0.008, edge_loss=0.6, edge_loss_size=0.04, roughness=0.001, rough_scale=0.01, strata=0.0,
                     pits=0.6, pit_size=0.0035, pit_depth=0.7, cavities=0.0, aggregate=0.65, aggregate_size=0.012,
                     cement_loss=0.003),
}


def _preset(self, context):
    if self.stone != 'CUSTOM':
        G._building = True
        try:
            for k, v in PRESETS[self.stone].items():
                setattr(self, k, v)
        finally:
            G._building = False
    G.settings_changed(self, context)


def _source_poll(self, obj):
    return obj.type == 'MESH' and G.kind_of(obj) is None and obj != self.id_data


# здесь хранятся значения, которые сохраняются вместе с объектом
class ProjectDamageErosionGenSettings(bpy.types.PropertyGroup):
    live_update: BoolProperty(name='Live Update', default=True, description='Rebuild when a setting, a Wear Zone or the source changes')
    report: StringProperty(name='Report', default='')
    seed: IntProperty(name='Seed', default=0, min=0, max=2 ** 31 - 1, soft_max=1000, update=_u, description='Random seed')
    shape: EnumProperty(name='Shape', default='SOURCE', update=_u, description='What is weathered',
                        items=(('SOURCE', 'Source Mesh', 'A closed mesh object', 'MESH_DATA', 0),
                               ('BLOCK', 'Block', 'A stone block', 'MESH_CUBE', 1),
                               ('COLUMN', 'Column', 'A round column', 'MESH_CYLINDER', 2),
                               ('BOULDER', 'Boulder', 'A lumpy rock on the ground', 'MESH_ICOSPHERE', 3),
                               ('WALL', 'Ashlar Wall', 'A wall of stones laid in mortar', 'MOD_BUILD', 4)))
    source: PointerProperty(name='Source', type=bpy.types.Object, poll=_source_poll, update=_u,
                            description='The mesh to weather (closed; its scale is applied, its placement is not)')
    hide_source: BoolProperty(name='Hide Source', default=True, update=_u, description='Hide the source in the viewport and in renders')
    stone: EnumProperty(name='Stone', default='STONE', update=_preset, description='Weathering preset',
                        items=(('STONE', 'Weathered Stone', 'Rounded edges, chips, small pits'),
                               ('SANDSTONE', 'Sandstone', 'Soft layers, honeycomb hollows'),
                               ('LIMESTONE', 'Limestone', 'Dissolved: many pits and small hollows'),
                               ('GRANITE', 'Granite', 'Hard: little rounding, rough grain'),
                               ('CONCRETE', 'Old Concrete', 'Spalled edges, pores, exposed aggregate'),
                               ('CUSTOM', 'Custom', 'Own settings')))
    size_x: _dist('Size X', 0.6, 'Size along X', soft_max=5.0, minimum=0.01)
    size_y: _dist('Size Y', 0.4, 'Size along Y (wall: thickness)', soft_max=5.0, minimum=0.01)
    size_z: _dist('Size Z', 0.4, 'Height', soft_max=5.0, minimum=0.01)
    shape_bevel: _dist('Bevel', 0.003, 'The shape\'s own small edge round before weathering', soft_max=0.05, precision=4)
    block_width: _dist('Stone Length', 0.4, 'Length of the wall stones', soft_max=2.0, minimum=0.02)
    block_height: _dist('Course Height', 0.2, 'Height of a course of stones', soft_max=1.0, minimum=0.02)
    joint_width: _dist('Joint', 0.012, 'Width of the mortar joints', soft_max=0.05, precision=4)
    mortar_recess: _dist('Mortar Recess', 0.008, 'The mortar sits this far behind the stone faces', soft_max=0.05, precision=4)
    resolution_quality: G.resolution_property(0.006)
    resolution: _dist('Resolution', 0.006, 'Voxel size: smaller is finer and slower', soft_max=0.03, minimum=0.0005, precision=4)
    max_voxels: IntProperty(name='Max Voxels', default=8000000, min=1000, max=200000000, soft_max=40000000, update=_u,
                            description='Upper limit: the resolution is coarsened to stay below it')
    amount: _fac('Amount', 1.0, 'Overall weathering strength', maximum=2.0)
    variation: _fac('Variation', 0.5, 'Patches (and wall stones) weathered more and less')
    variation_scale: _dist('Patch Size', 0.25, 'Size of the more and less weathered patches', soft_max=2.0, minimum=0.01)
    exposure: _fac('Exposure', 0.3, 'Faces turned to the weather direction wear more')
    exposure_dir: FloatVectorProperty(name='Weather Direction', default=(0.0, 0.0, 1.0), subtype='DIRECTION', size=3, update=_u,
                                      description='Where the weather comes from, in the object\'s space')
    edge_round: _dist('Edge Round', 0.025, 'Radius of the worn, rounded edges (concave corners stay)', soft_max=0.15, precision=4)
    edge_loss: _fac('Edge Chips', 0.5, 'Chunks broken off along the edges', maximum=1.5)
    edge_loss_size: _dist('Chip Size', 0.05, 'Size of the chunks lost at the edges', soft_max=0.3)
    roughness: _dist('Roughness', 0.0015, 'Granular surface: amplitude', soft_max=0.01, precision=4)
    rough_scale: _dist('Grain', 0.015, 'Granular surface: feature size', soft_max=0.1, minimum=0.0005, precision=4)
    strata: _dist('Layers', 0.0, 'Bedding layers: depth of the grooves (0: none)', soft_max=0.02, precision=4)
    strata_spacing: _dist('Layer Spacing', 0.05, 'Thickness of the bedding layers', soft_max=0.3, minimum=0.002)
    strata_angle: FloatProperty(name='Layer Tilt', default=0.0, min=-90.0, max=90.0, update=_u, description='Tilt of the layers, degrees')
    pits: _fac('Pits', 0.5, 'Small pits in the surface')
    pit_size: _dist('Pit Size', 0.006, 'Size of the pits', soft_max=0.03, precision=4)
    pit_depth: _fac('Pit Depth', 0.5, 'Depth of the pits', maximum=1.5)
    cavities: _fac('Hollows', 0.0, 'Larger hollows: honeycomb weathering, tafoni')
    cavity_size: _dist('Hollow Size', 0.06, 'Size of the hollows', soft_max=0.3)
    cavity_depth: _fac('Hollow Depth', 0.5, 'Depth of the hollows (deep ones are undercut)', maximum=1.5)
    aggregate: _fac('Aggregate', 0.0, 'Concrete: exposed aggregate stones')
    aggregate_size: _dist('Stone Size', 0.012, 'Size of the aggregate stones', soft_max=0.05, precision=4)
    cement_loss: _dist('Cement Loss', 0.003, 'The cement between the stones is worn back this far', soft_max=0.02, precision=4)
    min_piece: _fac('Min Piece', 0.002, 'Loose bits smaller than this share of the faces are dropped', maximum=0.2)
    uv_scale: FloatProperty(name='UV Scale', default=1.0, min=0.0001, soft_max=10.0, update=_u, description='UV units per metre (box mapping)')


def _convert(d, s):
    d['shape'] = SHAPES[s.shape]
    v = s.exposure_dir
    d['exposure_x'], d['exposure_y'], d['exposure_z'] = float(v[0]), float(v[1]), float(v[2])


def _source_arrays(context, s):
    """The source mesh (evaluated, triangulated) in its own space with its scale applied."""
    src = s.source
    if src is None:
        raise RuntimeError('pick a Source mesh (or another shape)')
    dg = context.evaluated_depsgraph_get()
    ev = src.evaluated_get(dg)
    me = ev.to_mesh()
    try:
        me.calc_loop_triangles()
        nv, nt = len(me.vertices), len(me.loop_triangles)
        if nt == 0:
            raise RuntimeError('the source %s has no faces' % src.name)
        pts = np.zeros(nv * 3, dtype=np.float64)
        me.vertices.foreach_get('co', pts)
        tris = np.zeros(nt * 3, dtype=np.int32)
        me.loop_triangles.foreach_get('vertices', tris)
    finally:
        ev.to_mesh_clear()
    scale = np.array(src.matrix_world.to_scale(), dtype=np.float64)
    pts = pts.reshape(-1, 3) * scale
    return np.ascontiguousarray(pts), np.ascontiguousarray(tris.reshape(-1, 3))


def _zone_rows(obj):
    rows = []
    for m, e in G.marker_rows(obj, 'WEAR'):
        sc = m.to_scale()
        rows.append((m.translation.x, m.translation.y, m.translation.z,
                     e.empty_display_size * max(abs(sc.x), abs(sc.y), abs(sc.z), 1e-6), float(e.get('strength', 1.0))))
    return np.ascontiguousarray(np.array(rows, dtype=np.float64).reshape(-1, 5))


def _track_source(obj, s):
    for users in G._source_users.values():
        users.discard(obj.name)
    if s.shape == 'SOURCE' and s.source is not None:
        G._source_users.setdefault(s.source.name, set()).add(obj.name)
        hide = bool(s.hide_source)
        if s.source.hide_render != hide:
            s.source.hide_render = hide
        try:
            if s.source.hide_get() != hide:
                s.source.hide_set(hide)
        except RuntimeError:   # not in the view layer
            pass


def build(context, obj, s):
    c = G.core()
    fn = G.require(c, 'build_erosion')
    d = G.params_dict(c, 'erosion_defaults', s, convert=_convert)
    if s.shape == 'SOURCE':
        pts, tris = _source_arrays(context, s)
        obj[G.SOURCE] = s.source.name
    else:
        pts, tris = np.zeros((0, 3)), np.zeros((0, 3), dtype=np.int32)
        if G.SOURCE in obj:
            del obj[G.SOURCE]
    _track_source(obj, s)
    r = fn(d, pts, tris, _zone_rows(obj))
    if not r['ok']:
        raise RuntimeError(r.get('error') or 'nothing was built')
    parts = ['%.1f mm voxels' % (r['resolution'] * 1000.0)]
    for key, label in (('chunks', 'chips'), ('cavities', 'hollows'), ('pits', 'pits'), ('stones', 'stones')):
        if r[key]:
            parts.append('%d %s' % (r[key], label))
    return r, ', '.join(parts)


def add_marker(context, obj, mtype):
    s = G.settings_of(obj)
    local = obj.matrix_world.inverted_safe() @ context.scene.cursor.location
    if local.length > 3.0 * max(s.size_x, s.size_y, s.size_z):
        local = local * 0.0
    size = 0.3 * max(s.size_x, s.size_y, s.size_z) if s.shape != 'SOURCE' else 0.2
    return G.add_marker(context, obj, 'WEAR', local, size, display='SPHERE', name=obj.name + '_Wear', strength=1.0)


# ---------------------------------------------------------------------------
# materials

def _stone(nodes, links, out):
    cav = G.attr_node(nodes, 'erosion_cavity', y=250)
    edge = G.attr_node(nodes, 'erosion_edge', y=0)
    wear = G.attr_node(nodes, 'erosion_wear', y=-250)
    tex = G.noise_tex(nodes, 25.0, x=-600, y=500)
    base = G.mix_color(nodes, links, tex.outputs['Fac'], (0.50, 0.47, 0.42), (0.62, 0.59, 0.53), x=-300, y=450)
    weathered = G.mix_color(nodes, links, wear.outputs['Fac'], base, (0.46, 0.44, 0.40), x=-100, y=350)
    col = G.mix_color(nodes, links, edge.outputs['Fac'], weathered, (0.70, 0.67, 0.61), x=100, y=250)
    dirt = G.map_range(nodes, links, cav.outputs['Fac'], 0.0, 0.7, x=-300, y=150)
    col = G.mix_color(nodes, links, dirt, col, (0.18, 0.17, 0.14), x=300, y=150)
    b = G.principled(nodes, links, out, (0.55, 0.52, 0.47), 0.0, 0.9)
    links.new(col, b.inputs['Base Color'])


def _fresh(nodes, links, out):
    tex = G.noise_tex(nodes, 60.0, x=-400)
    col = G.mix_color(nodes, links, tex.outputs['Fac'], (0.66, 0.63, 0.57), (0.76, 0.73, 0.67), x=0)
    b = G.principled(nodes, links, out, (0.7, 0.67, 0.6), 0.0, 0.8)
    links.new(col, b.inputs['Base Color'])


def _aggregate(nodes, links, out):
    tex = G.noise_tex(nodes, 120.0, x=-400)
    col = G.mix_color(nodes, links, tex.outputs['Fac'], (0.30, 0.28, 0.25), (0.52, 0.47, 0.40), x=0)
    b = G.principled(nodes, links, out, (0.4, 0.37, 0.33), 0.0, 0.55)
    links.new(col, b.inputs['Base Color'])


def _mortar(nodes, links, out):
    cav = G.attr_node(nodes, 'erosion_cavity', y=0)
    col = G.mix_color(nodes, links, cav.outputs['Fac'], (0.66, 0.64, 0.58), (0.30, 0.28, 0.24), x=0)
    b = G.principled(nodes, links, out, (0.66, 0.64, 0.58), 0.0, 0.95)
    links.new(col, b.inputs['Base Color'])


def materials():
    return [G.node_material('PD Weathered Stone', _stone), G.node_material('PD Fresh Break', _fresh),
            G.node_material('PD Aggregate', _aggregate), G.node_material('PD Mortar', _mortar)]


def add_object(op, context):
    src = context.active_object
    use_src = src is not None and src.type == 'MESH' and G.kind_of(src) is None and src.select_get()
    name = (src.name + ' Eroded') if use_src else 'Erosion'
    mesh = bpy.data.meshes.new(name)
    obj = bpy.data.objects.new(name, mesh)
    (src.users_collection[0] if use_src and src.users_collection else context.collection).objects.link(obj)
    if use_src:
        loc, rot, _sc = src.matrix_world.decompose()
        obj.matrix_world = Matrix.LocRotScale(loc, rot, None)
    else:
        obj.location = context.scene.cursor.location
    obj[G.STAMP] = KEY
    if use_src:
        s = G.settings_of(obj)
        G._building = True
        try:
            s.source = src
            s.shape = 'SOURCE'
            dims = np.array(src.dimensions)
            # about 160 voxels along the longest side, at least 0.5 mm
            s.resolution = float(max(dims.max() / 160.0, 0.0005))
        finally:
            G._building = False
    else:
        # Added at the 3D cursor with no mesh to weather: start from a stone
        # block instead of the Source Mesh shape, which had nothing to read
        # and cancelled with "pick a Source mesh".
        s = G.settings_of(obj)
        G._building = True
        try:
            s.shape = 'BLOCK'
        finally:
            G._building = False
    return obj


# собираем видимые настройки этого раздела
def draw(layout, s, context, obj):
    col = G.box(layout, 'Source Mesh', 'MESH_DATA')
    col.prop(s, 'source', text='Source')
    col.prop(s, 'resolution_quality')
    col.prop(s, 'seed')

    col = G.box(layout, 'Weathering', 'MOD_SMOOTH')
    col.prop(s, 'stone', text='')
    col.prop(s, 'amount')
    row = col.row(align=True)
    row.prop(s, 'variation')
    row.prop(s, 'variation_scale', text='Patch')
    col.prop(s, 'exposure')
    if s.exposure > 0:
        col.prop(s, 'exposure_dir', text='')
    col = G.box(layout, 'Edges', 'MOD_BEVEL')
    col.prop(s, 'edge_round')
    row = col.row(align=True)
    row.prop(s, 'edge_loss')
    row.prop(s, 'edge_loss_size', text='Size')

    col = G.box(layout, 'Surface', 'TEXTURE')
    row = col.row(align=True)
    row.prop(s, 'roughness')
    row.prop(s, 'rough_scale')
    col.prop(s, 'strata')
    if s.strata > 0:
        row = col.row(align=True)
        row.prop(s, 'strata_spacing', text='Spacing')
        row.prop(s, 'strata_angle', text='Tilt')

    col = G.box(layout, 'Pits and Hollows', 'MESH_UVSPHERE')
    row = col.row(align=True)
    row.prop(s, 'pits')
    row.prop(s, 'pit_size', text='Size')
    col.prop(s, 'pit_depth')
    row = col.row(align=True)
    row.prop(s, 'cavities')
    row.prop(s, 'cavity_size', text='Size')
    col.prop(s, 'cavity_depth')

    col = G.box(layout, 'Concrete', 'MOD_REMESH')
    row = col.row(align=True)
    row.prop(s, 'aggregate')
    row.prop(s, 'aggregate_size', text='Size')
    if s.aggregate > 0:
        col.prop(s, 'cement_loss')
    col.prop(s, 'min_piece')
    col.prop(s, 'uv_scale')
    col.prop(s, 'max_voxels')


KIND = G.Kind(KEY, 'Erosion', 'MOD_SMOOTH', PROP, build, draw, materials,
              description='Weathered stone or concrete: rounded and chipped edges, pits, hollows, layers. '
                          'With a mesh selected, an eroded copy of it',
              smooth_angle=180.0, add_object=add_object)
KIND.add_marker = add_marker


# подключаем свойства и действия этого модуля к Blender
def register():
    bpy.utils.register_class(ProjectDamageErosionGenSettings)
    setattr(bpy.types.Object, PROP, PointerProperty(type=ProjectDamageErosionGenSettings))
    G.register_kind(KIND)


# убираем подключения модуля при выключении аддона
def unregister():
    G.KINDS.pop(KEY, None)
    if hasattr(bpy.types.Object, PROP):
        delattr(bpy.types.Object, PROP)
    if ProjectDamageErosionGenSettings.is_registered:
        bpy.utils.unregister_class(ProjectDamageErosionGenSettings)
