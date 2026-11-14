# листы ткани: источник, столкновения, настройки и создание геометрии
from . import quality_ui
# ProjectDamage cloth sheets - GPL-3.0-or-later; see LICENSE.
#
# Generate Cloth Sheets: torn cloth / tarp sheets (core: cloth_sheets.cpp).
#   Source    Around Empties   - crumpled sheets and clumps near the empties
#             Along Curve      - draped over the curve (a rope / beam) or
#                                crumpled along it
#             Hanging from Curve - curtains, swags and corner drapes pinned to
#                                the curve (a beam, a rope, a rail)
#             The curve: a Curve object (Bezier / Poly / NURBS) or a Grease
#             Pencil object (every stroke).
#   Colliders a collection: every mesh in it is a collider the cloth settles
#             on (floors, rubble, furniture). Empty: a floor under the lowest
#             source point.
#   Damage    ragged, eaten-away edges and holes (clean: no loose bits).
# Result: one object "PD_ClothSheets_<source>" in world space, with a Solidify
# (Diameter = cloth thickness) and an optional smoothing; UV map "UVMap"
# (metres of cloth: tiling fabric textures keep their scale); point attributes
# cloth_sheet (random per sheet) and cloth_edge (1 at torn / cut edges) for
# colour variation and dirt.
from . import ui_image_hints
import bpy
from pathlib import Path
import numpy as np
from mathutils import Vector
from mathutils.geometry import interpolate_bezier

from . import core_loader

_building=False

def _changed(self,context):
    from . import cloth_live
    cloth_live.changed(self,context)

def _material_changed(self,context):
    if _building:return
    obj=self.result
    if obj is not None and _is_sheet(obj):
        material=self.material or default_material(self.sheet_type)
        if material is not None:
            obj.data.materials.clear();obj.data.materials.append(material)



MODES = (('EMPTIES', 'Around Empties', 'Crumpled sheets and clumps near the empties of a collection'),
         ('CURVE', 'Along Curve', 'Draped over the curve (a rope / beam) or crumpled along it'),
         ('HANGING', 'Hanging from Curve', 'Curtains, swags and corner drapes pinned to the curve'))
TYPES = (('MIXED', 'Mixed', 'A mix of the types the source allows'),
         ('CURTAIN', 'Curtain', 'Top edge pinned along the curve, gathered: scallops between the pins'),
         ('SWAG', 'Swag', 'Pinned at the top corners (and maybe the middle): deep swags'),
         ('CORNER', 'Corner', 'Hung by one corner and a side point: a diagonal drape'),
         ('DRAPED', 'Draped', 'Laid over the curve: folds over and hangs both sides'),
         ('CRUMPLED', 'Crumpled', 'Dropped and collapsed into a heap'))
MODE_INDEX = {'EMPTIES': 0, 'CURVE': 1, 'HANGING': 2}
TYPE_INDEX = {'MIXED': 0, 'CURTAIN': 1, 'SWAG': 2, 'CORNER': 3, 'DRAPED': 4, 'CRUMPLED': 5}


def default_material(sheet_type):
    """Append the preset material; an explicit UI override takes precedence."""
    name = 'Curtain Material' if sheet_type == 'CURTAIN' else 'Cloth Default Shader'
    material = bpy.data.materials.get(name)
    if material is not None:
        return material
    library = Path(__file__).resolve().parent / 'blends' / 'cloth_materials.blend'
    if not library.is_file():
        return None
    with bpy.data.libraries.load(str(library), link=False) as (source, target):
        if name not in source.materials:
            return None
        target.materials = [name]
    return target.materials[0]


def owner_of(obj):
    if obj is None:return None
    if obj.get('project_damage_cloth_asset'):return obj
    owner = obj.get('project_damage_cloth_owner')
    if isinstance(owner,bpy.types.Object) and owner.get('project_damage_cloth_asset'):return owner
                                                                          
    for scene in bpy.data.scenes:
        settings=getattr(scene,'project_damage_cloth',None)
        if settings is None:continue
        source=settings.curve if settings.source!='EMPTIES' else next((o for o in settings.empties.all_objects if o.type=='EMPTY'),None) if settings.empties else None
        name=settings.curve.name if settings.source!='EMPTIES' and settings.curve else settings.empties.name if settings.empties else ''
        result=bpy.data.objects.get('PD_ClothSheets_'+name)
        if source is not None and result is not None and obj in (source,result):return source
    return None


