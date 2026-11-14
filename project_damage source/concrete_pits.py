# мелкие поры и отслоения бетона, а также служебное создание карт
from . import quality_ui
# SPDX-License-Identifier: GPL-3.0-or-later
"""Concrete Pitting: air voids, small chipped dents and flaked-off cement skin
on cast concrete, displaced into the source mesh (core: concrete_pits.cpp).

- Zones: a vertex group (Weight Paint). Weight 1 = full pitting, 0 = none;
  in between it thins the pores and dents out and shallows them.
- Nothing tiles on the mesh: every feature is a function of the object-space
  position, seeded per pore, with no repeating lattice.
- Bake Seamless Map writes the same relief as a tileable height map and a
  normal map, and builds a node group that mixes three differently scaled
  copies of it so the repeat does not show either.

build.rebuild_object calls before_core() (fills params.pits and the zone
weights) and fingerprints() calls hash_params()."""
from .blender_compat import uses_nodes
import os

import bpy
import numpy as np

from . import live

GROUP_NAME = 'PD_Pits'
NODE_GROUP = 'PD Concrete Pits Texture'


def _length(name, default, description, soft_max=None, minimum=0.0):
    return bpy.props.FloatProperty(name=name, default=default, min=minimum,
                                   soft_max=soft_max if soft_max is not None else max(default * 4.0, 1.0),
                                   description=description + ' (cm)', update=live.touch_prop)


def _factor(name, default, description, maximum=1.0, soft_max=None):
    return bpy.props.FloatProperty(name=name, default=default, min=0.0, max=maximum,
                                   soft_max=soft_max if soft_max is not None else maximum,
                                   description=description, update=live.touch_prop)


