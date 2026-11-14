# настройки старого варианта проёма с кирпичами
# ProjectDamage wall hole with bricks - GPL-3.0-or-later; see LICENSE.
#
# A breach through a plastered brick wall whose THICKNESS is masonry (core:
# wall_hole.cpp). The hole follows the brick joints, steps back wythe by
# wythe, the joints round it are raked and the plaster is knocked off both
# faces; the removed bricks lie in a heap at the foot of the wall.
#   Sites          empties of a collection, else the 3D cursor
#   Brick Models   a collection of your own brick objects (this file): they
#                  replace the rubble AND the rim bricks, each fitted to the
#                  generated brick bounds (longest side = length, shortest =
#                  height); without it the rubble is plain brick boxes
# Result faces use the Brick Damage classes (break / seams / render), so its
# materials apply; the bricks go to "<object>_wall_hole_bricks".
from . import output_collections
import bpy
import numpy as np
from mathutils import Vector
from . import live, ownership


# здесь хранятся значения, которые сохраняются вместе с объектом
class ProjectDamageWallHoleSettings(bpy.types.PropertyGroup):
    enable: bpy.props.BoolProperty(name='Wall Hole', default=False, description='A breach through the wall with the masonry of its thickness exposed')
    sites: bpy.props.PointerProperty(name='Sites', type=bpy.types.Collection, description='Every empty is a hole (dropped on the nearest face). Empty: the 3D cursor')
    seed: bpy.props.IntProperty(name='Seed', default=0, min=0)
    hole_width: bpy.props.FloatProperty(name='Width (cm)', default=60.0, min=5.0, soft_max=300.0)
    hole_height: bpy.props.FloatProperty(name='Height (cm)', default=130.0, min=5.0, soft_max=400.0)
    hole_lift: bpy.props.FloatProperty(name='Lift (cm)', default=0.0, soft_min=-200.0, soft_max=200.0, description='Opening centre above the site')
    jagged: bpy.props.FloatProperty(name='Jagged', default=0.6, min=0.0, max=1.5)
    edge_ragged: bpy.props.FloatProperty(name='Ragged Edge', default=0.6, min=0.0, max=3.0, description='Random margin per brick on the outline: a ragged, not comb-like edge')
    depth_variation: bpy.props.FloatProperty(name='Step Back', default=0.35, min=0.0, max=1.0, description='Each wythe gets its own outline: the edge steps back into the wall')
    brick_length: bpy.props.FloatProperty(name='Brick Length (cm)', default=21.5, min=1.0, soft_max=60.0)
    brick_height: bpy.props.FloatProperty(name='Brick Height (cm)', default=6.5, min=1.0, soft_max=30.0)
    brick_width: bpy.props.FloatProperty(name='Brick Width (cm)', default=10.25, min=1.0, soft_max=40.0)
    mortar: bpy.props.FloatProperty(name='Mortar (cm)', default=1.0, min=0.0, soft_max=5.0)
    bond: bpy.props.EnumProperty(name='Bond', items=(('0', 'Running', 'Stretchers, half-brick shift per course'), ('1', 'Header Courses', 'Every second course headers across the wall')), default='0')
    joint_depth: bpy.props.FloatProperty(name='Joint Depth (cm)', default=0.8, min=0.0, soft_max=4.0, description='Raked joints round the opening')
    plaster_thickness: bpy.props.FloatProperty(name='Plaster (cm)', default=1.5, min=0.0, soft_max=6.0)
    plaster_band: bpy.props.FloatProperty(name='Plaster Band (cm)', default=8.0, min=0.0, soft_max=60.0, description='How far the plaster is knocked off beyond the bricks')
    broken_chance: bpy.props.FloatProperty(name='Broken Bricks', default=0.35, min=0.0, max=1.0)
    rubble_enable: bpy.props.BoolProperty(name='Rubble', default=True)
    rubble_share: bpy.props.FloatProperty(name='Rubble Share', default=0.6, min=0.0, max=1.0, description='Share of the removed bricks lying at the foot of the wall')
    rubble_spread: bpy.props.FloatProperty(name='Rubble Spread', default=1.0, min=0.1, max=3.0)
    rubble_max: bpy.props.IntProperty(name='Rubble Max', default=300, min=0, max=5000)
    brick_collection: bpy.props.PointerProperty(name='Brick Models', type=bpy.types.Collection, description='Your brick objects: they replace the rubble and the rim bricks, fitted to the generated brick bounds')


