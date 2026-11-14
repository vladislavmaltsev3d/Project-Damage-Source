# рисуем линию среза во вьюпорте и превращаем её в объёмный каттер
# SPDX-License-Identifier: GPL-3.0-or-later
"""Draw a view-aligned polyline and turn its thickened silhouette into one cutter."""
from . import output_collections
import math

import bmesh
import bpy
from bpy.app.handlers import persistent
from bpy_extras import view3d_utils
from mathutils import Vector
from mathutils.geometry import delaunay_2d_cdt

_sessions = {}
_handle = None


def _bounds(context, source):
    evaluated = source.evaluated_get(context.evaluated_depsgraph_get())
    return [evaluated.matrix_world @ Vector(point) for point in evaluated.bound_box]


class SliceDraft:
    def __init__(self, context, source):
        bounds = _bounds(context, source)
        self.source = source
        self.area = context.area.as_pointer()
        self.scene = context.scene.as_pointer()
        self.center = sum(bounds, Vector()) / len(bounds)
        self.span = max(2 * max((point - self.center).length for point in bounds), 1e-6)
        self.thickness = self.span * 0.015
        self.points = []
        self.normal = None
        self.eye = None

    def add(self, region, view, mouse):
        if self.normal is None:
            camera = view.view_matrix.inverted()
            axes = camera.to_quaternion()
            self.u = axes @ Vector((1, 0, 0))
            self.v = axes @ Vector((0, 1, 0))
            self.normal = self.u.cross(self.v).normalized()
            if view.is_perspective:
                self.eye = camera.translation.copy()
        direction = view3d_utils.region_2d_to_vector_3d(region, view, mouse)
        dot = direction.dot(self.normal)
        if abs(dot) < 1e-5:
            return False
        origin = view3d_utils.region_2d_to_origin_3d(
            region, view, mouse, clamp=view.view_distance + self.span * 4)
        point = origin + direction * ((self.center - origin).dot(self.normal) / dot)
        delta = point - self.center
        coords = Vector((delta.dot(self.u), delta.dot(self.v)))
        if not self.points or (self.points[-1] - coords).length > self.span * 1e-5:
            self.points.append(coords)
        return True

    def world(self, point, depth=0.0):
        return self.center + self.u * point.x + self.v * point.y + self.normal * depth


def _project_caps(draft, coords, high):
    eye = draft.eye
    # Perspective uses the original view rays, as long as both caps remain in front of the eye.
    if eye is not None:
        distance = (eye - draft.center).dot(draft.normal)
        if distance > max(high, 0.0) + draft.span * 1e-5:
            return [eye + (draft.world(Vector((x, y))) - eye) * (1 - z / distance) - draft.center
                    for x, y, z in coords]
    return [draft.world(Vector((x, y)), z) - draft.center for x, y, z in coords]


def slice_geometry(points, thickness, low, high):
    """Triangulate the union of the 2D stroke capsules, then extrude its boundary."""
    radius = thickness / 2
    vertices, outlines = [], []
    for a, b in zip(points, points[1:]):
        angle = math.atan2(b.y - a.y, b.x - a.x)
        start = len(vertices)
        for center, base in ((a, angle + math.pi / 2), (b, angle - math.pi / 2)):
            vertices.extend(center + radius * Vector((math.cos(base + i*math.pi/8),
                                                       math.sin(base + i*math.pi/8)))
                            for i in range(9))
        outlines.append(tuple(range(start, len(vertices))))
    tolerance = max(thickness * 1e-6, 1e-12)
    verts, _, triangles, _, _, _ = delaunay_2d_cdt(vertices, [], outlines, 1, tolerance, False)
    if not triangles:
        raise ValueError('The slice outline is too small. Increase its thickness or add another point.')
    boundary = {}
    for face in triangles:
        for a, b in zip(face, face[1:] + face[:1]):
            key = (min(a, b), max(a, b))
            if key in boundary:
                del boundary[key]
            else:
                boundary[key] = (a, b)
    size = len(verts)
    coords = [(point.x, point.y, depth) for depth in (low, high) for point in verts]
    faces = [tuple(reversed(tri)) for tri in triangles]
    faces.extend(tuple(index + size for index in tri) for tri in triangles)
    faces.extend((a, b, b + size, a + size) for a, b in boundary.values())
    return coords, faces


