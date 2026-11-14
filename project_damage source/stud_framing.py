# вариант деревянного каркаса с поперечинами и гвоздями
# SPDX-License-Identifier: GPL-3.0-or-later
"""Stud Framing: a separate generator next to the three Lath assets.

The tile runs the native Lath effect with style 2: no laths at all. The skin
(drywall or a thin plaster coat, the Lath plaster thickness) breaks away over a
timber stud frame. The cut opens a cavity the full stud depth deep, and the
frame fills it: studs, bottom and doubled top plates at the wall's ends,
staggered noggins between the studs and nails left standing in the stud faces.
Members crossing a hole stay or snap with splintered ends.

Outline, plaster, hole and crack settings are shared with Lath Damage; this
module owns the framing-only settings, the first-use look and the timber / nail
materials of the separate frame object.
"""
from .blender_compat import enable_nodes
import bpy
import numpy as np

from .effect_schema import FRAMING_META

STAMP = 'project_damage_framing_look'
WOOD_NAME = 'ProjectDamage_FramingTimber'
NAIL_NAME = 'ProjectDamage_FramingNail'

                                                                               
_UI = {
    'noggin_spacing': ('Noggin Spacing (cm)', 150.0, None),
    'frame_plates': ('Top and Bottom Plates', None, None),
    'double_plate': ('Double Top Plate', None, None),
    'nail_amount': ('Nails', 1.0, 1.0),
    'nail_length': ('Nail Length', 3.0, 10.0),
}

                                                                            
                                                                           
                                    
LOOK = {
    'lath_plaster_min': 1.2,                     
    'lath_plaster_max': 1.6,
    'lath_terraces': 1,
    'lath_keys': 0.0,
    'lath_crumble': 0.6,
    'lath_hole_enable': False,
    'lath_hole_laths': 0.75,                                        
    'lath_lath_splinter': 2.0,
    'lath_lath_bend': 1.0,
    'lath_stud_spacing': 40.6,                              
    'lath_stud_width': 3.8,
    'lath_stud_depth': 8.9,
}

                                                                           
HIDDEN = {
    'lath_cavity_depth', 'lath_cavity_smooth', 'lath_cavity_slope', 'lath_keys', 'lath_key_size',
    'lath_lath_enable', 'lath_lath_amount', 'lath_lath_width', 'lath_lath_thickness', 'lath_lath_gap',
    'lath_lath_scale', 'lath_lath_angle', 'lath_lath_length', 'lath_lath_embed', 'lath_lath_jitter',
    'lath_lath_tilt', 'lath_lath_warp', 'lath_lath_edge', 'lath_lath_missing', 'lath_lath_broken',
}


def add_properties(cls, update):
    """Register lath_<field> properties on ProjectDamageProps for every framing field."""
    for field, kind, default, description in FRAMING_META:
        label, soft_max, hard_max = _UI[field]
        kw = dict(name=label, default=default, description=description, update=update)
        if kind == 'bool':
            prop = bpy.props.BoolProperty(**kw)
        else:
            prop = bpy.props.FloatProperty(**kw, min=0.0, soft_max=soft_max, **({'max': hard_max} if hard_max else {}))
        cls.__annotations__['lath_' + field] = prop


def apply_look(src):
    p = src.project_damage
    if src.get(STAMP):
        return
    for name, value in LOOK.items():
        if hasattr(p, name):
            setattr(p, name, value)
    src[STAMP] = True


# показываем настройки выбранного эффекта
def draw_settings(layout, p):
    from .panels import draw_stud_framing_settings
    draw_stud_framing_settings(layout,p)


def split_nails(mesh, stud_width, attr='project_damage_lath_piece'):
    """Material slot 1 for nail pieces (thin round pieces), 0 for timber."""
    n = len(mesh.polygons)
    if not n:
        return
    co = np.empty(len(mesh.vertices) * 3)
    mesh.vertices.foreach_get('co', co)
    co = co.reshape(-1, 3)
    pid = np.zeros(n, dtype=np.int32)
    a = mesh.attributes.get(attr)
    if a is not None:
        a.data.foreach_get('value', pid)
    lt = np.empty(n, dtype=np.int32)
    mesh.polygons.foreach_get('loop_total', lt)
    lv = np.empty(len(mesh.loops), dtype=np.int32)
    mesh.loops.foreach_get('vertex_index', lv)
    first = np.empty(n, dtype=np.int32)
    mesh.polygons.foreach_get('loop_start', first)
    face_vert = lv[first]
    mat = np.zeros(n, dtype=np.int32)
    order = np.argsort(pid, kind='stable')
    bounds = np.flatnonzero(np.diff(pid[order])) + 1
    for sel in np.split(order, bounds):
        pts = co[face_vert[sel]]
        d = pts - pts.mean(axis=0)
        s = np.linalg.svd(d, compute_uv=False)
        spread = s / np.sqrt(max(1, len(d)))
                                                                           
        if len(spread) > 1 and spread[1] < 0.12 * stud_width:
            mat[sel] = 1
    mesh.polygons.foreach_set('material_index', mat)


def wood_material():
    from .material_defaults import get
    return get('framing')


def nail_material():
    mat = bpy.data.materials.get(NAIL_NAME)
    if mat is not None:
        return mat
    mat = bpy.data.materials.new(NAIL_NAME)
    enable_nodes(mat)
    mat.diffuse_color = (0.36, 0.36, 0.37, 1)
    bsdf = mat.node_tree.nodes.get('Principled BSDF')
    bsdf.inputs['Base Color'].default_value = (0.36, 0.36, 0.37, 1)
    bsdf.inputs['Metallic'].default_value = 1.0
    bsdf.inputs['Roughness'].default_value = 0.45
    return mat
