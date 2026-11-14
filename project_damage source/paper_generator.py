# деформируем выбранный меш как бумагу и добавляем разрывы
# SPDX-License-Identifier: GPL-3.0-or-later
"""Native paper deformation and tearing of the managed source mesh."""
from . import output_collections
import math

import bpy
import numpy as np
from mathutils import Vector

from . import background_compute, core_loader, generated_assets, live
from . import material_controls, material_library, ownership, paint, params


ASSET = 'paper_generator'
MATERIAL_FIELDS = ('front_material', 'back_material', 'edge_material')
DRAW_LAYERS = (('Tear', (0.08, 0.47, 1.0, 0.85)), ('Hole', (1.0, 0.30, 0.06, 0.85)))


def _length(name, default, description, minimum=0.0):
    return bpy.props.FloatProperty(name=name, default=default, min=minimum,
                                   precision=3, update=live.touch_prop, description=description)


def _factor(name, default, description, maximum=1.0):
    return bpy.props.FloatProperty(name=name, default=default, min=0.0, max=maximum,
                                   subtype='FACTOR', update=live.touch_prop, description=description)


def _count(name, default, maximum, description, minimum=0):
    return bpy.props.IntProperty(name=name, default=default, min=minimum,
                                 max=maximum, update=live.touch_prop, description=description)


def _materials(mesh, settings):
    kinds = mesh.attributes.get('.pd_paper_surface_type')
    original = mesh.attributes.get('.pd_paper_source_material')
    if kinds is None or original is None:
        return
    count = len(mesh.polygons)
    classes = np.empty(count, np.int32)
    indices = np.empty(count, np.int32)
    kinds.data.foreach_get('value', classes)
    original.data.foreach_get('value', indices)
    front = settings.front_material
    for kind, material in enumerate((front, settings.back_material or front, settings.edge_material or front)):
        if material is None:
            continue
        index = next((i for i, value in enumerate(mesh.materials) if value == material), None)
        if index is None:
            index = len(mesh.materials)
            mesh.materials.append(material)
        indices[classes == kind] = index
    mesh.polygons.foreach_set('material_index', indices)


def _update_materials(settings, context):
    if live._APPLYING or live._HISTORY_RESTORING:
        return
    from . import build
    source = settings.id_data
    output = build.result_of(source)
    if source.project_damage.paper_generator_enable and output is not None:
        _materials(output.data, settings)
        material_controls.apply_shared_output_material(source)


def _grease_pencil_only(self, obj):
    return obj.type == 'GREASEPENCIL'


