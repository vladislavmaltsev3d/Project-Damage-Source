# размещаем траву на поверхности и по рисунку
from . import preview_quality
# ProjectDamage grass scatter - GPL-3.0-or-later; see LICENSE.
#
# Scatter Grass: dry or fresh grass shards on ground objects (core:
# grass_scatter.cpp) as their own object "<object>_Grass".
#   Where      faces no steeper than Slope Limit (no grass on walls); density
#              per m2 = Base + Edges (foot of walls / kerbs, crack and chip
#              rims, within Edge Radius) + Cavity (cracks, pits, hollows -
#              the more enclosed, the denser)
#   Draw       Grease Pencil strokes: Overlay = grass ONLY along the strokes;
#              Add = the strokes add grass where it is thin, the rest stays
#   Look       Dry: thin, long, curled, many broken pieces; Fresh: wider,
#              straighter, tips lifted. UV map "UVMap" (U across, V base ->
#              tip), attributes grass_variation (0..1 per blade) and
#              grass_tip (0 base .. 1 tip) for the material
import bpy
from . import material_controls
import numpy as np
from mathutils import Vector

from . import core_loader


def _poll_gp(self, obj):
    return obj.type in {'GREASEPENCIL', 'GPENCIL'}


# здесь хранятся значения, которые сохраняются вместе с объектом
class ProjectDamageGrassSettings(bpy.types.PropertyGroup):
    master_scale: bpy.props.FloatProperty(name='Master Scale',default=100.0,min=.01,soft_max=400.0,subtype='PERCENTAGE',description='Scale blade lengths, width, pile height and influence radii together. Density changes inversely with area so scatter spacing scales too. Multiplies the global Master Scale')
    kind: bpy.props.EnumProperty(name='Grass', items=(('DRY', 'Dry', 'Straw: thin, long, curled, many broken pieces'), ('FRESH', 'Fresh', 'Green: wider, straighter, tips lifted')), default='DRY')
    slope_limit: bpy.props.FloatProperty(name='Slope Limit', default=35.0, min=0.0, max=89.0, description='Faces steeper than this (degrees) get no grass: walls stay clean')
    base_density: bpy.props.FloatProperty(name='Base Density', default=300.0, min=0.0, soft_max=5000.0, description='Blades per m2 everywhere on the ground')
    edge_density: bpy.props.FloatProperty(name='Edge Density', default=2500.0, min=0.0, soft_max=20000.0, description='Extra blades per m2 at the foot of walls and kerbs, along crack and chip rims')
    edge_radius: bpy.props.FloatProperty(name='Edge Radius', default=0.06, min=0.001, soft_max=1.0, subtype='DISTANCE')
    edge_angle: bpy.props.FloatProperty(name='Edge Angle', default=25.0, min=1.0, max=179.0, description='Corners sharper than this (degrees) count as edges')
    cavity_density: bpy.props.FloatProperty(name='Cavity Density', default=3000.0, min=0.0, soft_max=20000.0, description='Extra blades per m2 in cracks, pits and hollows')
    cavity_radius: bpy.props.FloatProperty(name='Cavity Radius', default=0.08, min=0.001, soft_max=1.0, subtype='DISTANCE', description='How far around a point is checked for enclosing walls')
    stroke_mode: bpy.props.EnumProperty(name='Draw Mode', items=(('OVERLAY', 'Overlay', 'Grass only along the strokes'), ('ADD', 'Add', 'The strokes add grass where it is thin; the rest stays')), default='ADD')
    stroke_density: bpy.props.FloatProperty(name='Stroke Density', default=2500.0, min=0.0, soft_max=20000.0)
    stroke_radius: bpy.props.FloatProperty(name='Stroke Radius', default=0.08, min=0.001, soft_max=1.0, subtype='DISTANCE')
    max_shards: bpy.props.IntProperty(name='Max Blades', default=60000, min=100, max=2000000)
    length_min: bpy.props.FloatProperty(name='Length Min', default=0.03, min=0.0005, soft_max=0.5, subtype='DISTANCE')
    length_max: bpy.props.FloatProperty(name='Length Max', default=0.14, min=0.0005, soft_max=0.5, subtype='DISTANCE')
    width: bpy.props.FloatProperty(name='Blade Width', default=0.0036, min=0.0001, soft_max=0.02, subtype='DISTANCE')
    pile_height: bpy.props.FloatProperty(name='Pile Height', default=0.006, min=0.0, soft_max=0.1, subtype='DISTANCE', description='How high blades heap where it is dense')
    segments: bpy.props.IntProperty(name='Segments', default=4, min=2, max=8)
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
                old=generated_assets.output(source,'grass')
                if old is not None:old.hide_set(True);old.hide_render=True
            return result
        bg.queue(source,'generate_grass',work,guard=lambda:bg.valid_id(target))
        return generated_assets.output(source,'grass')
    return bg.immediate(_generate_steps(context,source,target,s,reports))

