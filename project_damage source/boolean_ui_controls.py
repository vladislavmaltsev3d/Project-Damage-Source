# общие регуляторы Boolean Break и редактируемые линии арматуры
# SPDX-License-Identifier: GPL-3.0-or-later
"""Boolean-only master controls and editable reinforcement curves."""
from . import output_collections
import bpy


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


# действие «Rebar Curve»
class PROJECT_DAMAGE_OT_rebar_curve(bpy.types.Operator):
    bl_idname='project_damage.rebar_curve'
    bl_label='Rebar Curve'
    bl_description='Create or edit a Bezier spline to guide the reinforcement bars'
    bl_options={'REGISTER','UNDO'}
    action:bpy.props.EnumProperty(items=(('NEW','New Curve','Create an editable bar path'),('EDIT','Edit Curve','Edit the assigned spline')))

    def execute(self,context):
        from . import ownership,paint,live
        source=ownership.context_source(context)
        if source is None:return {'CANCELLED'}
        p=source.project_damage
        curve=p.rebar_drawing
        if self.action=='NEW':
            data=bpy.data.curves.new(source.name+'_RebarPath','CURVE');data.dimensions='3D'
            spline=data.splines.new('BEZIER');spline.bezier_points.add(1)
            lo=[min(v[i] for v in source.bound_box) for i in range(3)]
            hi=[max(v[i] for v in source.bound_box) for i in range(3)]
            for point,t in zip(spline.bezier_points,(.25,.75)):
                point.co=(lo[0]+(hi[0]-lo[0])*t,lo[1],(lo[2]+hi[2])*.5)
                point.handle_left_type=point.handle_right_type='AUTO'
            curve=bpy.data.objects.new(data.name,data)
            output_collections.link_object(context,source,curve)
            curve.matrix_world=source.matrix_world.copy();curve.hide_render=True
            curve['project_damage_paint_source']=source
            p.rebar_drawing=curve
        if curve is None or curve.type!='CURVE':
            self.report({'WARNING'},'Assign a Curve object to edit a spline');return {'CANCELLED'}
        paint._activate(context,curve)
        live.drawing_started(source)
        if not bpy.app.background:bpy.ops.object.mode_set(mode='EDIT')
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