# здесь хранятся значения, которые сохраняются вместе с объектом
class ProjectDamagePaperSettings(bpy.types.PropertyGroup):
    width: _length('Width (cm)', 21.0, 'Width of the sheet along its local X axis', 0.01)
    height: _length('Height (cm)', 29.7, 'Height of the sheet along its local Y axis', 0.01)
    resolution: _length('Resolution (cm)', 0.2, 'Target edge length. Smaller values capture finer tears and creases', 0.001)
    thickness: _length('Thickness (cm)', 0.0, 'Zero makes a single sheet. Positive values add back faces and edge walls')
    quads: bpy.props.BoolProperty(name='Quad Interior', default=True, update=live.touch_prop, description='Use a quad grid away from torn borders')
    seed: _count('Seed', 0, 2147483647, 'Repeatable random variation in wrinkles, holes and torn edges')
    max_verts: _count('Vertex Budget', 200000, 50000000, 'Limit refinement of the source mesh. Existing vertices are retained; cuts can add intersection vertices', 1000)
    edge_rough: _factor('Original Edge Roughness', 0.1, 'Irregularity of the uncut sheet border')
    crumple_strength: _factor('Crumple Strength', 0.5, 'Zero removes the random crease field. Higher values steepen creases', 3.0)
    crumple_size: _length('Crumple Size (cm)', 4.0, 'Average distance between large creases', 0.001)
    crumple_detail: _factor('Crumple Detail', 0.5, 'Weight of fine creases over the broad folds')
    crumple_sharpness: _factor('Crumple Sharpness', 0.8, 'Blend between soft wrinkles and sharp creases with flat facets')
    waviness: _factor('Waviness', 0.15, 'Strength of broad undulations in the sheet')
    wave_size: _length('Wave Size (cm)', 15.0, 'Wavelength of broad undulations', 0.001)
    folds_x: _count('Folds X', 0, 32, 'Number of straight folds spaced along the X axis')
    folds_y: _count('Folds Y', 0, 32, 'Number of straight folds spaced along the Y axis')
    fold_depth: _length('Fold Depth (cm)', 0.4, 'Height of the zigzag profile across folds')
    torn_width: _length('Tear Variation (cm)', 2.0, 'How far the generated torn edge can wander around the selected damage boundary')
    torn_scale: _length('Tear Scale (cm)', 3.0, 'Size of the large lobes along tears', 0.001)
    tongues: _factor('Paper Tongues', 0.6, 'Amount of elongated points of paper left along torn edges')
    tongue_stretch: bpy.props.FloatProperty(name='Tongue Stretch', default=3.0, min=1.0, max=8.0, update=live.touch_prop,
                                           description='Length-to-width ratio of elongated tips')
    tongue_angle: bpy.props.FloatProperty(name='Pull Direction', default=-math.pi/2, subtype='ANGLE', update=live.touch_prop,
                                         description='Direction of the pull in the paper plane')
    tongue_angle_random: _factor('Pull Direction Variation', 0.25, 'How much the pull direction varies along a tear')
    torn_detail: _factor('Tear Detail', 0.7, 'Strength of small teeth and fibres on torn edges')
    flakes: _factor('Loose Flakes', 0.5, 'Fraction of small disconnected pieces to keep near torn edges')
    peel_width: _length('Printed Layer Peel (cm)', 0.6, 'Width of the peel attribute near torn edges. Zero disables the band')
    rag_size: _length('Hole Rim Feature Size (cm)', 1.2, 'Size of the irregular features along hole borders', 0.001)
    jagged: _factor('Hole Rim Jaggedness', 0.45, 'Strength of large jagged changes along hole rims')
    fiber: _factor('Hole Rim Fibres', 0.4, 'Amount of fine fibre-like irregularity on hole rims')
    roundness: _factor('Hole Rim Roundness', 0.5, 'Rounding of the hole silhouette relative to the rim feature size')
    tear_gap: _length('Tear Gap (cm)', 0.0, 'Spacing between kept pieces. Zero uses the narrowest opening that separates them')
    keep_pieces: bpy.props.BoolProperty(name='Keep Torn Pieces', default=False, update=live.touch_prop, description='Keep all sufficiently large torn pieces instead of the largest sheet')
    min_piece: _factor('Minimum Piece Share', 0.02, 'Discard pieces smaller than this fraction of the largest piece')
    edge_curl: _factor('Edge Curl', 0.0, 'Lift of torn rims above the surrounding sheet')
    curl_width: _length('Curl Width (cm)', 2.0, 'Distance that a lifted rim extends into the sheet')
    edge_dist_max: _length('Edge Attribute Range (cm)', 3.0, 'Maximum distance stored in edge_dist and tear_dist')
    hole_amount: _count('Hole Amount', 0, 4096, 'Requested number of holes inside the sheet')
    hole_size_min: _length('Minimum Hole Radius (cm)', 1.0, 'Lower bound for procedural hole radii')
    hole_size_max: _length('Maximum Hole Radius (cm)', 3.5, 'Upper bound for procedural hole radii')
    hole_irregular: _factor('Hole Irregularity', 0.4, 'Amount of broad lobes on procedural holes')
    hole_elongation: _factor('Hole Elongation', 0.3, 'How much procedural holes stretch away from a circle')
    hole_margin: _length('Hole Margin (cm)', 1.5, 'Minimum distance from the sheet border to procedural holes')
    corner_tears: _count('Torn Corners', 0, 4, 'Number of original corners to tear off')
    corner_size_min: _length('Minimum Corner Size (cm)', 3.0, 'Smallest length removed along a torn corner')
    corner_size_max: _length('Maximum Corner Size (cm)', 8.0, 'Largest length removed along a torn corner')
    edge_bites: _count('Edge Bites', 0, 256, 'Number of procedural bites cut out of the sheet border')
    bite_size_min: _length('Minimum Bite Width (cm)', 1.5, 'Narrowest bite along the sheet border')
    bite_size_max: _length('Maximum Bite Width (cm)', 5.0, 'Widest bite along the sheet border')
    bite_depth: _factor('Bite Depth', 0.5, 'Depth of each border bite as a fraction of its width')
    stroke_hole_width: _length('Drawn Gash Width (cm)', 1.2, 'Width cut around an open stroke on the Hole layer')
    tear_snap: _length('Tear Snap (cm)', 3.0, 'Extend an open tear endpoint to the border when it is within this distance')
    close_share: _factor('Close Contours', 0.12, 'Treat near-closed strokes as loops when the end gap is below this share of stroke length')
    paint_only: bpy.props.BoolProperty(name='Drawing Overrides Procedural', default=False, update=live.touch_prop, description='Skip procedural holes and tears when the drawing has valid strokes')
    drawing: bpy.props.PointerProperty(name='Drawing', type=bpy.types.Object, poll=_grease_pencil_only, update=live.touch_prop)
    smooth: bpy.props.BoolProperty(name='Smooth by Angle', default=True, update=live.touch_prop, description='Smooth the sheet and retain sharp edges at creases over 30 degrees')
    front_material: bpy.props.PointerProperty(name='Front', type=bpy.types.Material, update=_update_materials)
    back_material: bpy.props.PointerProperty(name='Back', type=bpy.types.Material, update=_update_materials)
    edge_material: bpy.props.PointerProperty(name='Edge', type=bpy.types.Material, update=_update_materials)


