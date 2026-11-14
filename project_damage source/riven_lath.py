# вариант рваной дранки, его деревянные профили и материалы
# SPDX-License-Identifier: GPL-3.0-or-later
"""Riven Lath: a third Lath asset next to Lath Damage and Lath and Plaster V2.

The tile runs the same native Lath effect with style 1: hand-split (riven) lath
boards nailed to vertical studs. Boards wave in the wall plane, change width,
have split, wedge-shaped edges, butt-joint only on studs in stepped bundles,
and snapped boards over a hole stay nailed to a stud or swing down from it.
Plaster, cavity, hole, crack and lath-size settings are shared with Lath Damage;
this module owns the riven-only settings, the first-use look and the wood
material / plank UVs of the separate lath object.
"""
from .blender_compat import enable_nodes
import bpy
import numpy as np

from .effect_schema import RIVEN_LATH_META

STAMP = 'project_damage_riven_look'
MAT_NAME = 'ProjectDamage_RivenLathWood'

                                                                             
_UI = {
    'riven_wave': ('Waviness', 1.5, 2.0),
    'riven_wave_length': ('Wave Length (cm)', 120.0, None),
    'riven_width_var': ('Width Variation', 1.0, 1.0),
    'riven_twist': ('Twist', 15.0, 30.0),
    'riven_rough': ('Split Edges', 1.0, 1.0),
    'stud_enable': ('Studs', None, None),
    'stud_spacing': ('Stud Spacing (cm)', 80.0, None),
    'stud_width': ('Stud Width (cm)', 10.0, None),
    'stud_depth': ('Stud Depth (cm)', 20.0, None),
    'board_studs': ('Board Length (studs)', 6, 64),
    'joint_bundle': ('Joint Bundle (rows)', 20, 1000),
    'lath_hang': ('Hanging Boards', 1.0, 1.0),
}

                                                                           
                                                                              
                                                  
LOOK = {
    'lath_lath_width': 3.2,
    'lath_lath_gap': 1.1,
    'lath_lath_thickness': 0.8,
    'lath_cavity_depth': 9.0,
    'lath_keys': 0.2,
    'lath_plaster_min': 1.8,
    'lath_plaster_max': 2.8,
    'lath_hole_laths': 0.4,
    'lath_lath_missing': 0.05,
    'lath_lath_broken': 0.12,
}


def add_properties(cls, update):
    """Register lath_<field> properties on ProjectDamageProps for every riven field."""
    for field, kind, default, description in RIVEN_LATH_META:
        if field == 'style':
            continue                                             
        label, soft_max, hard_max = _UI[field]
        kw = dict(name=label, default=default, description=description, update=update)
        if kind == 'bool':
            prop = bpy.props.BoolProperty(**kw)
        elif kind == 'int32_t':
            prop = bpy.props.IntProperty(**kw, min=0, soft_max=soft_max, **({'max': hard_max} if hard_max else {}))
        else:
            prop = bpy.props.FloatProperty(**kw, min=0.0, soft_max=soft_max, **({'max': hard_max} if hard_max else {}))
        cls.__annotations__['lath_' + field] = prop


def apply_look(src):
    p = src.project_damage
    if src.get(STAMP) or p.lath_enable or p.lath_v2_enable:
        src[STAMP] = True
        return
    for name, value in LOOK.items():
        if hasattr(p, name):
            setattr(p, name, value)
    src[STAMP] = True


# показываем настройки выбранного эффекта
def draw_settings(layout,p):
    from .panels import draw_riven_settings
    draw_riven_settings(layout,p)


def piece_uvs(mesh, unit, attr='project_damage_lath_piece', uv_name='UVMap'):
    """UVs along each plank: U along the piece's longest axis, V across it.

    `unit` is the length (mesh units) of one UV tile, 10 cm in the add-on;
    pieces get their own offsets so neighbouring boards do not repeat the
    same grain. The material's node sizes assume metre scene units."""
    n_loops = len(mesh.loops)
    if not n_loops:
        return
    co = np.empty(len(mesh.vertices) * 3)
    mesh.vertices.foreach_get('co', co)
    co = co.reshape(-1, 3)
    lv = np.empty(n_loops, dtype=np.int32)
    mesh.loops.foreach_get('vertex_index', lv)
    lt = np.empty(len(mesh.polygons), dtype=np.int32)
    mesh.polygons.foreach_get('loop_total', lt)
    pid = np.zeros(len(mesh.polygons), dtype=np.int32)
    a = mesh.attributes.get(attr)
    if a is not None:
        a.data.foreach_get('value', pid)
    loop_piece = np.repeat(pid, lt)
    uv = np.zeros((n_loops, 2))
    order = np.argsort(loop_piece, kind='stable')
    bounds = np.flatnonzero(np.diff(loop_piece[order])) + 1
    for sel in np.split(order, bounds):
        pts = co[lv[sel]]
        d = pts - pts.mean(axis=0)
        _, _, vt = np.linalg.svd(d, full_matrices=False)
        p = int(loop_piece[sel[0]])
        uv[sel, 0] = d @ vt[0] / unit + (p % 97) * 0.137
        uv[sel, 1] = (d @ vt[1] + 0.6 * (d @ vt[2])) / unit + (p % 89) * 0.073
    layer = mesh.uv_layers.get(uv_name) or mesh.uv_layers.new(name=uv_name)
    layer.data.foreach_set('uv', uv.ravel().astype(np.float32))


