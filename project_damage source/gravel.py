# размещаем мелкие камни на поверхности и по рисунку
from . import preview_quality
# ProjectDamage gravel scatter - GPL-3.0-or-later; see LICENSE.
#
# Scatter Gravel: angular stones / grit on ground objects (core:
# grass_scatter.cpp, shape = gravel) as their own object "<object>_Gravel".
# The same distribution as Grass Scatter: no stones on faces steeper than
# Slope Limit; density per m2 = Base + Edges (foot of walls / kerbs, crack and
# chip rims) + Cavity (cracks, pits, hollows); Grease Pencil Overlay / Add.
#   Stones   low-poly angular chunks (Angularity), flattened (Flatness), sizes
#            from Size Min to Size Max weighted to the small ones (Size Skew),
#            finer where debris gathers (Sorting), partly sunk (Embed)
#   Output   UV map "UVMap" (spherical per stone, random offset), attributes
#            gravel_variation (per stone) and gravel_height (0 bottom .. 1 top)
import bpy
from . import material_controls
import numpy as np
from mathutils import Vector

from . import core_loader


def _poll_gp(self, obj):
    return obj.type in {'GREASEPENCIL', 'GPENCIL'}


# здесь хранятся значения, которые сохраняются вместе с объектом
class ProjectDamageGravelSettings(bpy.types.PropertyGroup):
    master_scale: bpy.props.FloatProperty(name='Master Scale',default=1.,min=.01,soft_max=5.,description='Multiply Size Min and Size Max together; leave distribution density and stone locations unchanged')
    rock_subdivision: bpy.props.IntProperty(name='Rock Subdivision',default=0,min=0,max=2,description='Smooth each rock with triangular Loop subdivision: 0 keeps the original mesh, each level gives four times as many faces')
    slope_limit: bpy.props.FloatProperty(name='Slope Limit', default=35.0, min=0.0, max=89.0, description='Faces steeper than this (degrees) get no gravel: walls stay clean')
    base_density: bpy.props.FloatProperty(name='Base Density', default=150.0, min=0.0, soft_max=5000.0, description='Stones per m2 everywhere on the ground')
    edge_density: bpy.props.FloatProperty(name='Edge Density', default=900.0, min=0.0, soft_max=20000.0, description='Extra stones per m2 at the foot of walls and kerbs, along crack and chip rims')
    edge_radius: bpy.props.FloatProperty(name='Edge Radius', default=0.06, min=0.001, soft_max=1.0, subtype='DISTANCE')
    edge_angle: bpy.props.FloatProperty(name='Edge Angle', default=25.0, min=1.0, max=179.0)
    cavity_density: bpy.props.FloatProperty(name='Cavity Density', default=1500.0, min=0.0, soft_max=20000.0, description='Extra stones per m2 in cracks, pits and hollows')
    cavity_radius: bpy.props.FloatProperty(name='Cavity Radius', default=0.08, min=0.001, soft_max=1.0, subtype='DISTANCE')
    stroke_mode: bpy.props.EnumProperty(name='Draw Mode', items=(('OVERLAY', 'Overlay', 'Gravel only along the strokes'), ('ADD', 'Add', 'The strokes add gravel where it is thin; the rest stays')), default='ADD')
    stroke_density: bpy.props.FloatProperty(name='Stroke Density', default=1500.0, min=0.0, soft_max=20000.0)
    stroke_radius: bpy.props.FloatProperty(name='Stroke Radius', default=0.08, min=0.001, soft_max=1.0, subtype='DISTANCE')
    max_stones: bpy.props.IntProperty(name='Max Stones', default=40000, min=100, max=2000000)
    size_min: bpy.props.FloatProperty(name='Size Min', default=0.004, min=0.0005, soft_max=0.2, subtype='DISTANCE')
    size_max: bpy.props.FloatProperty(name='Size Max', default=0.025, min=0.0005, soft_max=0.5, subtype='DISTANCE')
    size_skew: bpy.props.FloatProperty(name='Size Skew', default=2.5, min=0.2, max=8.0, description='Above 1: many small stones, few big ones')
    flatness: bpy.props.FloatProperty(name='Flatness', default=0.55, min=0.1, max=1.5, description='Height / width')
    angularity: bpy.props.FloatProperty(name='Angularity', default=0.35, min=0.0, max=0.9)
    embed: bpy.props.FloatProperty(name='Embed', default=0.3, min=0.0, max=0.9, description='Share of the height sunk into the ground')
    sorting: bpy.props.FloatProperty(name='Sorting', default=0.6, min=0.0, max=0.9, description='Stones get finer where debris gathers (grit in cracks)')
    pile_height: bpy.props.FloatProperty(name='Pile Height', default=0.004, min=0.0, soft_max=0.1, subtype='DISTANCE')
    seed: bpy.props.IntProperty(name='Seed', default=0, min=0)
    material: bpy.props.PointerProperty(name='Material', type=bpy.types.Material)