LENGTHS = (
    'width', 'height', 'resolution', 'thickness', 'crumple_size', 'wave_size', 'fold_depth',
    'torn_width', 'torn_scale', 'peel_width', 'rag_size', 'tear_gap', 'curl_width', 'edge_dist_max',
    'hole_size_min', 'hole_size_max', 'hole_margin', 'corner_size_min', 'corner_size_max',
    'bite_size_min', 'bite_size_max', 'stroke_hole_width', 'tear_snap',
)
VALUES = (
    'seed', 'max_verts', 'edge_rough', 'crumple_strength', 'crumple_detail', 'crumple_sharpness',
    'waviness', 'folds_x', 'folds_y', 'tongues', 'tongue_stretch', 'tongue_angle_random',
    'torn_detail', 'flakes', 'jagged', 'fiber', 'roundness', 'min_piece', 'edge_curl', 'hole_amount',
    'hole_irregular', 'hole_elongation', 'corner_tears', 'edge_bites', 'bite_depth', 'close_share',
)
FLAGS = ('quads', 'keep_pieces', 'paint_only')


def _owner(context):
    source = ownership.context_source(context)
    if source is not None and source.type == 'MESH' and source.project_damage.paper_generator_enable:
        return source
    return None


def _strokes(context, source, drawing, layer, frame):
    origin, basis = frame
    paths = [(path - origin) @ basis.T for path in paint.read_paths(context, source, drawing, layer_names={layer})]
    paths = [np.ascontiguousarray(path, dtype=np.float64) for path in paths]
    cyclic = [int(len(path) > 2 and np.array_equal(path[0], path[-1])) for path in paths]
    return [np.ascontiguousarray(path[:-1]) if closed else path for path, closed in zip(paths, cyclic)], cyclic