PROPS = {
    'pits_enable': bpy.props.BoolProperty(
        name='Concrete Pitting', default=False, update=live.touch_prop,
        description='Small pores, chipped dents and flaked-off cement skin displaced into the surface, '
                    'before any other damage is cut'),
    'pits_seed': bpy.props.IntProperty(name='Seed', default=0, min=0, update=live.touch_prop,
                                       description='Variation, mixed with the main Seed'),
    'pits_group': bpy.props.StringProperty(
        name='Zones', default='', update=live.touch_prop,
        description='Vertex group painting where the pitting goes (1 = full, 0 = none). '
                    'Empty = the whole object'),
    'pits_zone_feather': _factor('Zone Feather', 0.25, 'Weights below this fade the relief out softly'),
    'pits_avoid': bpy.props.BoolProperty(name='Avoid Selection', default=True, update=live.touch_prop,
                                         description='No pitting on the Avoid faces'),
    'pits_strength': bpy.props.FloatProperty(name='Strength', default=1.0, min=0.0, soft_max=3.0,
                                             update=live.touch_prop, description='Multiplies every depth'),
    'pits_thickness_cap': _factor('Thickness Cap', 0.25, 'Inward travel never exceeds this share of the wall behind a point (0 = off)'),
                
    'pits_edge_length': _length('Edge Length', 0.3, 'Edge length over the zone: carries the grain, waviness and dents', 2.0, 0.01),
    'pits_fine_edge': _length('Fine Edge', 0.04, 'Smallest edge, used inside the pores and the flaked patches. '
                              'Pores narrower than about three of these are left to the baked map', 0.2, 0.005),
    'pits_detail': bpy.props.FloatProperty(name='Resolution', default=1.0, min=0.05, max=4.0, update=live.touch_prop,
                                           description='Multiplies the resolution (2 = half the edge lengths)'),
    'pits_max_tris': bpy.props.IntProperty(name='Max Triangles', default=500000, min=10000, soft_max=10000000,
                                           update=live.touch_prop,
                                           description='Triangle budget; the edges grow to fit it on big objects'),
           
    'pits_pore_density': bpy.props.FloatProperty(name='Pores', default=0.0, min=0.0, soft_max=40.0, update=live.touch_prop,
                                                 description='Pores per 10 x 10 cm at weight 1, before clustering'),
    'pits_pore_size_min': _length('Size Min', 0.2, 'Smallest pore diameter', 2.0, 0.005),
    'pits_pore_size_max': _length('Size Max', 1.2, 'Largest pore diameter', 5.0, 0.005),
    'pits_pore_small_bias': _factor('Small Bias', 0.6, '0 = sizes spread evenly, 1 = almost all small'),
    'pits_pore_depth': _factor('Depth', 0.45, 'Depth as a share of the diameter', 2.0, 1.0),
    'pits_pore_sharp': _factor('Sharp Walls', 0.65, '0 = soft bowls, 1 = sheer walls with a flat floor'),
    'pits_pore_jagged': _factor('Jagged', 0.7, 'Irregular, lobed outline', 1.5, 1.5),
    'pits_pore_stretch': _factor('Stretch', 0.45, 'Elongation (1 = up to 2.5 : 1)'),
    'pits_pore_rim': _factor('Crumbled Rim', 0.35, 'Chipped rim around the opening, as a share of the radius', 2.0, 1.0),
    'pits_pore_merge': _factor('Merged Groups', 0.35, 'Share of pores that are a group of 2-4 voids run together'),
    'pits_clustering': _factor('Clustering', 0.6, '0 = even scatter, 1 = pores gather in bands and patches'),
    'pits_cluster_scale': _length('Cluster Size', 30.0, 'Size of those gatherings', 150.0, 0.1),
             
    'pits_flake_enable': bpy.props.BoolProperty(name='Flaking', default=True, update=live.touch_prop,
                                                description='Patches where the cement skin has chipped off down to the sand'),
    'pits_flake_coverage': _factor('Coverage', 0.12, 'Share of the zone that has lost its skin (at weight 1)'),
    'pits_flake_scale': _length('Patch Size', 22.0, 'Size of the flaked patches', 100.0, 0.1),
    'pits_flake_depth': _length('Depth', 0.22, 'Depth of the deepest terrace', 1.0, 0.0),
    'pits_flake_layers': bpy.props.IntProperty(name='Terraces', default=2, min=1, max=4, update=live.touch_prop,
                                               description='Ledges the skin broke off in'),
    'pits_flake_jagged': _factor('Ragged Edge', 0.6, 'Ragged patch outline', 2.0, 2.0),
    'pits_flake_rough': _factor('Rough Floor', 0.6, 'Sand and aggregate on the broken floor, as a share of the depth', 2.0, 1.5),
    'pits_aggregate_size': _length('Grain Size', 0.5, 'Size of the sand / aggregate grains on that floor', 2.0, 0.01),
    'pits_flake_pores': bpy.props.FloatProperty(name='Pores Inside', default=5.0, min=0.0, soft_max=20.0, update=live.touch_prop,
                                                description='Extra pores inside a flaked patch (x Pores)'),
           
    'pits_dent_density': bpy.props.FloatProperty(name='Dents', default=0.35, min=0.0, soft_max=5.0, update=live.touch_prop,
                                                 description='Shallow dents per 10 x 10 cm at weight 1'),
    'pits_dent_size_min': _length('Size Min', 0.8, 'Smallest dent diameter', 5.0, 0.01),
    'pits_dent_size_max': _length('Size Max', 3.5, 'Largest dent diameter', 10.0, 0.01),
    'pits_dent_depth': _factor('Depth', 0.08, 'Depth as a share of the diameter', 0.5, 0.3),
    'pits_dent_chip': _factor('Chipped Side', 0.6, '0 = soft round dish, 1 = one side broken off steeply'),
             
    'pits_grain': _length('Grain', 0.012, 'Fine sand roughness height', 0.1),
    'pits_grain_size': _length('Grain Size', 0.25, 'Its feature size (fades out where the mesh is coarser)', 1.0, 0.01),
    'pits_undulation': _length('Waviness', 0.04, 'Broad, low waviness height', 0.3),
    'pits_undulation_scale': _length('Waviness Size', 45.0, 'Its feature size', 200.0, 0.1),
          
    'pits_bake_tile': _length('Tile Size', 100.0, 'Width of one tile of the baked map', 400.0, 1.0),
    'pits_bake_res': bpy.props.EnumProperty(name='Resolution', default='2048', update=None,
                                            items=(('1024', '1024', ''), ('2048', '2048', ''), ('4096', '4096', ''), ('8192', '8192', '')),
                                            description='Pixels across the tile'),
    'pits_bake_flakes': bpy.props.BoolProperty(name='Include Flaking', default=False,
                                               description='Bake the flaked patches too. Off by default: they are the '
                                                           'biggest features and the first to make a repeat noticeable'),
    'pits_bake_dir': bpy.props.StringProperty(name='Folder', default='//textures/', subtype='DIR_PATH',
                                              description='Where the maps are written'),
}

                                                                 
FIELDS = (
    ('seed', False), ('avoid', False), ('strength', False), ('thickness_cap', False),
    ('edge_length', True), ('fine_edge', True), ('detail', False), ('max_tris', False),
    ('pore_density', False), ('pore_size_min', True), ('pore_size_max', True), ('pore_small_bias', False),
    ('pore_depth', False), ('pore_sharp', False), ('pore_jagged', False), ('pore_stretch', False),
    ('pore_rim', False), ('pore_merge', False), ('clustering', False), ('cluster_scale', True),
    ('flake_enable', False), ('flake_coverage', False), ('flake_scale', True), ('flake_depth', True),
    ('flake_layers', False), ('flake_jagged', False), ('flake_rough', False), ('aggregate_size', True),
    ('flake_pores', False), ('dent_density', False), ('dent_size_min', True), ('dent_size_max', True),
    ('dent_depth', False), ('dent_chip', False), ('grain', True), ('grain_size', True),
    ('undulation', True), ('undulation_scale', True), ('zone_feather', False),
)


