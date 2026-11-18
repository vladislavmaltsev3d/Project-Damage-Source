# меняем материалы готовой геометрии без повторного расчёта эффекта
# SPDX-License-Identifier: GPL-3.0-or-later
"""Apply a shader to the material regions on existing generated meshes."""
from contextlib import contextmanager
from hashlib import blake2b
from time import monotonic
import numpy as np


STAMP = 'project_damage_effect_id'
REGION_EFFECTS = {
    'surface_secondary_material': 31,
    'metaltear_material': 34,
    'erosion_material': 32,
    'pile_material': 28,
    'deposit_material': 29,
}
OUTPUTS = {
    'buildup_material': ('beads', 0),
    'pile_material': ('piles', 0),
    'deposit_material': ('deposits', 0),
    'coating_shell_material': ('coating', 0),
    'brick_front_material': ('bricks', 0),
    'brick_seam_material': ('bricks', 1),
    'lath_material': ('lath', 0),
    'pipe_material': ('pipes', 0),
    'cable_material': ('cables', 0),
    'concrete_rebar_material': ('concrete_rebar', 0),
    'surface_rebar_material': ('surface_rebar', 0),
    'rebar_drawn_material': ('drawn_rebar', 0),
    'hang_cable_material': ('rebar', 1),
}
REBAR_FIELDS = {
    '0': 'rebar_material', '1': 'rebar_brick_material', '2': 'rebar_plank_material',
    '3': 'rebar_framing_material', '4': 'rebar_chips_material', '5': 'rebar_plank_material',
}
FRACTURE_OUTPUTS = {
    'debris_material': 'debris', 'rbf_inner_material': 'rbf', 'cellf_inner_material': 'cellf',
}
_pending = {}


def _source_geometry(obj):
    mesh = obj.data
    digest = blake2b(digest_size=16)
    for items, name, width, dtype in (
        (mesh.vertices, 'co', 3, np.float32),
        (mesh.edges, 'vertices', 2, np.int32),
        (mesh.loops, 'vertex_index', 1, np.int32),
        (mesh.polygons, 'loop_total', 1, np.int32),
    ):
        values = np.empty(len(items) * width, dtype=dtype)
        items.foreach_get(name, values)
        digest.update(memoryview(values).cast('B'))
    return mesh.session_uid, tuple(tuple(row) for row in obj.matrix_world), digest.digest()


def expect_rna_notification(obj):
    if obj.type != 'MESH' or obj.mode != 'OBJECT':
        return
    # The notification follows the next redraw, which a shader compile can delay by seconds.
    _pending[obj.session_uid] = (obj, _source_geometry(obj), monotonic() + 30.0)
    while len(_pending) > 32:
        del _pending[next(iter(_pending))]


def consume_rna_notifications(depsgraph):
    """Ignore the material pointer's subsequent RNA recalc on an unchanged source."""
    if not _pending:
        return set()
    updated = {update.id.original.session_uid for update in depsgraph.updates
               if update.is_updated_geometry or update.is_updated_transform}
    ignored = set()
    now = monotonic()
    for uid, (obj, state, expiry) in tuple(_pending.items()):
        try:
            if now >= expiry:
                _pending.pop(uid)
            elif uid in updated or obj.data.session_uid in updated:
                _pending.pop(uid)
                if _source_geometry(obj) == state:
                    ignored.update((uid, obj.data.session_uid))
        except ReferenceError:
            _pending.pop(uid, None)
    return ignored


def clear():
    _pending.clear()

@contextmanager
def shader_edit(context):
    from . import live
    previous = live._APPLYING
    live._APPLYING = True
    try:
        yield
        # Adding a material relation can report geometry updates across the
        # view layer. Evaluate those notifications before resuming Live Update.
        if not previous and context.view_layer is not None:
            context.view_layer.update()
    finally:
        live._APPLYING = previous


def face_values(mesh, name):
    attr = mesh.attributes.get(name)
    if attr is None or attr.domain != 'FACE' or attr.data_type not in {'BOOLEAN', 'INT'}:
        return None
    dtype = bool if attr.data_type == 'BOOLEAN' else np.int32
    values = np.empty(len(mesh.polygons), dtype=dtype)
    attr.data.foreach_get('value', values)
    return values


