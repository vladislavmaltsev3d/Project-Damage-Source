# материалы и настройки внутреннего деревянного каркаса
# SPDX-License-Identifier: GPL-3.0-or-later
"""Wood Framing, the fourth Boolean fill (Fill Type = Wood Framing).

The core (break_framing.cpp) hollows the object behind a drywall skin around
the breaks and returns the timber frame - studs, plates, noggins, headers,
joists - and the nails left standing where the board came off, through the
same rebar_* outputs as the other fills. rebar_part is 0 for wood and 1 for
nails; rebar_grain carries each point in its member's own frame, which the
default wood material turns into grain and growth rings.
"""
from .blender_compat import enable_nodes
import bpy
import numpy as np

FIELDS = (
    ('stud_width', 'double', 3.8, True), ('stud_depth', 'double', 8.9, True),
    ('stud_spacing', 'double', 40.6, True), ('joist_depth', 'double', 14.0, True),
    ('noggin_spacing', 'double', 60.0, True), ('double_plate', 'bool', True, False),
    ('skin', 'double', 1.25, True), ('cavity_reach', 'double', 45.0, True),
    ('paper_tear', 'double', 4.0, True), ('paper_depth', 'double', 0.25, True),
    ('nails', 'double', 0.6, False), ('nail_length', 'double', 1.0, True),
    ('wood_broken', 'double', 0.2, False), ('facet_angle', 'double', 20.0, False),
)

FILL_ITEM = ('3', 'Wood Framing', 'Timber stud frame behind a drywall skin: studs, plates, noggins, headers, '
             'joists and nails. The object is hollowed behind the skin around the breaks')

PRESETS = {
                                        
    '16': dict(material='3', stud_width=3.8, stud_depth=8.9, stud_spacing=40.6, joist_depth=14.0,
               noggin_spacing=60.0, double_plate=True, skin=1.25, paper_tear=4.0, nails=0.6,
               wood_broken=0.2, scale=1.0, show_all=False),
                                                  
    '17': dict(material='3', stud_width=4.5, stud_depth=7.0, stud_spacing=62.5, joist_depth=16.0,
               noggin_spacing=80.0, double_plate=False, skin=1.25, paper_tear=3.0, nails=0.5,
               wood_broken=0.15, scale=1.0, show_all=False),
                                                                                 
    '18': dict(material='3', stud_width=3.8, stud_depth=8.9, stud_spacing=40.6, joist_depth=14.0,
               noggin_spacing=120.0, double_plate=False, skin=1.8, paper_tear=0.0, nails=0.7,
               wood_broken=0.3, scale=1.0, show_all=False),
                                                                              
    '19': dict(material='3', stud_width=5.0, stud_depth=10.0, stud_spacing=40.6, joist_depth=20.0,
               noggin_spacing=0.0, double_plate=True, skin=2.5, paper_tear=0.0, nails=0.8,
               wood_broken=0.35, scale=1.0, show_all=False),
}

PRESET_ITEMS = (
    ('16', 'Stud Wall', 'Wood Framing: 2x4 studs at 16 in behind 1/2 in drywall, noggins, doubled top plate'),
    ('17', 'Stud Wall (Metric)', 'Wood Framing: 45x70 studs at 62.5 cm behind 12.5 mm board'),
    ('18', 'Stage / Platform', 'Wood Framing: joists under the deck, framed skirts, thick ply skin'),
    ('19', 'Old Plaster Wall', 'Wood Framing: rough studs behind a thick plaster skin, no noggins'),
)

HINTS = {
    'rebar_stud_width': 'Thickness of the frame members along the face, in cm before Scale (the narrow side of a 2x4: 3.8)',
    'rebar_stud_depth': 'Depth of wall members into the object, in cm before Scale (the wide side of a 2x4: 8.9). '
                        'A member that would leave a thin gap runs through to the far skin',
    'rebar_stud_spacing': 'Distance between studs and between joists, centre to centre. Anchored at the object origin, '
                          'so moving a breaker shows other studs of the same frame',
    'rebar_joist_depth': 'Depth of the members under decks, floors and ceilings (faces flatter than 45 degrees)',
    'rebar_noggin_spacing': 'Height between rows of noggins / blocking between the studs. 0 = none. '
                            'Each noggin is shifted up or down by bay so it can be nailed',
    'rebar_double_plate': 'Double the top plates and the headers over openings',
    'rebar_skin': 'Drywall or plaster thickness. The object is hollow behind it around the breaks',
    'rebar_cavity_reach': 'How far past the breaks the object is hollowed. Larger hides the end of the cavity better',
    'rebar_paper_tear': 'Width of the face paper torn off around each hole (exposed gypsum). 0 = clean edge',
    'rebar_paper_depth': 'Thickness of the paper layer that tears off around the holes',
    'rebar_nails': 'Chance that a nail stays standing in the frame where the board came off',
    'rebar_nail_length': 'Controls the exposed length of each nail from its timber fixing point',
    'rebar_wood_broken': 'Chance that a member crossing a hole is snapped, with splintered ends. '
                         'Pieces nothing holds are dropped',
    'rebar_facet_angle': 'Curved shapes are framed as flat panels; faces within this angle of each other share one panel',
}


def wood_material():
    from .material_defaults import get
    return get('framing')


def nail_material():
    name = 'ProjectDamage_FramingNail'
    mat = bpy.data.materials.get(name)
    if mat is not None:
        return mat
    mat = bpy.data.materials.new(name)
    mat.diffuse_color = (0.35, 0.34, 0.33, 1.0)
    enable_nodes(mat)
    bsdf = mat.node_tree.nodes.get('Principled BSDF')
    if bsdf is not None:
        bsdf.inputs['Base Color'].default_value = (0.36, 0.35, 0.34, 1.0)
        bsdf.inputs['Metallic'].default_value = 0.9
        bsdf.inputs['Roughness'].default_value = 0.42
    return mat


def finish_mesh(mesh, source, part, grain, *, props=None):
    """Wood / nail slots and the grain attribute on the framing output mesh."""
    p = source.project_damage if props is None else props
    wood = getattr(p, 'rebar_framing_material', None) or wood_material()
    nail = getattr(p, 'rebar_nail_material', None) or nail_material()
    mesh.materials.clear()
    mesh.materials.append(wood)
    mesh.materials.append(nail)
    if part is not None and len(part) == len(mesh.polygons):
        values = np.asarray(part, dtype=np.int32)
        attr = mesh.attributes.new('rebar_part', 'INT', 'FACE')
        attr.data.foreach_set('value', values)
        mesh.polygons.foreach_set('material_index', (values == 1).astype(np.int32))
    if grain is not None and len(grain) == 3 * len(mesh.vertices):
        g = mesh.attributes.new('framing_grain', 'FLOAT_VECTOR', 'POINT')
        g.data.foreach_set('vector', np.asarray(grain, dtype=np.float32).ravel())


# собираем видимые настройки этого раздела
def draw(layout,p):
    from .panels import draw_framing_settings
    draw_framing_settings(layout,p)
