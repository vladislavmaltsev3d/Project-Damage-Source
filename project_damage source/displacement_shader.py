# подключаем маску закрепления к displacement в материале
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pin displacement along the seams of generated damage surfaces."""
import bpy
import numpy as np
from bpy.app.handlers import persistent
from mathutils import Vector

GROUP_NAME = 'Displacement Pin | Projected Image'
FRAME_LABEL = 'ASSIGNMENT YOUR DISPLACEMENT'
GROUP_MARK = 'project_damage_displacement_pin_group'
LOCAL_GROUP_MARK = 'project_damage_displacement_passthrough'
OUTPUT_MARK = 'project_damage_displacement_output'
ZONES = {'CUT': ('Cut Faces', 'boolean_break_material'), 'RIM': ('Rim Damage', 'edge_damage_material')}
ZERO_SOCKET = 'Zero Value'
MASK_READY = 'project_damage_displacement_ready'
PIN_OWNER = 'project_damage_pin_owner'
PIN_SOURCE = 'project_damage_pin_source'
PIN_MATERIALS = 'project_damage_pin_materials'
EFFECTS = {
    'surface': ('surface', 'Cavities'), 'edge': ('edge', 'Cut Edges'),
    'corner': ('corner', 'Corner Cavities'), 'fracture': ('fracture', 'Crack Faces'),
    'ground': ('ground', 'Broken Areas'), 'fissure': ('fissure', 'Chasm Walls'),
    'concrete': ('concrete', 'Exposed Concrete'), 'concrete_v2': ('concrete', 'Exposed Concrete'),
    'hit': ('hit', 'Crater Faces'),
    'pipes': ('concrete', 'Exposed Concrete'), 'cables': ('concrete', 'Exposed Concrete'),
    'lath': ('lath', 'Exposed Layers'), 'lath_v2': ('lath', 'Exposed Layers'),
    'lath_riven': ('lath', 'Exposed Layers'), 'lath_framing': ('lath', 'Exposed Layers'),
    'brick': ('brick', 'Damaged Faces'), 'brick_v2': ('brick', 'Damaged Faces'),
    'hang': ('hang', 'Hole Walls'), 'erosion': ('erosion', 'Eroded Surface'),
}


def _field(key, name):
    return f'{key}_shader_pin_{name}'


def add_properties(cls, update):
    for key in (*EFFECTS, 'pits'):
        cls.__annotations__[_field(key, 'enable')] = bpy.props.BoolProperty(
            name='Use Shader Displacement', default=False, update=update,
            description='Generate a pin mask on the damaged faces for use in their material shaders')
        cls.__annotations__[_field(key, 'distance')] = bpy.props.FloatProperty(
            name='Displacement Pin Distance (cm)', default=2.0, min=0.0, soft_max=30.0, update=update,
            description='Pin this distance inward from the damage seam, then fade in displacement over the next band. Zero pins only the seam')
        cls.__annotations__[_field(key, 'smooth')] = bpy.props.FloatProperty(
            name='Smoothness Only Inside', default=0.0, min=0.0, max=5.0, update=update,
            description='Smooth damage geometry while holding the pinned band and untouched surface fixed. Zero keeps the relief. Five gives the strongest smoothing')
        if key == 'pits':
            for suffix in ('enable', 'distance', 'smooth'):
                cls.__annotations__[_field(key, suffix)].keywords['options'] = {'HIDDEN'}



def effect_key(key):
    return 'fracture' if key.startswith('fracture') else key


@persistent
def migrate_zero_socket(*_args):
    for tree in getattr(bpy.data, 'node_groups', ()):
        if tree.bl_idname != 'ShaderNodeTree' or not tree.get(GROUP_MARK) or not tree.is_editable:
            continue
        for socket in tree.interface.items_tree:
            if socket.item_type == 'SOCKET' and socket.in_out == 'INPUT' and socket.name == 'Pin Group':
                socket.name = ZERO_SOCKET


def pin_group(passthrough=False):
    from .break_surface import DISPLACEMENT_MASK
    passthrough = True
    migrate_zero_socket()
    for tree in bpy.data.node_groups:
        if (tree.bl_idname == 'ShaderNodeTree' and tree.get(GROUP_MARK)
                and bool(tree.get(LOCAL_GROUP_MARK)) == passthrough):
            return tree
    tree = bpy.data.node_groups.new(GROUP_NAME, 'ShaderNodeTree')
    for label in ('Your Displacement', ZERO_SOCKET):
        socket = tree.interface.new_socket(name=label, in_out='INPUT', socket_type='NodeSocketVector')
        socket.default_value = (0, 0, 0)
    tree.interface.new_socket(name='Result', in_out='OUTPUT', socket_type='NodeSocketVector')
    inputs = tree.nodes.new('NodeGroupInput')
    inputs.location = (-360, -40)
    output = tree.nodes.new('NodeGroupOutput')
    output.location = (340, 100)
    attribute = tree.nodes.new('ShaderNodeAttribute')
    attribute.attribute_name = DISPLACEMENT_MASK
    attribute.location = (-360, 230)
    inverse = tree.nodes.new('ShaderNodeMath')
    inverse.operation = 'SUBTRACT'
    inverse.label = 'Pin weight = 1 − mask'
    inverse.inputs[0].default_value = 1
    inverse.location = (-150, 230)
    mix = tree.nodes.new('ShaderNodeMix')
    mix.data_type = 'VECTOR'
    mix.factor_mode = 'UNIFORM'
    mix.location = (105, 100)
    tree.links.new(attribute.outputs['Fac'], inverse.inputs[1])
    pin = inverse.outputs[0]
    if passthrough:
        ready = tree.nodes.new('ShaderNodeAttribute')
        ready.attribute_name = MASK_READY
        ready.label = 'Project Damage pin data'
        ready.location = (-360, 480)
        factor = tree.nodes.new('ShaderNodeMath')
        factor.operation = 'MULTIPLY'
        factor.location = (45, 370)
        tree.links.new(ready.outputs['Fac'], factor.inputs[0])
        tree.links.new(pin, factor.inputs[1])
        pin = factor.outputs[0]
        tree[LOCAL_GROUP_MARK] = True
    tree.links.new(pin, mix.inputs[0])
    tree.links.new(inputs.outputs['Your Displacement'], mix.inputs[4])
    tree.links.new(inputs.outputs[ZERO_SOCKET], mix.inputs[5])
    tree.links.new(mix.outputs[1], output.inputs['Result'])
    tree[GROUP_MARK] = True
    return tree


def connect_material(material, *, passthrough=False):
    from .blender_compat import enable_nodes
    passthrough = True

    enable_nodes(material)
    tree = material.node_tree
    outputs = [node for node in tree.nodes if node.type == 'OUTPUT_MATERIAL']
    if not outputs:
        outputs = [tree.nodes.new('ShaderNodeOutputMaterial')]
    for output in [node for node in outputs if node.is_active_output] or outputs[:1]:
        socket = output.inputs['Displacement']
        old_link = next(iter(socket.links), None)
        old_node = old_link.from_node if old_link is not None else None
        if old_node and old_node.type == 'GROUP' and old_node.node_tree and old_node.node_tree.get(GROUP_MARK):
            migrate_zero_socket()
            if not old_node.node_tree.get(LOCAL_GROUP_MARK):
                old_node.node_tree = pin_group(True)
            continue
        group = next((node for node in tree.nodes if node.type == 'GROUP' and node.node_tree
                      and node.node_tree.get(GROUP_MARK) and node.get(OUTPUT_MARK) == output.name
                      and bool(node.node_tree.get(LOCAL_GROUP_MARK)) == passthrough
                      and not node.outputs['Result'].is_linked and node != old_node), None)
        if group is None:
            frame = tree.nodes.new('NodeFrame')
            frame.label = FRAME_LABEL
            frame.location = output.location_absolute - Vector((300, 110))
            group = tree.nodes.new('ShaderNodeGroup')
            group.node_tree = pin_group(passthrough)
            group.label = GROUP_NAME
            group.width = 230
            group.parent = frame
            group.location = (0, 0)
            group[OUTPUT_MARK] = output.name
        input_socket = group.inputs['Your Displacement']
        for link in list(input_socket.links):
            tree.links.remove(link)
        if old_link is not None:
            previous_socket = old_link.from_socket
            tree.links.remove(old_link)
            tree.links.new(previous_socket, input_socket)
        tree.links.new(group.outputs['Result'], socket)


def _damage_faces(mesh, ids, key, props):
    from . import effect_groups

    stamp, _ = EFFECTS[key]
    if ids is None:
        return None
    if key == 'surface':
        return (ids == effect_groups.stamp_of('surface')) | (ids == 31)
    if key == 'hang' and props.hang_mode == '1':
        return ids == effect_groups.stamp_of('boolean')
    return ids == effect_groups.stamp_of(stamp)


def finish_effects(source, mesh, factor, points, polygons):
    from . import break_surface, effect_groups

    p = source.project_damage
    keys = {effect_key(key) for key in effect_groups.active_keys(source)}
    enabled = [key for key in EFFECTS if key in keys and getattr(p, _field(key, 'enable'))]
    if not enabled and mesh.attributes.get(break_surface.DISPLACEMENT_MASK) is None:
        return
    if enabled:
        ids = effect_groups._face_values(mesh, effect_groups.ATTRIBUTE, np.int32)
        totals = np.empty(len(mesh.polygons), dtype=np.int32)
        mesh.polygons.foreach_get('loop_total', totals)
        for key in enabled:
            faces = _damage_faces(mesh, ids, key, p)
            if faces is None or not faces.any():
                continue
            smooth = 0.0 if key == 'hang' and p.hang_mode == '1' else getattr(p, _field(key, 'smooth'))
            yield from break_surface._finish_region_steps(
                mesh, faces, totals, source.matrix_world, factor,
                getattr(p, _field(key, 'distance')), smooth, True)
    if mesh.attributes.get(break_surface.DISPLACEMENT_MASK) is not None:
        ready = mesh.attributes.get(MASK_READY)
        if ready is not None and (ready.domain != 'POINT' or ready.data_type != 'FLOAT'):
            mesh.attributes.remove(ready)
            ready = None
        if ready is None:
            ready = mesh.attributes.new(MASK_READY, 'FLOAT', 'POINT')
        ready.data.foreach_set('value', np.ones(len(mesh.vertices), dtype=np.float32))


def _effect_materials(source, key):
    from . import build, effect_groups
    result = build.result_of(source)
    if result is None:
        return ()
    mesh = result.data
    if mesh.attributes.get(MASK_READY) is None:
        return ()
    ids = effect_groups._face_values(mesh, effect_groups.ATTRIBUTE, np.int32)
    faces = _damage_faces(mesh, ids, key, source.project_damage)
    if faces is None or not faces.any():
        return ()
    slots = np.empty(len(mesh.polygons), dtype=np.int32)
    mesh.polygons.foreach_get('material_index', slots)
    return tuple(dict.fromkeys(mesh.materials[index] for index in np.unique(slots[faces])
                               if 0 <= index < len(mesh.materials) and mesh.materials[index] is not None))


def zone_materials(source, zone):
    from . import build, effect_groups, material_uv
    from .break_surface import RIM_FACES

    p = source.project_damage
    configured = (p.all_generators_material_applied if p.one_material_all else None) or getattr(p, ZONES[zone][1])
    result = build.result_of(source)
    actual = []
    if result is not None:
        mesh = result.data
        if zone == 'CUT':
            ids = effect_groups._face_values(mesh, effect_groups.ATTRIBUTE, np.int32)
            faces = ids == effect_groups.stamp_of('boolean') if ids is not None else None
        else:
            faces = effect_groups._face_values(mesh, RIM_FACES, bool)
        if faces is not None and faces.any():
            slots = np.empty(len(mesh.polygons), dtype=np.int32)
            mesh.polygons.foreach_get('material_index', slots)
            actual = [mesh.materials[i] for i in np.unique(slots[faces])
                      if i < len(mesh.materials) and mesh.materials[i] is not None]
    if configured is None:
        return list(dict.fromkeys(actual))
    related = [mat for mat in actual if material_uv.base_material(mat) == material_uv.base_material(configured)]
    return list(dict.fromkeys([configured, *related]))


def _original(material):
    from . import material_uv
    return material.get(PIN_SOURCE) or material_uv.base_material(material)


def _zone_faces(source, mesh, token):
    from . import effect_groups, break_surface
    ids = effect_groups._face_values(mesh, effect_groups.ATTRIBUTE, np.int32)
    if token == 'CUT':
        return ids == effect_groups.stamp_of('boolean') if ids is not None else None
    if token == 'RIM':
        return effect_groups._face_values(mesh, break_surface.RIM_FACES, bool)
    return _damage_faces(mesh, ids, token, source.project_damage)


def restore_materials(source, result):
    from . import material_uv
    if result is None:
        return
    records = source.get(PIN_MATERIALS, {})
    if not records:
        return
    mesh = result.data
    for token, rows in records.items():
        if token not in ZONES and token not in EFFECTS:
            continue
        mask = _zone_faces(source, mesh, token)
        if mask is None or not mask.any():
            continue
        for row in rows.values():
            original, material = row.get('source'), row.get('material')
            if (not isinstance(original, bpy.types.Material) or not isinstance(material, bpy.types.Material)
                    or material.get(PIN_OWNER) != source):
                continue
            slots = {i for i, mat in enumerate(mesh.materials) if mat and _original(mat) == original}
            faces = [f for f in mesh.polygons if mask[f.index] and f.material_index in slots]
            if not faces:
                continue
            material = material_uv.for_object(material, result)
            index = next((i for i, mat in enumerate(mesh.materials) if mat == material), None)
            if index is None:
                index = len(mesh.materials)
                mesh.materials.append(material)
            for face in faces:
                face.material_index = index


def local_materials(source, materials, token):
    from . import build, material_controls, material_uv, live
    records = source.get(PIN_MATERIALS, {})
    records = records.to_dict() if hasattr(records, 'to_dict') else dict(records)
    rows = records.setdefault(token, {})
    result = []
    previous = live._APPLYING
    live._APPLYING = True
    try:
        for material in materials:
            original = _original(material)
            found = next((row.get('material') for row in rows.values() if row.get('source') == original), None)
            if (found is None or found.get(PIN_OWNER) != source or any(
                    ob != source and ownership_source(ob) != source and any(s.material == found for s in ob.material_slots)
                    for ob in bpy.data.objects)):
                found = material.copy()
                found.name = original.name + ' | PD Pin'
                found[PIN_OWNER] = source
                found[PIN_SOURCE] = original
            record_key = next((key for key, row in rows.items() if row.get('source') == original), str(original.session_uid))
            rows[record_key] = {'source': original, 'material': found}
            result.append(found)
            fields = [ZONES[token][1]] if token in ZONES else [name for name, _ in material_controls.material_fields(EFFECTS[token][0])]
            if source.project_damage.one_material_all:
                fields.extend(['all_generators_material', 'all_generators_material_applied'])
            for field in fields:
                current = getattr(source.project_damage, field, None)
                if isinstance(current, bpy.types.Material) and _original(current) == original:
                    setattr(source.project_damage, field, found)
        source[PIN_MATERIALS] = records
        restore_materials(source, build.result_of(source))
    finally:
        live._APPLYING = previous
    return list(dict.fromkeys(result))


def ownership_source(obj):
    from . import ownership
    return ownership.source_of(obj)


# действие «Apply Shader Displacement»
class PROJECT_DAMAGE_OT_shader_displacement_apply(bpy.types.Operator):
    bl_idname = 'project_damage.shader_displacement_apply'
    bl_label = 'Apply Shader Displacement'
    bl_description = 'Insert the pin group into the assigned material and keep its existing displacement input'
    bl_options = {'REGISTER', 'UNDO'}

    zone: bpy.props.EnumProperty(items=[(key, label, '') for key, (label, _) in ZONES.items()])
    effect: bpy.props.StringProperty(options={'SKIP_SAVE'})

    @classmethod
    def poll(cls, context):
        from . import ownership
        src = ownership.context_source(context)
        return src is not None and src.type == 'MESH'

    def execute(self, context):
        from . import ownership
        source = ownership.context_source(context)
        key = self.effect
        if key:
            if key not in EFFECTS or not getattr(source.project_damage, _field(key, 'enable')):
                return {'CANCELLED'}
            materials = _effect_materials(source, key)
            label = EFFECTS[key][1]
        else:
            if not (source.project_damage.ui_boolean_enable and source.project_damage.break_shader_displacement):
                return {'CANCELLED'}
            materials = zone_materials(source, self.zone)
            label = ZONES[self.zone][0]
        if not materials:
            self.report({'WARNING'}, 'Update the geometry and assign a material to ' + label + ' first')
            return {'CANCELLED'}
        if any(not mat.is_editable for mat in materials):
            self.report({'WARNING'}, 'Make the assigned material local before applying the displacement group')
            return {'CANCELLED'}
        for material in local_materials(source, materials, key or self.zone):
            connect_material(material, passthrough=True)
        self.report({'INFO'}, 'Displacement pin connected for ' + label)
        return {'FINISHED'}


def draw_effect(layout, p, key):
    from . import effect_layout
    key = effect_key(key)
    if key not in EFFECTS:
        return
    box = effect_layout.section(layout, 'Shader Displacement')
    box.prop(p, _field(key, 'enable'), toggle=True, icon='MOD_DISPLACE')
    if not getattr(p, _field(key, 'enable')):
        return
    box.prop(p, _field(key, 'distance'))
    row = box.row(align=True)
    smooth = 'break_smooth_inside' if key == 'hang' and p.hang_mode == '1' else _field(key, 'smooth')
    row.prop(p, smooth, slider=True)
    row.operator('project_damage.setting_info', text='', icon='QUESTION').text = (
        'Smoothness Only Inside softens the damaged faces while keeping the seam fixed. '
        'This helps prevent nearby faces from overlapping when their shader adds displacement.')
    box.operator(PROJECT_DAMAGE_OT_shader_displacement_apply.bl_idname,
                 text='Apply to ' + EFFECTS[key][1], icon='MATERIAL').effect = key


# собираем видимые настройки этого раздела
def draw(layout, p):
    from . import effect_layout
    box = effect_layout.section(layout, 'Shader Displacement')
    box.prop(p, 'break_shader_displacement', toggle=True, icon='MOD_DISPLACE')
    if not p.break_shader_displacement:
        return
    box.prop(p, 'break_displacement_pin')
    row = box.row(align=True)
    row.prop(p, 'break_smooth_inside', slider=True)
    row.operator('project_damage.setting_info', text='', icon='QUESTION').text = (
        'Smoothness Only Inside softens the cut mesh before shader displacement. '
        'Sharp ridges can move neighboring faces into each other. Smoothing reduces that risk '
        'while the wall and the pinned band stay fixed. Keep displacement strength low enough '
        'that faces on opposite sides of a narrow gap stay apart.')
    actions = box.column(align=True)
    for zone, (label, _) in ZONES.items():
        actions.operator(PROJECT_DAMAGE_OT_shader_displacement_apply.bl_idname,
                         text='Apply to ' + label, icon='MATERIAL').zone = zone


# подключаем свойства и действия этого модуля к Blender
def register():
    bpy.utils.register_class(PROJECT_DAMAGE_OT_shader_displacement_apply)
    bpy.app.handlers.load_post.append(migrate_zero_socket)
    bpy.app.timers.register(migrate_zero_socket, first_interval=0.1)


# убираем подключения модуля при выключении аддона
def unregister():
    if bpy.app.timers.is_registered(migrate_zero_socket):
        bpy.app.timers.unregister(migrate_zero_socket)
    if migrate_zero_socket in bpy.app.handlers.load_post:
        bpy.app.handlers.load_post.remove(migrate_zero_socket)
    bpy.utils.unregister_class(PROJECT_DAMAGE_OT_shader_displacement_apply)
