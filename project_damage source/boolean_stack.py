"""Independent Boolean cutter lists and fill settings on a source object."""

import bpy
import numpy as np
from types import SimpleNamespace


INSTANCE = 'boolean_instance'
VALUES = 'boolean_values'
ON = 'boolean_enabled'
EDIT = '_pd_boolean_edit'
SERIAL = '_pd_boolean_serial'
CUTTER_SNAPSHOTS = '_pd_boolean_cutter_snapshots'
OUTPUT_OFFSET = 1000000
EFFECT_OFFSET = 2000000
RIM_OFFSET = 3000000
FACE_OWNER = 'project_damage_boolean_instance'
PREFIXES = ('rebar_', 'boolean_', 'break_', 'breaker_', 'frame_restructure_', 'pipe_', 'cable_')
LEGACY_INSTANCE_PREFIXES = ('rebar_', 'boolean_central_', 'boolean_wood_', 'boolean_frame_', 'boolean_stable_',
                            'boolean_volume_', 'boolean_generated_', 'boolean_pipe_', 'boolean_cable_', 'pipe_', 'cable_')
FLAGS = {'rebar_enable', 'pipe_break_enable', 'cable_break_enable'}


def _is_field(r):
    return r.identifier == 'breakers' or (not r.is_readonly
            and r.type in {'BOOLEAN', 'INT', 'FLOAT', 'STRING', 'ENUM', 'POINTER'}
            and r.identifier.startswith(PREFIXES)
            and r.identifier != 'break_variant'
            and not r.identifier.startswith(('pipe_damage_', 'cable_damage_')))


def fields(p):
    # The subset is fixed for a registered class; walking all RNA properties
    # on every frame change and depsgraph event was the main playback cost.
    from . import rna_fields
    return rna_fields.select(p, 'boolean_stack', _is_field)


def field_map(p):
    from . import rna_fields
    return rna_fields.mapping(p, 'boolean_stack', _is_field)


def cutter_settings(p):
    return {
        str(i): {
            'obj': row.obj, 'from_collection': bool(row.from_collection),
        }
        for i, row in enumerate(p.breakers) if row.obj is not None
    }


def values(p):
    from .material_controls import TUBE_MATERIAL_FIELDS, automatic_material_keys
    saved = {}
    for prop in fields(p):
        value = getattr(p, prop.identifier)
        if prop.identifier == 'breakers':
            saved[prop.identifier] = cutter_settings(p)
        elif value is not None:
            saved[prop.identifier] = list(value) if getattr(prop, 'is_array', False) or isinstance(value, set) else value
    for field in TUBE_MATERIAL_FIELDS:
        for key in automatic_material_keys(field):
            if key in p:
                saved[key] = p[key]
    return saved


def restore(p, saved):
    from .material_controls import TUBE_MATERIAL_FIELDS, restore_automatic_material
    for prop in fields(p):
        name = prop.identifier
        if name == 'breakers':
            p.breakers.clear()
            for data in saved.get('breakers', {}).values():
                obj = data.get('obj')
                if obj is None:
                    continue
                row = p.breakers.add()
                row.from_collection = data.get('from_collection', False)
                row.obj = obj
        elif name not in saved:
            p.property_unset(name)
        else:
            value = saved[name]
            setattr(p, name, set(value) if prop.type == 'ENUM' and prop.is_enum_flag else value)
    for field in TUBE_MATERIAL_FIELDS:
        restore_automatic_material(p, field, saved)


def rows(source):
    return (row for row in source.project_damage_effects if row.key == 'boolean' and row.get(INSTANCE))


def by_id(source, serial):
    return next((row for row in rows(source) if row[INSTANCE] == serial), None)


def edited(source):
    return by_id(source, source.get(EDIT, 0))


def save(source):
    row = edited(source)
    if row is not None:
        row[VALUES] = values(source.project_damage)


def refresh_master(source):
    p = source.project_damage
    active = any(row.get(ON) for row in rows(source))
    if p.ui_boolean_enable != active:
        p.ui_boolean_enable = active


def select(source, row):
    if SERIAL in source:
        ensure_cutter_snapshots(source)
    serial = row.get(INSTANCE, 0) if row is not None else 0
    if serial == source.get(EDIT, 0):
        return
    from . import live
    previous = live._APPLYING
    live._APPLYING = True
    p = source.project_damage
    try:
        before = edited(source)
        if before is not None:
            before[VALUES] = values(p)
        restore(p, row.get(VALUES, {}) if serial else {})
        source[EDIT] = serial
    finally:
        live._APPLYING = previous
    if serial:
        from . import boolean_break
        boolean_break.sync_collection(source)


