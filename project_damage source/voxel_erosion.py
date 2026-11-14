# эрозия объёма: качество сетки, маска и сглаживание повреждения
from . import quality_ui
# SPDX-License-Identifier: GPL-3.0-or-later
# Modified work: new effect added to ProjectDamage (Cornelius Dammrich, GPL-3).
"""Voxel Erosion: weathered, broken stone / concrete edges cut into the source
(core: voxel_erosion.cpp, effect stamp 32, DamageClass::Coating cutter).

Every closed piece is voxelised, rounded (edges and corners recede first),
displaced by a domain-warped noise and meshed again; the result keeps only
what lies inside that eroded shape. Size-independent by default: the look is
computed in a reference space where the piece is 2 units across.

build.rebuild_object calls before_core() (fills params.erosion) and
fingerprints() calls hash_params(); after effect_groups.tag_mesh it calls
after_result() (shade smooth + optional UV transfer on the eroded faces)."""

import bpy
import numpy as np

from . import live

STAMP = 32
GROUP_NAME = 'PD_Erosion'
DEFAULT_MATERIAL = 'ProjectDamage_Erosion'
NOISE_ITEMS = (
    ('ROCKY', 'Rocky', 'Hetero-terrain noise: broad, rocky erosion with soft valleys'),
    ('SOFT', 'Soft', 'fBm noise: smooth, even weathering'),
    ('CRACKED', 'Cracked', 'Voronoi edges: chipped breaks along a crack network'),
    ('CELLS', 'Cells', 'Smooth Voronoi cells: rounded, pebble-like bites'),
)
NOISE_INDEX = {key: i for i, (key, _, _) in enumerate(NOISE_ITEMS)}
MASK_ITEMS = (
    ('NONE', 'None', 'Erode everywhere'),
    ('GROUP', 'Vertex Group', 'Weight 1 = full erosion, 0 = untouched'),
)


def _mask_mode_get(self):
    value = self.get('erosion_mask_mode', 0)
    return 1 if value in (1, 'GROUP') else 0


def _mask_mode_set(self, value):
    self['erosion_mask_mode'] = 1 if value == 1 else 0


def _hint(general, low, high):
    """'General \\n Lowest \\n Highest' - two line breaks, so ui_hints keeps it verbatim."""
    return general + '\n' + low + '\n' + high