def wanted(p):
    return bool(getattr(p, 'pits_enable', False) and not p.all_off)


def _gscale(p):
    from .params import _clamp_scale
    return _clamp_scale(p.global_scale / 100.0)


def fill_block(block, p, gscale=1.0):
    for name, is_length in FIELDS:
        value = getattr(p, 'pits_' + name)
        if is_length:
            value = value * gscale
        setattr(block, name, value)


def _zone_weights(source, snap, prep, points):
    """Per core input point, or None (no group set / group not found)."""
    name = source.project_damage.pits_group
    if not name:
        return None, None
    vg = source.vertex_groups.get(name)
    if vg is None:
        return None, "Concrete Pitting: vertex group '%s' not found; pitting the whole object" % name
    if snap is None:
        from .mesh_io import read_mask_weights
        w = read_mask_weights(source.data, vg.index)
    else:
        w = prep.point_values(snap.groups[vg.index])
    w = np.ascontiguousarray(np.clip(np.asarray(w, dtype=np.float64), 0.0, 1.0))
    if len(w) != len(points):
        return None, 'Concrete Pitting: the zone weights do not match the prepared mesh; pitting the whole object'
    if not np.any(w > 0.0):
        return w, "Concrete Pitting: the group '%s' is empty (all weights 0); nothing to pit" % name
    return w, None


# готовим данные этого эффекта перед расчётом геометрии
def before_core(context, source, params, snap, prep, points, polys, factor, reports):
    """build.rebuild_object, before the fingerprint and the core call."""
    p = source.project_damage
    block = getattr(params, 'pits', None)
    if not wanted(p):
        if block is not None:
            block.enable = False
            block.set_weights(np.zeros(0))
        return
    if block is None:
        reports.append(({'WARNING'}, 'Concrete Pitting: rebuild the core and restart Blender to use it'))
        return
    fill_block(block, p, _gscale(p))
    block.enable = True
    weights, note = _zone_weights(source, snap, prep, points)
    if note:
        reports.append(({'WARNING'}, note))
    block.set_weights(weights if weights is not None else np.zeros(0))
    if len(points) < 200 and weights is not None:
        reports.append(({'INFO'}, 'Concrete Pitting: the zones are read from the vertices, and this mesh has only %d; '
                         'subdivide it for finer zones' % len(points)))