def assign(obj, material, mask=None, *, uv_name=None):
    """Swap a dedicated slot or update just the selected face material indices."""
    from . import material_uv

    if obj is None or obj.type != 'MESH' or not obj.is_editable or obj.mode == 'EDIT':
        return False
    if mask is not None and not mask.any():
        return False
    mesh = obj.data
    material = material_uv.for_object(material, obj, uv_name or material_uv.UV_NAME)
    indices = np.empty(len(mesh.polygons), dtype=np.int32)
    mesh.polygons.foreach_get('material_index', indices)
    selected = indices if mask is None else indices[mask]
    if not len(selected):
        return False
    slots = list(obj.material_slots)
    current = int(selected[0])
    uniform = bool(np.all(selected == current))
    if uniform and current < len(slots):
        if slots[current].material == material:
            return False
        if mask is None or not np.any(indices[~mask] == current):
            if mesh.users > 1 and slots[current].link == 'DATA':
                obj.data = mesh.copy()
            obj.material_slots[current].material = material
            return True

    slot = next((i for i, item in enumerate(slots) if item.material == material), None)
    if slot is not None and np.all(selected == slot):
        return False
    if mesh.users > 1:
        mesh = obj.data = mesh.copy()
    if slot is None:
        if not len(slots) and mask is not None and not mask.all():
            mesh.materials.append(None)
        slot = len(mesh.materials)
        mesh.materials.append(material)
    if mask is None:
        indices.fill(slot)
    else:
        indices[mask] = slot
    mesh.polygons.foreach_set('material_index', indices)
    mesh.update_gpu_tag()
    return True


def assign_slot(obj, index, material):
    if obj is None or not obj.is_editable or obj.mode == 'EDIT' or index >= len(obj.material_slots):
        return
    from . import material_uv
    material = material_uv.for_object(material, obj)
    slot = obj.material_slots[index]
    if slot.material == material:
        return
    if obj.data.users > 1 and slot.link == 'DATA':
        obj.data = obj.data.copy()
    obj.material_slots[index].material = material


def fracture_key(source):
    from . import fracture_stack
    items = source.project_damage_effects
    index = source.project_damage_effect_index
    if 0 <= index < len(items) and items[index].key in fracture_stack.IDS:
        return items[index].key
    if not len(items):
        layers = fracture_stack.layers(source)
        if layers:
            return layers[0][0]
    return None


def body_mask(mesh, source, field):
    from . import fracture_stack, material_controls, mesh_io, result_transfer

    props = source.project_damage
    regions = {prop: name for _, name, prop in result_transfer.MATERIAL_REGIONS}
    if field in regions:
        mask = face_values(mesh, regions[field])
        return None if mask is None else mask.astype(bool)

    ids = face_values(mesh, STAMP)
    if field in REGION_EFFECTS:
        return None if ids is None else ids == REGION_EFFECTS[field]
    if field == 'concrete_rebar_material':
        part = face_values(mesh, mesh_io.CLASS_NAMES[5])
        return None if ids is None or part is None else (ids == 10) & part
    if field == 'clay_material':
        from . import clay
        return (ids == 0) if ids is not None and clay.wanted(props) else None
    cls = next((key for key, prop in material_controls.CLASS_PROPS.items() if prop == field), None)
    if cls is None:
        return None
    key = fracture_key(source) if cls in (1, 2, 3) else None
    if key is not None and ids is not None:
        # Fracture layers use a single material per layer, independent of crack depth.
        return ids == fracture_stack.IDS[key] if cls == 1 else None

    values = face_values(mesh, mesh_io.CLASS_NAMES[cls])
    if values is None:
        return None
    mask = values.astype(bool, copy=False)
    if ids is not None:
        if cls == 4:
            mask |= ids == 2
        elif cls == 0:
            mask &= ids != 2
        if cls in (1, 2, 3):
            mask &= ~np.isin(ids, tuple(fracture_stack.IDS.values()))
        for prop, effect in REGION_EFFECTS.items():
            if prop != 'surface_secondary_material' or getattr(props, prop, None) is not None:
                mask &= ids != effect
        if cls == 5:
            mask &= ids != 10
        mask &= ids != 33  # Peeling has separate coating and substrate materials.

    for prop, name in regions.items():
        precise = face_values(mesh, name)
        if precise is None:
            continue
        if prop.startswith('brick_'):
            if cls == 4 and prop in {'brick_behind_material', 'brick_render_material'} and getattr(props, prop) is None:
                mask |= precise
            else:
                mask &= ~precise
        elif getattr(props, prop) is not None:
            mask &= ~precise
    return mask