def _frame(points):
    origin = (points.min(axis=0) + points.max(axis=0)) * 0.5
    covariance = (points - origin).T @ (points - origin)
    _, axes = np.linalg.eigh(covariance)
    normal = axes[:, 0]
    if normal[np.argmax(np.abs(normal))] < 0:
        normal = -normal
    reference = np.eye(3)[np.argmin(np.abs(normal))]
    horizontal = reference - normal * np.dot(reference, normal)
    horizontal /= np.linalg.norm(horizontal)
    vertical = np.cross(normal, horizontal)
    basis = np.stack((horizontal, vertical, normal))
    local = (points - origin) @ basis.T
    center = (local.min(axis=0) + local.max(axis=0)) * 0.5
    origin += center @ basis
    return origin, basis


def _transfer_groups(context, target, original, mesh, rest_positions, positions):
    if not target.vertex_groups:
        return mesh
    names = [group.name for group in target.vertex_groups]
    transferred = False
    donor = bpy.data.objects.new('_pd_paper_weight_source', original)
    receiver = bpy.data.objects.new('_pd_paper_weight_target', mesh)
    context.collection.objects.link(donor)
    context.collection.objects.link(receiver)
    try:
        for name in names:
            if donor.vertex_groups.get(name) is None:
                donor.vertex_groups.new(name=name)
            receiver.vertex_groups.new(name=name)
        mesh.vertices.foreach_set('co', rest_positions.ravel())
        mesh.update()
        modifier = receiver.modifiers.new('Preserve paper weights', 'DATA_TRANSFER')
        modifier.object = donor
        modifier.use_object_transform = False
        modifier.use_vert_data = True
        modifier.data_types_verts = {'VGROUP_WEIGHTS'}
        modifier.vert_mapping = 'POLYINTERP_NEAREST'
        modifier.layers_vgroup_select_src = 'ALL'
        modifier.layers_vgroup_select_dst = 'NAME'
        with context.temp_override(object=receiver, active_object=receiver,
                                   selected_objects=[receiver], selected_editable_objects=[receiver]):
            bpy.ops.object.modifier_apply(modifier=modifier.name)
        result = receiver.data
        result.vertices.foreach_set('co', positions.ravel())
        result.update()
        target.data = result
        transferred = True
        return result
    finally:
        if receiver.data == mesh:
            mesh.vertices.foreach_set('co', positions.ravel())
            mesh.update()
        bpy.data.objects.remove(receiver, do_unlink=True)
        bpy.data.objects.remove(donor, do_unlink=True)
        if transferred and mesh.users == 0:
            bpy.data.meshes.remove(mesh)