def hash_params(h, params):
    """fingerprints(): the result mesh depends on every pits field and the weights."""
    block = getattr(params, 'pits', None)
    if block is None or not block.enable:
        return
    h.update(b'pits')
    h.update(repr(tuple(getattr(block, name) for name, _ in FIELDS)).encode())
    w = np.asarray(block.weights, dtype=np.float64)
    h.update(str(len(w)).encode())
    h.update(w.tobytes())


                                                                             
              

def _core():
    from . import core_loader
    return core_loader.load()


def height_map(p, core, main_seed=0, tile=None, res=None, flakes=None):
    """(res, res) float32 height in cm (0 = surface, negative = into it)."""
    block = core.ConcretePitsParams()
    fill_block(block, p, 1.0)
    block.enable = True
    if flakes is not None:
        block.flake_enable = bool(flakes and p.pits_flake_enable)
    tile = float(tile if tile is not None else p.pits_bake_tile)
    res = int(res if res is not None else p.pits_bake_res)
    return np.asarray(core.concrete_pits_height_map(block, int(main_seed), tile, res), dtype=np.float32)


def normal_map(h, pixel_cm):
    """Tangent-space normals (OpenGL, +Y up) from a tiling height map, wrapped."""
    gx = (np.roll(h, -1, axis=1) - np.roll(h, 1, axis=1)) / (2.0 * pixel_cm)
    gy = (np.roll(h, -1, axis=0) - np.roll(h, 1, axis=0)) / (2.0 * pixel_cm)
    n = np.dstack((-gx, -gy, np.ones_like(h)))
    n /= np.linalg.norm(n, axis=2, keepdims=True)
    return n * 0.5 + 0.5


def _save_png(path, rgb, depth16):
    """rgb: (H, W, 3) or (H, W) in 0..1, row 0 at the bottom (Blender's order)."""
    H, W = rgb.shape[:2]
    name = os.path.basename(path)
    img = bpy.data.images.new(name + '.tmp', W, H, alpha=False, float_buffer=True, is_data=True)
    try:
        px = np.ones((H, W, 4), dtype=np.float32)
        if rgb.ndim == 2:
            px[..., 0] = px[..., 1] = px[..., 2] = rgb
        else:
            px[..., :3] = rgb
        img.pixels.foreach_set(px.ravel())
        scene = bpy.context.scene
        settings = scene.render.image_settings
        keep = (settings.file_format, settings.color_mode, settings.color_depth,
                scene.view_settings.view_transform, scene.view_settings.look,
                scene.view_settings.exposure, scene.view_settings.gamma)
        try:
            settings.file_format = 'PNG'
            settings.color_mode = 'BW' if rgb.ndim == 2 else 'RGB'
            settings.color_depth = '16' if depth16 else '8'
                                                            
            scene.view_settings.view_transform = 'Raw' if 'Raw' in [i.identifier for i in scene.view_settings.bl_rna.properties['view_transform'].enum_items] else 'Standard'
            scene.view_settings.look = 'None'
            scene.view_settings.exposure = 0.0
            scene.view_settings.gamma = 1.0
            img.save_render(path, scene=scene)
        finally:
            (settings.file_format, settings.color_mode, settings.color_depth,
             scene.view_settings.view_transform, scene.view_settings.look,
             scene.view_settings.exposure, scene.view_settings.gamma) = keep
    finally:
        bpy.data.images.remove(img)


def _load_data_image(path):
    for img in bpy.data.images:
        if bpy.path.abspath(img.filepath) == path:
            img.reload()
            img.colorspace_settings.is_data = True
            return img
    img = bpy.data.images.load(path)
    img.colorspace_settings.is_data = True
    return img