def migrate(source):
    if SERIAL in source:
        ensure_cutter_snapshots(source)
        return
    p = source.project_damage
    row = next((row for row in source.project_damage_effects if row.key == 'boolean'), None)
    if row is None and (len(p.breakers) or p.rebar_enable or p.pipe_break_enable or p.cable_break_enable):
        row = source.project_damage_effects.add()
        row.key = 'boolean'
    source[SERIAL] = 0
    source[CUTTER_SNAPSHOTS] = True
    if row is not None:
        row[INSTANCE] = source[SERIAL] = source[EDIT] = 1
        row[ON] = p.ui_boolean_enable
        row[VALUES] = values(p)


def ensure_cutter_snapshots(source):
    if source.get(CUTTER_SNAPSHOTS):
        return
    shared = {name: value for name, value in values(source.project_damage).items()
              if not name.startswith((*LEGACY_INSTANCE_PREFIXES, '_pd_auto_'))}
    for row in rows(source):
        row[VALUES] = {**shared, **dict(row.get(VALUES, {}))}
    source[CUTTER_SNAPSHOTS] = True


def add(source, variant):
    from . import live, wall_stack
    previous = live._APPLYING
    live._APPLYING = True
    try:
        migrate(source)
        if wall_stack.SERIAL in source:
            wall_stack.select(source, None)
        row = source.project_damage_effects.add()
        row.key = 'boolean'
        row[INSTANCE] = source[SERIAL] = source[SERIAL] + 1
        row[ON] = True
        select(source, row)
        p = source.project_damage
        for flag in FLAGS:
            setattr(p, flag, False)
        p.break_variant = variant
        p.all_off = False
        refresh_master(source)
        source.project_damage_effect_index = len(source.project_damage_effects) - 1
        return row
    finally:
        live._APPLYING = previous


def chosen_index(source, serial=0):
    collection = source.project_damage_effects
    i = source.project_damage_effect_index
    if serial:
        return next((i for i, row in enumerate(collection) if row.get(INSTANCE) == serial), None)
    if i < len(collection) and collection[i].key == 'boolean':
        return i
    return next((i for i in reversed(range(len(collection))) if collection[i].key == 'boolean'), None)


def remove(source, index):
    from . import paint
    collection = source.project_damage_effects
    drawing = layer_props(source, collection[index]).rebar_drawing
    if collection[index] == edited(source):
        select(source, None)
    collection.remove(index)
    selected = min(source.project_damage_effect_index, max(0, len(collection) - 1))
    source.project_damage_effect_index = selected
    if collection and collection[selected].key == 'boolean':
        select(source, collection[selected])
    refresh_master(source)
    if drawing is not None and all(layer_props(source, row).rebar_drawing != drawing for row in rows(source)):
        paint.delete_unshared_drawing(source, drawing)


class Props:
    def __init__(self, base, saved):
        self._base = base
        self._saved = saved
        self._fields = field_map(base)

    def get(self, name, default=None):
        if name.startswith(('_pd_auto_pipe_', '_pd_auto_cable_')):
            return self._saved.get(name, default)
        return self._saved.get(name, self._base.get(name, default) if name not in self._fields else default)

    def __getattr__(self, name):
        if name in {'pipe_damage_enable', 'cable_damage_enable'}:
            return False
        prop = self._fields.get(name)
        if prop is None:
            return getattr(self._base, name)
        if name == 'breakers':
            from .boolean_break import collection_cutters
            manual = dict.fromkeys(row.get('obj') for row in self._saved.get(name, {}).values()
                                   if row.get('obj') is not None and not row.get('from_collection'))
            collection = self._saved.get('break_objects')
            automatic = collection_cutters(self._base.id_data, collection) if collection is not None else ()
            return [SimpleNamespace(obj=obj) for obj in dict.fromkeys((*manual, *automatic))]
        if name in self._saved:
            return self._saved[name]
        if prop.type == 'POINTER':
            return None
        return prop.default_array if getattr(prop, 'is_array', False) else prop.default


class BaseTubes:
    def __init__(self, base):
        self.base = base

    def __getattr__(self, name):
        if name in {'pipe_break_enable', 'cable_break_enable'}:
            return False
        return getattr(self.base, name)


def layer_props(source, row):
    base = source.project_damage
    return Props(base, values(base) if row == edited(source) else dict(row.get(VALUES, {})))


