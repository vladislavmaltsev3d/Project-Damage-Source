# сохраняем исходный меш и, по выбору пользователя, промежуточные состояния
import json
import bpy

INITIAL = 'project_damage_initial_mesh'
GROUPS = 'project_damage_initial_groups'


def save_all(context):
    addon = context.preferences.addons.get(__package__)
    return bool(addon and getattr(addon.preferences, 'save_all_mesh_states', False))


def remember_initial(source, mesh=None):
    if not isinstance(source.get(INITIAL), bpy.types.Mesh):
        source[INITIAL] = mesh if mesh is not None else source.data
        source[GROUPS] = json.dumps([group.name for group in source.vertex_groups])


def restore_initial(source):
    initial = source.get(INITIAL)
    if not isinstance(initial, bpy.types.Mesh) or source.data == initial:
        return
    from .apply_selected import assign_mesh
    old = source.data
    assign_mesh(source, initial, json.loads(source.get(GROUPS, '[]')))
    discard_unused((old,))


def discard_unused(meshes):
    for mesh in set(meshes):
        try:
            if isinstance(mesh, bpy.types.Mesh) and mesh.users == 0:
                bpy.data.meshes.remove(mesh)
        except ReferenceError:
            pass