def _generate_steps(context, source, target, s, reports):
    from . import background_compute as bg
    core = core_loader.load()
    if core is None or not hasattr(core, 'scatter_grass'):
        reports.append(({'ERROR'}, 'Scatter Grass: the native core has no grass scatter (rebuild the core)'))
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
    strokes = paint.read_paths(context, target, source.project_damage.grass_drawing)
    strokes = [np.ascontiguousarray(path @ world[:3,:3].T + world[:3,3]) for path in strokes]
    scale = 1.0 / max(source.project_damage.global_scale / 100.0 * s.master_scale / 100.0, 0.0001)
    up = Vector((0.0, 0.0, 1.0))
    p = core.GrassParams()
    p.seed = (s.seed + source.project_damage.seed) % 2147483647
    p.fresh = s.kind == 'FRESH'
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
    p.max_shards = max(1, int(s.max_shards * (preview_quality.percent(source.project_damage) / 100.0)))
    p.length_min = min(s.length_min, s.length_max) / scale
    p.length_max = max(s.length_min, s.length_max) / scale
    p.width = s.width / scale
    p.pile_height = s.pile_height / scale
    p.segments = s.segments
    r = yield from bg.native(core,'scatter_grass',points, polys, strokes, p, **surface_guidance.kwargs(core))
    gp_, gf, guv = r['points'], r['polys'], r['uvs']
    inv = np.asarray(to_local, dtype=np.float64)
    gp_ = gp_ @ inv[:3,:3].T + inv[:3,3]
    name = source.name + '_project_damage_grass'
    if len(gf) == 0:
        reports.append(({'WARNING'}, 'Scatter Grass: no blades (check Slope Limit, the densities, or the strokes in Overlay mode)'))
        return None
    mesh = bpy.data.meshes.new(name)
    mesh.vertices.add(len(gp_))
    mesh.vertices.foreach_set('co', gp_.astype(np.float32).ravel())
    nf = len(gf)
    mesh.loops.add(nf * 4)
    mesh.polygons.add(nf)
    mesh.loops.foreach_set('vertex_index', gf.astype(np.int32).ravel())
    mesh.polygons.foreach_set('loop_start', np.arange(0, nf * 4, 4, dtype=np.int32))
    try:
        mesh.polygons.foreach_set('loop_total', np.full(nf, 4, dtype=np.int32))
    except (AttributeError, RuntimeError):
        pass                                                             
    mesh.update(calc_edges=True)
    uv = mesh.uv_layers.new(name='UVMap')
    uv.data.foreach_set('uv', guv[gf.ravel()].astype(np.float32).ravel())
    a1 = mesh.attributes.new('grass_variation', 'FLOAT', 'POINT')
    a1.data.foreach_set('value', r['variation'].astype(np.float32))
    a2 = mesh.attributes.new('grass_tip', 'FLOAT', 'POINT')
    a2.data.foreach_set('value', r['tip'].astype(np.float32))
    mesh.shade_smooth()
    mat = s.material
    if mat is None:
        from . import material_defaults
        mat = material_defaults.get('grass_dry' if s.kind=='DRY' else 'grass_fresh')
    mesh.materials.append(mat)
    from . import generated_assets
    obj = generated_assets.place(context, source, target, 'grass', mesh)
    reports.append(({'INFO'}, 'Scatter Grass: %d blades' % r['shards']))
    return obj


# раздел «Grass Scatter»
class PROJECT_DAMAGE_PT_grass(bpy.types.Panel):
    bl_label = 'Grass Scatter'
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "⚒ Project Damage"
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        obj = context.active_object
        return obj is not None and obj.type == 'MESH'

    def draw(self, context):
        layout=self.layout;obj=context.active_object;s=obj.project_damage_grass
        row=layout.row(align=True);row.scale_y=1.5
        row.prop(s,'kind',text='',expand=True)
        box=layout.box();material_controls.centered_heading(box,'General',icon='SETTINGS')
        column=box.column(align=True);column.prop(s,'master_scale',text='Master Scale');column.prop(s,'seed',text='Seed')
        box=layout.box();material_controls.centered_heading(box,'Density and Distribution',icon='PARTICLES')
        column=box.column(align=True)
        column.prop(s,'slope_limit');column.prop(s,'base_density');column.prop(s,'edge_density')
        column.prop(s,'edge_radius');column.prop(s,'edge_angle');column.prop(s,'cavity_density');column.prop(s,'cavity_radius')
        box=layout.box();material_controls.centered_heading(box,'Draw',icon='GREASEPENCIL')
        column=box.column(align=True)
        from . import paint
        paint.drawing_row(column,obj.project_damage,'GRASS',1)
        row=column.row(align=True);row.prop(s,'stroke_mode',expand=True)
        column.prop(s,'stroke_density');column.prop(s,'stroke_radius')
        box=layout.box();material_controls.centered_heading(box,'Blades',icon='CURVES')
        column=box.column(align=True)
        material_controls.inline(column,s,'material',text='Grass Blades')
        column.prop(s,'length_min');column.prop(s,'length_max');column.prop(s,'width')
        column.prop(s,'pile_height');column.prop(s,'segments');column.prop(s,'max_shards')


_SETTINGS = ProjectDamageGrassSettings
_PANEL = PROJECT_DAMAGE_PT_grass

# подключаем свойства и действия этого модуля к Blender
def register():
    from . import live, generated_assets
    for name, prop in _SETTINGS.__annotations__.items():
        prop.keywords['update'] = generated_assets.material_changed if name == 'material' else live.touch_prop
    bpy.utils.register_class(_SETTINGS)
    bpy.types.Object.project_damage_grass = bpy.props.PointerProperty(type=_SETTINGS)

# убираем подключения модуля при выключении аддона
def unregister():
    if hasattr(bpy.types.Object, 'project_damage_grass'):
        del bpy.types.Object.project_damage_grass
    if _SETTINGS.is_registered:
        bpy.utils.unregister_class(_SETTINGS)

                                                
from . import surface_guidance