def settings(context):
    owner=owner_of(context.active_object)
    return owner.project_damage_cloth if owner is not None and owner.get('project_damage_cloth_asset') else context.scene.project_damage_cloth


def _poll_curve(self, obj):
    return obj.type in {'CURVE', 'GREASEPENCIL', 'GPENCIL'}


def _get_amount(settings):
    return settings.get('amount', 2 if settings.source == 'HANGING' else 6)


def _set_amount(settings, value):
    settings['amount'] = value


def _result_name(settings):
    source = settings.empties if settings.source == 'EMPTIES' else settings.curve
    return 'PD_ClothSheets_' + source.name if source else 'PD_ClothSheets'


def _is_sheet(obj):
    return (obj is not None and obj.type == 'MESH' and
            obj.data.attributes.get('cloth_sheet') is not None and
            obj.data.attributes.get('cloth_edge') is not None)


def _result(settings, owner, active, *, recover=False):
    def owned(obj):
        if not _is_sheet(obj):
            return False
        tagged = obj.get('project_damage_cloth_owner')
        return tagged == owner if tagged else obj.name == _result_name(settings)

                                                                         
    if isinstance(settings.id_data, bpy.types.Object) and owned(settings.result):
        return settings.result
    if owned(active):
        return active
    candidate = bpy.data.objects.get(_result_name(settings))
    if owned(candidate):
        return candidate
    if recover and owner is not None:
        return next((obj for obj in bpy.data.objects if _is_sheet(obj) and
                     obj.get('project_damage_cloth_owner') == owner), None)
    return None


