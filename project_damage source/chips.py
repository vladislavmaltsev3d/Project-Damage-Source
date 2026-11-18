# настройки деревянных сколов и их связь с расчётом
from . import quality_ui
# SPDX-License-Identifier: GPL-3.0-or-later
"""Wooden Chips, the fifth Boolean fill (Fill Type = Wooden Chips): an impact on
plywood.

The core (break_chips.cpp) treats the object as a stack of veneer plies whose
grain turns from ply to ply. Each ply tears along its own grain around the
breaks, the plies nearest the impact face open widest, so the break steps down
like a torn crater. The splinters come back through the same rebar_* outputs as
the other fills; rebar_grain carries each point in its shard's own frame, which
the default timber material turns into grain.
"""
import bpy
import numpy as np

from . import live

# (core field, kind, default, is a length in cm): rebar.FIELDS adds these, so
# params.py passes them to the core and build.py puts them in the cache key.
FIELDS = (
    ('chip_plies', 'int32_t', 5, False), ('chip_ply_thickness', 'double', 0.0, True),
    ('chip_ply_angle', 'double', 90.0, False), ('chip_length', 'double', 12.0, True),
    ('chip_width', 'double', 1.0, True), ('chip_sharp', 'double', 0.8, False),
    ('chip_lift', 'double', 0.5, False), ('chip_amount', 'double', 1.0, False),
    ('chip_crater', 'double', 4.0, True), ('chip_face', 'int32_t', 0, False),
)

# Explicit ids: Blender saves the number, so they must never move.
FILL_ITEM = ('4', 'Wooden Chips', 'Plywood impact: veneer plies torn along their alternating grain, a stepped '
             'crater and bent splinters at its edge. The object is torn around the breaks', 'NONE', 4)

FACE_ITEMS = (
    ('0', 'Auto', 'The face the breakers sit nearest; a breaker centred in the panel counts as the + face', 'NONE', 0),
    ('1', '+ Face', 'The hit comes from the positive side of the stack axis', 'NONE', 1),
    ('2', '- Face', 'The hit comes from the negative side of the stack axis', 'NONE', 2),
)

HINTS = {
    'rebar_chip_plies': 'Veneer plies in the panel. Each ply tears on its own, so more plies give more, finer steps '
                        'in the crater and thinner splinters',
    'rebar_chip_ply_thickness': 'Thickness of one ply, in cm before Scale. 0 divides the object evenly between the '
                                'plies. A stack thinner than the object leaves the rest whole; plies that do not fit '
                                'are dropped',
    'rebar_chip_ply_angle': 'How far the grain turns from one ply to the next. 90 is plywood (crossed plies); '
                            '0 is solid wood, with every tear running the same way',
    'rebar_chip_length': 'Typical length of a tear along the grain, in cm before Scale. A few tears reach about twice '
                         'this; the splinters are as long as their tears',
    'rebar_chip_width': 'Width of a fibre bundle, and so of the splinters, in cm before Scale. Smaller is finer and '
                        'makes more polygons',
    'rebar_chip_sharp': 'Tear ends and splinter tips: 0 blunt and ragged, 1 long needle points',
    'rebar_chip_lift': 'How far splinters bend out of the panel. 0 keeps them in their ply, 1 bends them up to about '
                       '75 degrees. Plies near the impact face bend out of it; on a hole that goes through, the far '
                       'plies bend out of the back',
    'rebar_chip_amount': 'Splinters per tear. 0 leaves the stepped crater only',
    'rebar_chip_crater': 'How much wider the ply at the impact face is torn than the break itself, in cm before Scale. '
                         'The extra width falls off ply by ply and reaches none at the last ply',
    'rebar_chip_face': 'The side that was hit, which is the side where the crater opens widest. Auto takes the face '
                       'the breakers sit nearest',
    'rebar_chips_material': 'Material of the splinters. Empty uses the bundled timber shader, which reads the grain '
                            'attribute each splinter carries. The crater walls use Cut Faces',
}


def wood_material():
    from .material_defaults import get
    return get('framing')


