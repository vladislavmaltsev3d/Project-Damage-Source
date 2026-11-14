# рисуем повреждения самостоятельных конструкций через Grease Pencil
"""Grease Pencil damage bands for independent structural surfaces."""
import math
import bpy
from mathutils import Vector, Matrix
from mathutils.kdtree import KDTree

_session = None


def source_for_drawing(drawing):
    if drawing is None:
        return None
    if _session is not None:
        source = _session[0]
        try:
            if source.pd_structure.drawing == drawing:
                return source
        except ReferenceError:
            pass
    source = drawing.get('pd_structure_owner')
    return source if isinstance(source, bpy.types.Object) and source.get('pd_structure_source') else None


def _restore_source(source, display, hidden):
    try:
        source.display_type = display
        source.hide_set(hidden)
        for key in ('pd_structure_draw_display', 'pd_structure_draw_hidden'):
            if key in source:del source[key]
    except (ReferenceError, RuntimeError):
        pass


def forget_projection():
    global _session
    _session=None


def restore_projection():
    global _session
    session, _session = _session, None
    if session is not None:
        _restore_source(*session)


def prepare_projection(source):
    global _session
    display = source.get('pd_structure_draw_display', source.display_type)
    hidden = source.get('pd_structure_draw_hidden', source.hide_get())
    _session = (source, display, hidden)
    source['pd_structure_draw_display'] = display
    source['pd_structure_draw_hidden'] = hidden
    source.hide_set(False)
    source.display_type = 'SOLID'


def restore_saved_projection(source):
    if 'pd_structure_draw_display' not in source:return
    p=source.pd_structure
    drawing=bpy.context.active_object if bpy.context.active_object in (p.drawing,getattr(p,'brick_loose_drawing',None)) else p.drawing
    if drawing and bpy.context.active_object == drawing and drawing.mode in {'PAINT_GREASE_PENCIL', 'PAINT_GPENCIL'}:
        prepare_projection(source)
    else:
        _restore_source(source, source['pd_structure_draw_display'], source.get('pd_structure_draw_hidden', False))


def start(context, source, drawing):
    global _session
    from . import paint
    paint.finish_surface_projection()
    restore_projection()
    prepare_projection(source)
    try:
        paint._start_drawing(context, drawing)
    except Exception:
        restore_projection()
        raise


def finish(context, source):
    from . import paint, live, structure_surface
    live.finish_drawing(source)
    restore_projection()
    paint._activate(context, structure_surface.output(source) or source)
    if source.pd_structure.live_update:
        signature = structure_surface.state(source)
        if structure_surface._states.get(source.session_uid) != signature:
            structure_surface.queue(source)


def new_drawing(context, source, role="DAMAGE"):
    label='Loose Bricks' if role=='LOOSE' else 'Structure Damage'
    data = bpy.data.grease_pencils.new(source.name + ' '+label+' Drawing')
    data.layers.new(label, set_active=True).frames.new(context.scene.frame_current)
    drawing = bpy.data.objects.new(data.name, data)
    (source.users_collection[0] if source.users_collection else context.collection).objects.link(drawing)
    drawing.parent = source
    drawing.matrix_parent_inverse = Matrix.Identity(4)
    drawing['pd_structure_owner'] = source
    drawing['project_damage_paint_source'] = source
    drawing.hide_render = True
    material = bpy.data.materials.new('Structure Damage Ink')
    bpy.data.materials.create_gpencil_data(material)
    material.grease_pencil.color = (.1,.55,1.,.85) if role=='LOOSE' else (1., .2, .03, .85)
    data.materials.append(material)
    setattr(source.pd_structure,'brick_loose_drawing' if role=='LOOSE' else 'drawing',drawing)
    source.pd_structure.draw_damage = True
    return drawing