# здесь хранятся значения, которые сохраняются вместе с объектом
class ProjectDamageClothSettings(bpy.types.PropertyGroup):
    live_update:bpy.props.BoolProperty(name='Live Update',default=True,update=_changed,description='Automatically update cloth after settings, the assigned curve/empties or colliders change. Disable to use Update Cloth Sheets manually')
    source: bpy.props.EnumProperty(name='Source', items=MODES, default='HANGING')
    empties: bpy.props.PointerProperty(name='Empties', type=bpy.types.Collection, description='Sheets gather near every empty in this collection')
    curve: bpy.props.PointerProperty(name='Curve', type=bpy.types.Object, poll=_poll_curve, description='A Bezier Curve or Grease Pencil source. Move Cloth Hanging Curve to reposition hanging cloth; edit its Bezier points/handles to change the rail shape')
    sheet_type: bpy.props.EnumProperty(name='Type', items=TYPES, default='MIXED')
    amount: bpy.props.IntProperty(name='Amount', default=2, min=1, max=200, get=_get_amount, set=_set_amount)
    result: bpy.props.PointerProperty(name='Generated Cloth Sheets', type=bpy.types.Object, options={'HIDDEN'})
    noisiness: bpy.props.FloatProperty(name='Noisiness', default=0.5, min=0.0, max=2.0, description='Crumple and fine wrinkles')
    gravity: bpy.props.FloatProperty(name='Gravity', default=1.0, min=0.0, max=3.0, description='x 9.81 m/s2: how strongly it sags and settles')
    scale: bpy.props.FloatProperty(name='Scale', default=1.2, min=0.05, soft_max=10.0, subtype='DISTANCE', description='Sheet size (the longer side, +/- 30 %)')
    radius: bpy.props.FloatProperty(name='Radius', default=1.0, min=0.0, soft_max=20.0, subtype='DISTANCE', description='Spawn radius around the empties / off the curve')
    diameter: bpy.props.FloatProperty(name='Diameter', default=0.004, min=0.0002, soft_max=0.05, subtype='DISTANCE', description='Cloth thickness: the Solidify and the collision offset')
    clumps: bpy.props.IntProperty(name='Clumps', default=2, min=0, max=50, description='Crumpled sheets gather into this many heaps (0 = scattered)')
    clump_tightness: bpy.props.FloatProperty(name='Clump Tightness', default=0.6, min=0.0, max=1.0)
    stiffness: bpy.props.FloatProperty(name='Stiffness', default=0.25, min=0.0, max=1.0, description='0 silk .. 1 stiff canvas / tarp')
    friction: bpy.props.FloatProperty(name='Friction', default=0.6, min=0.0, max=1.0)
    curve_radius: bpy.props.FloatProperty(name='Curve Radius', default=0.03, min=0.001, soft_max=0.5, subtype='DISTANCE', description='The curve as a rope / beam for Draped')
    pin_spacing: bpy.props.FloatProperty(name='Pin Spacing', default=0.35, min=0.02, soft_max=2.0, subtype='DISTANCE', description='Curtain: distance between the pins along the curve')
    detail: bpy.props.FloatProperty(name='Detail', default=0.04, min=0.008, soft_max=0.2, subtype='DISTANCE', description='Simulation cell size: smaller = finer folds, slower')
    quality: bpy.props.IntProperty(name='Quality', default=240, min=30, max=2000, description='Simulation steps (1/120 s each)')
    colliders: bpy.props.PointerProperty(name='Colliders', type=bpy.types.Collection, description='Every mesh in this collection is a collider the cloth settles on')
    damaged: bpy.props.BoolProperty(name='Damaged Ends', default=True)
    tear: bpy.props.FloatProperty(name='Tear', default=0.5, min=0.0, max=3.0, description='Ragged, eaten-away edges (deepest along the hem)')
    holes: bpy.props.FloatProperty(name='Holes', default=0.4, min=0.0, max=10.0, description='Holes per m2')
    wind: bpy.props.FloatProperty(name='Wind', default=0.3, min=0.0, max=3.0, description='Wind pressure on the cloth while it settles: irregular billows and folds (0 = still air)')
    wind_direction: bpy.props.FloatVectorProperty(name='Wind Direction', default=(1.0, 0.3, 0.0), subtype='DIRECTION', size=3)
    turbulence: bpy.props.FloatProperty(name='Turbulence', default=0.6, min=0.0, max=2.0, description='Gusts: how much the wind wanders in space and time')
    self_collision: bpy.props.BoolProperty(name='Self Collision', default=True, description='Cloth never passes through cloth: curtains layer instead of overlapping (off = faster)')
    self_distance: bpy.props.FloatProperty(name='Self Distance', default=0.01, min=0.001, soft_max=0.1, subtype='DISTANCE', description='The gap kept between cloth layers')
    smooth: bpy.props.BoolProperty(name='Smooth', default=True, description='Subdivision on top of the solved mesh')
    seed: bpy.props.IntProperty(name='Seed', default=0, min=0)
    material: bpy.props.PointerProperty(name='Material', type=bpy.types.Material)


def _settle_supported_sheets(result, triangles, thickness, layer_gap, stack):
    """Seat each connected patch on the first finite support below it.
    A previously seated patch may support another one in a pile."""
    from mathutils.bvhtree import BVHTree
    from mathutils import Vector
    points = result['points'].copy()
    faces = result['polys']
    if not len(faces):
        return result
    surface = BVHTree.FromPolygons(triangles.tolist(), np.arange(len(triangles)).reshape(-1,3).tolist(), all_triangles=True)
                                                                       
                                                                     
    parents = list(range(len(points)))
    def root(i):
        while i != parents[i]:
            parents[i] = parents[parents[i]]
            i = parents[i]
        return i
    for row in faces.tolist():
        first = root(row[0])
        for v in row[1:]:
            parents[root(v)] = first
    components = {}
    for v in np.unique(faces):
        components.setdefault(root(int(v)), []).append(int(v))
    face_groups = {}
    for i, row in enumerate(faces):
        face_groups.setdefault(root(int(row[0])), []).append(i)
    keep = []
    supports = [(surface, thickness)]
    down = Vector((0,0,-1))
    for component, vertices in sorted(components.items(), key=lambda item:points[item[1],2].min()):
        drop = float('inf')
        for tree, gap in supports:
            for v in vertices:
                origin = Vector(points[v]); origin.z += gap
                hit,normal,_,distance = tree.ray_cast(origin,down)
                if hit is not None and abs(normal.z)>0.05:
                    drop = min(drop, distance-gap-gap/abs(normal.z))
        if not np.isfinite(drop):
            continue
                                                                                   
        points[vertices,2] -= max(-thickness,drop)
        rows = face_groups[component]
        keep.extend(rows)
        if stack:
            remap = {v:i for i,v in enumerate(vertices)}
            polys = [[remap[v] for v in dict.fromkeys(row)] for row in faces[rows].tolist()]
            supports.append((BVHTree.FromPolygons(points[vertices].tolist(),polys),max(thickness,layer_gap)))
    kept_faces = faces[sorted(keep)]
    vertices, indices = np.unique(kept_faces, return_inverse=True)
    return dict(result, points=points[vertices], polys=indices.reshape(-1,4), uvs=result['uvs'][vertices],
                edge=result['edge'][vertices], sheet=result['sheet'][vertices], sheets=len(np.unique(result['sheet'][vertices])))