def layers(source):
    if source.project_damage.all_off:
        return []
    if SERIAL in source:
        ensure_cutter_snapshots(source)
    return [(row[INSTANCE], layer_props(source, row)) for row in rows(source) if row.get(ON)]


def references(source):
    for _, p in layers(source):
        if p.rebar_drawing is not None:
            yield p.rebar_drawing


def cutter_objects(source, *, include_disabled=False):
    if SERIAL not in source:
        return tuple(row.obj for row in source.project_damage.breakers if row.obj is not None)
    return tuple(dict.fromkeys(item.obj for row in rows(source) if include_disabled or row.get(ON)
                               for item in layer_props(source, row).breakers))


def cutter_signature(source):
    def links(items):
        return tuple((item.obj.name, cutter.name if (cutter := item.obj.project_damage_breaker.cutter) else None)
                     for item in items if item.obj is not None)

    if SERIAL not in source:
        return links(source.project_damage.breakers)
    return tuple((serial, links(p.breakers)) for serial, p in layers(source))


def pack(context, source, core, params, factor, fills):
    from . import params as tools, preview_quality, rebar
    packed = []
    breakers = []
    for serial, p in fills:
        layer = core.Params()
        layer.wall_instance = serial
        layer.seed = p.seed
        layer.boolean_tube_fill = p.pipe_break_enable or p.cable_break_enable
        layer.break_surf_dmg = p.break_rim_enable and p.break_surf_dmg
        layer.break_rim_amount = p.break_rim_amount if p.break_rim_enable else 0
        layer.break_rim_size = p.break_rim_size / 100
        layer.break_rim_depth = p.break_rim_depth / 100
        tools.fill_break_rebar(p, core, layer.break_rebar,
                              tools._clamp_scale(p.global_scale / 100),
                              tools._clamp_scale(preview_quality.percent(p) / 100))
        layer.break_rebar.generated_geometry = p.boolean_generated_enable
        rebar.fill_strokes(context, source, layer.break_rebar, factor, props=p)
        for spec in tools.fill_breakers(source, p, core, factor):
            spec.boolean_owner = serial
            breakers.append(spec)
        packed.append(layer)
    params.breakers = breakers
    params.break_rebar.enable = False
    params.boolean_layers = packed


def face_materials(mesh, out, fills, default):
    from . import break_surface, effect_groups
    ids = np.asarray(out.get('effect_ids', ()), dtype=np.int32)
    if len(ids) != len(mesh.polygons) or not np.any(ids >= EFFECT_OFFSET):
        return
    indices = np.empty(len(ids), dtype=np.int32)
    mesh.polygons.foreach_get('material_index', indices)
    owners = np.zeros(len(ids), dtype=np.int32)
    for serial, p in fills:
        cuts = ids == EFFECT_OFFSET + serial
        rim = ids == RIM_OFFSET + serial
        owners[cuts | rim] = serial
        material = p.boolean_break_material or default
        if material is not None and cuts.any():
            slot = mesh.materials.find(material.name)
            if slot < 0:
                mesh.materials.append(material)
                slot = len(mesh.materials) - 1
            indices[cuts] = slot
    mesh.polygons.foreach_set('material_index', indices)
    attribute = mesh.attributes.get(FACE_OWNER) or mesh.attributes.new(FACE_OWNER, 'INT', 'FACE')
    attribute.data.foreach_set('value', owners)
    rim = ids >= RIM_OFFSET
    if rim.any():
        attribute = mesh.attributes.get(break_surface.RIM_FACES) or mesh.attributes.new(
            break_surface.RIM_FACES, 'BOOLEAN', 'FACE')
        attribute.data.foreach_set('value', rim)
    out['effect_ids'] = np.where(rim, 0, np.where(owners > 0, effect_groups.stamp_of('boolean'), ids))


def cleanup_outputs(source, fills):
    from . import cables, pipes, rebar, wall_stack
    uid = source.get('project_damage_uid')
    if not uid:
        return
    active = {OUTPUT_OFFSET + serial for serial, _ in fills}
    for obj in list(bpy.data.objects):
        serial = obj.get(wall_stack.INSTANCE, 0)
        if serial <= OUTPUT_OFFSET or serial in active:
            continue
        if not any(obj.get(stamp) == uid for stamp in (rebar.REBAR_STAMP, rebar.DRAWN_REBAR_STAMP,
                                                       pipes.PIPES_STAMP, cables.CABLES_STAMP)):
            continue
        mesh = obj.data
        bpy.data.objects.remove(obj, do_unlink=True)
        if not mesh.users:
            bpy.data.meshes.remove(mesh)