def _new_cutter(context, draft):
    from . import boolean_break

    source = draft.source
    depth = [(p - draft.center).dot(draft.normal) for p in _bounds(context, source)]
    padding = max(draft.span * 0.03, draft.thickness * 2)
    low, high = min(depth) - padding, max(depth) + padding
    coords, faces = slice_geometry(draft.points, draft.thickness, low, high)
    mesh = bpy.data.meshes.new('Slice Cutter')
    cutter = None
    try:
        mesh.from_pydata(_project_caps(draft, coords, high), [], faces)
        mesh.update()
        bm = bmesh.new()
        try:
            bm.from_mesh(mesh)
            if not all(edge.is_manifold for edge in bm.edges) or not all(v.is_manifold for v in bm.verts):
                raise ValueError('The slice touches itself at a narrow point. Adjust the thickness or redraw it.')
        finally:
            bm.free()
        cutter = bpy.data.objects.new('Slice Cutter', mesh)
        output_collections.link_object(context,source,cutter)
    except Exception:
        if cutter is not None:
            bpy.data.objects.remove(cutter, do_unlink=True)
        if mesh.users == 0:
            bpy.data.meshes.remove(mesh)
        raise
    cutter.location = draft.center
    cutter[boolean_break.PRIMITIVE_KEY] = 'SLICE'
    cutter.project_damage_breaker.strength = 20.0
    return cutter


def _draft(context):
    if context.window is None or context.area is None:
        return None
    draft = _sessions.get(context.window.as_pointer())
    return (draft if draft is not None and draft.area == context.area.as_pointer()
            and draft.scene == context.scene.as_pointer() else None)


def _available(draft, context):
    try:
        source = draft.source
        return (context.view_layer.objects.get(source.name) == source
                and _can_slice(source.project_damage))
    except ReferenceError:
        return False


def _can_slice(props):
    return props.ui_boolean_enable or (props.hang_enable and props.hang_mode == '1')


def _contains(region, x, y):
    return region.x <= x < region.x + region.width and region.y <= y < region.y + region.height


def _canvas(area, event):
    x, y = event.mouse_x, event.mouse_y
    for region in area.regions:
        if region.type != 'WINDOW' and region.width > 1 and region.height > 1 and _contains(region, x, y):
            return None
    return next((r for r in area.regions if r.type == 'WINDOW' and _contains(r, x, y)), None)


def draw_panel(layout, source, context):
    from . import effect_layout, params
    layout.label(text='Select a cutter to edit Break Surface.', icon='INFO')
    box = effect_layout.section(layout, 'Slicers')
    draft = _draft(context)
    if draft is None:
        box.operator('project_damage.slice_draw', text='Add Slice', icon='ADD')
    elif _available(draft, context) and draft.source == source:
        row = box.row()
        row.enabled = len(draft.points) >= 2
        row.operator('project_damage.slice_apply', text='Apply Slice', icon='CHECKMARK')
        box.label(text=f'{len(draft.points)} points  ·  {draft.thickness * params.unit_factor(context):.3g} cm')
        box.label(text='Click points · Wheel · Esc', icon='INFO')