def generate(context,source,target,s,reports):
    from . import background_compute as bg,generated_assets
    if bg.async_for(source):
        def work():
            messages=[]
            result=yield from _generate_steps(bpy.context,source,target,s,messages)
            for flags,message in messages:
                if 'ERROR' in flags:raise RuntimeError(message)
            if result is None:
                old=generated_assets.output(source,'gravel')
                if old is not None:old.hide_set(True);old.hide_render=True
            return result
        bg.queue(source,'generate_gravel',work,guard=lambda:bg.valid_id(target))
        return generated_assets.output(source,'gravel')
    return bg.immediate(_generate_steps(context,source,target,s,reports))

def _generate_steps(context, source, target, s, reports):
    from . import background_compute as bg
    core = core_loader.load()
    if core is None or not hasattr(core, 'scatter_grass') or not hasattr(core.GrassParams(), 'shape'):
        reports.append(({'ERROR'}, 'Scatter Gravel: the native core has no gravel scatter (rebuild the core)'))
        return None
    deps = context.evaluated_depsgraph_get()
    ev = target.evaluated_get(deps)
    me = ev.to_mesh()
    try:
        n = len(me.vertices)
        co = np.empty(n * 3, dtype=np.float64)
        me.vertices.foreach_get('co', co)
        ls = np.empty(len(me.polygons), dtype=np.int64); me.polygons.foreach_get('loop_start', ls)
        lt = np.empty(len(me.polygons), dtype=np.int64); me.polygons.foreach_get('loop_total', lt)
        lv = np.empty(len(me.loops), dtype=np.int64); me.loops.foreach_get('vertex_index', lv)
    finally:
        ev.to_mesh_clear()
    polys = []
    for a, t in zip(ls.tolist(), lt.tolist()):
        ring = lv[a:a + t].tolist()
        if t == 3:
            polys.append(ring + [ring[2]])
        elif t == 4:
            polys.append(ring)
        else:
            for k in range(1, t - 1):
                polys.append([ring[0], ring[k], ring[k + 1], ring[k + 1]])
    points = np.ascontiguousarray(co.reshape(-1, 3))
    polys = np.ascontiguousarray(np.array(polys, dtype=np.int32).reshape(-1, 4))
    from . import paint
    to_local = target.matrix_world.inverted_safe()
    world = np.asarray(target.matrix_world, dtype=np.float64)
    points = np.ascontiguousarray(points @ world[:3,:3].T + world[:3,3])
    strokes = paint.read_paths(context, target, source.project_damage.gravel_drawing)
    strokes = [np.ascontiguousarray(path @ world[:3,:3].T + world[:3,3]) for path in strokes]
    scale = 1.0 / max(source.project_damage.global_scale / 100.0, 0.0001)
    up = Vector((0.0, 0.0, 1.0))
    p = core.GrassParams()
    p.shape = 1
    p.seed = (s.seed + source.project_damage.seed) % 2147483647
    p.up_x, p.up_y, p.up_z = up.x, up.y, up.z
    p.slope_limit = s.slope_limit
    p.base_density = s.base_density * scale * scale
    p.edge_density = s.edge_density * scale * scale
    p.edge_radius = s.edge_radius / scale
    p.edge_angle = s.edge_angle
    p.cavity_density = s.cavity_density * scale * scale
    p.cavity_radius = s.cavity_radius / scale
    p.stroke_mode = 0 if s.stroke_mode == 'OVERLAY' else 1
    p.stroke_density = s.stroke_density * scale * scale if strokes else 0.0
    p.stroke_radius = s.stroke_radius / scale
    p.max_shards = max(1, int(s.max_stones * (preview_quality.percent(source.project_damage) / 100.0)))
    p.stone_min = min(s.size_min, s.size_max) * s.master_scale / scale
    p.stone_max = max(s.size_min, s.size_max) * s.master_scale / scale
    p.stone_skew = s.size_skew
    p.flatness = s.flatness
    p.angularity = s.angularity
    p.embed = s.embed
    p.sorting = s.sorting
    p.pile_height = s.pile_height / scale
    r = yield from bg.native(core,'scatter_grass',points, polys, strokes, p, **surface_guidance.kwargs(core))
    from .scatter_ui import subdivide_rocks
    sp,sf,suv,variation,height=subdivide_rocks(r['points'],r['polys'][:,:3],r['uvs'],r['variation'],r['tip'],s.rock_subdivision)
    inv = np.asarray(to_local, dtype=np.float64)
    sp = sp @ inv[:3,:3].T + inv[:3,3]
    name = source.name + '_project_damage_gravel'
    if len(sf) == 0:
        reports.append(({'WARNING'}, 'Scatter Gravel: no stones (check Slope Limit, the densities, or the strokes in Overlay mode)'))
        return None
                                              
    tris = sf[:, :3]
    mesh = bpy.data.meshes.new(name)
    mesh.vertices.add(len(sp))
    mesh.vertices.foreach_set('co', sp.astype(np.float32).ravel())
    nf = len(tris)
    mesh.loops.add(nf * 3)
    mesh.polygons.add(nf)
    mesh.loops.foreach_set('vertex_index', tris.astype(np.int32).ravel())
    mesh.polygons.foreach_set('loop_start', np.arange(0, nf * 3, 3, dtype=np.int32))
    try:
        mesh.polygons.foreach_set('loop_total', np.full(nf, 3, dtype=np.int32))
    except (AttributeError, RuntimeError):
        pass
    mesh.update(calc_edges=True)
    uv = mesh.uv_layers.new(name='UVMap')
    uv.data.foreach_set('uv', suv[tris.ravel()].astype(np.float32).ravel())
    a1 = mesh.attributes.new('gravel_variation', 'FLOAT', 'POINT')
    a1.data.foreach_set('value', variation.astype(np.float32))
    a2 = mesh.attributes.new('gravel_height', 'FLOAT', 'POINT')
    a2.data.foreach_set('value', height.astype(np.float32))
    mat = s.material
    if mat is None:
        from . import material_defaults
        mat = material_defaults.get('gravel')
    mesh.materials.append(mat)
    from . import generated_assets
    obj = generated_assets.place(context, source, target, 'gravel', mesh)
    reports.append(({'INFO'}, 'Scatter Gravel: %d stones' % r['shards']))
    return obj