P = bpy.props
PROPS = {
    'erosion_enable': P.BoolProperty(
        name='Voxel Erosion', default=False, update=live.touch_prop,
        description=_hint('Erodes the object like weathered or broken stone: edges and corners break away first',
                          'Off: the object is left untouched',
                          'On: every closed piece of the object is eroded')),
    'erosion_seed': P.IntProperty(
        name='Seed', default=0, min=0, max=100000, update=live.touch_prop,
        description=_hint('Picks a different layout of the same break pattern; each piece also gets its own variation',
                          'Lowest: the first layout',
                          'Highest: any other value gives a new, repeatable layout')),
    'erosion_damage': P.FloatProperty(
        name='Damage', default=25.0, min=0.0, max=100.0, subtype='PERCENTAGE', update=live.touch_prop,
        description=_hint('How much material breaks away',
                          'Lowest: only the sharpest corners get small bites',
                          'Highest: deep, heavily rounded breaks with large chunks gone')),
    'erosion_noise_type': P.EnumProperty(
        name='Pattern', items=NOISE_ITEMS, default='CRACKED', update=live.touch_prop,
        description=_hint('The pattern that shapes the breaks',
                          'Rocky / Soft: natural, flowing stone erosion',
                          'Cracked / Cells: chunky chips along cracks or rounded cell bites')),
    'erosion_noise_scale': P.FloatProperty(
        name='Pattern Frequency', default=1.0, min=0.01, soft_max=10.0, max=1000.0, update=live.touch_prop,
        description=_hint('Size of the break pattern',
                          'Lowest: a few large, broad breaks',
                          'Highest: many small, fine chips')),
    'erosion_noise_contrast': P.FloatProperty(
        name='Break Sharpness', default=0.0, min=0.0, max=100.0, subtype='PERCENTAGE', update=live.touch_prop,
        description=_hint('Pushes the pattern toward fully intact or clearly broken',
                          'Lowest: gradual, even wear over the whole surface',
                          'Highest: more untouched surface with sharper, isolated breaks')),
    'erosion_round_first': P.BoolProperty(
        name='Round Edges Before Erosion', default=True, update=live.touch_prop,
        description=_hint('Rounds the shape before the pattern is applied, so damage gathers on edges and corners',
                          'Off: damage spreads more evenly over flat faces',
                          'On: classic worn-stone edges; Damage also controls the rounding')),
    'erosion_surface_smooth': P.FloatProperty(name='Smooth Eroded Area', default=0., min=0., max=100., subtype='PERCENTAGE', update=live.touch_prop, description='Relax only the newly eroded surface. 0% preserves the raw broken shape; 100% strongly reduces small jagged peaks. Intact faces and the damage boundary stay fixed'),
    'erosion_post_smooth': P.FloatProperty(
        name='Overall Smoothing', default=0.0, min=0.0, max=100.0, subtype='PERCENTAGE', update=live.touch_prop,
        description=_hint('Softens the finished breaks',
                          'Lowest: crisp, freshly broken edges',
                          'Highest: soft, weathered, sand-blasted erosion')),
    'erosion_edge_wall_depth': P.FloatProperty(
        name='Damage Rim Depth', default=3.0, min=0.0, soft_max=10.0, max=25.0,
        subtype='PERCENTAGE', update=live.touch_prop,
        description=_hint('Creates a short inward wall along the erosion boundary before blending into the noisy recess. Percentage of the reference piece size or Damage Scale; automatically reduced on thin parts',
                          '0%: the original gradual transition, with no added edge wall',
                          '25%: a deep edge wall; thin parts and Thickness Cap limit its depth. Small Voxel Size resolves narrow walls better')),
    'erosion_voxel_size': P.FloatProperty(
        name='Voxel Size', default=0.35, min=0.015, soft_max=2.0, max=10.0, update=live.touch_prop,
        description=_hint('Resolution of the eroded surface (relative to the piece size)',
                          'Lowest: fine detail, slower and heavier',
                          'Highest: coarse, blocky and fast')),
    'erosion_relative_scale': P.BoolProperty(
        name='Relative Scale', default=True, update=live.touch_prop,
        description=_hint('Ties every size of the effect to the size of each piece',
                          'Off: sizes follow Damage Scale (fixed cm), so big and small objects get equal-size breaks',
                          'On: the same look on any object size; small pieces get small breaks')),
    'erosion_ref_object': P.PointerProperty(
        name='Reference Object', type=bpy.types.Object, update=live.touch_prop,
        description=_hint('Uses another object\'s size as the scale for every piece (Relative Scale)',
                          'Empty: each piece uses its own size',
                          'Set: all pieces share one damage scale taken from this object')),
    'erosion_scale_size': P.FloatProperty(
        name='Damage Scale', default=200.0, min=1.0, soft_max=1000.0, update=live.touch_prop,
        description=_hint('Fixed size of the damage when Relative Scale is off, in cm (follows Master Scale)',
                          'Lowest: small, fine breaks and a fine voxel grid',
                          'Highest: large breaks and a coarse grid')),
    'erosion_thickness_cap': P.FloatProperty(
        name='Depth Limit', default=0.0, min=0.0, max=1.0, update=live.touch_prop,
        description=_hint('Limits how deep erosion may go, as a share of the local wall thickness',
                          'Lowest (0): off, thin parts may break through',
                          'Highest: damage stays shallow and never cuts through thin walls')),
    'erosion_max_voxels': P.IntProperty(
        name='Max Voxels', default=8000000, min=100000, soft_max=64000000, update=live.touch_prop,
        description=_hint('Voxel budget per piece; the grid grows coarser to fit it',
                          'Lowest: fast and light, big objects lose fine detail',
                          'Highest: full detail on big objects, more memory and time')),
    'erosion_avoid': P.BoolProperty(
        name='Avoid Selection', default=True, update=live.touch_prop,
        description=_hint('Keeps the Avoid faces untouched',
                          'Off: Avoid faces erode like the rest',
                          'On: Avoid faces stay intact')),
    'erosion_global_coords': P.BoolProperty(
        name='Global Coordinates', default=False, update=live.touch_prop,
        description=_hint('Where the break pattern is anchored',
                          'Off: the pattern sticks to the object when it moves',
                          'On: the pattern is fixed in the world, so moving the object changes the damage')),
    'erosion_offset': P.FloatVectorProperty(
        name='Pattern Offset', default=(0.0, 0.0, 0.0), size=3, soft_min=-10.0, soft_max=10.0, update=live.touch_prop,
        description=_hint('Slides the pattern over the object without changing its strength',
                          'Lowest: the pattern moves one way',
                          'Highest: the pattern moves the other way')),
    'erosion_rotation': P.FloatVectorProperty(
        name='Pattern Rotation', default=(0.0, 0.0, 0.0), size=3, subtype='EULER', update=live.touch_prop,
        description=_hint('Turns the pattern, e.g. to run cracks along a different direction',
                          'Lowest: the pattern turns one way',
                          'Highest: the pattern turns the other way')),
    'erosion_stretch': P.FloatVectorProperty(
        name='Pattern Stretch', default=(1.0, 1.0, 1.0), size=3, soft_min=0.05, soft_max=10.0, update=live.touch_prop,
        description=_hint('Stretches or squeezes the pattern per axis',
                          'Lowest: the pattern is stretched long along that axis',
                          'Highest: the pattern is squeezed into finer features along that axis')),
    'erosion_mask_mode': P.EnumProperty(
        name='Mask', items=MASK_ITEMS, default='NONE', get=_mask_mode_get, set=_mask_mode_set, update=live.touch_prop,
        description=_hint('Limits where erosion happens',
                          'None: the whole object erodes',
                          'Vertex Group: vertex weights decide where it erodes')),
    'erosion_group': P.StringProperty(
        name='Vertex Group', default='', update=live.touch_prop,
        description=_hint('Vertex group painting where erosion goes',
                          'Weight 0: untouched',
                          'Weight 1: full erosion')),
    'erosion_mask_image': P.PointerProperty(
        name='Mask Image', type=bpy.types.Image, update=live.touch_prop,
        description=_hint('Image that protects areas from erosion',
                          'Black: full erosion',
                          'White: protected')),
    'erosion_mask_uv': P.StringProperty(
        name='Mask UV Map', default='', update=live.touch_prop,
        description=_hint('UV map used to place the mask image',
                          'Empty: the active UV map',
                          'Set: this UV map')),
    'erosion_mask_box': P.BoolProperty(
        name='Box Projection', default=False, update=live.touch_prop,
        description=_hint('Places the mask image by box projection instead of UVs',
                          'Off: uses the UV map (best when the UVs are clean)',
                          'On: automatic projection, no UVs needed and no stretching')),
    'erosion_mask_invert': P.BoolProperty(
        name='Invert Mask', default=False, update=live.touch_prop,
        description=_hint('Swaps protected and eroded areas',
                          'Off: white is protected',
                          'On: black is protected')),
    'erosion_mask_adjust': P.FloatProperty(
        name='Mask Strength', default=0.0, min=-1.0, max=1.0, update=live.touch_prop,
        description=_hint('How strongly white areas resist erosion',
                          'Lowest: even white areas get damaged',
                          'Highest: white areas are fully protected and the protection reaches further')),
    'erosion_material': P.PointerProperty(
        name='Inside Material', type=bpy.types.Material, update=live.touch_prop,
        description=_hint('Material of the freshly broken surfaces',
                          'Empty: the default ProjectDamage_Erosion material',
                          'Set: this material; the source materials are not edited')),
    'erosion_shade_smooth': P.BoolProperty(
        name='Shade Smooth', default=True, update=live.touch_prop,
        description=_hint('Shading of the broken surfaces',
                          'Off: faceted, crisp breaks',
                          'On: smooth shading')),
    'erosion_uv_transfer': P.BoolProperty(
        name='UV From Surface', default=False, update=live.touch_prop,
        description=_hint('How the broken surfaces get their UVs',
                          'Off: box-projected UVs, like the other effects',
                          'On: continues the original UV layout, so the texture flows into the breaks')),
    'erosion_uv_map': P.StringProperty(
        name='UV Map', default='', update=live.touch_prop,
        description=_hint('UV map that UV From Surface continues',
                          'Empty: the active UV map',
                          'Set: this UV map')),
}

                                                         
FIELDS = (
    ('seed', 'seed'), ('avoid', 'avoid'), ('damage', 'damage'), ('noise_scale', 'noise_scale'),
    ('noise_contrast', 'noise_contrast'), ('round_first', 'round_first'), ('post_smooth', 'post_smooth'),
    ('edge_wall_depth', 'edge_wall_depth'), ('voxel_size', 'voxel_size'), ('thickness_cap', 'thickness_cap'), ('max_voxels', 'max_voxels'),
    ('global_coords', 'global_coords'),
)
_HASHED = tuple(core for _, core in FIELDS) + (
    'enable', 'noise_type', 'ref_size', 'pos_x', 'pos_y', 'pos_z', 'rot_x', 'rot_y', 'rot_z',
    'scl_x', 'scl_y', 'scl_z')


