# добавляем детали к форме грунтового разлома
# SPDX-License-Identifier: GPL-3.0-or-later
"""Ground Fissures: Realistic style (recipe ground-fissure-v2).

Broken concrete instead of clean cell-fracture blocks: faceted fracture
faces on slabs, sockets and chasm walls, chipped and crumbled edges, broken
corners, an uneven underside, irregular slab outlines, wedge and triangle
slabs (corner breaks, diagonal cracks), hairline cracks across slab tops,
sheared angular rubble and small debris.

The core takes these values once it is rebuilt (GroundFissureParams
realistic / edge_damage / face_rough / surface_cracks / debris /
debris_size / split_chance). With an older core the add-on keeps the Classic fissure until Blender is restarted with the updated native core.
"""
import bpy
from . import live

FIELDS = (
    ('realistic', 'realistic'),
    ('edge_damage', 'edge_damage'),
    ('face_rough', 'face_rough'),
    ('surface_cracks', 'surface_cracks'),
    ('debris', 'debris'),
    ('debris_size', 'debris_size'),
    ('split_chance', 'split_chance'),
)

PROPS = {
    'fissure_realistic': bpy.props.BoolProperty(
        name='Realistic', default=True, update=live.touch_prop,
        description='Broken concrete: rough faceted faces, chipped edges and corners, irregular outlines, '
                    'hairline cracks and debris. Off = the Classic clean-block fissure'),
    'fissure_edge_damage': bpy.props.FloatProperty(
        name='Edge Damage', default=0.7, min=0.0, max=2.0, update=live.touch_prop,
        description='Chips slab edges and corners and crumbles the chasm rim. 0: no added chipping; 2: heavy edge damage'),
    'fissure_face_rough': bpy.props.FloatProperty(
        name='Face Roughness', default=0.7, min=0.0, max=2.0, update=live.touch_prop,
        description='Relief on slab sides, matching sockets and chasm walls. 0: flat fracture faces; 2: strong faceted relief'),
    'fissure_surface_cracks': bpy.props.FloatProperty(
        name='Surface Cracks', default=0.6, min=0.0, max=3.0, update=live.touch_prop,
        description='Adds hairline cracks to slab tops, approximately this many per slab. 0: none; 3: dense surface cracking'),
    'fissure_debris': bpy.props.FloatProperty(
        name='Debris', default=1.0, min=0.0, max=3.0, update=live.touch_prop,
        description='Density of small fragments on the chasm floor and rims. 0: no extra debris; 3: three times the normal density'),
    'fissure_debris_size': bpy.props.FloatProperty(
        name='Debris Size', default=1.0, min=0.25, max=3.0, update=live.touch_prop,
        description='Debris size relative to slab thickness. 0.25: quarter-size fragments; 1: normal size; 3: triple-size fragments'),
    'fissure_split_chance': bpy.props.FloatProperty(
        name='Corner Breaks', default=0.55, min=0.0, max=1.0, subtype='FACTOR', update=live.touch_prop,
        description='Share of slabs split into triangular pieces: a broken-off corner, two corners, '
                    'or a diagonal crack corner to corner. 0: no splits; 1: every eligible slab splits. The loose piece sags a few degrees. '
                    'Slab Skew sets how strongly the cross cracks lean into wedges and triangles'),
}


def add_properties(project_damage_props):
    ann = project_damage_props.__annotations__
    for k, v in PROPS.items():
        ann[k] = v


def fill(p, props):
    """props -> core Params, after params.fill_params set everything else."""
    block = getattr(p, 'fissure', None)
    if block is None or not hasattr(block, 'realistic'):
        return
    for suffix, field in FIELDS:
        if not hasattr(block, field):
            continue
        value = getattr(props, 'fissure_' + suffix)
        setattr(block, field, bool(value) if field == 'realistic' else float(value))


def hash_props(h, props, params, hash_value):
    """Into build.fingerprints: the new values while Ground Fissures is on."""
    block = getattr(params, 'fissure', None)
    if block is None or not getattr(block, 'enable', False):
        return
    hash_value(h, 'fissure_realism-2')
    for suffix, _field in FIELDS:
        hash_value(h, getattr(props, 'fissure_' + suffix))