def _draw():
    import gpu
    from gpu_extras.batch import batch_for_shader

    context = bpy.context
    draft = _draft(context)
    if draft is None or not draft.points or context.region is None or context.region_data is None:
        return

    def project(point):
        return view3d_utils.location_3d_to_region_2d(context.region, context.region_data, draft.world(point))

    projected = [project(point) for point in draft.points]
    strips, lines, dots = [], [], []
    for i, (a, b) in enumerate(zip(draft.points, draft.points[1:])):
        if projected[i] is None or projected[i+1] is None:
            continue
        lines.extend((projected[i], projected[i+1]))
        d = (b - a).normalized()
        w = Vector((-d.y, d.x)) * (draft.thickness / 2)
        corners = [project(q) for q in (a-w, b-w, b+w, a+w)]
        if all(p is not None for p in corners):
            strips.extend(corners[j] for j in (0,1,2,0,2,3))
    circle = [Vector((math.cos(i*math.tau/12), math.sin(i*math.tau/12))) * 5.5 for i in range(12)]
    for center in projected:
        if center is not None:
            for a, b in zip(circle, circle[1:] + circle[:1]):
                dots.extend((center, center + a, center + b))

    blend = gpu.state.blend_get()
    gpu.state.blend_set('ALPHA')
    try:
        fill = gpu.shader.from_builtin('UNIFORM_COLOR')
        for coords, color in ((strips, (0.18, .66, 1., .25)), (dots, (.88, .97, 1., 1.))):
            if coords:
                fill.bind()
                fill.uniform_float('color', color)
                batch_for_shader(fill, 'TRIS', {'pos': coords}).draw(fill)
        if lines:
            line = gpu.shader.from_builtin('POLYLINE_UNIFORM_COLOR')
            line.bind()
            line.uniform_float('viewportSize', gpu.state.viewport_get()[2:])
            line.uniform_float('lineWidth', 2.0)
            line.uniform_float('color', (.2, .73, 1., 1.))
            batch_for_shader(line, 'LINES', {'pos': lines}).draw(line)
    finally:
        gpu.state.blend_set(blend)


# действие «Add Slice»
class PROJECT_DAMAGE_OT_slice_draw(bpy.types.Operator):
    bl_idname = 'project_damage.slice_draw'
    bl_label = 'Add Slice'
    bl_description = 'Draw points in the viewport, adjust width with the wheel, then press Apply Slice'

    @classmethod
    def poll(cls, context):
        from . import build, ownership
        src = ownership.context_source(context)
        return (context.mode == 'OBJECT' and context.window is not None and context.area is not None
                and context.area.type == 'VIEW_3D' and src is not None
                and src.is_editable and build.result_of(src) is not None and _can_slice(src.project_damage))

    def invoke(self, context, event):
        from . import ownership
        self.key = context.window.as_pointer()
        if self.key in _sessions:
            self.report({'INFO'}, 'Finish the active slice or press Esc')
            return {'CANCELLED'}
        _sessions[self.key] = SliceDraft(context, ownership.context_source(context))
        context.window_manager.modal_handler_add(self)
        context.area.tag_redraw()
        return {'RUNNING_MODAL'}

    def modal(self, context, event):
        draft = _sessions.get(self.key)
        if draft is None:
            return {'FINISHED'}
        area = next((a for a in context.window.screen.areas if a.as_pointer() == draft.area), None)
        if (area is None or area.type != 'VIEW_3D' or draft.scene != context.scene.as_pointer()
                or not _available(draft, context) or
                (event.type == 'ESC' and event.value == 'PRESS')):
            self.cancel(context)
            return {'CANCELLED'}
        region = _canvas(area, event)
        if region is None or event.shift or event.alt or event.ctrl or event.oskey:
            return {'PASS_THROUGH'}
        if event.type == 'LEFTMOUSE' and context.mode == 'OBJECT':
            if event.value == 'PRESS':
                mouse = (event.mouse_x - region.x, event.mouse_y - region.y)
                if not draft.add(region, region.data or area.spaces.active.region_3d, mouse):
                    self.report({'INFO'}, 'Orbit back toward the slice plane before adding a point')
                area.tag_redraw()
            return {'RUNNING_MODAL'}
        if event.type in {'WHEELUPMOUSE', 'WHEELDOWNMOUSE'} and len(draft.points) >= 2:
            step = 1.14 if event.type == 'WHEELUPMOUSE' else 1 / 1.14
            draft.thickness = min(draft.span * .5, max(draft.span * 1e-4, draft.thickness * step))
            area.tag_redraw()
            return {'RUNNING_MODAL'}
        return {'PASS_THROUGH'}

    def cancel(self, context):
        _sessions.pop(self.key, None)
        if context.area is not None:
            context.area.tag_redraw()