def wanted(p):
    return bool(getattr(p, 'erosion_enable', False) and not p.all_off)


def _gscale(p):
    from .params import _clamp_scale
    return _clamp_scale(p.global_scale / 100.0)


def _mean_scale(source):
    s = source.matrix_world.to_scale()
    return max(1e-9, (abs(s.x) + abs(s.y) + abs(s.z)) / 3.0)


def _ref_size(source, p, factor):
    """Core-space (local cm) size that maps to 2 reference units; 0 = per piece."""
    if p.erosion_relative_scale:
        ref = p.erosion_ref_object
        if ref is None:
            return 0.0
        return max(ref.dimensions) / _mean_scale(source) * factor
                                                                                  
    return p.erosion_scale_size * _gscale(p) / _mean_scale(source)


def _zone_weights(source, snap, prep, points):
    name = source.project_damage.erosion_group
    if not name:
        return None, 'Voxel Erosion: Mask is Vertex Group but no group is set; eroding everywhere'
    vg = source.vertex_groups.get(name)
    if vg is None:
        return None, "Voxel Erosion: vertex group '%s' not found; eroding everywhere" % name
    if snap is None:
        from .mesh_io import read_mask_weights
        w = read_mask_weights(source.data, vg.index)
    else:
        w = prep.point_values(snap.groups[vg.index])
    w = np.ascontiguousarray(np.clip(np.asarray(w, dtype=np.float64), 0.0, 1.0))
    if len(w) != len(points):
        return None, 'Voxel Erosion: the vertex group does not match the prepared mesh; eroding everywhere'
    return w, None