def _generate(context, source, target, s):
    core = core_loader.load()
    if not hasattr(core, 'apply_paper'):
        raise RuntimeError('The selected geometry core lacks mesh Paper deformation. Restart Blender with the bundled core.')
    original = target.data
    if not original.polygons:
        raise ValueError('Select a mesh with faces for Paper deformation.')
    original.calc_loop_triangles()
    triangles = original.loop_triangles
    points = np.empty((len(original.vertices), 3), np.float64)
    original.vertices.foreach_get('co', points.ravel())
    triangle_vertices = np.empty((len(triangles), 3), np.int32)
    triangle_loops = np.empty((len(triangles), 3), np.int32)
    triangle_faces = np.empty(len(triangles), np.int32)
    triangles.foreach_get('vertices', triangle_vertices.ravel())
    triangles.foreach_get('loops', triangle_loops.ravel())
    triangles.foreach_get('polygon_index', triangle_faces)
    origin, basis = _frame(points)
    factor = params.unit_factor(context)
    p = core.PaperGenParams()
    for field in LENGTHS:
        setattr(p, field, getattr(s, field) / factor)
    for field in VALUES:
        setattr(p, field, getattr(s, field))
    for field in FLAGS:
        setattr(p, field, int(getattr(s, field)))
    from . import preview_quality
    p.resolution /= max(.1, preview_quality.percent(source.project_damage) / 100.0)
    local = np.ascontiguousarray((points - origin) @ basis.T)
    extent = np.ptp(local, axis=0)
    p.width = max(float(extent[0]) + 2.0 * p.resolution, p.resolution * 4.0)
    p.height = max(float(extent[1]) + 2.0 * p.resolution, p.resolution * 4.0)
    p.tongue_angle = math.degrees(s.tongue_angle)
    tears, tears_cyclic = _strokes(context, source, s.drawing, 'Tear', (origin, basis))
    holes, holes_cyclic = _strokes(context, source, s.drawing, 'Hole', (origin, basis))
    result = yield from background_compute.native(core, 'apply_paper', local, triangle_vertices, p,
                                                  tears, tears_cyclic, holes, holes_cyclic)
    if not result['ok']:
        raise ValueError('The cuts remove the entire mesh. Reduce the damage or edit the drawing.')
    positions = np.asarray(result['points']) @ basis + origin
    sizes = np.asarray(result['face_sizes'], dtype=np.int32).ravel()
    vertices = np.asarray(result['face_verts'], dtype=np.int32).ravel()
    source_faces = np.asarray(result['source_faces'], dtype=np.int32).ravel()
    source_polygons = triangle_faces[source_faces]
    corner_faces = np.repeat(source_faces, sizes)
    barycentric = np.asarray(result['source_bary'])
    mesh = bpy.data.meshes.new(source.name + '_project_damage_paper')
    try:
        mesh.vertices.add(len(positions))
        mesh.vertices.foreach_set('co', positions.ravel())
        mesh.loops.add(len(vertices))
        mesh.loops.foreach_set('vertex_index', vertices)
        mesh.polygons.add(len(sizes))
        mesh.polygons.foreach_set('loop_start', np.cumsum(sizes, dtype=np.int32) - sizes)
        mesh.polygons.foreach_set('loop_total', sizes)
        for material in original.materials:
            mesh.materials.append(material)
        if not mesh.materials:
            mesh.materials.append(None)
        materials = np.empty(len(original.polygons), np.int32)
        original.polygons.foreach_get('material_index', materials)
        for name, values in (('.pd_paper_surface_type', np.asarray(result['face_mat'], dtype=np.int32).ravel()),
                             ('.pd_paper_source_material', materials[source_polygons])):
            mesh.attributes.new(name, 'INT', 'FACE').data.foreach_set('value', values)
        _materials(mesh, s)
        mesh.update(calc_edges=True)
        for layer in original.uv_layers:
            values = np.empty((len(original.loops), 2), np.float32)
            layer.uv.foreach_get('vector', values.ravel())
            interpolated = np.einsum('ij,ijk->ik', barycentric, values[triangle_loops[corner_faces]])
            mesh.uv_layers.new(name=layer.name, do_init=False).uv.foreach_set('vector', interpolated.ravel())
        if original.uv_layers.active:
            mesh.uv_layers.active = mesh.uv_layers.get(original.uv_layers.active.name)
        for attribute in original.attributes:
            if attribute.domain == 'FACE' and attribute.data_type in {'INT', 'BOOLEAN', 'FLOAT'} and not attribute.name.startswith('.') and attribute.name not in {'sharp_face', 'material_index'}:
                values = np.empty(len(original.polygons), np.float32 if attribute.data_type == 'FLOAT' else np.int32)
                attribute.data.foreach_get('value', values)
                mesh.attributes.new(attribute.name, attribute.data_type, 'FACE').data.foreach_set('value', values[source_polygons])
        for field in ('edge_dist', 'tear_dist', 'crease', 'peel'):
            mesh.attributes.new(field, 'FLOAT', 'POINT').data.foreach_set('value', np.asarray(result[field], dtype=np.float32).ravel())
        if target.vertex_groups:
            first_corners = np.unique(vertices, return_index=True)[1]
            rest_positions = np.einsum('ij,ijk->ik', barycentric[first_corners],
                                       points[triangle_vertices[corner_faces[first_corners]]])
            mesh = _transfer_groups(context, target, original, mesh, rest_positions, positions)
        mesh.polygons.foreach_set('use_smooth', np.full(len(mesh.polygons), s.smooth, dtype=np.bool_))
        if s.smooth:
            mesh.set_sharp_from_angle(angle=math.radians(30))
        mesh.update()
        target.data = mesh
        if original.users == 0:
            bpy.data.meshes.remove(original)
        return target
    except Exception:
        if mesh.users == 0:
            bpy.data.meshes.remove(mesh)
        raise


