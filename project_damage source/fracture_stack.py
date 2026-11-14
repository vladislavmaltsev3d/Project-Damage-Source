# храним отдельные настройки нескольких эффектов Fracture
"""Independent fracture settings; the visible RNA properties are an edit buffer."""
from types import SimpleNamespace
import bpy
import numpy as np
from .fracture_assets import FRACTURE_KEYS, TYPE_BY_ASSET, PROXIMITY_ASSETS, active_key

VERSION = '_pd_fracture_stack'
EDIT = '_pd_fracture_edit'
VALUES = 'fracture_values'
ON = 'fracture_enabled'
IDS = {'fracture': 4, 'fracture_concrete': 100, 'fracture_glass': 101,
       'fracture_proximity_default': 102, 'fracture_proximity_glass': 103,
       'fracture_proximity_radial': 104, 'fracture_radial': 105,
       'fracture_through': 106, 'fracture_volume': 107, 'fracture_wood': 108}


def fields(p):
    return [r for r in p.bl_rna.properties if r.identifier.startswith(('fract_', 'fracture_'))
            and r.identifier not in {'fract_enable', 'fract_through_enable'}]


def capture(p):
    values = {}
    for r in fields(p):
        v = getattr(p, r.identifier)
        if v is None:
            continue
        if getattr(r, 'is_array', False) or isinstance(v, set):
            v = list(v)
        values[r.identifier] = v
    values['guides'] = {str(i): row.obj for i, row in enumerate(p.guides) if row.obj}
    values['_paint_initialized'] = bool(p.get('fract_paint_initialized', False))
    return values


def row_for(source, key):
    return next((r for r in source.project_damage_effects if r.key == key), None)


def ensure(source):
    if source.get(VERSION):
        return
    p = source.project_damage
    key = active_key(p)
    on = bool(p.fract_enable or p.fract_through_enable)
    row = row_for(source, key)
    if on and row is None:
        row = source.project_damage_effects.add(); row.key = key
    for r in source.project_damage_effects:
        if r.key in FRACTURE_KEYS:
            r[ON] = bool(on and r.key == key)
    if row is not None:
        row[VALUES] = capture(p)
    source[EDIT] = key
    source[VERSION] = 1


def save(source):
    row = row_for(source, source.get(EDIT, ''))
    if row is not None:
        row[VALUES] = capture(source.project_damage)


def restore(p, values, key):
    for r in fields(p):
        name = r.identifier
        if r.type == 'COLLECTION':
            continue
        if name in values:
            value = values[name]
            if r.type == 'ENUM' and r.is_enum_flag:
                value = set(value)
        else:
            p.property_unset(name)
            continue
        setattr(p, name, value)
    if values.get('_paint_initialized', False):
        p['fract_paint_initialized'] = True
    elif 'fract_paint_initialized' in p:
        del p['fract_paint_initialized']
    p.guides.clear()
    for obj in values.get('guides', {}).values():
        if obj is not None:
            p.guides.add().obj = obj
    p.fract_asset = key
    p.fract_type = TYPE_BY_ASSET.get(key, 'FRACTURE')
    if not values and key in PROXIMITY_ASSETS:
        p.fract_mode = 'PROCEDURAL'; p.fract_prox_mode = PROXIMITY_ASSETS[key]
        p.fract_l1_amount = p.fract_l2_amount = p.fract_l3_amount = 0
        p.fract_prox_strength = 100.


def flags(source):
    p = source.project_damage
    rows = [r for r in source.project_damage_effects if r.key in FRACTURE_KEYS and r.get(ON, False)]
    p.fract_enable = any(r.key not in {'fracture_through', 'fracture_volume'} for r in rows)
    p.fract_through_enable = any(r.key in {'fracture_through', 'fracture_volume'} for r in rows)
    key=source.get(EDIT)
    if key in FRACTURE_KEYS:
        p.fract_asset=key
        if key in TYPE_BY_ASSET or key=='fracture':p.fract_type=TYPE_BY_ASSET.get(key,'FRACTURE')


def select(source, key):
    if key not in FRACTURE_KEYS:
        return
    ensure(source)
    if source.get(EDIT) == key and (row := row_for(source, key)) is not None and VALUES in row:
        return
    from . import live
    previous = live._APPLYING; live._APPLYING = True
    try:
        save(source)
        row = row_for(source, key)
        if row is None:
            row = source.project_damage_effects.add(); row.key = key; row[ON] = False
        restore(source.project_damage, row.get(VALUES, {}), key)
        source[EDIT] = key
        save(source)
        flags(source)
    finally:
        live._APPLYING = previous


def index_changed(source, context):
    from . import live
    if live._HISTORY_RESTORING:
        return
    if not source.get(VERSION):
        return
    i = source.project_damage_effect_index
    if 0 <= i < len(source.project_damage_effects):
        select(source, source.project_damage_effects[i].key)


def enabled(source, key):
    if not source.get(VERSION):
        p = source.project_damage
        return bool((p.fract_enable or p.fract_through_enable) and active_key(p) == key)
    row = row_for(source, key)
    return row is not None and bool(row.get(ON, False))


def set_enabled(source, key, on):
    ensure(source)
    if on:
        select(source, key)
        source.project_damage_effect_index=next(i for i,r in enumerate(source.project_damage_effects) if r.key==key)
    row = row_for(source, key)
    if row is not None:
        row[ON] = bool(on)
    flags(source)
    if on:
        source.project_damage.all_off = False


