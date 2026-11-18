"""Per-object instances of the wall damage generators."""
from types import SimpleNamespace

import bpy
import numpy as np


FLAGS = {
    'concrete': 'concrete_enable', 'concrete_v2': 'concrete_v2_enable',
    'pipes': 'pipe_damage_enable', 'cables': 'cable_damage_enable',
    'lath': 'lath_enable', 'lath_v2': 'lath_v2_enable',
    'lath_riven': 'lath_riven_enable', 'lath_framing': 'lath_framing_enable',
}
KEYS = frozenset(FLAGS)
WOOD = frozenset(key for key in KEYS if key.startswith('lath'))
INSTANCE = 'wall_instance'
ON = 'wall_enabled'
VALUES = 'wall_values'
EDIT = '_pd_wall_edit'
SERIAL = '_pd_wall_serial'
MATERIALS = ('surface_damage_material', 'fracture_l1_material')
BASE_MATERIALS = '_pd_wall_base_materials'
PROXIMITY_SNAPSHOTS = '_pd_wall_proximity_snapshots'
PROXIMITY_FIELDS = {'concrete_proximity', 'lath_proximity'}
MATERIAL_CLASSES = {4: MATERIALS[0], 1: MATERIALS[1]}
EFFECT_OFFSET = 10000


def family(key):
    return 'lath' if key in WOOD else 'concrete'


def fields(p, key):
    prefixes = (family(key) + '_',)
    if key in {'pipes', 'cables'}:
        prefixes += (key[:-1] + '_',)
    flags = set(FLAGS.values())

    def accepted(r):
        return ((not r.is_readonly or r.identifier in PROXIMITY_FIELDS)
                and r.identifier not in flags
                and (r.identifier in MATERIALS or
                     (r.identifier.startswith(prefixes)
                      and not r.identifier.startswith(('pipe_break_', 'cable_break_')))))
    # Fixed per registered class and prefix set: cached instead of walking
    # every RNA property on each frame change and depsgraph event.
    from . import rna_fields
    return rna_fields.select(p, ('wall_stack', prefixes), accepted)


def field_map(p, key):
    from . import rna_fields
    prefixes = (family(key) + '_',)
    if key in {'pipes', 'cables'}:
        prefixes += (key[:-1] + '_',)
    slot = ('wall_stack', prefixes)
    fields(p, key)
    return rna_fields.mapping(p, slot, lambda r: True)


def object_inputs(collection):
    return {
        str(i): dict(obj=item.obj, radius=item.radius, use_size=item.use_size, enabled=item.enabled)
        for i, item in enumerate(collection) if item.obj is not None
    }


def settings(p, key):
    values = {}
    for prop in fields(p, key):
        name = prop.identifier
        value = getattr(p, name)
        if prop.type == 'COLLECTION':
            values[name] = object_inputs(value)
        elif value is not None:
            values[name] = list(value) if getattr(prop, 'is_array', False) or isinstance(value, set) else value
    if key in {'pipes', 'cables'}:
        from .material_controls import automatic_material_keys
        for marker in automatic_material_keys(key[:-1] + '_material'):
            if marker in p:
                values[marker] = p[marker]
    return values


def restore(p, key, values):
    for prop in fields(p, key):
        name = prop.identifier
        if prop.type == 'COLLECTION':
            collection = getattr(p, name)
            collection.clear()
            for data in values.get(name, {}).values():
                entry = collection.add()
                for field, value in data.items():
                    setattr(entry, field, value)
        elif name not in values:
            p.property_unset(name)
        else:
            value = values[name]
            if prop.type == 'ENUM' and prop.is_enum_flag:
                value = set(value)
            setattr(p, name, value)
    if key in {'pipes', 'cables'}:
        from .material_controls import restore_automatic_material
        restore_automatic_material(p, key[:-1] + '_material', values)


def rows(source):
    return (row for row in source.project_damage_effects if row.get(INSTANCE) and row.key in KEYS)


def edited(source):
    serial = source.get(EDIT, 0)
    return next((row for row in rows(source) if row[INSTANCE] == serial), None)