def after_build(context, source, target, reports):
    output = generated_assets.output(source, ASSET)
    if output is not None:
        output.hide_set(True)
        output.hide_render = True
    if source.project_damage.paper_generator_enable and not source.project_damage.all_off:
        try:
            yield from _generate(context, source, target, source.project_damage_paper)
        except (RuntimeError, ValueError) as exc:
            reports.append(({'WARNING'}, 'Paper Generator: ' + str(exc)))


def _make_drawing(context, owner):
    name = owner.name + ' Drawing'
    gp = bpy.data.grease_pencils.new(name)
    obj = bpy.data.objects.new(name, gp)
    output_collections.link_object(context,owner,obj)
    obj.parent = owner
    obj.matrix_world = owner.matrix_world.copy()
    obj['project_damage_paint_source'] = owner
    obj.hide_render = True
    for layer, color in DRAW_LAYERS:
        gp.layers.new(layer)
        material = bpy.data.materials.new(name + ' ' + layer)
        bpy.data.materials.create_gpencil_data(material)
        material.grease_pencil.color = color
        gp.materials.append(material)
    owner.project_damage_paper.drawing = obj
    return obj


# действие «Draw Paper Damage»
class PROJECT_DAMAGE_OT_draw_paper(bpy.types.Operator):
    bl_idname = 'project_damage.draw_paper'
    bl_label = 'Draw Paper Damage'
    bl_options = {'REGISTER', 'UNDO'}

    action: bpy.props.EnumProperty(items=(('Tear', 'Draw Tear', 'Remove an area or divide the sheet'),
                                          ('Hole', 'Draw Hole', 'Cut a closed hole or an open gash'),
                                          ('FINISH', 'Finish Drawing', 'Return to the sheet'),
                                          ('DELETE', 'Delete Drawing', 'Delete this paper’s drawing')))

    @classmethod
    def poll(cls, context):
        return _owner(context) is not None

    def invoke(self, context, event):
        if self.action == 'DELETE':
            return context.window_manager.invoke_confirm(self, event)
        return self.execute(context)

    def execute(self, context):
        from . import build
        owner = _owner(context)
        if owner is None:
            return {'CANCELLED'}
        if self.action == 'FINISH':
            paint._activate(context, build.result_of(owner) or owner)
            return {'FINISHED'}
        drawing = owner.project_damage_paper.drawing
        if self.action == 'DELETE':
            if drawing is None or drawing.parent != owner or drawing.get('project_damage_paint_source') != owner:
                return {'CANCELLED'}
            paint._activate(context, build.result_of(owner) or owner)
            owner.project_damage_paper.drawing = None
            gp = drawing.data
            bpy.data.objects.remove(drawing, do_unlink=True)
            if gp.users == 0:
                bpy.data.grease_pencils.remove(gp)
            return {'FINISHED'}
        drawing = drawing or _make_drawing(context, owner)
        previous_owner = drawing.get('project_damage_paint_source')
        if previous_owner is not None and previous_owner != owner:
            self.report({'WARNING'}, 'This drawing belongs to another generator. Choose a separate drawing to edit.')
            return {'CANCELLED'}
        drawing['project_damage_paint_source'] = owner
        layer = drawing.data.layers.get(self.action)
        if layer is None:
            layer = drawing.data.layers.new(self.action, set_active=True)
        drawing.data.layers.active = layer
        if layer.current_frame() is None:
            layer.frames.new(context.scene.frame_current)
        if drawing.parent == owner and len(drawing.data.materials) == 2:
            drawing.active_material_index = 0 if self.action == 'Tear' else 1
        paint._start_drawing(context, drawing)
        return {'FINISHED'}