class Props:
    def __init__(self, base, values, key, on=True):
        self._base = base; self._values = values; self.fract_asset = key
        self.fract_enable = on and key not in {'fracture_through', 'fracture_volume'}
        self.fract_through_enable = on and not self.fract_enable
        self.guides = [SimpleNamespace(obj=o) for o in values.get('guides', {}).values() if o]

    def __getattr__(self, name):
        if name in self._values:
            return self._values[name]
        if name.startswith(('fract_', 'fracture_')):
            r = self._base.bl_rna.properties.get(name)
            if r is not None and r.type == 'POINTER':
                return None
        return getattr(self._base, name)

    def get(self, name, default=None):
        if name == 'fract_paint_initialized':return self._values.get('_paint_initialized', False)
        return self._values.get(name, self._base.get(name, default))


def layers(source, include_disabled=False):
    p = source.project_damage
    if not source.get(VERSION):
        return [(active_key(p), p)] if p.fract_enable or p.fract_through_enable else []
    result = []
    for row in source.project_damage_effects:
        if row.key not in FRACTURE_KEYS or not (include_disabled or row.get(ON, False)):
            continue
        values = capture(p) if row.key == source.get(EDIT) else dict(row.get(VALUES, {}))
        result.append((row.key, Props(p, values, row.key)))
    return result


def state(source):
    return [(key, capture(p)) for key, p in layers(source)]


def references(source):
    for key, p in layers(source):
        for name in ('fract_l1_drawing', 'fract_l2_drawing', 'fract_l3_drawing', 'fract_helper_drawing'):
            if (obj := getattr(p, name, None)) is not None:
                yield obj
        for row in p.guides:
            if row.obj:
                yield row.obj
        for name in ('fract_prox_collection', 'fract_shard_collection'):
            coll = getattr(p, name, None)
            if coll:
                yield from coll.all_objects


def pack(context, source, core, parent, points, polys, factor, reports):
    from .params import fill_params, fill_fract_proximity
    from . import paint, mesh_io, background_wire
    entries = layers(source)
    if not hasattr(parent, 'fracture_layers'):
        if len(entries) > 1:
            raise RuntimeError('Restart Blender to load the core with multiple Fracture support')
        return
    native = []
    if not source.project_damage.all_off:
        for key, p in entries:
            layer = fill_params(p, core, obj=source, geometry=(points, polys, factor))
            layer.fracture_effect_id = IDS[key]
            paths = []; levels = []
            if p.fract_mode == 'PAINT':
                paths = paint.read_paths(context, source, p.fract_l1_drawing)
                levels = [0] * len(paths)
            elif p.fract_mode == 'PROCEDURAL':
                paths = mesh_io.read_guide_splines(context.evaluated_depsgraph_get(), source, p.guides)
                levels = [0] * len(paths)
            layer.fracture_guide_counts = [len(path) for path in paths]
            layer.fracture_guide_points = (np.concatenate(paths).reshape(-1) * factor).tolist() if paths else []
            layer.fracture_guide_levels = levels
            layer.fract_proximity = [] if layer.fract_paint_only else fill_fract_proximity(source, p, core, factor)
            if (p.fract_mode == 'PROXIMITY' or key in PROXIMITY_ASSETS) and not layer.fract_proximity:
                layer.fract1.amount = layer.fract2.amount = layer.fract3.amount = 0
                layer.fract_type = 0; layer.fract_prox_mode = 0
                reports.append(({'WARNING'}, key.replace('_', ' ').title() + ': assign a Points collection'))
            layer.avoid_fract = parent.avoid_fract
            layer.fract_surface_proxy = parent.fract_surface_proxy
            encoded=background_wire.encode(layer,core)
            encoded['fields']={name:value for name,value in encoded['fields'].items()
                               if name.startswith('fract') or name in {'seed','avoid_fract'}}
            native.append(background_wire.decode(encoded,core))
    if native:
        encoded=background_wire.encode(native[0],core)
        encoded['fields']={name:value for name,value in encoded['fields'].items() if name.startswith('fract')}
        background_wire.decode(encoded,core,parent)
    parent.fracture_layers = native


def apply_materials(source, mesh, out):
    ids = np.asarray(out.get('effect_ids', ()), dtype=np.int32)
    if len(ids) != len(mesh.polygons):
        return
    indices = np.empty(len(mesh.polygons), np.int32)
    mesh.polygons.foreach_get('material_index', indices)
    for key, p in layers(source):
        material = p.fracture_l1_material
        if material is None:
            from .mesh_io import DAMAGE_MATERIAL_NAMES
            material = bpy.data.materials.get(DAMAGE_MATERIAL_NAMES[1])
        if material is None:
            continue
        slot = next((i for i, m in enumerate(mesh.materials) if m == material), None)
        if slot is None:
            slot = len(mesh.materials); mesh.materials.append(material)
        indices[ids == IDS[key]] = slot
    mesh.polygons.foreach_set('material_index', indices)


def reset(source):
    for name in (VERSION, EDIT):
        if name in source:
            del source[name]


def preset_data(source):
    result = []
    for key, p in layers(source, include_disabled=True):
        values = {r.identifier: getattr(p, r.identifier) for r in fields(p)
                  if r.type in {'BOOLEAN', 'INT', 'FLOAT', 'ENUM', 'STRING'}}
        result.append((key, enabled(source, key), values))
    return result


def load_preset(source, data):
    from . import live
    previous=live._APPLYING;live._APPLYING=True
    try:
        ensure(source)
        for row in source.project_damage_effects:
            if row.key in FRACTURE_KEYS:row[ON]=False
        for key, on, values in data:
            if key not in FRACTURE_KEYS:continue
            select(source,key)
            restore(source.project_damage,values,key)
            save(source)
            row_for(source,key)[ON]=on
        flags(source)
    finally:
        live._APPLYING=previous