def output_object(source, key, field):
    from . import boolean_stack, runtime_lookup, wall_stack

    stamp = f'project_damage_{key}_of'
    if key not in {'rebar', 'drawn_rebar', 'pipes', 'cables', 'lath', 'concrete_rebar'}:
        return runtime_lookup.output(source, stamp)
    p = source.project_damage
    boolean = boolean_stack.edited(source)
    wall = wall_stack.edited(source)
    serial = 0
    if field == 'hang_cable_material':
        if not p.hang_enable:
            return None
    elif boolean is not None and key in {'rebar', 'drawn_rebar', 'pipes', 'cables'}:
        enabled = {'rebar': p.rebar_enable, 'drawn_rebar': p.rebar_enable,
                   'pipes': p.pipe_break_enable, 'cables': p.cable_break_enable}
        if not enabled[key]:
            return None
        serial = boolean_stack.OUTPUT_OFFSET + boolean[boolean_stack.INSTANCE]
    elif wall is not None and key not in {'rebar', 'drawn_rebar'}:
        matches = {'pipes': wall.key == 'pipes', 'cables': wall.key == 'cables',
                   'lath': wall.key in wall_stack.WOOD, 'concrete_rebar': wall.key in {'concrete', 'concrete_v2'}}
        if not matches[key]:
            return None
        serial = wall[wall_stack.INSTANCE]
    return next(wall_stack.output_objects(source, stamp, serial), None)


def apply_field(source, field, material):
    from . import boolean_break, boolean_stack, build, debris_output

    result = build.result_of(source)
    bodies = [result] if result is not None else []
    row = boolean_stack.edited(source)
    if boolean_break.wanted(source):
        bodies.extend(boolean_break.fragments_of(source))
    for obj in bodies:
        mask = body_mask(obj.data, source, field)
        if mask is not None and field == 'boolean_break_material' and row is not None:
            owners = face_values(obj.data, boolean_stack.FACE_OWNER)
            if owners is None:
                continue
            mask &= owners == row[boolean_stack.INSTANCE]
        if mask is not None:
            assign(obj, material, mask)

    output = OUTPUTS.get(field)
    if field in REBAR_FIELDS.values():
        kind = source.project_damage.rebar_fill if source.project_damage.rebar_enable else '0'
        if field == REBAR_FIELDS.get(kind):
            output = ('rebar', 0)
    elif field in {'rebar_mortar_material', 'rebar_nail_material'}:
        expected = '1' if field == 'rebar_mortar_material' else '3'
        if source.project_damage.rebar_fill == expected:
            output = ('rebar', 1)
    if output is not None:
        key, index = output
        obj = output_object(source, key, field)
        if obj is not None and field == 'rebar_brick_material' and source.project_damage.rebar_brick_3d:
            for slot in range(len(obj.material_slots)):
                assign_slot(obj, slot, material)
        else:
            assign_slot(obj, index, material)
    if field in FRACTURE_OUTPUTS:
        from . import fracture_uv
        for obj in fracture_uv.with_details(debris_output.objects_of(source, FRACTURE_OUTPUTS[field])):
            base = face_values(obj.data, 'project_damage_debris_base_poly')
            if base is not None:
                assign(obj, material, base < 0, uv_name=fracture_uv.layer_name(obj.data))


def callback(field, original):
    def update(props, context):
        from . import live, material_controls
        if live._APPLYING or live._HISTORY_RESTORING:
            return
        if field in material_controls.TUBE_MATERIAL_FIELDS:
            for key in material_controls.automatic_material_keys(field):
                if key in props:
                    del props[key]
        material = getattr(props, field)
        if material is None:
            # Existing generators may need to recover a procedural or inherited default.
            if original is not None:
                original(props, context)
            return
        from . import wall_stack
        with shader_edit(context):
            if not wall_stack.update_material(props.id_data, field, material):
                apply_field(props.id_data, field, material)
        expect_rna_notification(props.id_data)
    return update


def register_callbacks(annotations):
    from . import material_controls, result_transfer

    fields = (set(material_controls.CLASS_PROPS.values()) | REGION_EFFECTS.keys()
              | OUTPUTS.keys() | set(REBAR_FIELDS.values()) | FRACTURE_OUTPUTS.keys()
              | {prop for _, _, prop in result_transfer.MATERIAL_REGIONS}
              | {'clay_material', 'rebar_mortar_material', 'rebar_nail_material'})
    for name in fields:
        if name in annotations:
            options = annotations[name].keywords
            options['update'] = callback(name, options.get('update'))