# готовим данные этого эффекта перед расчётом геометрии
def before_core(context, source, params, snap, prep, points, polys, factor, reports):
    """build.rebuild_object, before the fingerprint and the core call."""
    p = source.project_damage
    block = getattr(params, 'erosion', None)
    if not wanted(p):
        if block is not None:
            block.enable = False
            block.clear_masks()
        return
    if block is None:
        reports.append(({'WARNING'}, 'Voxel Erosion: rebuild the core and restart Blender to use it'))
        return
    for leaf, attr in FIELDS:
        setattr(block, attr, getattr(p, 'erosion_' + leaf))
    block.enable = True
    block.seed = int(p.erosion_seed) + 111
    block.noise_type = NOISE_INDEX[p.erosion_noise_type]
    block.ref_size = float(_ref_size(source, p, factor))
    block.pos_x, block.pos_y, block.pos_z = p.erosion_offset
    block.rot_x, block.rot_y, block.rot_z = p.erosion_rotation
    block.scl_x, block.scl_y, block.scl_z = p.erosion_stretch
    if p.erosion_global_coords:
        mw = source.matrix_world
        m = np.array([[mw[r][c] for c in range(3)] + [mw[r][3] * factor] for r in range(3)], dtype=np.float64)
        block.set_world(np.ascontiguousarray(m.ravel()))
    block.clear_masks()
    if p.erosion_mask_mode == 'GROUP':
        weights, note = _zone_weights(source, snap, prep, points)
        if note:
            reports.append(({'WARNING'}, note))
        if weights is not None:
            block.set_zone_weights(weights)


