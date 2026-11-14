# отдельные слои получают понятные материалы, общий бетон остаётся общим
import bpy
import numpy as np
from bpy.app.handlers import persistent
from .blender_compat import enable_nodes

ROLES = {
    'wallpaper_material': 'Printed Paper',
    'wallpaper_back_material': 'Paper Backing',
    'wall_material': 'Exposed Wall',
    'bevel_material': 'Bevel Surface',
    'wear_material': 'Edge Wear',
    'scratch_groove_material': 'Scratch Grooves',
    'scratch_scuff_material': 'Scuffed Paint',
}
TAG = 'project_damage_material_role'
_plain_inputs = None
_enabled = False


def _value(value):
    return tuple(value) if hasattr(value, '__len__') and not isinstance(value, str) else value


def _shader_defaults():
    global _plain_inputs
    if _plain_inputs is None:
        tree = bpy.data.node_groups.new('_PD_Default_Shader_Check', 'ShaderNodeTree')
        try:
            node = tree.nodes.new('ShaderNodeBsdfPrincipled')
            _plain_inputs = {socket.identifier: _value(socket.default_value) for socket in node.inputs}
        finally:
            bpy.data.node_groups.remove(tree)
    return _plain_inputs


def legacy_placeholder(material):
    # исправляем только нетронутую старую заглушку, пользовательский шейдер сохраняем
    from . import material_uv, mesh_io
    material = material_uv.base_material(material)
    if material is None or material.name not in mesh_io.DAMAGE_MATERIAL_NAMES or material.animation_data:
        return False
    tree = material.node_tree
    if tree is None or tree.animation_data or len(tree.nodes) != 2 or len(tree.links) != 1:
        return False
    shader = next((node for node in tree.nodes if node.type == 'BSDF_PRINCIPLED'), None)
    link = tree.links[0]
    if shader is None or link.from_node != shader or link.to_node.type != 'OUTPUT_MATERIAL' or link.to_socket.name != 'Surface':
        return False
    color = (.38, .33, .27, 1.) if material.name == 'ProjectDamage_Chips' else (.18, .15, .12, 1.)
    if not np.allclose(material.diffuse_color, color, rtol=0, atol=1e-6):
        return False
    expected = dict(_shader_defaults())
    expected['Base Color'] = color
    expected['Roughness'] = .8
    for socket in shader.inputs:
        value = _value(socket.default_value)
        other = expected.get(socket.identifier)
        if isinstance(value, (float, int, tuple)):
            if other is None or not np.allclose(value, other, rtol=0, atol=1e-6):return False
        elif value != other:return False
    return True


def default(field, template=None):
    from . import material_uv
    name = ROLES[field] + ' | Project Damage'
    material = next((mat for mat in bpy.data.materials if mat.get(TAG) == field and not mat.get(material_uv.TAG)), None)
    material = material or bpy.data.materials.get(name)
    if material is None:
        if template is not None and legacy_placeholder(template):
            from .material_uv import base_material
            material = base_material(template).copy()
            material.name = name
        else:
            material = bpy.data.materials.new(name)
            enable_nodes(material)
            color = (.38, .33, .27, 1.)
            material.diffuse_color = color
            shader = material.node_tree.nodes.get('Principled BSDF')
            shader.inputs['Base Color'].default_value = color
            shader.inputs['Roughness'].default_value = .8
        material[TAG] = field
    return material


def resolve(props, field, fallback=None):
    from . import live, material_uv
    material = getattr(props, field)
    if material is not None and not legacy_placeholder(material):return material
    candidate = material if material is not None else fallback
    if candidate is not None and not legacy_placeholder(candidate):
        material = material_uv.base_material(candidate)
    else:
        material = default(field, candidate)
    previous = live._APPLYING
    live._APPLYING = True
    try:setattr(props, field, material)
    finally:live._APPLYING = previous
    return material


def region_material(mesh, indices):
    if not len(indices):return None
    slots = np.empty(len(mesh.polygons), dtype=np.int32)
    mesh.polygons.foreach_get('material_index', slots)
    slot = int(np.bincount(slots[indices]).argmax())
    return mesh.materials[slot] if slot < len(mesh.materials) else None


def repair_object(source, result):
    from . import material_update, result_transfer, mesh_finalization
    if result.mode != 'OBJECT' or not result.is_editable or not source.is_editable:return 0
    changed = 0
    with material_update.shader_edit(bpy.context):
        for _, attribute, field in result_transfer.MATERIAL_REGIONS:
            if field not in ROLES:continue
            mask = material_update.face_values(result.data, attribute)
            if mask is None or not mask.any():continue
            indices = np.flatnonzero(mask)
            material = resolve(source.project_damage, field, region_material(result.data, indices))
            changed += bool(material_update.assign(result, material, mask.astype(bool)))
        if changed:mesh_finalization.remove_unused_slots(result)
    return changed


def repair_scene():
    if not _enabled:return None
    from . import live, ownership
    if live._APPLYING or live._HISTORY_RESTORING:return .5
    for result in tuple(bpy.data.objects):
        if result.type != 'MESH' or not result.get('project_damage_result_of'):continue
        source = ownership.source_of(result)
        if source is not None and source != result:
            repair_object(source, result)
    return None


@persistent
def after_load(*_):
    if _enabled and not bpy.app.timers.is_registered(repair_scene):
        bpy.app.timers.register(repair_scene, first_interval=.5)


def register():
    global _enabled
    _enabled = True
    if after_load not in bpy.app.handlers.load_post:bpy.app.handlers.load_post.append(after_load)
    after_load()


def unregister():
    global _enabled
    _enabled = False
    if after_load in bpy.app.handlers.load_post:bpy.app.handlers.load_post.remove(after_load)
    if bpy.app.timers.is_registered(repair_scene):bpy.app.timers.unregister(repair_scene)