def finish_mesh(mesh, source, grain, *, props=None):
    """The splinter material and the grain attribute on the output mesh."""
    p = source.project_damage if props is None else props
    mesh.materials.clear()
    mesh.materials.append(getattr(p, 'rebar_chips_material', None) or wood_material())
    if grain is not None and len(grain) == 3 * len(mesh.vertices):
        g = mesh.attributes.new('framing_grain', 'FLOAT_VECTOR', 'POINT')
        g.data.foreach_set('vector', np.asarray(grain, dtype=np.float32).ravel())


def add_properties(cls):
    """Replace the generic rebar_chip_* properties made from FIELDS by ones with
    proper ranges, units and tooltips (same names, same defaults)."""
    a = cls.__annotations__
    prop = bpy.props
    up = live.touch_prop
    a['rebar_chip_plies'] = prop.IntProperty(name='Plies', default=5, min=1, soft_max=15, max=32, update=up,
                                             description=HINTS['rebar_chip_plies'])
    a['rebar_chip_ply_thickness'] = prop.FloatProperty(name='Ply Thickness (cm)', default=0.0, min=0.0, soft_max=2.0,
                                                       update=up, description=HINTS['rebar_chip_ply_thickness'])
    a['rebar_chip_ply_angle'] = prop.FloatProperty(name='Grain Turn per Ply', default=90.0, min=-180.0, max=180.0,
                                                   update=up, description=HINTS['rebar_chip_ply_angle'])
    a['rebar_chip_length'] = prop.FloatProperty(name='Chip Length (cm)', default=12.0, min=0.05, soft_max=60.0,
                                                update=up, description=HINTS['rebar_chip_length'])
    a['rebar_chip_width'] = prop.FloatProperty(name='Chip Width (cm)', default=1.0, min=0.02, soft_max=6.0,
                                               update=up, description=HINTS['rebar_chip_width'])
    a['rebar_chip_sharp'] = prop.FloatProperty(name='Sharpness', default=0.8, min=0.0, max=1.0, subtype='FACTOR',
                                               update=up, description=HINTS['rebar_chip_sharp'])
    a['rebar_chip_lift'] = prop.FloatProperty(name='Lift', default=0.5, min=0.0, soft_max=1.0, max=1.5, subtype='FACTOR',
                                              update=up, description=HINTS['rebar_chip_lift'])
    a['rebar_chip_amount'] = prop.FloatProperty(name='Amount', default=1.0, min=0.0, soft_max=3.0, max=10.0,
                                                update=up, description=HINTS['rebar_chip_amount'])
    a['rebar_chip_crater'] = prop.FloatProperty(name='Crater Widening (cm)', default=4.0, min=0.0, soft_max=30.0,
                                                update=up, description=HINTS['rebar_chip_crater'])
    a['rebar_chip_face'] = prop.EnumProperty(name='Impact Face', items=FACE_ITEMS, default='0', update=up,
                                             description=HINTS['rebar_chip_face'])
    a['rebar_chips_material'] = prop.PointerProperty(name='Splinter Material', type=bpy.types.Material, update=up,
                                                     description=HINTS['rebar_chips_material'])


# собираем видимые настройки этого раздела
def draw(layout, p):
    """Fill Type = Wooden Chips, in the old-style fill panel."""
    layout.prop(p, 'rebar_chips_material')
    draw_settings(layout, p)


# показываем настройки выбранного эффекта
def draw_settings(col, p):
    col.prop(p, 'rebar_scale', text='Scale')
    col.prop(p, 'rebar_seed', text='Seed')
    col.prop(p, 'rebar_chip_plies', text='Plies')
    col.prop(p, 'rebar_chip_ply_thickness', text='Ply Thickness')
    col.prop(p, 'rebar_chip_length', text='Chip Length')
    col.prop(p, 'rebar_chip_width', text='Chip Width')
    col.prop(p, 'rebar_chip_sharp', text='Sharpness')
    col.prop(p, 'rebar_chip_lift', text='Lift')
    col.prop(p, 'rebar_chip_amount', text='Amount')
    col.prop(p, 'rebar_chip_crater', text='Crater Widening')
    col.prop(p, 'rebar_angle', text='Grain Direction')
    col.prop(p, 'rebar_chip_ply_angle', text='Grain Turn per Ply')
    from . import break_surface
    break_surface.draw_rebar_axes(col, p)
    row = col.row(align=True)
    row.prop(p, 'rebar_chip_face', text='Impact Face', expand=True)
    quality_ui.draw(col, p, 'rebar_detail', text='Detail')