def bake_maps(context, source, directory=None):
    """Writes <name>_height.png (16 bit, 1 = surface) and <name>_normal.png.
    Returns (height_path, normal_path, depth_cm, tile_cm)."""
    p = source.project_damage
    core = _core()
    if not getattr(core, 'concrete_pits', False):
        raise RuntimeError('Concrete Pitting needs the rebuilt core (restart Blender after installing it)')
    tile = float(p.pits_bake_tile)
    res = int(p.pits_bake_res)
    h = height_map(p, core, getattr(p, 'seed', 0), tile, res, flakes=p.pits_bake_flakes)
    lo, hi = float(h.min()), float(h.max())
    depth = max(hi - lo, 1e-6)
    directory = bpy.path.abspath(directory if directory is not None else p.pits_bake_dir)
    if not directory:
        directory = bpy.app.tempdir
    os.makedirs(directory, exist_ok=True)
    stem = bpy.path.clean_name(source.name) + '_pits'
    hp = os.path.join(directory, stem + '_height.png')
    npth = os.path.join(directory, stem + '_normal.png')
    _save_png(hp, (h - lo) / depth, True)
    _save_png(npth, normal_map(h, tile / res), False)
    img = _load_data_image(hp)
    img['pd_pits_depth_cm'] = depth
    img['pd_pits_surface'] = (0.0 - lo) / depth
    img['pd_pits_tile_cm'] = tile
    _load_data_image(npth)
    return hp, npth, depth, tile, (0.0 - lo) / depth


                                                                             
                        

def _link(nt, a, b):
    nt.links.new(a, b)