def hash_params(h, params):
    """fingerprints(): the result depends on every erosion field and mask array."""
    block = getattr(params, 'erosion', None)
    if block is None or not block.enable:
        return
    h.update(b'erosion')
    h.update(repr(tuple(getattr(block, name) for name in _HASHED)).encode())
    h.update(repr((block.mask_width, block.mask_height)).encode())


def hash_inputs(h, source):
    """fingerprints(): mask arrays, ref object size and world matrix (cheap digests).
    Needed because the core block's arrays are not readable from Python."""
    p = source.project_damage
    if not wanted(p):
        return
    h.update(repr(p.erosion_surface_smooth).encode())
    if p.erosion_global_coords:
        h.update(repr(tuple(tuple(r) for r in source.matrix_world)).encode())
    if p.erosion_relative_scale and p.erosion_ref_object is not None:
        h.update(repr(tuple(p.erosion_ref_object.dimensions)).encode())
    if p.erosion_mask_mode == 'GROUP' and p.erosion_group:
        vg = source.vertex_groups.get(p.erosion_group)
        if vg is not None:
            from .mesh_io import read_mask_weights
            h.update(np.asarray(read_mask_weights(source.data, vg.index), dtype=np.float64).tobytes())



                                                                             
                                                     

def _stamped(mesh):
    attr = mesh.attributes.get('project_damage_effect_id')
    if attr is None or attr.domain != 'FACE' or len(mesh.polygons) == 0:
        return None
    ids = np.empty(len(mesh.polygons), dtype=np.int32)
    attr.data.foreach_get('value', ids)
    return ids == STAMP


def after_result(context, source, mesh, core):
    """build.rebuild_object, right after effect_groups.tag_mesh(res_mesh, ...)."""
    p = source.project_damage
    if not wanted(p):
        return
    faces = _stamped(mesh)
    if faces is None or not faces.any():
        return
    if p.erosion_surface_smooth > 0:
        smooth_eroded(mesh, faces, p.erosion_surface_smooth / 100.)
    smooth = np.empty(len(mesh.polygons), dtype=bool)
    mesh.polygons.foreach_get('use_smooth', smooth)
    smooth[faces] = bool(p.erosion_shade_smooth)
    mesh.polygons.foreach_set('use_smooth', smooth)
    if p.erosion_uv_transfer and hasattr(core, 'erosion_transfer_uvs'):
        _transfer_uvs(source, mesh, faces, core)
    mesh.update()


def smooth_eroded(mesh, faces, amount):
    n = len(mesh.vertices)
    co = np.empty(n*3, dtype=np.float64);mesh.vertices.foreach_get('co', co);co=co.reshape(-1,3)
    edges=np.empty(len(mesh.edges)*2,dtype=np.int32);mesh.edges.foreach_get('vertices',edges);edges=edges.reshape(-1,2)
    if not len(edges):return
    loops=np.empty(len(mesh.loops),dtype=np.int32);mesh.loops.foreach_get('vertex_index',loops)
    counts=np.empty(len(mesh.polygons),dtype=np.int32);mesh.polygons.foreach_get('loop_total',counts)
    eroded=np.repeat(faces,counts)
    eligible=np.zeros(n,dtype=bool);eligible[loops[eroded]]=True;eligible[loops[~eroded]]=False
    a,b=edges.T
    degree=np.bincount(edges.ravel(),minlength=n)
    limit=np.full(n,np.inf);length=np.linalg.norm(co[a]-co[b],axis=1)
    np.minimum.at(limit,a,length);np.minimum.at(limit,b,length)
    for iteration in range(max(1,round(12*amount))):
        for step in (.45,-.47):
            total=np.zeros_like(co)
            for axis in range(3):
                total[:,axis]=np.bincount(a,weights=co[b,axis],minlength=n)+np.bincount(b,weights=co[a,axis],minlength=n)
            delta=(total/np.maximum(1,degree)[:,None]-co)*step*amount
            distance=np.linalg.norm(delta,axis=1)
            delta*=np.minimum(1.,limit*.2/np.maximum(distance,1e-20))[:,None]
            co[eligible]+=delta[eligible]
    mesh.vertices.foreach_set('co',co.ravel())
    mesh.update()


