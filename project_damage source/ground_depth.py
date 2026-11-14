# задаём глубину грунтовых эффектов, в том числе на плоском меше
import bpy
from . import live

GROUND_VERTICAL = ('crack_depth','gap_depth','plate_sink','plate_tilt','pothole_depth','heave_height')
FISSURE_VERTICAL = ('branch_depth','slab_thick_min','slab_thick_max')

def add_properties(cls):
    cls.__annotations__['ground_depth'] = bpy.props.FloatProperty(
        name='Depth (cm)', default=6.0, min=0.02, soft_max=100.0, max=100000.0, update=live.touch_prop,
        description='Global vertical size of Ground Cracks, in cm before Master Scale. Also scales crack depth, plate thickness and sinking. 0.02: very shallow; 6: normal depth; higher values make deeper cavities and thicker slices. Open surfaces are extended down automatically; closed objects keep their original thickness')

def fill(params, props):
    ground = params.ground
    if ground.enable:
        scale = props.ground_depth / 6.0
        for name in GROUND_VERTICAL:
            setattr(ground, name, getattr(ground, name) * scale)
    fissure = params.fissure
    if fissure.enable:
        scale = props.fissure_depth / 450.0
        for name in FISSURE_VERTICAL:
            setattr(fissure, name, getattr(fissure, name) * scale)

def shell_thickness(props, thickness):
    if props is None or props.all_off:
        return thickness
    from .params import _clamp_scale, _fissure_scale
    global_scale = _clamp_scale(props.global_scale/100.0)
    if props.ground_enable:
        scale = props.ground_depth / 6.0 * global_scale
        deepest = max(props.ground_pothole_depth * 1.5, props.ground_gap_depth * 2.0,
                      props.ground_crack_depth * 1.5, props.ground_plate_sink + props.ground_plate_tilt)
        cap = max(0.01, props.ground_thickness_cap) if props.ground_thickness_cap > 0 else 1.0
        thickness = max(thickness, deepest * scale * 1.2 / cap)
    if props.fissure_enable:
        scale = global_scale * _fissure_scale(props)
        vertical = props.fissure_depth / 450.0
        deepest = props.fissure_depth * (1.0 + max(0.,props.fissure_depth_random))
        if props.fissure_branch_enable:
            deepest = max(deepest, props.fissure_branch_depth * vertical)
        cap = max(0.01, props.fissure_thickness_cap) if props.fissure_thickness_cap > 0 else 1.0
        thickness = max(thickness, deepest * 1.2 / cap * scale)
        if props.fissure_slab_enable:
            thickness = max(thickness, 2.5 * max(props.fissure_slab_thick_min,props.fissure_slab_thick_max) * vertical * scale)
    return thickness