def ensure_node_group(height_img, tile_m, depth_m, surface_level):
    """Three copies of the tile at non-commensurate scales (1, 1.17, 0.89) and
    offsets, chosen between by low noise masks, give a pattern that never
    lines up again within any practical distance. Heights blend by MIN where
    the masks overlap, so pores stay crisp instead of fading into each other.
    Object coordinates, box projection: walls, floors and ceilings alike."""
    ng = bpy.data.node_groups.get(NODE_GROUP)
    if ng is None:
        ng = bpy.data.node_groups.new(NODE_GROUP, 'ShaderNodeTree')
    ng.nodes.clear()
    iface = ng.interface
    iface.clear()
    iface.new_socket('Tile Size', in_out='INPUT', socket_type='NodeSocketFloat')
    iface.new_socket('Strength', in_out='INPUT', socket_type='NodeSocketFloat')
    iface.new_socket('Break Tiling', in_out='INPUT', socket_type='NodeSocketFloat')
    iface.new_socket('Normal', in_out='INPUT', socket_type='NodeSocketVector')
    iface.new_socket('Normal', in_out='OUTPUT', socket_type='NodeSocketVector')
    iface.new_socket('Height', in_out='OUTPUT', socket_type='NodeSocketFloat')
    iface.new_socket('Pit Mask', in_out='OUTPUT', socket_type='NodeSocketFloat')
    items = {s.name + s.in_out: s for s in iface.items_tree}
    items['Tile SizeINPUT'].default_value = tile_m
    items['StrengthINPUT'].default_value = 1.0
    items['Break TilingINPUT'].default_value = 1.0
    items['Break TilingINPUT'].min_value = 0.0
    items['Break TilingINPUT'].max_value = 1.0
    items['NormalINPUT'].hide_value = True
    N = ng.nodes
    gin = N.new('NodeGroupInput'); gin.location = (-1400, 0)
    gout = N.new('NodeGroupOutput'); gout.location = (900, 0)
    tc = N.new('ShaderNodeTexCoord'); tc.location = (-1400, 300)
    div = N.new('ShaderNodeVectorMath'); div.operation = 'DIVIDE'; div.location = (-1200, 300)
    _link(ng, tc.outputs['Object'], div.inputs[0])
    comb = N.new('ShaderNodeCombineXYZ'); comb.location = (-1350, 120)
    for k in range(3):
        _link(ng, gin.outputs['Tile Size'], comb.inputs[k])
    _link(ng, comb.outputs[0], div.inputs[1])
    layers = []
    for i, (scale, off) in enumerate(((1.0, (0.0, 0.0, 0.0)), (1.17, (0.37, 0.61, 0.23)), (0.89, (0.71, 0.13, 0.52)))):
        mapn = N.new('ShaderNodeMapping'); mapn.location = (-1000, 300 - i * 260)
        mapn.inputs['Scale'].default_value = (scale, scale, scale)
        mapn.inputs['Location'].default_value = off
        _link(ng, div.outputs[0], mapn.inputs['Vector'])
        tex = N.new('ShaderNodeTexImage'); tex.location = (-800, 300 - i * 260)
        tex.image = height_img
        tex.projection = 'BOX'
        tex.projection_blend = 0.25
        tex.interpolation = 'Cubic'
        _link(ng, mapn.outputs[0], tex.inputs['Vector'])
        layers.append(tex)
                                                          
    def mask(seed_off, loc_y):
        nz = N.new('ShaderNodeTexNoise'); nz.location = (-800, loc_y)
        nz.inputs['Scale'].default_value = 0.9
        nz.inputs['Detail'].default_value = 1.0
        mp = N.new('ShaderNodeMapping'); mp.location = (-1000, loc_y)
        mp.inputs['Location'].default_value = seed_off
        _link(ng, div.outputs[0], mp.inputs['Vector'])
        _link(ng, mp.outputs[0], nz.inputs['Vector'])
        ramp = N.new('ShaderNodeMapRange'); ramp.location = (-600, loc_y)
        ramp.inputs['From Min'].default_value = 0.46
        ramp.inputs['From Max'].default_value = 0.54
        _link(ng, nz.outputs['Fac'], ramp.inputs['Value'])
        amt = N.new('ShaderNodeMath'); amt.operation = 'MULTIPLY'; amt.location = (-420, loc_y)
        _link(ng, ramp.outputs['Result'], amt.inputs[0])
        _link(ng, gin.outputs['Break Tiling'], amt.inputs[1])
        return amt.outputs[0]
    m1 = mask((13.1, 7.7, 3.3), -560)
    m2 = mask((-5.9, 21.4, 11.8), -760)
                                                                           
                                                                          
                                                                              
    S = surface_level

    def depth(col, x, y):
        d = N.new('ShaderNodeMath'); d.operation = 'SUBTRACT'; d.location = (x, y)
        d.inputs[0].default_value = S
        _link(ng, col, d.inputs[1])
        return d.outputs[0]

    def weight(m, sign, x, y):
        w = N.new('ShaderNodeMath'); w.operation = 'MULTIPLY_ADD'; w.use_clamp = True; w.location = (x, y)
        _link(ng, m, w.inputs[0])
        w.inputs[1].default_value = 2.0 * sign
        w.inputs[2].default_value = 2.0 if sign < 0 else 0.0
        return w.outputs[0]

    def combine(d_base, d_other, m, x):
        wb = weight(m, -1.0, x, -350)
        wo = weight(m, 1.0, x, -500)
        a = N.new('ShaderNodeMath'); a.operation = 'MULTIPLY'; a.location = (x + 170, 100)
        _link(ng, d_base, a.inputs[0]); _link(ng, wb, a.inputs[1])
        b = N.new('ShaderNodeMath'); b.operation = 'MULTIPLY'; b.location = (x + 170, -100)
        _link(ng, d_other, b.inputs[0]); _link(ng, wo, b.inputs[1])
        mx = N.new('ShaderNodeMath'); mx.operation = 'MAXIMUM'; mx.location = (x + 340, 0)
        _link(ng, a.outputs[0], mx.inputs[0]); _link(ng, b.outputs[0], mx.inputs[1])
        return mx.outputs[0]

    d = combine(depth(layers[0].outputs['Color'], -560, 300), depth(layers[1].outputs['Color'], -560, 40), m1, -380)
    d = combine(d, depth(layers[2].outputs['Color'], -560, -220), m2, -20)
    hgt_node = N.new('ShaderNodeMath'); hgt_node.operation = 'SUBTRACT'; hgt_node.location = (380, 0)
    hgt_node.inputs[0].default_value = S
    _link(ng, d, hgt_node.inputs[1])
    hgt = hgt_node.outputs[0]
                                            
    st = N.new('ShaderNodeMath'); st.operation = 'MULTIPLY'; st.location = (400, -200)
    st.inputs[1].default_value = depth_m
    _link(ng, gin.outputs['Strength'], st.inputs[0])
    bump = N.new('ShaderNodeBump'); bump.location = (600, 0)
    bump.inputs['Strength'].default_value = 1.0
    _link(ng, st.outputs[0], bump.inputs['Distance'])
    _link(ng, hgt, bump.inputs['Height'])
    _link(ng, gin.outputs['Normal'], bump.inputs['Normal'])
    _link(ng, bump.outputs['Normal'], gout.inputs['Normal'])
    _link(ng, hgt, gout.inputs['Height'])
    pm = N.new('ShaderNodeMapRange'); pm.location = (600, -250)
    pm.inputs['From Min'].default_value = max(surface_level - 0.02, 0.0)
    pm.inputs['From Max'].default_value = max(surface_level - 0.25, 0.0)
    _link(ng, hgt, pm.inputs['Value'])
    _link(ng, pm.outputs['Result'], gout.inputs['Pit Mask'])
    ng['pd_pits_surface'] = surface_level
    ng['pd_pits_depth_m'] = depth_m
    return ng