def _transfer_uvs(source, mesh, faces, core):
    p = source.project_damage
    src = source.data
    src_layer = src.uv_layers.get(p.erosion_uv_map) if p.erosion_uv_map else src.uv_layers.active
    dst_layer = (mesh.uv_layers.get(src_layer.name) if src_layer is not None else None) or mesh.uv_layers.active
    if src_layer is None or dst_layer is None:
        return
    src.calc_loop_triangles()
    nt = len(src.loop_triangles)
    if nt == 0:
        return
    tri_verts = np.empty(nt * 3, dtype=np.int32)
    src.loop_triangles.foreach_get('vertices', tri_verts)
    tri_loops = np.empty(nt * 3, dtype=np.int32)
    src.loop_triangles.foreach_get('loops', tri_loops)
    co = np.empty(len(src.vertices) * 3, dtype=np.float64)
    src.vertices.foreach_get('co', co)
    loop_uv = np.empty(len(src.loops) * 2, dtype=np.float64)
    src_layer.data.foreach_get('uv', loop_uv)
    src_uvs = loop_uv.reshape(-1, 2)[tri_loops].reshape(nt, 6)

    n_poly = len(mesh.polygons)
    loop_start = np.empty(n_poly, dtype=np.int32); mesh.polygons.foreach_get('loop_start', loop_start)
    loop_total = np.empty(n_poly, dtype=np.int32); mesh.polygons.foreach_get('loop_total', loop_total)
    loop_vert = np.empty(len(mesh.loops), dtype=np.int32); mesh.loops.foreach_get('vertex_index', loop_vert)
    dco = np.empty(len(mesh.vertices) * 3, dtype=np.float64); mesh.vertices.foreach_get('co', dco)
    sel = np.flatnonzero(faces)
    loops = np.concatenate([np.arange(loop_start[f], loop_start[f] + loop_total[f]) for f in sel])
    owner = np.repeat(np.arange(len(sel), dtype=np.int32), loop_total[sel])
    q = np.ascontiguousarray(dco.reshape(-1, 3)[loop_vert[loops]])
    out = core.erosion_transfer_uvs(np.ascontiguousarray(co.reshape(-1, 3)),
                                    np.ascontiguousarray(tri_verts.reshape(-1, 3)),
                                    np.ascontiguousarray(src_uvs), q, owner)
    uv = np.empty(len(mesh.loops) * 2, dtype=np.float64)
    dst_layer.data.foreach_get('uv', uv)
    uv = uv.reshape(-1, 2)
    uv[loops] = np.asarray(out)
    dst_layer.data.foreach_set('uv', uv.ravel())


                                                                             
                                                                      

def draw_patterns(layout, p):
                                                                    
    from . import ui_image_hints
    for pair in (NOISE_ITEMS[:2], NOISE_ITEMS[2:]):
        row=layout.row(align=True)
        for key,label,description in pair:
            tile=row.box().column(align=True)
            preview=tile.row();preview.alignment='CENTER'
            icon=ui_image_hints._icon('erosion_pattern_'+key.lower())
            if icon:preview.template_icon(icon_value=icon,scale=4.0)
            button=tile.row(align=True);button.scale_y=1.3
            button.prop_enum(p,'erosion_noise_type',key,text=label)