# собираем видимые настройки этого раздела
def draw(layout, owner):
    s = owner.project_damage_paper
    box = layout.box()
    material_controls.centered_heading(box, 'Materials', 'MATERIAL')
    for field in MATERIAL_FIELDS if s.thickness > 0 else MATERIAL_FIELDS[:1]:
        material_library.draw_property(box, s, field)

    box = layout.box()
    material_controls.centered_heading(box, 'Mesh Deformation', 'MESH_DATA')
    column = box.column(align=True)
    for field in ('resolution', 'thickness', 'seed', 'smooth', 'max_verts'):
        column.prop(s, field)

    drawing = layout.box()
    material_controls.centered_heading(drawing, 'Draw Damage', 'GREASEPENCIL')
    row = drawing.row(align=True)
    row.prop(s, 'drawing')
    if s.drawing is not None and s.drawing.parent == owner and s.drawing.get('project_damage_paint_source') == owner:
        row.operator_context = 'INVOKE_DEFAULT'
        row.operator('project_damage.draw_paper', text='', icon='TRASH').action = 'DELETE'
    row = drawing.row(align=True)
    row.scale_y = 1.5
    row.operator('project_damage.draw_paper', text='Draw Tear', icon='GREASEPENCIL').action = 'Tear'
    row.operator('project_damage.draw_paper', text='Draw Hole', icon='GREASEPENCIL').action = 'Hole'
    if s.drawing is not None:
        for field in ('paint_only', 'stroke_hole_width', 'tear_snap', 'close_share'):
            drawing.prop(s, field)

    for title, icon, fields in (
        ('Crumple & Folds', 'MOD_DISPLACE', ('crumple_strength', 'crumple_size', 'crumple_detail',
                                            'crumple_sharpness', 'waviness', 'wave_size',
                                            'folds_x', 'folds_y', 'fold_depth')),
        ('Procedural Damage', 'MOD_BOOLEAN', ('hole_amount', 'corner_tears', 'edge_bites')),
        ('Torn Edges', 'MOD_EXPLODE', ('torn_width', 'torn_scale', 'tongues', 'tongue_stretch',
                                      'tongue_angle', 'tongue_angle_random', 'torn_detail', 'flakes',
                                      'peel_width', 'keep_pieces', 'tear_gap', 'min_piece',
                                      'edge_curl', 'curl_width')),
        ('Hole Rims', 'MESH_CIRCLE', ('rag_size', 'jagged', 'fiber', 'roundness')),
    ):
        box = layout.box()
        material_controls.centered_heading(box, title, icon)
        column = box.column(align=True)
        for field in fields:
            if field in {'tear_gap', 'min_piece'} and not s.keep_pieces:
                continue
            if field == 'curl_width' and not s.edge_curl:
                continue
            column.prop(s, field)
            children = ()
            if field == 'hole_amount' and s.hole_amount:
                children = ('hole_size_min', 'hole_size_max', 'hole_irregular', 'hole_elongation', 'hole_margin')
            elif field == 'corner_tears' and s.corner_tears:
                children = ('corner_size_min', 'corner_size_max')
            elif field == 'edge_bites' and s.edge_bites:
                children = ('bite_size_min', 'bite_size_max', 'bite_depth')
            for child in children:
                column.prop(s, child)



_CLASSES = (ProjectDamagePaperSettings, PROJECT_DAMAGE_OT_draw_paper)


# подключаем свойства и действия этого модуля к Blender
def register():
    for cls in _CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.Object.project_damage_paper = bpy.props.PointerProperty(type=ProjectDamagePaperSettings)


# убираем подключения модуля при выключении аддона
def unregister():
    if hasattr(bpy.types.Object, 'project_damage_paper'):
        del bpy.types.Object.project_damage_paper
    for cls in reversed(_CLASSES):
        if cls.is_registered:
            bpy.utils.unregister_class(cls)
