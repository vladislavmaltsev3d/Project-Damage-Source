"""Closed contours projected onto the damage of a single generator."""
import bpy
import numpy as np


# Native effect IDs used to identify the cutters that belong to a generator.
MODULES = {'concrete': 10, 'hit': 11, 'wood': 12, 'lath': 13, 'brick': 18}
TARGETS = {'THROUGH_HOLE_' + module.upper(): module for module in MODULES}


def add_properties(cls):
    from . import live, paint

    for module in MODULES:
        prefix = module + '_through_hole_'
        cls.__annotations__.update({
            prefix + 'mode': bpy.props.EnumProperty(
                name='Hole Placement', default='AUTO', update=live.touch_prop,
                items=(('AUTO', 'Automatic', 'Use procedural holes'),
                       ('DRAW', 'Draw', 'Cut closed contours where this generator already damaged the object'))),
            prefix + 'drawing': bpy.props.PointerProperty(
                name='Through Hole Drawing', type=bpy.types.Object, poll=paint.drawing_poll,
                update=live.touch_prop),
            prefix + 'surface': bpy.props.PointerProperty(
                name='Drawing Surface', type=bpy.types.Object, poll=mesh_surface,
                description='Optional mesh for placing new strokes, including a layer in front of the target',
                update=live.touch_prop),
            prefix + 'reach': bpy.props.FloatProperty(
                name='Search Distance (cm)', default=2.0, min=0.0, soft_max=30.0,
                description='How far to look on either side of the drawn contour for damage made by this generator',
                update=live.touch_prop),
        })


def mesh_surface(_self, obj):
    return obj.type == 'MESH'


def draw_controls(layout, props, module):
    """Draw the placement controls and return whether to show automatic settings."""
    from . import paint

    prefix = module + '_through_hole_'
    layout.prop(props, prefix + 'mode', expand=True)
    if getattr(props, prefix + 'mode') == 'AUTO':
        return True
    paint.drawing_row(layout, props, 'THROUGH_HOLE_' + module.upper())
    layout.prop(props, prefix + 'surface')
    layout.prop(props, prefix + 'reach')
    return False


def drawing_of(props, module):
    prefix = module + '_through_hole_'
    enabled = getattr(props, module + ('_break_enable' if module == 'wood' else '_hole_enable'))
    if enabled and getattr(props, prefix + 'mode') == 'DRAW':
        return getattr(props, prefix + 'drawing')
    return None


def references(props):
    for module in MODULES:
        drawing = drawing_of(props, module)
        if drawing is not None:
            yield drawing


def _contour(core, path, factor, linear, effect, reach):
    if len(path) < 3:
        return None
    metric = (path * factor) @ linear.T
    if np.linalg.norm(metric[-1] - metric[0]) < 1e-6:
        metric = metric[:-1]
    if len(metric) < 3:
        return None
    centre = metric.mean(axis=0)
    centred = metric - centre
    _, dimensions, basis = np.linalg.svd(centred, full_matrices=False)
    if dimensions[1] < 1e-5:
        return None
    basis[2] = np.cross(basis[0], basis[1])
    projected = centred @ basis.T
    frame = np.eye(4)
    frame[:3, :3] = basis @ linear
    frame[:3, 3] = -basis @ centre
    contour = core.PaintedThroughHole()
    contour.effect = effect
    contour.outline = projected[:, :2].reshape(-1).tolist()
    contour.to_plane = frame[:3].reshape(-1).tolist()
    contour.to_source = np.linalg.inv(frame)[:3].reshape(-1).tolist()
    # Include the stroke surface in a zero-reach search despite tessellation noise.
    epsilon = 0.002
    contour.near_min = float(projected[:, 2].min()) - reach - epsilon
    contour.near_max = float(projected[:, 2].max()) + reach + epsilon
    return contour


def pack(context, source, core, params, factor, wall_layers=()):
    from . import paint, wall_stack

    p = source.project_damage
    if p.all_off:
        return
    layers = [(module, effect, getattr(params, module), p) for module, effect in MODULES.items()]
    native = params.wall_layers
    for (serial, key, props), layer in zip(wall_layers, native, strict=True):
        family = wall_stack.family(key)
        layers.append((family, wall_stack.EFFECT_OFFSET + serial, getattr(layer, family), props))

    contours = []
    linear = np.asarray(source.matrix_world, dtype=np.float64)[:3, :3]
    for module, effect, block, props in layers:
        toggle = 'break_enable' if module == 'wood' else 'hole_enable'
        prefix = module + '_through_hole_'
        if not block.enable or not getattr(block, toggle) or getattr(props, prefix + 'mode') != 'DRAW':
            continue
        if not hasattr(core, 'PaintedThroughHole'):
            raise ValueError('Restart Blender to load the core with painted through holes')
        setattr(block, toggle, False)
        reach = getattr(props, prefix + 'reach')
        drawing = getattr(props, prefix + 'drawing')
        for path in paint.read_paths(context, source, drawing, min_points=3):
            contour = _contour(core, path, factor, linear, effect, reach)
            if contour is not None:
                contours.append(contour)
    if hasattr(params, 'through_holes'):
        params.through_holes = contours
    if native:
        params.wall_layers = native


def hash_inputs(h, params, hash_value):
    for contour in getattr(params, 'through_holes', ()):
        hash_value(h, contour.effect)
        hash_value(h, contour.near_min)
        hash_value(h, contour.near_max)
        for data in (contour.outline, contour.to_plane, contour.to_source):
            hash_value(h, len(data))
            h.update(np.asarray(data, dtype=np.float64).tobytes())
