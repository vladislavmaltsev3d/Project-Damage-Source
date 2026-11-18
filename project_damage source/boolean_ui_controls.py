# общие регуляторы Boolean Break и редактируемые линии арматуры
# SPDX-License-Identifier: GPL-3.0-or-later
"""Boolean-only master controls and editable reinforcement curves."""
from . import output_collections
import bpy
from mathutils import Vector


def touch(self, context):
    from . import live
    live.touch_prop(self, context)


def add_properties(cls):
    cls.__annotations__['surface_rebar_enable']=bpy.props.BoolProperty(
        name='Reinforcement Mesh',default=False,update=touch,
        description='Show a reinforcement grid only inside the concrete craters made by Surface Damage')
    cls.__annotations__['surface_rebar_amount']=bpy.props.FloatProperty(
        name='Amount',default=1.,min=0.,soft_max=10.,update=touch,
        description='Grid density multiplier. Higher values add more bars; 0 removes the grid')
    cls.__annotations__['surface_rebar_radius']=bpy.props.FloatProperty(
        name='Radius (cm)',default=.15,min=.01,soft_max=2.,update=touch,
        description='Radius of each reinforcement bar before Surface Scale and Master Scale')
    cls.__annotations__['surface_rebar_cover']=bpy.props.FloatProperty(
        name='Deepness (cm)',default=.4,min=0.,soft_max=10.,update=touch,
        description='Depth below the original surface. A crater must reach below the bars to expose them')
    cls.__annotations__['surface_rebar_material']=bpy.props.PointerProperty(
        name='Reinforcement',type=bpy.types.Material,update=touch,
        description='Material for the Surface Damage reinforcement grid; empty uses the bundled rusty rebar material')
    cls.__annotations__['boolean_quality'] = bpy.props.FloatProperty(
        name='Resolution', default=100., min=5., soft_max=300., subtype='PERCENTAGE', update=touch,
        description='Boolean cutter and fill geometry quality. Lower is faster; higher adds geometry. Does not change other effects')
    cls.__annotations__['break_displacement_pin'] = bpy.props.FloatProperty(
        name='Displacement Pin Distance (cm)', default=2.0, min=0.0, soft_max=30.0, update=touch,
        description='Width of the pinned band measured along the cut from the original wall. '
                    'Displacement fades in over the next band of the same width. 0 pins just the shared rim. '
                    'In the cut material, multiply Displacement Scale by the Fac of a Geometry Attribute '
                    'named project_damage_break_displacement')
    cls.__annotations__['break_shader_displacement'] = bpy.props.BoolProperty(
        name='Use Shader Displacement', default=False, update=touch,
        description='Generate the pin mask for the Cut Faces and Rim Damage shaders')
    cls.__annotations__['break_smooth_inside'] = bpy.props.FloatProperty(
        name='Smoothness Only Inside', default=0.0, min=0.0, max=5.0, soft_max=5.0, update=touch,
        description='Smooth the cut mesh under the Cut Faces material. The opening and the Displacement '
                    'Pin Distance band stay fixed. 0 leaves the cut unchanged; 1 is standard smoothing. '
                    'Values up to 5 apply progressively stronger smoothing')
    cls.__annotations__['boolean_frame_amount'] = bpy.props.FloatProperty(
        name='Amount', default=1., min=0., soft_max=10., update=touch,
        description='Timber frame density multiplier: 2 halves stud and noggin spacing; 0 removes the frame')
    for prefix in ('pipe', 'cable'):
        cls.__annotations__['boolean_'+prefix+'_scale'] = bpy.props.FloatProperty(
            name='Scale', default=100., min=1., soft_max=400., subtype='PERCENTAGE', update=touch,
            description='Scale the Boolean fill dimensions together, including diameter, deepness and bundle spacing. Does not alter Pipe/Cable Damage')
    cls.__annotations__['boolean_plank_amount'] = bpy.props.FloatProperty(
        name='Amount', min=0., max=1., subtype='FACTOR',
        get=lambda self: 1.-self.rebar_missing,
        set=lambda self,value: setattr(self,'rebar_missing',1.-value), update=touch,
        description='Share of wooden boards present: 1 keeps every board; 0 removes all boards')


def tube_settings(settings, p, prefix):
    """Only the Boolean path uses these controls; defaults are byte-neutral."""
    quality=max(.05,p.boolean_quality/100.)
    settings.sides=max(4,min(96,round(settings.sides*quality)))
    return settings


PENDING_START = '_pd_rebar_curve_start'
PENDING_ADD = '_pd_rebar_curve_add'
LEGACY_DRAFT = 'pd_pending_rebar_curve'


def _curve_state(source):
    from . import boolean_stack
    row = boolean_stack.edited(source)
    return source if row is None else row


def _legacy_draft(source, state):
    from . import boolean_stack
    curve = source.get(LEGACY_DRAFT)
    if (isinstance(curve, bpy.types.Object) and curve.type == 'CURVE'
            and curve.get('project_damage_paint_source') == source
            and curve.get(boolean_stack.INSTANCE, 0) == state.get(boolean_stack.INSTANCE, 0)):
        return curve
    return None


def start_point(source):
    state = _curve_state(source)
    if PENDING_START in state:
        return Vector(state[PENDING_START])
    legacy = _legacy_draft(source, state)
    if legacy is not None and len(legacy.data.splines) == 1:
        points = legacy.data.splines[0].bezier_points
        if len(points) == 1:
            return legacy.matrix_world @ points[0].co
    return None