def migrate_object_inputs(source):
    if SERIAL not in source or source.get(PROXIMITY_SNAPSHOTS):
        return
    p = source.project_damage
    shared = {name: object_inputs(getattr(p, name)) for name in PROXIMITY_FIELDS}
    for row in rows(source):
        name = family(row.key) + '_proximity'
        values = dict(row.get(VALUES, {}))
        if name not in values:
            values[name] = shared[name]
            row[VALUES] = values
    source[PROXIMITY_SNAPSHOTS] = True


def save(source):
    migrate_object_inputs(source)
    row = edited(source)
    if row is not None:
        row[VALUES] = settings(source.project_damage, row.key)


def set_flags(source, row):
    p = source.project_damage
    for key, flag in FLAGS.items():
        value = bool(row is not None and row.key == key and row.get(ON))
        if getattr(p, flag) != value:
            setattr(p, flag, value)


def select(source, row):
    migrate_object_inputs(source)
    p = source.project_damage
    serial = row.get(INSTANCE, 0) if row is not None else 0
    if serial == source.get(EDIT, 0):
        return
    from . import live
    previous = live._APPLYING
    live._APPLYING = True
    try:
        previous_row = edited(source)
        if previous_row is not None:
            save(source)
            restore(p, previous_row.key, {})
        else:
            source[BASE_MATERIALS] = {name: getattr(p, name) for name in MATERIALS if getattr(p, name)}
        source[EDIT] = serial
        if serial:
            restore(p, row.key, row.get(VALUES, {}))
        else:
            for name in MATERIALS:
                setattr(p, name, source.get(BASE_MATERIALS, {}).get(name))
        set_flags(source, row if serial else None)
    finally:
        live._APPLYING = previous


def next_id(source):
    serial = source.get(SERIAL, 0) + 1
    source[SERIAL] = serial
    return serial


def migrate(source):
    if SERIAL in source:
        return
    p = source.project_damage
    source[SERIAL] = 0
    source[BASE_MATERIALS] = {name: getattr(p, name) for name in MATERIALS if getattr(p, name)}
    for row in source.project_damage_effects:
        if row.key not in KEYS:
            continue
        row[INSTANCE] = next_id(source)
        row[ON] = getattr(p, FLAGS[row.key])
        if row[ON]:
            row[VALUES] = settings(p, row.key)
        elif row.key in WOOD:
            saved = source.get('project_damage_lath_variant_values', {}).get(row.key)
            if saved:
                row[VALUES] = dict(saved)
    i = source.project_damage_effect_index
    if i < len(source.project_damage_effects):
        row = source.project_damage_effects[i]
        if row.get(INSTANCE):
            select(source, row)
            return
    set_flags(source, None)


def add(source, key):
    from . import generator_defaults, live
    previous = live._APPLYING
    live._APPLYING = True
    try:
        migrate(source)
        row = source.project_damage_effects.add()
        row.key = key
        row[INSTANCE] = next_id(source)
        row[ON] = True
        select(source, row)
        generator_defaults.reset(source, key)
        set_flags(source, row)
        save(source)
        source.project_damage.all_off = False
        source.project_damage_effect_index = len(source.project_damage_effects) - 1
        return row
    finally:
        live._APPLYING = previous


def row_enabled(source, row, key):
    if row is not None and row.get(INSTANCE):
        return bool(row.get(ON))
    if SERIAL in source and key in KEYS:
        return any(item.key == key and item.get(ON) for item in rows(source))
    return bool(getattr(source.project_damage, FLAGS[key]))


def chosen_index(source, key, instance=0):
    collection = source.project_damage_effects
    selected = source.project_damage_effect_index
    if instance:
        return next((i for i, r in enumerate(collection) if r.get(INSTANCE) == instance), None)
    if selected < len(collection) and collection[selected].key == key:
        return selected
    return next((i for i in range(len(collection) - 1, -1, -1) if collection[i].key == key), None)