def remove_drawing(context, source, role="DAMAGE"):
    prop="brick_loose_drawing" if role=="LOOSE" else "drawing"
    drawing = getattr(source.pd_structure,prop)
    if drawing is None:
        return
    if context.active_object == drawing:
        finish(context, source)
    setattr(source.pd_structure,prop,None)
    if drawing in (source.pd_structure.drawing,getattr(source.pd_structure,'brick_loose_drawing',None)):
        return
    if drawing.get('project_damage_paint_source') != source:
        return
    users = bpy.data.user_map(subset={drawing}).get(drawing, set())
    if any(isinstance(user, bpy.types.Object) and user not in {drawing, source} for user in users):
        return
    data = drawing.data
    materials = list(data.materials)
    bpy.data.objects.remove(drawing, do_unlink=True)
    if not data.users:
        bpy.data.grease_pencils.remove(data)
    for material in materials:
        if material and not material.users:
            bpy.data.materials.remove(material)


# действие «Draw Structure Damage»
class PROJECT_DAMAGE_OT_structure_drawing(bpy.types.Operator):
    bl_idname = 'project_damage.structure_drawing'
    bl_label = 'Draw Structure Damage'
    bl_options = {'REGISTER', 'UNDO'}
    action: bpy.props.EnumProperty(items=[(key, label, description) for key, label, description in (
        ('NEW', 'New Drawing', 'Create a Grease Pencil damage drawing on the control plane'),
        ('DRAW', 'Draw / Edit', 'Draw damage on the control plane, ignoring foreground objects'),
        ('FINISH', 'Finish Drawing', 'Return to the generated structure'),
        ('REMOVE', 'Remove Drawing', 'Unlink this drawing; delete it only if it belongs exclusively to this generator'))])
    source_name: bpy.props.StringProperty(options={'HIDDEN'})
    role: bpy.props.EnumProperty(items=[('DAMAGE','Damage',''),('LOOSE','Loose Bricks','')],default='DAMAGE',options={'HIDDEN'})

    @classmethod
    def description(cls, context, properties):
        return {'NEW': 'Draw damage on the control plane. Drawing Radius sets the affected band width',
                'DRAW': 'Edit the damage strokes; Live Update rebuilds the structure after a stroke changes',
                'FINISH': 'Finish painting and return to the structure settings',
                'REMOVE': 'Remove this damage drawing; shared or externally assigned drawings are kept'}.get(properties.action, cls.bl_label)

    def execute(self, context):
        from . import structure_surface as ss
        source = bpy.data.objects.get(self.source_name) if self.source_name else ss.owner(context)
        if source is None or not source.get('pd_structure_source'):
            return {'CANCELLED'}
        try:
            if self.action == 'FINISH':
                finish(context, source)
            elif self.action == 'REMOVE':
                remove_drawing(context, source,self.role)
            else:
                drawing = new_drawing(context, source,self.role) if self.action == 'NEW' else getattr(source.pd_structure,'brick_loose_drawing' if self.role=='LOOSE' else 'drawing')
                if drawing is None:
                    self.report({'WARNING'}, 'Create or assign a Grease Pencil drawing first')
                    return {'CANCELLED'}
                start(context, source, drawing)
        except (RuntimeError, ValueError) as exc:
            self.report({'ERROR'}, str(exc))
            return {'CANCELLED'}
        return {'FINISHED'}


def paths(source):
    from . import paint
    settings = source.pd_structure
    if not settings.draw_damage or settings.drawing is None:
        return []
    return paint.read_paths(bpy.context, source, settings.drawing, min_points=1)