def add_to_material(mat, ng):
    """Puts the group in the material; feeds the Principled BSDF's Normal when
    nothing is plugged into it yet. Returns True when linked."""
    if mat is None or not uses_nodes(mat):
        return False
    nt = mat.node_tree
    node = next((n for n in nt.nodes if n.type == 'GROUP' and n.node_tree == ng), None)
    bsdf = next((n for n in nt.nodes if n.type == 'BSDF_PRINCIPLED'), None)
    if node is None:
        node = nt.nodes.new('ShaderNodeGroup')
        node.node_tree = ng
        node.label = 'Concrete Pits (no tiling)'
        node.location = (bsdf.location.x - 300, bsdf.location.y - 400) if bsdf else (-300, -300)
    if bsdf is not None and not bsdf.inputs['Normal'].is_linked:
        nt.links.new(node.outputs['Normal'], bsdf.inputs['Normal'])
        return True
    return False


                                                                             
           

def _source(context):
    from . import ownership
    return ownership.context_source(context)


# действие «Paint Zones»
class PROJECT_DAMAGE_OT_pits_paint_zones(bpy.types.Operator):
    bl_idname = 'project_damage.pits_paint_zones'
    bl_label = 'Paint Zones'
    bl_description = ('Create (or reuse) the PD_Pits vertex group, link it as the pitting zones '
                      'and switch to Weight Paint')
    bl_options = {'REGISTER', 'UNDO'}

    def execute(self, context):
        src = _source(context)
        if src is None or src.type != 'MESH':
            self.report({'ERROR'}, 'Select the source mesh')
            return {'CANCELLED'}
        p = src.project_damage
        name = p.pits_group or GROUP_NAME
        vg = src.vertex_groups.get(name)
        if vg is None:
            vg = src.vertex_groups.new(name=name)
        src.vertex_groups.active_index = vg.index
        p.pits_group = vg.name
        if context.view_layer.objects.active is not src:
            for o in context.selected_objects:
                o.select_set(False)
            src.hide_set(False)
            src.select_set(True)
            context.view_layer.objects.active = src
        try:
            bpy.ops.object.mode_set(mode='WEIGHT_PAINT')
        except RuntimeError as exc:
            self.report({'WARNING'}, str(exc))
        if len(src.data.vertices) < 400:
            self.report({'INFO'}, 'This mesh has %d vertices: subdivide it to paint finer zones' % len(src.data.vertices))
        return {'FINISHED'}


# действие «Bake Seamless Map»
class PROJECT_DAMAGE_OT_pits_bake_map(bpy.types.Operator):
    bl_idname = 'project_damage.pits_bake_map'
    bl_label = 'Bake Seamless Map'
    bl_description = ('Write the pitting as a tileable 16-bit height map and a normal map, and build the '
                      '"PD Concrete Pits Texture" node group that mixes three copies so no repeat shows')
    bl_options = {'REGISTER', 'UNDO'}

    add_material: bpy.props.BoolProperty(name='Add to Material', default=True,
                                         description="Add the node group to the source's active material "
                                                     'and feed its Principled BSDF Normal')

    def execute(self, context):
        src = _source(context)
        if src is None:
            self.report({'ERROR'}, 'Select the source object')
            return {'CANCELLED'}
        try:
            hp, npth, depth, tile, surface = bake_maps(context, src)
        except (RuntimeError, OSError, ValueError) as exc:
            self.report({'ERROR'}, str(exc))
            return {'CANCELLED'}
        from .params import unit_factor
        cm_per_unit = unit_factor(context)
        img = _load_data_image(hp)
        ng = ensure_node_group(img, tile / cm_per_unit, depth / cm_per_unit, surface)
        linked = False
        if self.add_material:
            mat = src.active_material
            from . import build
            res = build.result_of(src)
            if res is not None and res.active_material is not None:
                mat = res.active_material
            linked = add_to_material(mat, ng)
        self.report({'INFO'}, 'Concrete Pitting maps: %s (depth range %.2f cm, tile %.0f cm)%s'
                    % (os.path.dirname(hp), depth, tile, '; linked to the material Normal' if linked else ''))
        return {'FINISHED'}