# показываем настройки выбранного эффекта
def draw_settings(layout, context):
    from . import ownership, effect_layout, material_controls
    src = ownership.context_source(context)
    if src is None:
        return
    p = src.project_damage
    layout.use_property_split = False
    layout.use_property_decorate = False

    box = effect_layout.section(layout, 'Damage')
    material_controls.inline(box,p,'erosion_material',text='Eroded Surface')
    col = box.column(align=True)
    col.prop(p, 'erosion_damage')
    draw_patterns(col, p)
    col.prop(p, 'erosion_noise_scale')
    col.prop(p, 'erosion_noise_contrast')
    col.prop(p, 'erosion_seed')
    col = box.column(align=True)
    col.prop(p, 'erosion_round_first')
    col.prop(p, 'erosion_post_smooth')
    col.prop(p, 'erosion_surface_smooth')
    col.prop(p, 'erosion_edge_wall_depth')
    col.prop(p, 'erosion_thickness_cap')
    pass

    box = effect_layout.section(layout, 'Scale & Quality')
    col = box.column(align=True)
    quality_ui.draw(col, p, 'erosion_voxel_size')
    col.prop(p, 'erosion_max_voxels')
    col = box.column(align=True)
    col.prop(p, 'erosion_relative_scale')
    if p.erosion_relative_scale:
        col.prop(p, 'erosion_ref_object')
    else:
        col.prop(p, 'erosion_scale_size')

    box = effect_layout.section(layout, 'Pattern Placement')
    col = box.column(align=True)
    col.prop(p, 'erosion_global_coords')
    col.prop(p, 'erosion_offset')
    col.prop(p, 'erosion_rotation')
    col.prop(p, 'erosion_stretch')

    box = effect_layout.section(layout, 'Mask')
    row = box.row(align=True)
    row.prop(p, 'erosion_mask_mode', expand=True)
    if p.erosion_mask_mode == 'GROUP' and src.type == 'MESH':
        row = box.row(align=True)
        row.prop_search(p, 'erosion_group', src, 'vertex_groups', text='Group')
        row.operator('project_damage.erosion_mask_group', text='', icon='ADD').action = 'NEW'
        row = box.row(align=True)
        if context.mode == 'EDIT_MESH' and context.active_object == src:
            row.operator('project_damage.erosion_mask_group', text='Assign Selection', icon='CHECKMARK').action = 'ASSIGN'
            row.operator('project_damage.erosion_mask_group', text='Remove Selection', icon='REMOVE').action = 'REMOVE'
            box.operator('project_damage.erosion_mask_group', text='Exit Mask Editing', icon='CHECKMARK').action = 'FINISH'
        else:
            row.operator('project_damage.erosion_mask_group', text='Edit Mask Vertices', icon='EDITMODE_HLT').action = 'EDIT'
        box.label(text='Weight 0: intact. Weight 1: full erosion.', icon='INFO')