def fill(context, obj, core, params, factor):
    """Fill params.wall_hole (core units = object units x factor)."""
    s = getattr(obj, 'project_damage_wallhole', None)
    w = params.wall_hole
    w.enable = False
    if s is None or not obj.project_damage.wallhole_enable or obj.project_damage.all_off:
        return
    inv = obj.matrix_world.inverted_safe()
    sites = []
    if s.sites is not None:
        for e in sorted(s.sites.all_objects, key=lambda o: o.name):
            if e.type == 'EMPTY':
                q = (inv @ e.matrix_world.translation) * factor
                sites += [q.x, q.y, q.z]
    if not sites:
        q = (inv @ context.scene.cursor.location) * factor
        sites = [q.x, q.y, q.z]
    w.enable = True
    w.sites = sites
    w.seed = s.seed
    w.hole_width = s.hole_width
    w.hole_height = s.hole_height
    w.hole_lift = s.hole_lift
    w.jagged = s.jagged
    w.edge_ragged = s.edge_ragged
    w.depth_variation = s.depth_variation
    w.brick_length = s.brick_length
    w.brick_height = s.brick_height
    w.brick_width = s.brick_width
    w.mortar = s.mortar
    w.bond = int(s.bond)
    w.joint_depth = s.joint_depth
    w.plaster_thickness = s.plaster_thickness
    w.plaster_band = s.plaster_band
    w.broken_chance = s.broken_chance
    w.custom_bricks = s.brick_collection is not None
    w.rubble_enable = s.rubble_enable
    w.rubble_share = s.rubble_share
    w.rubble_spread = s.rubble_spread
    w.rubble_max = s.rubble_max
    up = (inv.to_3x3() @ Vector((0.0, 0.0, 1.0))).normalized()
    w.up_x, w.up_y, w.up_z = up.x, up.y, up.z


HASH_FIELDS = ('enable', 'seed', 'hole_width', 'hole_height', 'hole_lift', 'jagged', 'edge_ragged', 'depth_variation',
               'brick_length', 'brick_height', 'brick_width', 'mortar', 'bond', 'joint_depth', 'plaster_thickness',
               'plaster_band', 'broken_chance', 'custom_bricks', 'rubble_enable', 'rubble_share', 'rubble_spread',
               'rubble_max', 'up_x', 'up_y', 'up_z')


def after_build(context, obj, out, factor):
    """The rubble (and, with Brick Models, the rim bricks) as one object."""
    name = obj.name + '_wall_hole_bricks'
    uid=obj.get('project_damage_uid')
    old=next((o for o in bpy.data.objects if uid and o.get('project_damage_wallhole_of')==uid),None)
    wh = out.get('wall_hole') if hasattr(out, 'get') else None
    s = getattr(obj, 'project_damage_wallhole', None)
    rim = np.asarray(wh['rim_frame']) if wh is not None else np.zeros((0, 12))
    rub = np.asarray(wh['rubble_frame']) if wh is not None else np.zeros((0, 12))
    if s is None or not obj.project_damage.wallhole_enable or obj.project_damage.all_off or (len(rim) == 0 and len(rub) == 0):
        if old is not None:
            mesh = old.data
            bpy.data.objects.remove(old, do_unlink=True)
            if mesh is not None and mesh.users == 0:
                bpy.data.meshes.remove(mesh)
        return None
    frames = np.vstack([rim.reshape(-1, 12), rub.reshape(-1, 12)]) / factor
    keys = np.concatenate([np.asarray(wh['rim_key']).reshape(-1), np.asarray(wh['rubble_key']).reshape(-1)]).astype(np.int64)
    cuts = np.vstack([np.asarray(wh['rim_cut']).reshape(-1, 8), np.zeros((len(rub), 8))])
    collection = output_collections.destination(context,obj)
    if s.brick_collection is not None:
        from . import brick3d
        new = brick3d.place_bricks({'rebar_brick_frame': frames, 'rebar_brick_key': keys, 'rebar_brick_cut': cuts}, obj,
                                   model_collection=s.brick_collection, collection=collection, name=name + '_tmp')
    else:
                                                              
        corners = [(x, y, z) for x in (-.5, .5) for y in (-.5, .5) for z in (-.5, .5)]
        quads = ((0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3))
        verts, faces = [], []
        for row in frames:
            M = np.array(row[:9]).reshape(3, 3).T
            c = np.array(row[9:12])
            o = len(verts)
            for q in corners:
                verts.append(M @ np.array(q) + c)
            for qd in quads:
                faces.append([o + i for i in qd])
        mesh = bpy.data.meshes.new(name + '_tmp')
        mesh.from_pydata([tuple(v) for v in verts], [], faces)
        from .material_defaults import get
        mat = obj.project_damage.brick_front_material or get('brick')
        mesh.materials.append(mat)
        new = bpy.data.objects.new(name + '_tmp', mesh)
        collection.objects.link(new)
        new.matrix_world = obj.matrix_world
    if new is None:
        return None
    new['project_damage_wallhole_of']=uid
    ownership.bind(obj,new)
    if not new.data.materials:
        from .material_defaults import get
        new.data.materials.append(obj.project_damage.brick_front_material or get('brick'))
    if old is not None:
        old_mesh = old.data
        old.data = new.data
        bpy.data.objects.remove(new, do_unlink=True)
        if old_mesh is not None and old_mesh.users == 0:
            bpy.data.meshes.remove(old_mesh)
        ownership.bind(obj,old)
        output_collections.link_object(context,obj,old)
        return old
    new.name = name
    new.data.name = name
    return new