def remove(source, index):
    from . import paint
    row = source.project_damage_effects[index]
    serial = row[INSTANCE]
    save(source)
    drawings = {v for k, v in row.get(VALUES, {}).items()
                if 'drawing' in k and isinstance(v, bpy.types.Object)}
    if serial == source.get(EDIT):
        select(source, None)
    collection = source.project_damage_effects
    collection.remove(index)
    selected = source.project_damage_effect_index
    selected = min(selected - int(index < selected), max(0, len(collection) - 1))
    source.project_damage_effect_index = selected
    if selected < len(collection):
        select(source, collection[selected])
    for drawing in drawings:
        paint.delete_unshared_drawing(source, drawing)


class Props:
    def __init__(self, base, key, values):
        self._base = base
        self._values = values
        self._key = key
        self._isolated = field_map(base, key)

    def __getitem__(self, key):
        return self._values[key]

    def __setitem__(self, key, value):
        self._values[key] = value

    def get(self, key, default=None):
        return self._values.get(key, default)

    def __getattr__(self, name):
        if name in FLAGS.values():
            return name == FLAGS[self._key]
        if name == 'ui_boolean_enable':
            return False
        prop = self._isolated.get(name)
        if prop is None:
            return getattr(self._base, name)
        if prop.type == 'COLLECTION':
            return [SimpleNamespace(**dict(item)) for item in self._values.get(name, {}).values()]
        if name in self._values:
            return self._values[name]
        if prop.type == 'POINTER':
            return None
        return prop.default_array if getattr(prop, 'is_array', False) else prop.default


def layers(source, include_disabled=False):
    migrate_object_inputs(source)
    p = source.project_damage
    if p.all_off and not include_disabled:
        return []
    active = source.get(EDIT, 0)
    return [(row[INSTANCE], row.key, Props(p, row.key,
             settings(p, row.key) if row[INSTANCE] == active else dict(row.get(VALUES, {}))))
            for row in rows(source) if include_disabled or row.get(ON)]


def references(source):
    from . import through_holes
    for _, key, p in layers(source):
        key_family = family(key)
        drawing = getattr(p, key_family + '_drawing')
        if drawing is not None:
            yield drawing
        if (hole := through_holes.drawing_of(p, key_family)) is not None:
            yield hole
        for item in getattr(p, key_family + '_proximity'):
            if item.enabled and item.obj is not None:
                yield item.obj


def fingerprint(context, source):
    from .incremental import _state
    return [(serial, key, _state(context, source, p._values)) for serial, key, p in layers(source)]


def pack(context, source, core, params, points, polys, factor, reports, layers):
    from . import background_wire, cables, extra_effects, paint, params as parameter_tools, pipes
    params.concrete.enable = False
    params.lath.enable = False
    native = []
    for serial, key, props in layers:
        layer = parameter_tools.fill_params(props, core, obj=source, geometry=(points, polys, factor))
        name = family(key)
        strokes = []
        if key in {'pipes', 'cables'}:
            service = pipes if key == 'pipes' else cables
            strokes = service.before_core(context, source, layer, points, polys, factor, [], reports, props=props)
        elif name == 'concrete' and props.concrete_mode == '1':
            strokes = extra_effects.concrete_draw_paths(context, source, points, polys, factor, props=props)
        elif name == 'lath' and props.lath_mode == '1':
            strokes = [np.ascontiguousarray(path * factor) for path in
                       paint.read_paths(context, source, props.lath_drawing, min_points=2)]
        layer.wall_instance = serial
        # Keep the original random layout of legacy instances. Subsequent copies get their own pattern.
        layer.seed = (layer.seed + (serial - 1) * 9973) % 2147483647
        layer.wall_stroke_counts = [len(stroke) for stroke in strokes]
        layer.wall_stroke_points = np.concatenate(strokes).reshape(-1).tolist() if strokes else []
        encoded = background_wire.encode(layer, core)
        encoded['fields'] = {field: value for field, value in encoded['fields'].items()
                             if field in {name, 'seed', 'wall_instance', 'wall_stroke_counts', 'wall_stroke_points'}}
        native.append(background_wire.decode(encoded, core))
    params.wall_layers = native


def default_materials():
    from .mesh_io import DAMAGE_MATERIAL_NAMES
    return {cls: bpy.data.materials.get(DAMAGE_MATERIAL_NAMES[cls]) for cls in MATERIAL_CLASSES}