_CLASSES = (PROJECT_DAMAGE_OT_pits_paint_zones, PROJECT_DAMAGE_OT_pits_bake_map)


                                                                             
    

def _section(layout, title):
    from . import effect_layout
    return effect_layout.section(layout, title)


# показываем настройки выбранного эффекта
def draw_settings(layout, context):
    src=_source(context)
    if src is None:return
    p=src.project_damage
    layout.use_property_split=False;layout.use_property_decorate=False
    box=_section(layout,'General');col=box.column(align=True)
    col.prop(p,'pits_strength');col.prop(p,'pits_seed')
    quality_ui.draw(col, p,'pits_detail',text='Resolution');col.prop(p,'pits_max_tris')
    col.prop(p,'pits_thickness_cap')
    if p.pits_pore_density>0:
        col.prop(p,'pits_edge_length');col.prop(p,'pits_fine_edge')
    box=_section(layout,'Flaking');row=box.row(align=True);row.alignment='CENTER'
    row.prop(p,'pits_flake_enable',text='Flaking',toggle=True,icon='CHECKMARK')
    if p.pits_flake_enable:
        col=box.column(align=True)
        col.prop(p, 'pits_flake_coverage')
        col.prop(p, 'pits_flake_scale')
        col.prop(p, 'pits_flake_depth')
        col.prop(p, 'pits_flake_layers')
        col.prop(p, 'pits_flake_jagged')
        col.prop(p, 'pits_flake_rough')
        col.prop(p, 'pits_aggregate_size')
        col.prop(p, 'pits_flake_pores')
    box=_section(layout,'Pores');col=box.column(align=True)
    col.prop(p,'pits_pore_density',text='Pores')
    if p.pits_pore_density>0:
        col.prop(p, 'pits_pore_size_min')
        col.prop(p, 'pits_pore_size_max')
        col.prop(p, 'pits_pore_small_bias')
        col.prop(p, 'pits_pore_depth')
        col.prop(p, 'pits_pore_sharp')
        col.prop(p, 'pits_pore_jagged')
        col.prop(p, 'pits_pore_stretch')
        col.prop(p, 'pits_pore_rim')
        col.prop(p, 'pits_pore_merge')
        col.prop(p, 'pits_clustering')
        if p.pits_clustering>0:col.prop(p,'pits_cluster_scale')
    box=_section(layout,'Dents');col=box.column(align=True)
    col.prop(p,'pits_dent_density',text='Dents')
    if p.pits_dent_density>0:
        col.prop(p, 'pits_dent_size_min')
        col.prop(p, 'pits_dent_size_max')
        col.prop(p, 'pits_dent_depth')
        col.prop(p, 'pits_dent_chip')
    box=_section(layout,'Surface');col=box.column(align=True)
    col.prop(p, 'pits_grain')
    col.prop(p, 'pits_grain_size')
    col.prop(p, 'pits_undulation')
    col.prop(p, 'pits_undulation_scale')


# подключаем свойства и действия этого модуля к Blender
def register():
    for cls in _CLASSES:
        bpy.utils.register_class(cls)


# убираем подключения модуля при выключении аддона
def unregister():
    for cls in reversed(_CLASSES):
        try:
            bpy.utils.unregister_class(cls)
        except RuntimeError:
            pass