class PROJECT_DAMAGE_OT_wall_hole(bpy.types.Operator):
    bl_idname = 'project_damage.wall_hole'
    bl_label = 'Wall Hole'
    bl_description = 'Breach the active wall: masonry inside its thickness, raked joints, knocked-off plaster, rubble'
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        obj = ownership.context_source(context)
        return context.mode=='OBJECT' and obj is not None and obj.type == 'MESH'

    def execute(self, context):
        obj = ownership.context_source(context)
        obj.project_damage.wallhole_enable = True
        return bpy.ops.project_damage.update()


# раздел «Wall Hole (Bricks)»
class PROJECT_DAMAGE_PT_wall_hole(bpy.types.Panel):
    bl_label = 'Wall Hole (Bricks)'
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "⚒ Project Damage"
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        obj = context.active_object
        return obj is not None and obj.type == 'MESH'

    def draw(self, context):
        layout = self.layout
        s = context.active_object.project_damage_wallhole
        column = layout.column()
        column.prop(s, 'enable')
        column.prop(s, 'sites')
        column.prop(s, 'seed')
        box = layout.box()
        box.label(text='Opening')
        column = box.column(align=True)
        column.prop(s, 'hole_width')
        column.prop(s, 'hole_height')
        column.prop(s, 'hole_lift')
        column.prop(s, 'jagged')
        column.prop(s, 'edge_ragged')
        column.prop(s, 'depth_variation')
        box = layout.box()
        box.label(text='Masonry')
        column = box.column(align=True)
        column.prop(s, 'brick_length')
        column.prop(s, 'brick_height')
        column.prop(s, 'brick_width')
        column.prop(s, 'mortar')
        column.prop(s, 'bond')
        column.prop(s, 'joint_depth')
        column.prop(s, 'broken_chance')
        box = layout.box()
        box.label(text='Plaster')
        column = box.column(align=True)
        column.prop(s, 'plaster_thickness')
        column.prop(s, 'plaster_band')
        box = layout.box()
        box.label(text='Bricks')
        column = box.column(align=True)
        column.prop(s, 'brick_collection')
        column.prop(s, 'rubble_enable')
        column.prop(s, 'rubble_share')
        column.prop(s, 'rubble_spread')
        column.prop(s, 'rubble_max')
        row = layout.row()
        row.scale_y = 1.4
        row.operator('project_damage.wall_hole', icon='MOD_BUILD')


_CLASSES = (ProjectDamageWallHoleSettings, PROJECT_DAMAGE_OT_wall_hole)


# подключаем свойства и действия этого модуля к Blender
def register():
    for prop in ProjectDamageWallHoleSettings.__annotations__.values():
        prop.keywords['update']=live.touch_prop
    for cls in _CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.Object.project_damage_wallhole = bpy.props.PointerProperty(type=ProjectDamageWallHoleSettings)


# убираем подключения модуля при выключении аддона
def unregister():
    del bpy.types.Object.project_damage_wallhole
    for cls in reversed(_CLASSES):
        bpy.utils.unregister_class(cls)
