# собираем настройки и материалы дополнительных эффектов
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host helpers for Rust, Fabric Tears and Concrete Damage."""
from .blender_compat import enable_nodes
import bpy
from . import ownership
import numpy as np

def concrete_draw_paths(context, source, points, polys, factor):
    from . import paint
    props = source.project_damage
    drawing = props.concrete_drawing
    paths = [np.ascontiguousarray(path * factor, dtype=np.float64)
             for path in paint.read_paths(context, source, drawing)]
    if not paths or not props.concrete_v2_enable or drawing.type != 'GREASEPENCIL':
        return paths
    surface = drawing.get('project_damage_paint_surface')
    if not isinstance(surface, bpy.types.Object) or surface == source or ownership.source_of(surface) != source:
        return paths
    if not len(points) or not len(polys):
        return paths

    # Grease Pencil follows the visible result, while the generator cuts the
    # original. An interior sample on an earlier crater may miss Paint Snap.
    from mathutils.bvhtree import BVHTree
    from mathutils import Vector
    from .params import _clamp_scale
    scale = _clamp_scale(props.global_scale / 100.0)
    snap = props.concrete_paint_snap * scale
    extent = float(np.linalg.norm(np.ptp(points, axis=0)))
    limit = min(extent * .25, max(snap * 3, props.concrete_depth_max * scale * 2))
    if limit <= snap:
        return paths
    faces = [tuple(map(int, row[:3] if row[2] == row[3] else row)) for row in polys]
    tree = BVHTree.FromPolygons(points, faces)
    for path in paths:
        for index, sample in enumerate(path):
            position = Vector(sample)
            hit, normal, _, distance = tree.find_nearest(position, limit)
            if hit is not None and distance > snap and (position - hit).dot(normal) < 0:
                path[index] = hit
    return paths


def snapshot_color_mask(snap, name, channel):
    name = name or snap.color_active
    entry = snap.transfer_attrs.get(name)
    if entry is None or entry[1] not in {'FLOAT_COLOR', 'BYTE_COLOR'}:
        return None
    domain, kind, field, width, colors = entry
    colors = np.asarray(colors)
    if channel == 'LUMA':
        weights = colors[:, :3] @ np.array([.2126, .7152, .0722])
    else:
        weights = colors[:, {'R': 0, 'G': 1, 'B': 2, 'A': 3}[channel]]
    if domain == 'POINT':
        return np.clip(weights, 0, 1)
    if domain != 'CORNER':
        return None
    out = np.zeros(len(snap.points), dtype=np.float64)
    count = np.zeros(len(snap.points), dtype=np.int32)
    np.add.at(out, snap.loop_verts, weights)
    np.add.at(count, snap.loop_verts, 1)
    return np.clip(out / np.maximum(count, 1), 0, 1)


def resolve(context, source, snap, prep, points, polys, factor, avoid):
    from . import paint, mesh_io
    e = source.project_damage
    rust, fabric, fabric_avoid, concrete = None, None, None, []
    if e.all_off:
        return rust, fabric, fabric_avoid, concrete
                                                                              
    if e.fabric_enable and e.fabric_group:
        group = source.vertex_groups.get(e.fabric_group)
        if group is None:
            raise ValueError("Fabric Tears: select an existing tear vertex group or clear the field")
        fabric = mesh_io.read_mask_weights(source.data, group.index) if snap is None else snap.groups[group.index]
        if prep is not None:
            fabric = prep.point_values(fabric)
        fabric = np.ascontiguousarray(fabric, dtype=np.float64)
    if e.fabric_enable and prep is not None and prep.helper.any():
                                                                           
                                                                          
                                                                
        fabric_avoid = np.zeros(len(polys), dtype=bool) if avoid is None else avoid.copy()
        blocked = set()
        if avoid is not None:
            for face in polys[avoid & ~prep.helper]:
                blocked.update(int(prep.point_src[int(v)]) for v in face)
        for i in np.flatnonzero(prep.helper):
            fabric_avoid[i] = any(int(prep.point_src[int(v)]) in blocked for v in polys[i])
    if (e.concrete_enable or e.concrete_v2_enable) and e.concrete_mode == '1':
        concrete = concrete_draw_paths(context, source, points, polys, factor)
    return rust, fabric, fabric_avoid, concrete


def damage_materials(source, params, existing):
    """Useful defaults for the new classes, preserving user overrides."""
    result = dict(existing or {})
    other_adds = (params.build_coverage > 0 or params.build_corner_prob > 0
                  or (params.deposit_amount > 0 and not params.deposit_separate)
                  or params.heat_enable or params.rust_enable or params.coating_enable)
    if not other_adds and params.fabric.enable and not params.concrete.enable:
        original = next((m for m in source.data.materials if m), None)
        if original is not None:
            result.setdefault(5, original)                                                     
            if params.surf_amount == 0 and params.corner_prob == 0:
                result.setdefault(4, original)
    if not other_adds and params.concrete.enable and not params.fabric.enable and 5 not in result:
        steel = bpy.data.materials.get("ProjectDamage_Concrete_Rebar")
        if steel is None:
            steel = bpy.data.materials.new("ProjectDamage_Concrete_Rebar")
            enable_nodes(steel)
            shader = steel.node_tree.nodes.get("Principled BSDF")
            shader.inputs['Base Color'].default_value = (.12, .10, .08, 1)
            shader.inputs['Metallic'].default_value = .85
            shader.inputs['Roughness'].default_value = .65
            steel.diffuse_color = (.12, .10, .08, 1)
        result[5] = steel
    return result


def rust_materials(obj, exclude=()):
    """Wrap COPIES of result materials, never the source/shared originals."""
    if obj is None:
        return
    for i, original in enumerate(list(obj.data.materials)):
        if original is None or original in exclude:
            continue
        if original.get("project_damage_rust_shader"):
            continue
        cached = next((m for m in bpy.data.materials
                       if m.get("project_damage_rust_original") == original), None)
        if cached is not None:
            obj.data.materials[i] = cached
            continue
        mat = original.copy()
        mat.name = original.name + "_ProjectDamageRust"
        enable_nodes(mat)
        mat["project_damage_rust_shader"] = True
        mat["project_damage_rust_original"] = original
        nodes, links = mat.node_tree.nodes, mat.node_tree.links
        output = next((n for n in nodes if n.type == 'OUTPUT_MATERIAL' and n.is_active_output), None)
        if output is None:
            output = nodes.new('ShaderNodeOutputMaterial')
        base = output.inputs['Surface'].links[0].from_socket if output.inputs['Surface'].is_linked else None
        if base is None:
            bsdf = nodes.new('ShaderNodeBsdfPrincipled')
            bsdf.inputs['Base Color'].default_value = original.diffuse_color
            base = bsdf.outputs['BSDF']
        rust = nodes.new('ShaderNodeBsdfPrincipled')
        rust.label = "ProjectDamage Rust"
        rust.inputs['Base Color'].default_value = (.23, .055, .014, 1)
        rust.inputs['Roughness'].default_value = .92
        amount = nodes.new('ShaderNodeAttribute'); amount.attribute_name = 'project_damage_rust'
        streak = nodes.new('ShaderNodeAttribute'); streak.attribute_name = 'project_damage_rust_streak'
        factor = nodes.new('ShaderNodeMath'); factor.operation = 'MAXIMUM'; factor.use_clamp = True
        mix = nodes.new('ShaderNodeMixShader')
        links.new(amount.outputs['Fac'], factor.inputs[0])
        links.new(streak.outputs['Fac'], factor.inputs[1])
        links.new(factor.outputs[0], mix.inputs[0])
        links.new(base, mix.inputs[1])
        links.new(rust.outputs['BSDF'], mix.inputs[2])
        links.new(mix.outputs[0], output.inputs['Surface'])
        obj.data.materials[i] = mat


class PROJECT_DAMAGE_OT_rust_paint_mask(bpy.types.Operator):
    bl_idname = "project_damage.rust_paint_mask"
    bl_label = "Create / Draw Rust Contour"
    bl_description = "Create an editable Grease Pencil contour drawing for inline rust"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        from .build import PROJECT_DAMAGE_OT_update
        return PROJECT_DAMAGE_OT_update.poll(context)

    def execute(self, context):
        ownership.context_source(context).project_damage.rust_mode = 'PAINT'
        return bpy.ops.project_damage.paint_new(target='RUST')


# подключаем свойства и действия этого модуля к Blender
def register():
    pass                                                                        


# убираем подключения модуля при выключении аддона
def unregister():
    pass
