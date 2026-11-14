# общие настройки шума для повреждённой поверхности
from . import quality_ui
# SPDX-License-Identifier: GPL-3.0-or-later
"""Procedural surface noise for the damage assets.

The Boolean break surface controls (Noise Scale, Detail, Roughness,
Distortion, Strength) for every damage asset whose surface switches between
Procedural and Texture:

- Concrete Damage / V2, Spall Surface: Detail next to the Size, Distortion,
  Roughness and Strength it already has (octaves of the spall relief).
- Lath & Plaster (V2, Riven Lath, Stud Framing) and Brick Damage (V2):
  procedural noise adds relief to the backing. Brick Damage leaves this
  optional refinement off by default.
- Hanging Hole, Hole Surface: the rough walls. The defaults are the old walls.

The core takes the new values once it is rebuilt (LathDamageParams /
BrickDamageParams / HangHoleParams noise_*, ConcreteDamageParams
surface_detail). With an older core the add-on still works: the panels say
the core needs a rebuild, and Hanging Hole Strength still scales the walls.
"""
import bpy
from . import live

MODULES = ('concrete', 'lath', 'brick', 'hang')

                                                                                 
NOISE_FIELDS = (
    ('noise_scale', 'noise_scale', 'Noise Scale', 1.0, True),
    ('noise_detail', 'noise_detail', 'Detail', 1.0, False),
    ('noise_distortion', 'noise_distortion', 'Distortion', 1.0, False),
    ('noise_roughness', 'noise_roughness', 'Roughness', 1.0, False),
    ('noise_strength', 'noise_strength', 'Strength', 1.0, True),
)

                                                                          
                                                                         
                         
CONCRETE_FIELDS = (
    ('surface_distortion', 'concrete_surface_distortion', 1.0),
    ('surface_roughness', 'concrete_surface_roughness', 100.0),
    ('surface_strength', 'concrete_surface_strength', 100.0),
)
_OWN_CONCRETE = set()                                                      


def _detail(default, what):
    return bpy.props.FloatProperty(
        name='Detail', default=default, min=1.0, max=8.0, update=live.touch_prop,
        description='Octaves of the noise: more detail adds smaller and smaller grain (%s)' % what)


def _roughness():
    return bpy.props.FloatProperty(
        name='Roughness', default=0.5, min=0.0, max=0.95, subtype='FACTOR', update=live.touch_prop,
        description='How strong each finer octave is compared to the one before (0.5 = the original)')


def _distortion():
    return bpy.props.FloatProperty(
        name='Distortion', default=0.0, min=0.0, soft_max=2.0, max=4.0, update=live.touch_prop,
        description='Twists the noise into flowing, less regular shapes (0 = the original)')


def _back_props(module, what, strength=0.0):
    return {
        module + '_noise_scale': bpy.props.FloatProperty(
            name='Noise Scale', default=10.0, min=0.1, soft_max=60.0, update=live.touch_prop,
            description='Size of the noise features (cm)'),
        module + '_noise_detail': _detail(4.0, 'default 4'),
        module + '_noise_roughness': _roughness(),
        module + '_noise_distortion': _distortion(),
        module + '_noise_strength': bpy.props.FloatProperty(
            name='Strength', default=strength, min=0.0, soft_max=6.0, update=live.touch_prop,
            description='Peak-to-peak depth of the noise on %s (cm). 0 = as built, the old surface' % what),
    }


PROPS = {
    'concrete_surface_detail': _detail(3.0, '3 = the original spall'),
    'hang_noise_scale': bpy.props.FloatProperty(
        name='Noise Scale', default=100.0, min=5.0, soft_min=25.0, soft_max=400.0, max=2000.0, subtype='PERCENTAGE',
        update=live.touch_prop, description='Size of the wall noise, x the hole size (100% = the original)'),
    'hang_noise_detail': _detail(2.0, '2 = the original walls'),
    'hang_noise_roughness': _roughness(),
    'hang_noise_distortion': _distortion(),
    'hang_noise_strength': bpy.props.FloatProperty(
        name='Strength', default=100.0, min=0.0, soft_max=400.0, max=1000.0, subtype='PERCENTAGE',
        update=live.touch_prop, description='Depth of the wall noise, x Roughness (100% = the original)'),
}
PROPS.update(_back_props('lath', 'the area behind the laths'))
PROPS.update(_back_props('brick', 'the faces behind the knocked-out bricks', strength=2.0))
PROPS['brick_back_noise_enable'] = bpy.props.BoolProperty(
    name='Back Surface Noise', default=False, update=live.touch_prop,
    description='Add geometric relief to the backing behind exposed bricks. Off keeps the backing as built. On adds local detail up to a fixed triangle budget')

_CONCRETE_FALLBACK = {
    'concrete_surface_distortion': bpy.props.FloatProperty(
        name='Distortion', default=0.0, min=0.0, max=4.0, update=live.touch_prop,
        description='Domain warp of the spall relief, in Aggregate Size units (0 = the original)'),
    'concrete_surface_roughness': bpy.props.FloatProperty(
        name='Roughness', default=45.0, min=0.0, max=100.0, subtype='PERCENTAGE', update=live.touch_prop,
        description='Balance between broad ripples and fine pits (45% = the original)'),
    'concrete_surface_strength': bpy.props.FloatProperty(
        name='Strength', default=100.0, min=0.0, max=400.0, subtype='PERCENTAGE', update=live.touch_prop,
        description='Depth of the procedural spall relief (100% = the original)'),
}


