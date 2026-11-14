# чистим лишние слоты и сохраняем данные при завершении работы с мешем
import bpy
import numpy as np


def _uses_material_indices(group):
    pending = [group] if group else []
    visited = set()
    while pending:
        group = pending.pop()
        if group in visited:
            continue
        visited.add(group)
        for node in group.nodes:
            kind = node.bl_idname
            if kind in {'GeometryNodeInputMaterialIndex', 'GeometryNodeSetMaterialIndex'}:
                return True
            if kind in {'GeometryNodeInputNamedAttribute', 'GeometryNodeStoreNamedAttribute',
                        'GeometryNodeRemoveAttribute'}:
                name = node.inputs.get('Name')
                if name and (name.is_linked or name.default_value == 'material_index'):
                    return True
            if node.type == 'GROUP' and node.node_tree:
                pending.append(node.node_tree)
    return False


def _modifier_slots(obj, used, count):
    required = set(used)
    bevel_slots = []
    for mod in obj.modifiers:
        if mod.type == 'NODES' and _uses_material_indices(mod.node_group):
            return set(range(count)), []
        if mod.type == 'BEVEL' and mod.material >= 0:
            slot = min(mod.material, count - 1)
            used.add(slot)
            required.add(slot)
            bevel_slots.append((mod, slot))
        offsets = ()
        if mod.type == 'SOLIDIFY':
            offsets = (mod.material_offset, mod.material_offset_rim)
        elif mod.type == 'WIREFRAME':
            offsets = (mod.material_offset,)
        added = set()
        for offset in set(offsets) - {0}:
            for slot in used:
                other = min(max(slot + offset, 0), count - 1)
                # Keep the indices between the source and the offset target.
                required.update(range(min(slot, other), max(slot, other) + 1))
                added.add(other)
        used.update(added)
    return required, bevel_slots


def remove_unused_slots(obj):
    if obj.type != 'MESH' or obj.mode != 'OBJECT' or not obj.is_editable:
        return
    mesh = obj.data
    count = len(mesh.materials)
    if not count:
        return
    face_indices = np.empty(len(mesh.polygons), np.int32)
    mesh.polygons.foreach_get('material_index', face_indices)
    used = set(np.unique(face_indices).tolist())
    if any(index < 0 or index >= count for index in used):
        return
    if not used and obj.modifiers:
        used.add(0)
    required, bevel_slots = _modifier_slots(obj, used, count)
    unused = sorted(set(range(count)) - required, reverse=True)
    if not unused:
        return
    if mesh.users > 1 or mesh.library:
        obj.data = mesh.copy()
    active = obj.active_material_index
    new_indices = {slot: index for index, slot in enumerate(sorted(required))}
    removed = {material for index in unused
               for material in (mesh.materials[index], obj.material_slots[index].material)
               if material is not None}
    # Blender's object operator remaps both face indices and object-linked slots.
    with bpy.context.temp_override(object=obj, active_object=obj):
        for index in unused:
            obj.active_material_index = index
            bpy.ops.object.material_slot_remove()
    obj.active_material_index = new_indices.get(active, min(active, max(0, len(required) - 1)))
    for mod, slot in bevel_slots:
        mod.material = new_indices[slot]
    for material in removed:
        if (material.get('project_damage_shader_uv') == 'ProjectDamage_Zones'
                and material.get('project_damage_shader_owner') == obj
                and material.users == 0 and not material.use_fake_user and material.library is None):
            bpy.data.materials.remove(material)

def preserve_surface_data(before,after):
    from mathutils import Vector
    from mathutils.bvhtree import BVHTree
    from mathutils.geometry import barycentric_transform, closest_point_on_tri
    if not before.polygons or not after.polygons:return
    before.calc_loop_triangles()
    triangles=list(before.loop_triangles)
    tree=BVHTree.FromPolygons([v.co.copy() for v in before.vertices],[tuple(t.vertices) for t in triangles],all_triangles=True)
    extent=max((v.co.length for v in before.vertices),default=1.)
    tolerance=max(1e-6,extent*1e-6)
    slots={}
    for i,mat in enumerate(before.materials):
        if mat is None:continue
        existing=next((j for j,m in enumerate(after.materials) if m==mat),None)
        if existing is None:
            existing=len(after.materials);after.materials.append(mat)
        slots[i]=existing
    layers=[(uv,after.uv_layers.get(uv.name) or after.uv_layers.new(name=uv.name)) for uv in before.uv_layers]
    def point_key(co):return tuple(round(float(c)/tolerance) for c in co)
    exact={}
    by_polygon={}
    for tri in triangles:by_polygon.setdefault(tri.polygon_index,[]).append(tri)
    for face in before.polygons:
        key=tuple(sorted(point_key(before.vertices[v].co) for v in face.vertices))
        exact.setdefault(key,[]).append(face)
    for face in after.polygons:
        key=tuple(sorted(point_key(after.vertices[v].co) for v in face.vertices))
        original=next((f for f in exact.get(key,[]) if f.normal.dot(face.normal)>.99),None)
        if original is not None:
            face.material_index=slots.get(original.material_index,0)
            loops={point_key(before.vertices[before.loops[l].vertex_index].co):l for l in original.loop_indices}
            for old,new in layers:
                for loop in face.loop_indices:
                    new.data[loop].uv=old.data[loops[point_key(after.vertices[after.loops[loop].vertex_index].co)]].uv
            continue
        center=sum((after.vertices[v].co for v in face.vertices),Vector())/len(face.vertices)
        hit,normal,index,distance=tree.find_nearest(center)
        if hit is None or distance>tolerance or abs(normal.dot(face.normal))<.9:continue
        tri=triangles[index];original=before.polygons[tri.polygon_index]
        face.material_index=slots.get(original.material_index,0)
        for loop in face.loop_indices:
            co=after.vertices[after.loops[loop].vertex_index].co
            candidate=min(by_polygon[original.index],key=lambda t:(closest_point_on_tri(co,*(before.vertices[v].co for v in t.vertices))-co).length_squared)
            a,b,c=(before.vertices[v].co for v in candidate.vertices)
            if (closest_point_on_tri(co,a,b,c)-co).length>tolerance:continue
            for old,new in layers:
                ua,ub,uc=(Vector((*old.data[l].uv,0)) for l in candidate.loops)
                new.data[loop].uv=barycentric_transform(co,a,b,c,ua,ub,uc).xy
    if before.uv_layers.active and after.uv_layers.get(before.uv_layers.active.name):
        after.uv_layers.active=after.uv_layers[before.uv_layers.active.name]