class DamageField:
    def __init__(self, centers, radius, strokes, drawing_radius):
        self.parts = [(c, c, radius) for c in centers]
        for path in strokes:
            points = [Vector(p) for p in path]
            if len(points) == 1:
                self.parts.append((points[0], points[0], drawing_radius))
            for a, b in zip(points, points[1:]):
                count = min(2048, max(1, math.ceil((b-a).length / max(drawing_radius*2, 1e-6))))
                for i in range(count):
                    self.parts.append((a.lerp(b, i/count), a.lerp(b, (i+1)/count), drawing_radius))
        self.tree = KDTree(len(self.parts)) if self.parts else None
        self.reach = 0.
        for i, (a, b, r) in enumerate(self.parts):
            self.tree.insert((a+b)*.5, i)
            self.reach = max(self.reach, (b-a).length*.5+r)
        if self.tree is not None:
            self.tree.balance()

    def ratio(self, point):
        if self.tree is None:
            return math.inf
        best = math.inf
        for _, index, _ in self.tree.find_range(point, self.reach):
            a, b, radius = self.parts[index]
            delta = b-a
            t = max(0., min(1., (point-a).dot(delta)/delta.length_squared)) if delta.length_squared > 1e-16 else 0.
            best = min(best, (point-a-delta*t).length/radius)
        return best

    @staticmethod
    def sphere(origin, direction, center, radius):
        offset = origin-center
        along = offset.dot(direction)
        rest = radius*radius - (offset.length_squared-along*along)
        if rest <= 0:
            return []
        half = math.sqrt(rest)
        return [(-along-half, -along+half)]

    def remaining(self, origin, direction, left, right, amount):
        if amount <= 0 or not self.parts:
            return [(left, right)]
        removed = []
        for a, b, radius in self.parts:
            radius *= amount
            removed.extend(self.sphere(origin, direction, a, radius))
            delta = b-a
            length = delta.length
            if length < 1e-8:
                continue
            removed.extend(self.sphere(origin, direction, b, radius))
            axis = delta/length
            offset = origin-a
            projection = offset.dot(axis)
            speed = direction.dot(axis)
            if abs(speed) < 1e-8:
                if projection < 0 or projection > length:
                    continue
                low, high = -math.inf, math.inf
            else:
                low, high = sorted((-projection/speed, (length-projection)/speed))
            qa = max(0., 1-speed*speed)
            qb = 2*(offset.dot(direction)-projection*speed)
            qc = offset.length_squared-projection*projection-radius*radius
            if qa < 1e-8:
                if qc > 0:
                    continue
                hit = (low, high)
            else:
                discriminant = qb*qb-4*qa*qc
                if discriminant <= 0:
                    continue
                half = math.sqrt(discriminant)
                hit = (max(low, (-qb-half)/(2*qa)), min(high, (-qb+half)/(2*qa)))
            if hit[1] > hit[0]:
                removed.append(hit)
        kept = []
        cursor = left
        for low, high in sorted(removed):
            if high <= cursor or low >= right:
                continue
            if low > cursor+1e-5:
                kept.append((cursor, min(right, low)))
            cursor = max(cursor, high)
            if cursor >= right:
                break
        if cursor < right-1e-5:
            kept.append((cursor, right))
        return kept


# собираем видимые настройки этого раздела
def draw(layout, context, source):
    from . import material_controls
    p = source.pd_structure
    box = layout.box()
    material_controls.centered_heading(box, 'Draw Damage', icon='GREASEPENCIL')
    box.prop(p, 'draw_damage', toggle=True)
    column = box.column()
    column.active = p.draw_damage
    row = column.row(align=True)
    row.prop(p, 'drawing', text='Drawing')
    sub = row.row(align=True)
    sub.enabled = p.drawing is not None
    op = sub.operator('project_damage.structure_drawing', text='', icon='TRASH')
    op.action = 'REMOVE'; op.source_name = source.name
    row = column.row(align=True)
    for action, label in (('NEW', 'New Drawing'), ('DRAW', 'Draw / Edit')):
        sub = row.row(align=True); sub.enabled = action == 'NEW' or p.drawing is not None
        op = sub.operator('project_damage.structure_drawing', text=label, icon='GREASEPENCIL')
        op.action = action; op.source_name = source.name
    column.prop(p, 'drawing_radius')
    column.label(text='Removes whole bricks and their mortar.' if p.kind=='BRICK' else 'Strokes break a band around the line.', icon='INFO')
    column.label(text='Shape it with Opening below.' if p.kind=='BRICK' else 'Shape it with Opening and Bend below.')
    if context.active_object == p.drawing and p.drawing is not None:
        op = box.operator('project_damage.structure_drawing', text='Finish Drawing', icon='CHECKMARK')
        op.action = 'FINISH'; op.source_name = source.name