def add_properties(project_damage_props):
    ann = project_damage_props.__annotations__
    for k, v in PROPS.items():
        ann[k] = v
    for k, v in _CONCRETE_FALLBACK.items():
        if k not in ann:
            ann[k] = v
            _OWN_CONCRETE.add(k)


def _core_block(module):
    try:
        from . import core_loader
        return getattr(core_loader.load().Params(), module)
    except Exception:
        return None


_SUPPORT = {}


def core_supports(module):
    if module not in _SUPPORT:
        block = _core_block(module)
        field = 'surface_detail' if module == 'concrete' else 'noise_strength'
        _SUPPORT[module] = block is not None and hasattr(block, field)
    return _SUPPORT[module]


def fill(p, props, gscale=1.0):
    """props -> core Params, after params.fill_params set everything else."""
    block = getattr(p, 'concrete', None)
    if block is not None:
        if hasattr(block, 'surface_detail'):
            block.surface_detail = float(props.concrete_surface_detail)
        for field, prop, div in CONCRETE_FIELDS:
            if prop in _OWN_CONCRETE and hasattr(block, field):
                setattr(block, field, float(getattr(props, prop)) / div)
    for module in ('lath', 'brick'):
        block = getattr(p, module, None)
        if block is None:
            continue
        if module == 'brick' and not props.brick_back_noise_enable:
            block.tex_strength = 0.0
            if hasattr(block, 'noise_strength'):
                block.noise_strength = 0.0
            continue
        if not hasattr(block, 'noise_strength'):
            continue
        for field, suffix, _label, div, is_length in NOISE_FIELDS:
            value = float(getattr(props, module + '_' + suffix)) / div
            setattr(block, field, value * gscale if is_length else value)
        if module == 'brick':
            if block.surf_mode == 0:
                block.tex_edge = block.noise_scale / (5.0 * props.brick_back_resolution)
            elif block.tex_edge <= 0.0:
                block.tex_edge = block.tex_scale / (12.0 * props.brick_back_resolution)
    block = getattr(p, 'hang', None)
    if block is not None:
        if hasattr(block, 'noise_strength'):
            block.noise_scale = float(props.hang_noise_scale) / 100.0
            block.noise_detail = float(props.hang_noise_detail)
            block.noise_roughness = float(props.hang_noise_roughness)
            block.noise_distortion = float(props.hang_noise_distortion)
            block.noise_strength = float(props.hang_noise_strength) / 100.0
        else:
                                                         
            block.roughness = block.roughness * float(props.hang_noise_strength) / 100.0


def _prop_names(module):
    if module == 'concrete':
        return ('concrete_surface_detail',) + tuple(sorted(_OWN_CONCRETE))
    return tuple(module + '_' + suffix for _f, suffix, _l, _d, _len in NOISE_FIELDS)


def hash_props(h, props, params, hash_value):
    """Into build.fingerprints: the new values of every enabled module."""
    hash_value(h, 'asset_noise-2')
    for module in MODULES:
        block = getattr(params, module, None)
        if block is None or not getattr(block, 'enable', False):
            continue
        if module == 'brick':
            hash_value(h, props.brick_back_noise_enable)
            if not props.brick_back_noise_enable:
                continue
            hash_value(h, props.brick_back_resolution)
        for name in _prop_names(module):
            hash_value(h, float(getattr(props, name)))


                                                                             
    

def _rebuild_note(layout, module):
    if not core_supports(module):
        layout.label(text='Rebuild the core for these settings (see the notes)', icon='ERROR')


# собираем видимые настройки этого раздела
def draw(layout, p, module):
    """The procedural noise settings of one asset (Procedural selected)."""
    col = layout.column(align=True)
    if module == 'concrete':
        col.prop(p, 'concrete_aggregate_size', text='Size (cm)')
        quality_ui.draw(col, p, 'concrete_surface_detail', text='Detail')
        for _f, prop, _d in CONCRETE_FIELDS:
            if hasattr(p, prop):
                col.prop(p, prop)
    elif module == 'hang':
        col.prop(p, 'hang_noise_scale', text='Noise Scale')
        quality_ui.draw(col, p, 'hang_noise_detail', text='Detail')
        col.prop(p, 'hang_noise_distortion', text='Distortion')
        col.prop(p, 'hang_noise_roughness', text='Roughness')
        col.prop(p, 'hang_noise_strength', text='Strength')
    else:
        col.prop(p,module+'_noise_strength',text='Strength (cm)')
        if getattr(p,module+'_noise_strength')>0.0:
            col.prop(p,module+'_noise_scale',text='Noise Scale (cm)')
            col.prop(p,module+'_noise_detail',text='Detail')
            col.prop(p,module+'_noise_distortion',text='Distortion')
            col.prop(p,module+'_noise_roughness',text='Roughness')
        else:
            layout.label(text='Raise Strength to add procedural surface relief.',icon='INFO')
    _rebuild_note(layout, module)


def draw_box(layout, p, module):
    """Hanging Hole: its own box under Hole Surface."""
    from . import effect_layout
    box = layout.box()
    box.enabled = p.hang_enable and not p.all_off
    effect_layout.heading(box, 'Procedural Noise')
    draw(box, p, module)


def draw_concrete_detail(layout, p):
    quality_ui.draw(layout, p, 'concrete_surface_detail', text='Detail')