# раздел «Gravel Scatter»
class PROJECT_DAMAGE_PT_gravel(bpy.types.Panel):
    bl_label = 'Gravel Scatter'
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "⚒ Project Damage"
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        obj = context.active_object
        return obj is not None and obj.type == 'MESH'

    def draw(self, context):
        layout=self.layout;obj=context.active_object;s=obj.project_damage_gravel
        box=layout.box();material_controls.centered_heading(box,'General Settings',icon='SETTINGS')
        column=box.column(align=True)
        column.prop(s,'rock_subdivision');column.prop(s,'seed',text='Seed');column.prop(s,'master_scale',text='Master Scale')
        box=layout.box();material_controls.centered_heading(box,'Distribution and Density',icon='PARTICLES')
        column=box.column(align=True)
        column.prop(s,'slope_limit');column.prop(s,'base_density');column.prop(s,'edge_density')
        column.prop(s,'edge_radius');column.prop(s,'edge_angle');column.prop(s,'cavity_density');column.prop(s,'cavity_radius')
        box=layout.box();material_controls.centered_heading(box,'Draw',icon='GREASEPENCIL')
        column=box.column(align=True)
        from . import paint
        paint.drawing_row(column,obj.project_damage,'GRAVEL',1)
        row=column.row(align=True);row.prop(s,'stroke_mode',expand=True)
        column.prop(s,'stroke_density');column.prop(s,'stroke_radius')
        box=layout.box();material_controls.centered_heading(box,'Rocks',icon='MESH_ICOSPHERE')
        column=box.column(align=True)
        material_controls.inline(column,s,'material',text='Rocks')
        column.prop(s,'size_min');column.prop(s,'size_max');column.prop(s,'size_skew')
        column.prop(s,'flatness');column.prop(s,'angularity');column.prop(s,'embed')
        column.prop(s,'sorting');column.prop(s,'pile_height');column.prop(s,'max_stones')


_SETTINGS = ProjectDamageGravelSettings
_PANEL = PROJECT_DAMAGE_PT_gravel

# подключаем свойства и действия этого модуля к Blender
def register():
    from . import live, generated_assets
    for name, prop in _SETTINGS.__annotations__.items():
        prop.keywords['update'] = generated_assets.material_changed if name == 'material' else live.touch_prop
    bpy.utils.register_class(_SETTINGS)
    bpy.types.Object.project_damage_gravel = bpy.props.PointerProperty(type=_SETTINGS)

# убираем подключения модуля при выключении аддона
def unregister():
    if hasattr(bpy.types.Object, 'project_damage_gravel'):
        del bpy.types.Object.project_damage_gravel
    if _SETTINGS.is_registered:
        bpy.utils.unregister_class(_SETTINGS)

                                                
from . import surface_guidance