def face_materials(mesh, out, layers, defaults):
    from . import effect_groups
    ids = np.asarray(out.get('effect_ids', ()), dtype=np.int32)
    classes = np.asarray(out.get('tri_class', ()), dtype=np.int32)
    if len(ids) != len(mesh.polygons) or not np.any(ids >= EFFECT_OFFSET):
        return
    ids = ids.copy()
    instance_ids = np.zeros(len(ids), dtype=np.int32)
    material_indices = np.empty(len(mesh.polygons), dtype=np.int32)
    mesh.polygons.foreach_get('material_index', material_indices)
    for instance, key, p in layers:
        mask = ids == EFFECT_OFFSET + instance
        if not np.any(mask):
            continue
        instance_ids[mask] = instance
        ids[mask] = effect_groups.KEYS.index(family(key))
        for cls, name in MATERIAL_CLASSES.items():
            material = getattr(p, name) or defaults.get(cls)
            if material is not None:
                subset = mask & (classes == cls)
                if subset.any():
                    slot = next((i for i, mat in enumerate(mesh.materials) if mat == material), None)
                    if slot is None:
                        slot = len(mesh.materials)
                        mesh.materials.append(material)
                    material_indices[subset] = slot
    mesh.polygons.foreach_set('material_index', material_indices)
    out['effect_ids'] = ids
    mesh.attributes.new('project_damage_wall_instance', 'INT', 'FACE').data.foreach_set('value', instance_ids)


def geometry_for(out, name, serial):
    selected = np.asarray(out[name + '_instance']) == serial
    faces = np.asarray(out[name + '_polys'])[selected]
    used, indices = np.unique(faces, return_inverse=True)
    layer = {name + '_points': out[name + '_points'][used], name + '_polys': indices.reshape(faces.shape)}
    if name == 'lath':
        layer['lath_piece'] = out['lath_piece'][selected]
    return layer


def output_objects(source, stamp, serial=0):
    uid = source.get('project_damage_uid')
    return (ob for ob in bpy.data.objects if uid and ob.get(stamp) == uid and ob.get(INSTANCE, 0) == serial)


def update_material(source, field, material):
    row = edited(source)
    if row is None:
        return False
    from . import build, cables, concrete_rebar_output, lath_output, material_update, mesh_io, pipes
    serial = row[INSTANCE]
    stamp = {
        'pipe_material': pipes.PIPES_STAMP if row.key == 'pipes' else None,
        'cable_material': cables.CABLES_STAMP if row.key == 'cables' else None,
        'lath_material': lath_output.STAMP if row.key in WOOD else None,
        'concrete_rebar_material': concrete_rebar_output.STAMP if row.key in {'concrete', 'concrete_v2'} else None,
    }.get(field)
    if stamp:
        for obj in output_objects(source, stamp, serial):
            material_update.assign_slot(obj, 0, material)
        return True
    if field in MATERIALS:
        result = build.result_of(source)
        if result is not None:
            mesh = result.data
            instances = material_update.face_values(mesh, 'project_damage_wall_instance')
            cls = next(cls for cls, name in MATERIAL_CLASSES.items() if name == field)
            classes = material_update.face_values(mesh, mesh_io.CLASS_NAMES[cls])
            if instances is not None and classes is not None:
                material_update.assign(result, material, (instances == serial) & classes.astype(bool))
        return True
    return False


def cleanup_outputs(source, serials):
    from . import boolean_stack, cables, concrete_rebar_output, lath_output, pipes
    stamps = cables.CABLES_STAMP, concrete_rebar_output.STAMP, lath_output.STAMP, pipes.PIPES_STAMP
    uid = source.get('project_damage_uid')
    if not uid:
        return
    for ob in list(bpy.data.objects):
        serial = ob.get(INSTANCE)
        if serial and serial < boolean_stack.OUTPUT_OFFSET and serial not in serials and any(ob.get(stamp) == uid for stamp in stamps):
            mesh = ob.data
            bpy.data.objects.remove(ob, do_unlink=True)
            if mesh.users == 0:
                bpy.data.meshes.remove(mesh)