def material():
    """Weathered riven lath: grey-tan wood, grain along each board, tone per board."""
    mat = bpy.data.materials.get(MAT_NAME)
    if mat is not None:
        return mat
    mat = bpy.data.materials.new(MAT_NAME)
    enable_nodes(mat)
    mat.diffuse_color = (0.32, 0.25, 0.18, 1)
    nt = mat.node_tree
    nodes, links = nt.nodes, nt.links
    bsdf = nodes.get('Principled BSDF')
    uv = nodes.new('ShaderNodeUVMap'); uv.uv_map = 'UVMap'
    mp = nodes.new('ShaderNodeMapping'); mp.inputs['Scale'].default_value = (0.35, 9.0, 1.0)
    links.new(uv.outputs['UV'], mp.inputs['Vector'])
    grain = nodes.new('ShaderNodeTexNoise'); grain.inputs['Scale'].default_value = 1.6
    grain.inputs['Detail'].default_value = 9.0; grain.inputs['Roughness'].default_value = 0.62
    links.new(mp.outputs['Vector'], grain.inputs['Vector'])
    fine = nodes.new('ShaderNodeTexNoise'); fine.inputs['Scale'].default_value = 7.0
    fine.inputs['Detail'].default_value = 4.0
    links.new(mp.outputs['Vector'], fine.inputs['Vector'])
                                          
    pa = nodes.new('ShaderNodeAttribute'); pa.attribute_type = 'GEOMETRY'; pa.attribute_name = 'project_damage_lath_piece'
    wn = nodes.new('ShaderNodeTexWhiteNoise'); wn.noise_dimensions = '1D'
    links.new(pa.outputs['Fac'], wn.inputs['W'])
    tone = nodes.new('ShaderNodeValToRGB')
    el = tone.color_ramp.elements
    el[0].position = 0.0; el[0].color = (0.22, 0.16, 0.11, 1)
    el[1].position = 1.0; el[1].color = (0.50, 0.35, 0.21, 1)
    el.new(0.5).color = (0.36, 0.27, 0.18, 1)
    links.new(wn.outputs['Value'], tone.inputs['Fac'])
    streak = nodes.new('ShaderNodeValToRGB')
    streak.color_ramp.elements[0].position = 0.32; streak.color_ramp.elements[0].color = (0.5, 0.48, 0.46, 1)
    streak.color_ramp.elements[1].position = 0.7; streak.color_ramp.elements[1].color = (1.15, 1.12, 1.08, 1)
    links.new(grain.outputs['Fac'], streak.inputs['Fac'])
    mul = nodes.new('ShaderNodeMix'); mul.data_type = 'RGBA'; mul.blend_type = 'MULTIPLY'
    mul.inputs['Factor'].default_value = 1.0
    links.new(tone.outputs['Color'], mul.inputs[6]); links.new(streak.outputs['Color'], mul.inputs[7])
                                                                  
    ao = nodes.new('ShaderNodeAmbientOcclusion'); ao.inputs['Distance'].default_value = 0.015
    aor = nodes.new('ShaderNodeMapRange'); aor.inputs['To Min'].default_value = 0.45
    links.new(ao.outputs['AO'], aor.inputs['Value'])
    cmb = nodes.new('ShaderNodeCombineColor')
    for k in ('Red', 'Green', 'Blue'):
        links.new(aor.outputs['Result'], cmb.inputs[k])
    dirt = nodes.new('ShaderNodeMix'); dirt.data_type = 'RGBA'; dirt.blend_type = 'MULTIPLY'
    dirt.inputs['Factor'].default_value = 1.0
    links.new(mul.outputs[2], dirt.inputs[6]); links.new(cmb.outputs['Color'], dirt.inputs[7])
    links.new(dirt.outputs[2], bsdf.inputs['Base Color'])
    bsdf.inputs['Roughness'].default_value = 0.88
    hsum = nodes.new('ShaderNodeMath'); hsum.operation = 'MULTIPLY_ADD'; hsum.inputs[1].default_value = 0.5
    links.new(fine.outputs['Fac'], hsum.inputs[0]); links.new(grain.outputs['Fac'], hsum.inputs[2])
    bump = nodes.new('ShaderNodeBump'); bump.inputs['Strength'].default_value = 0.35
    bump.inputs['Distance'].default_value = 0.002
    links.new(hsum.outputs['Value'], bump.inputs['Height'])
    links.new(bump.outputs['Normal'], bsdf.inputs['Normal'])
    return mat