def _ensure_hanging_bezier(s,owner):
    curve=s.curve
    if s.source!='HANGING' or owner is None or owner.get('project_damage_cloth_asset')!='cloth_hanging' or curve!=owner or curve.type!='CURVE':return
    if len(curve.data.splines)!=1:return
    spline=curve.data.splines[0]
    if spline.type=='POLY' and len(spline.points)==2 and not spline.use_cyclic_u:
        coordinates=[p.co[:3] for p in spline.points]
        curve.data.splines.remove(spline)
        spline=curve.data.splines.new('BEZIER');spline.bezier_points.add(1)
        for point,co in zip(spline.bezier_points,coordinates):point.co=co;point.handle_left_type=point.handle_right_type='AUTO'
        spline.resolution_u=16
        curve.data.use_path=False


def _generate(context,s,owner,report):
    _ensure_hanging_bezier(s,owner)
    core = core_loader.load()
    if core is None or not hasattr(core, 'cloth_sheets'):
        report({'ERROR'}, 'Cloth Sheets: the native core has no cloth generator (rebuild the core)')
        return {'CANCELLED'}
    old = _result(s, owner, context.active_object, recover=True)
    deps = context.evaluated_depsgraph_get()
                              
    points = []
    if s.source == 'EMPTIES':
        if s.empties is None:
            report({'ERROR'}, 'Cloth Sheets: pick the Empties collection')
            return {'CANCELLED'}
        for o in s.empties.all_objects:
            if o.type == 'EMPTY':
                q = o.matrix_world.translation
                points.append((q.x, q.y, q.z))
        if not points:
            report({'ERROR'}, 'Cloth Sheets: the Empties collection has no empties')
            return {'CANCELLED'}
    curves = []
    if s.source in {'CURVE', 'HANGING'}:
        if s.curve is None:
            report({'ERROR'}, 'Cloth Sheets: pick the Curve')
            return {'CANCELLED'}
        ob = s.curve
        M = ob.matrix_world
        if ob.type == 'CURVE':
            for sp in ob.data.splines:
                line = []
                if sp.type == 'BEZIER':
                    bp = sp.bezier_points
                    n = len(bp)
                    segs = n if sp.use_cyclic_u else n - 1
                    res = max(2, sp.resolution_u)
                    for k in range(segs):
                        a, b = bp[k], bp[(k + 1) % n]
                        pts = interpolate_bezier(a.co, a.handle_right, b.handle_left, b.co, res + 1)
                        line += [M @ p for p in (pts if k == 0 else pts[1:])]
                else:
                    line = [M @ Vector(p.co[:3]) for p in sp.points]
                    if sp.use_cyclic_u and line:
                        line.append(line[0])
                if len(line) >= 2:
                    curves.append(np.ascontiguousarray([[p.x, p.y, p.z] for p in line], dtype=np.float64))
        elif ob.type == 'GREASEPENCIL':                                  
            for layer in ob.data.layers:
                frame = layer.current_frame()
                if frame is None or frame.drawing is None:
                    continue
                for stroke in frame.drawing.strokes:
                    line = [M @ Vector(p.position) for p in stroke.points]
                    if len(line) >= 2:
                        curves.append(np.ascontiguousarray([[p.x, p.y, p.z] for p in line], dtype=np.float64))
        else:                                                                     
            for layer in ob.data.layers:
                frame = layer.active_frame
                if frame is None:
                    continue
                for stroke in frame.strokes:
                    line = [M @ Vector(p.co) for p in stroke.points]
                    if len(line) >= 2:
                        curves.append(np.ascontiguousarray([[p.x, p.y, p.z] for p in line], dtype=np.float64))
        if not curves:
            report({'ERROR'}, 'Cloth Sheets: the curve has no points')
            return {'CANCELLED'}
                                                                         
    tris = []
    candidates = s.colliders.all_objects if s.colliders is not None else ()
    if s.colliders is None and s.source != 'HANGING':
                                                                           
        visible = [o for o in context.visible_objects if o.type == 'MESH' and not _is_sheet(o)]
        candidates = visible if len(visible) == 1 else ()
    if candidates:
        for o in candidates:
            if o.type != 'MESH' or o == old or (s.source != 'HANGING' and _is_sheet(o)):
                continue
            ev = o.evaluated_get(deps)
            me = ev.to_mesh()
            try:
                me.calc_loop_triangles()
                n = len(me.vertices)
                co = np.empty(n * 3, dtype=np.float64)
                me.vertices.foreach_get('co', co)
                lt = np.empty(len(me.loop_triangles) * 3, dtype=np.int64)
                me.loop_triangles.foreach_get('vertices', lt)
            finally:
                ev.to_mesh_clear()
            W = np.array(ev.matrix_world)
            co = co.reshape(-1, 3) @ W[:3, :3].T + W[:3, 3]
            tris.append(co[lt])
    collider = np.ascontiguousarray(np.vstack(tris) if tris else np.zeros((0, 3)), dtype=np.float64)
                    
    p = core.ClothParams()
    p.seed = s.seed
    p.mode = MODE_INDEX[s.source]
    p.type = TYPE_INDEX[s.sheet_type]
    p.amount = s.amount
    p.scale = s.scale
    p.radius = s.radius
    p.thickness = s.diameter
    p.gravity = s.gravity
    p.stiffness = s.stiffness
    p.noisiness = s.noisiness
    p.clumps = s.clumps
    p.clump_tightness = s.clump_tightness
    p.cell_size = s.detail
    p.steps = s.quality
    p.friction = s.friction
    p.curve_radius = s.curve_radius
    p.pin_spacing = s.pin_spacing
    p.damaged = s.damaged
    p.tear = s.tear
    p.holes = s.holes
    p.up_x, p.up_y, p.up_z = 0.0, 0.0, 1.0
    p.wind = s.wind
    p.wind_x, p.wind_y, p.wind_z = s.wind_direction
    p.turbulence = s.turbulence
    p.self_collision = s.self_collision
    p.self_distance = s.self_distance
    pts_in = np.ascontiguousarray(np.array(points, dtype=np.float64).reshape(-1, 3))
    r = core.cloth_sheets(pts_in, curves, collider, p)
    ground = s.source != 'HANGING' and len(collider) > 0
    if ground:
        r = _settle_supported_sheets(r, collider, s.diameter, s.self_distance, s.self_collision)
    cp, cf, cuv = r['points'], r['polys'], r['uvs']
                                                                      
    name = _result_name(s)
    if len(cf) == 0 and ground and old is not None:
                                                                                   
        mesh = old.data.copy()
        mesh.clear_geometry()
        for name in ('cloth_sheet', 'cloth_edge'):
            if mesh.attributes.get(name) is None:
                mesh.attributes.new(name, 'FLOAT', 'POINT')
        previous = old.data
        old.data = mesh
        if previous.users == 0:
            bpy.data.meshes.remove(previous)
        s.result = old
        report({'INFO'}, 'No cloth landed on the collider')
        return {'FINISHED'}
    if len(cf) == 0:
        report({'WARNING'}, 'Cloth Sheets: nothing was made (check the source, Amount and Scale)')
        return {'CANCELLED'}
    is_tri = cf[:, 2] == cf[:, 3]
    sizes = np.where(is_tri, 3, 4).astype(np.int32)
    loops = np.concatenate([cf[k, :sizes[k]] for k in range(len(cf))]).astype(np.int32)
    starts = np.concatenate([[0], np.cumsum(sizes)[:-1]]).astype(np.int32)
    mesh = bpy.data.meshes.new(name)
    mesh.vertices.add(len(cp))
    mesh.vertices.foreach_set('co', cp.astype(np.float32).ravel())
    mesh.loops.add(len(loops))
    mesh.polygons.add(len(cf))
    mesh.loops.foreach_set('vertex_index', loops)
    mesh.polygons.foreach_set('loop_start', starts)
    mesh.polygons.foreach_set('loop_total', sizes)
    mesh.update(calc_edges=True)
    uv = mesh.uv_layers.new(name='UVMap')
    uv.data.foreach_set('uv', cuv[loops].astype(np.float32).ravel())
    for attr_name, key in (('cloth_sheet', 'sheet'), ('cloth_edge', 'edge')):
        a = mesh.attributes.new(attr_name, 'FLOAT', 'POINT')
        a.data.foreach_set('value', r[key].astype(np.float32))
    mesh.shade_smooth()
    mat = s.material or default_material(s.sheet_type)
    if mat is None:
        mat = bpy.data.materials.get('PD_Cloth') or bpy.data.materials.new('PD_Cloth')
        mat.diffuse_color = (0.62, 0.58, 0.48, 1.0)
        mat.roughness = 0.9
    mesh.materials.append(mat)
    if old is None:
        obj = bpy.data.objects.new(name, mesh)
        context.scene.collection.objects.link(obj)
    else:
                                                                      
                                                               
        obj = old
        mesh.transform(obj.matrix_world.inverted_safe())
        previous = obj.data
        obj.data = mesh
        if previous.users == 0:
            bpy.data.meshes.remove(previous)
    if owner is not None:
        obj['project_damage_cloth_owner'] = owner
    s.result = obj
    sol = obj.modifiers.get('Thickness')
    if sol is None or sol.type != 'SOLIDIFY':
        sol = obj.modifiers.new('Thickness', 'SOLIDIFY')
    sol.thickness = s.diameter
    sol.offset = 0.0
    sub = obj.modifiers.get('Smooth')
    if s.smooth:
        if sub is None or sub.type != 'SUBSURF':
            sub = obj.modifiers.new('Smooth', 'SUBSURF')
        sub.levels = 1
        sub.render_levels = 1
    elif sub is not None and sub.type == 'SUBSURF':
        obj.modifiers.remove(sub)
    report({'INFO'}, 'Cloth Sheets: %d sheets, %d faces' % (r['sheets'], len(cf)))
    return {'FINISHED'}