# действие «Vertex Group Mask»
class PROJECT_DAMAGE_OT_erosion_mask_group(bpy.types.Operator):
    bl_idname = 'project_damage.erosion_mask_group'
    bl_label = 'Vertex Group Mask'
    bl_description = 'Create a mask, select vertices on the original mesh, then Assign Selection. Only weighted vertices erode. Remove Selection protects them again'
    bl_options = {'REGISTER', 'UNDO'}
    action: bpy.props.EnumProperty(items=(('NEW','New','Create a new mask'),
        ('EDIT','Edit','Edit vertices of the original mesh'),('ASSIGN','Assign','Assign selected vertices'),
        ('REMOVE','Remove','Remove selected vertices from the mask'),
        ('FINISH','Finish','Return to the damage preview')))
    effect: bpy.props.EnumProperty(items=(('EROSION','Voxel Erosion','Voxel Erosion mask'),
        ('PEELING','Peeling','Peeling mask')), default='EROSION', options={'HIDDEN', 'SKIP_SAVE'})

    @classmethod
    def description(cls, context, properties):
        name='Peeling' if properties.effect=='PEELING' else 'Voxel Erosion'
        return {
            'NEW': 'Create a new '+name+' mask on the original mesh. In Edit Mode, selected vertices are assigned with weight 1; otherwise enter Edit Mode to select the area',
            'EDIT': 'Edit the original mesh and select the vertices already in the '+name+' mask',
            'ASSIGN': 'Allow '+name+' on selected original vertices by assigning mask weight 1. Vertices outside the group stay protected',
            'REMOVE': 'Remove selected vertices from the '+name+' mask to protect them from this effect',
            'FINISH': 'Return to the damage preview. Live Update rebuilds the effect; in Manual mode press Update',
        }.get(properties.action, cls.bl_description)

    @classmethod
    def poll(cls, context):
        from . import ownership
        src = ownership.context_source(context)
        return src is not None and src.type == 'MESH' and src.library is None and src.data.library is None

    def execute(self, context):
        import bmesh
        from . import ownership
        src = ownership.context_source(context)
        p = src.project_damage
        field = 'peeling_group' if self.effect == 'PEELING' else 'erosion_group'
        vg = src.vertex_groups.get(getattr(p, field))
        editing = src == context.active_object and context.mode == 'EDIT_MESH'
        if self.action in {'ASSIGN', 'REMOVE'} and not editing:
            self.report({'WARNING'}, 'Select vertices on the original mesh in Edit Mode')
            return {'CANCELLED'}
        if self.action == 'FINISH':
            if context.mode != 'OBJECT': bpy.ops.object.mode_set(mode='OBJECT')
            from .build import result_of
            result = result_of(src)
            if result is not None and result.name in context.view_layer.objects:
                src.select_set(False);src.hide_set(True)
                result.hide_set(False);result.select_set(True)
                context.view_layer.objects.active = result
            live._mark_dirty(src)
            return {'FINISHED'}
        if self.action == 'REMOVE' and vg is None:
            self.report({'WARNING'}, 'Choose a mask group first')
            return {'CANCELLED'}
        previous = live._APPLYING
        live._APPLYING = True
        try:
            if vg is None or self.action == 'NEW':
                vg = src.vertex_groups.new(name='PD_Peeling_Mask' if self.effect == 'PEELING' else 'PD_Erosion_Mask')
                setattr(p, field, vg.name)
            if self.effect == 'EROSION': p.erosion_mask_mode = 'GROUP'
            src.vertex_groups.active_index = vg.index
            if editing and self.action in {'NEW', 'ASSIGN', 'REMOVE'}:
                bm = bmesh.from_edit_mesh(src.data)
                layer = bm.verts.layers.deform.verify()
                for vert in bm.verts:
                    if vert.select:
                        if self.action == 'REMOVE':
                            if vg.index in vert[layer]: del vert[layer][vg.index]
                        else:
                            vert[layer][vg.index] = 1.0
                bmesh.update_edit_mesh(src.data, loop_triangles=False, destructive=False)
            elif self.action in {'NEW', 'EDIT'}:
                if context.mode != 'OBJECT': bpy.ops.object.mode_set(mode='OBJECT')
                for obj in context.selected_objects: obj.select_set(False)
                from .build import result_of
                result = result_of(src)
                if result is not None: result.hide_set(True)
                src.hide_set(False)
                src.hide_viewport = False
                src.select_set(True)
                context.view_layer.objects.active = src
                bpy.ops.object.mode_set(mode='EDIT')
                bpy.ops.mesh.select_all(action='DESELECT')
                bpy.ops.object.vertex_group_select()
        finally:
            live._APPLYING = previous
        live._mark_dirty(src)
        return {'FINISHED'}


# подключаем свойства и действия этого модуля к Blender
def register():
    bpy.utils.register_class(PROJECT_DAMAGE_OT_erosion_mask_group)


# убираем подключения модуля при выключении аддона
def unregister():
    if PROJECT_DAMAGE_OT_erosion_mask_group.is_registered:
        bpy.utils.unregister_class(PROJECT_DAMAGE_OT_erosion_mask_group)

for _removed_mask in ('erosion_mask_image','erosion_mask_uv','erosion_mask_box','erosion_mask_invert','erosion_mask_adjust'):
    PROPS[_removed_mask].keywords['options'] = {'HIDDEN', 'SKIP_SAVE'}
    PROPS[_removed_mask].keywords.pop('update', None)