def curve_creation_step(source):
    if start_point(source) is not None:
        return 'END'
    return 'START' if _curve_state(source).get(PENDING_ADD) else 'ADD'


def _clear_start(source, state):
    from . import paint
    if PENDING_ADD in state:
        del state[PENDING_ADD]
    if PENDING_START in state:
        del state[PENDING_START]
    legacy = _legacy_draft(source, state)
    if legacy is not None:
        del source[LEGACY_DRAFT]
        paint.delete_unshared_drawing(source, legacy)


class PROJECT_DAMAGE_OT_rebar_curve(bpy.types.Operator):
    bl_idname = 'project_damage.rebar_curve'
    bl_label = 'Rebar Curve'
    bl_description = 'Use the 3D Cursor to set the endpoints of a bar curve or edit the assigned curve'
    bl_options = {'REGISTER', 'UNDO'}
    action: bpy.props.EnumProperty(items=(
        ('ADD', 'Add', 'Start creating a hand-drawn bar'),
        ('START', 'Set Start Point', 'Store the current 3D Cursor position as the start of the curve'),
        ('END', 'Set End Point', 'Create the curve from the saved start to the 3D Cursor'),
        ('EDIT', 'Edit Curve', 'Edit the assigned spline'),
        ('CANCEL', 'Cancel', 'Clear the saved start point'),
    ))

    @classmethod
    def poll(cls, context):
        from . import boolean_break, ownership
        source = ownership.context_source(context)
        return source is not None and source.type == 'MESH' and boolean_break.variant(source.project_damage) == 'REBAR'

    def execute(self, context):
        from . import background_compute, boolean_stack, build, live, ownership, paint
        source = ownership.context_source(context)
        if self.action == 'EDIT':
            curve = source.project_damage.rebar_drawing
            if curve is None or curve.type != 'CURVE':
                self.report({'WARNING'}, 'Assign a Curve object to edit a spline')
                return {'CANCELLED'}
            if context.active_object != curve or curve.mode != 'EDIT':
                paint._activate(context, curve)
                bpy.ops.object.mode_set(mode='EDIT')
            return {'FINISHED'}

        state = _curve_state(source)
        if self.action == 'ADD':
            state[PENDING_ADD] = True
            if not bpy.app.background:
                def instructions(menu, _context):
                    menu.layout.label(text='Place the 3D Cursor, then click Set Start Point in the panel.')
                    menu.layout.label(text='Move the 3D Cursor, then click Set End Point.')
                context.window_manager.popup_menu(instructions, title='Add Hand-Drawn Rebar', icon='INFO')
            return {'FINISHED'}
        position = context.scene.cursor.location.copy()
        if self.action == 'START':
            state[PENDING_START] = list(position)
            return {'FINISHED'}
        if self.action == 'CANCEL':
            _clear_start(source, state)
            return {'FINISHED'}
        start = start_point(source)
        if start is None:
            self.report({'WARNING'}, 'Set the start point before setting the end point')
            return {'CANCELLED'}
        if (position - start).length <= 1e-6:
            self.report({'WARNING'}, 'Move the 3D Cursor away from the start point')
            return {'CANCELLED'}
        if source.mode != 'OBJECT':
            self.report({'WARNING'}, 'Return the source mesh to Object Mode before creating the curve')
            return {'CANCELLED'}

        data = bpy.data.curves.new(source.name + '_RebarPath', 'CURVE')
        data.dimensions = '3D'
        points = data.splines.new('BEZIER').bezier_points
        points.add(1)
        points[0].co = (0, 0, 0)
        points[1].co = position - start
        for point in points:
            point.handle_left_type = point.handle_right_type = 'AUTO'
        curve = bpy.data.objects.new(data.name, data)
        curve.parent = source
        curve.matrix_parent_inverse = source.matrix_world.inverted_safe()
        curve.location = start
        curve.hide_render = True
        curve['project_damage_paint_source'] = source
        serial = state.get(boolean_stack.INSTANCE, 0)
        if serial:
            curve[boolean_stack.INSTANCE] = serial
        try:
            output_collections.link_object(context, source, curve)
        except RuntimeError as exc:
            bpy.data.objects.remove(curve, do_unlink=True)
            bpy.data.curves.remove(data)
            self.report({'ERROR'}, str(exc))
            return {'CANCELLED'}

        previous_curve = source.project_damage.rebar_drawing
        was_applying = live._APPLYING
        live._APPLYING = True
        try:
            source.project_damage.rebar_drawing = curve
            boolean_stack.save(source)
        finally:
            live._APPLYING = was_applying
        _clear_start(source, state)
        if previous_curve is not None:
            paint.delete_unshared_drawing(source, previous_curve)
        live.invalidate_index()
        background_compute.cancel(source)
        _, reports = build.rebuild_object(context, source)
        for level, message in reports:
            self.report(level, message)
        return {'FINISHED'}


# подключаем свойства и действия этого модуля к Blender
def register():
    from . import displacement_shader
    bpy.utils.register_class(PROJECT_DAMAGE_OT_rebar_curve)
    displacement_shader.register()
    from . import ownership,generated_assets
    if 'project_damage_surface_rebar_of' not in ownership.STAMPS:
        ownership.STAMPS+=('project_damage_surface_rebar_of',)
    generated_assets.LABELS['surface_rebar']='Surface Damage Reinforcement'
# убираем подключения модуля при выключении аддона
def unregister():
    from . import displacement_shader
    displacement_shader.unregister()
    bpy.utils.unregister_class(PROJECT_DAMAGE_OT_rebar_curve)