# действие «Apply Slice»
class PROJECT_DAMAGE_OT_slice_apply(bpy.types.Operator):
    bl_idname = 'project_damage.slice_apply'
    bl_label = 'Apply Slice'
    bl_description = 'Create a closed cutter through the entire target and update Boolean Break'
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        draft = _draft(context)
        return (draft is not None and len(draft.points) >= 2 and context.mode == 'OBJECT'
                and _available(draft, context))

    def execute(self, context):
        from . import background_compute, boolean_break, build, live
        draft = _draft(context)
        source = draft.source
        p = source.project_damage
        boolean_was_enabled = p.ui_boolean_enable
        previous_count = len(p.breakers)
        previous_index = p.breaker_index
        cutter = None
        cutter_index = None
        previous = live._APPLYING
        live._APPLYING = True
        try:
            cutter = _new_cutter(context, draft)
            boolean_break.link_cutter(source, cutter)
            p.ui_boolean_enable = boolean_was_enabled
            cutter_index = p.breaker_index
            background_compute.cancel(source)
            result, reports = build.rebuild_object(context, source)
            if result is None:
                raise RuntimeError(next((msg for kind, msg in reports if 'ERROR' in kind), 'Boolean Break failed'))
        except (RuntimeError, ValueError) as exc:
            if cutter is not None:
                mesh = cutter.data
                bpy.data.objects.remove(cutter, do_unlink=True)
                if mesh.users == 0:
                    bpy.data.meshes.remove(mesh)
                if len(p.breakers) > previous_count:
                    p.breakers.remove(previous_count)
                elif cutter_index is not None:
                    p.breakers[cutter_index].obj = None
                p.breaker_index = previous_index
                p.ui_boolean_enable = boolean_was_enabled
            self.report({'WARNING'}, str(exc))
            return {'CANCELLED'}
        finally:
            live._APPLYING = previous
        _sessions.pop(context.window.as_pointer(), None)
        context.area.tag_redraw()
        for kind, message in reports:
            self.report(kind, message)
        return {'FINISHED'}


@persistent
def clear_drafts(*_args):
    _sessions.clear()


_CLASSES = (PROJECT_DAMAGE_OT_slice_draw, PROJECT_DAMAGE_OT_slice_apply)


# подключаем свойства и действия этого модуля к Blender
def register():
    global _handle
    for cls in _CLASSES:
        bpy.utils.register_class(cls)
    if not bpy.app.background:
        _handle = bpy.types.SpaceView3D.draw_handler_add(_draw, (), 'WINDOW', 'POST_PIXEL')
    for callbacks in (bpy.app.handlers.load_pre, bpy.app.handlers.undo_pre, bpy.app.handlers.redo_pre):
        callbacks.append(clear_drafts)


# убираем подключения модуля при выключении аддона
def unregister():
    global _handle
    clear_drafts()
    if _handle is not None:
        bpy.types.SpaceView3D.draw_handler_remove(_handle, 'WINDOW')
        _handle = None
    for callbacks in (bpy.app.handlers.load_pre, bpy.app.handlers.undo_pre, bpy.app.handlers.redo_pre):
        if clear_drafts in callbacks:
            callbacks.remove(clear_drafts)
    for cls in reversed(_CLASSES):
        bpy.utils.unregister_class(cls)