def generate(context,s,owner,report=None):
    global _building
    if _building:return {'CANCELLED'}
    if report is None:report=lambda level,message:print('ProjectDamage Cloth:',message)
    _building=True
    try:
        result=_generate(context,s,owner,report)
        if result=={'FINISHED'}:
            from . import cloth_live
            cloth_live.generated(s,owner,context.scene)
        return result
    finally:_building=False


class PROJECT_DAMAGE_OT_cloth_sheets(bpy.types.Operator):
    bl_idname = 'project_damage.cloth_sheets'
    bl_label = 'Generate Cloth Sheets'
    bl_description = 'Simulate torn cloth sheets from the empties / curve onto the colliders; replaces the previous result of this source'
    bl_options = {'REGISTER', 'UNDO'}

    def execute(self, context):
        return generate(context,settings(context),owner_of(context.active_object),self.report)


# раздел «Cloth Sheets»
class PROJECT_DAMAGE_PT_cloth_sheets(bpy.types.Panel):
    bl_label = 'Cloth Sheets'
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "⚒ Project Damage"
    bl_options = {'DEFAULT_CLOSED'}

    def draw(self, context):
        layout = self.layout
        s = settings(context)
        from .material_controls import centered_heading
        box = layout.box()
        centered_heading(box, 'Quality', 'PREFERENCES')
        column = box.column(align=True)
        quality_ui.draw(column, s, 'detail')
        column.prop(s, 'quality')
        column.prop(s, 'smooth')
        column.prop(s, 'seed')
        column = layout.column()
        owner = owner_of(context.active_object)
        if owner is None:ui_image_hints.prop(column,s, 'source')
        if s.source == 'EMPTIES':
            ui_image_hints.prop(column,s, 'empties')
        else:
            ui_image_hints.prop(column,s, 'curve')
            if s.source=='HANGING':
                column.label(text='Move Cloth Hanging Curve',icon='INFO')
                column.label(text='to reposition the cloth.')
        column.prop(s, 'sheet_type')
        column.prop(s, 'colliders')
        existing = _result(s, owner, context.active_object)
        live=layout.row(align=True);live.scale_y=1.3
        live.prop(s,'live_update',text='Live Update',toggle=True,icon='FILE_REFRESH')
        action = layout.row()
        action.scale_y = 1.4
        action.operator('project_damage.cloth_sheets',
                        text='Update Cloth Sheets' if existing else 'Generate Cloth Sheets',
                        icon='FILE_REFRESH' if existing else 'MOD_CLOTH')
        from .material_controls import centered_heading
        box = layout.box()
        centered_heading(box, 'Sheets', 'MOD_CLOTH')
        column = box.column(align=True)
        column.prop(s, 'amount')
        column.prop(s, 'scale')
        column.prop(s, 'radius')
        column.prop(s, 'diameter')
        column.prop(s, 'noisiness')
        column.prop(s, 'gravity')
        column.prop(s, 'stiffness')
        column.prop(s, 'friction')
        box = layout.box()
        header = box.row()
        header.alignment = 'CENTER'
        header.prop(s, 'self_collision')
        column = box.column(align=True)
        column.enabled = s.self_collision
        column.prop(s, 'self_distance')
        box = layout.box()
        centered_heading(box, 'Wind', 'FORCE_WIND')
        column = box.column(align=True)
        column.prop(s, 'wind')
        column.prop(s, 'wind_direction')
        column.prop(s, 'turbulence')
        box = layout.box()
        centered_heading(box, 'Clumps', 'GROUP')
        column = box.column(align=True)
        column.prop(s, 'clumps')
        column.prop(s, 'clump_tightness')
        box = layout.box()
        centered_heading(box, 'Curve', 'CURVE_DATA')
        column = box.column(align=True)
        column.prop(s, 'curve_radius')
        column.prop(s, 'pin_spacing')
        box = layout.box()
        header = box.row()
        header.alignment = 'CENTER'
        header.prop(s, 'damaged')
        column = box.column(align=True)
        column.enabled = s.damaged
        column.prop(s, 'tear')
        column.prop(s, 'holes')
        from .material_library import draw_property
        draw_property(layout,s,'material',text='Cloth Material')


for _name,_definition in ProjectDamageClothSettings.__annotations__.items():
    if _name!='result':_definition.keywords['update']=_material_changed if _name=='material' else _changed

_CLASSES = (ProjectDamageClothSettings, PROJECT_DAMAGE_OT_cloth_sheets)


# подключаем свойства и действия этого модуля к Blender
def register():
    for cls in _CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.Object.project_damage_cloth = bpy.props.PointerProperty(type=ProjectDamageClothSettings)
    bpy.types.Scene.project_damage_cloth = bpy.props.PointerProperty(type=ProjectDamageClothSettings)
    from . import cloth_live
    cloth_live.register()


# убираем подключения модуля при выключении аддона
def unregister():
    from . import cloth_live
    cloth_live.unregister()
    del bpy.types.Object.project_damage_cloth
    del bpy.types.Scene.project_damage_cloth
    for cls in reversed(_CLASSES):
        bpy.utils.unregister_class(cls)
