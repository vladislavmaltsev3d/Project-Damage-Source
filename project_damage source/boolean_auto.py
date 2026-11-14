# подгоняем внутреннее заполнение к уже существующим повреждениям
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fit Boolean inner layers to damage already present on the source."""
import bpy
from mathutils import Vector
from mathutils.bvhtree import BVHTree
from . import ownership, live

SUPPORTED = {'CENTRAL', 'REBAR', 'BRICKS', 'PLANKS', 'FRAMING', 'PIPES', 'CABLES'}
HELPER = 'project_damage_auto_break_helper'


def preference_enabled(context):
    entry = context.preferences.addons.get(__package__)
    return bool(entry and getattr(entry.preferences, 'auto_setup_rebar', False))


def existing_damage_size(context, source):
    """Read the current damage result once; never create a cutter or modify it.

    Sample cut-face centres against the original skin for an actual depth.
    The source-size fallback also covers a pending first/background build.
    """
    import numpy as np
    from . import build, effect_groups
    _, _, _, _, radius, depth, _ = surface_frame(context, source)
    result = build.result_of(source)
    if result is None or not len(result.data.polygons):
        return radius, depth, False
    mesh = result.data
    ids = effect_groups._face_values(mesh, effect_groups.ATTRIBUTE, np.int32)
    if ids is None:
        return radius, depth, False
                                                                         
                                                                 
    mask = np.isin(ids, (1, 2, 3, 4, 9, 10, 11, 12, 13, 15, 16, 17,
                        18, 19, 20, 21, 22, 23, 26, 27, 32, 34))
    faces = np.flatnonzero(mask)
    if not len(faces):
        return radius, depth, False
    verts = [v.co.copy() for v in source.data.vertices]
    tree = BVHTree.FromPolygons(verts, [tuple(f.vertices) for f in source.data.polygons])
    transform = source.matrix_world.inverted_safe() @ result.matrix_world
    samples = faces[np.linspace(0, len(faces)-1, min(512, len(faces)), dtype=int)]
    distances = []
    for i in samples:
        q = transform @ mesh.polygons[int(i)].center
        _, _, _, distance = tree.find_nearest(q)
        if distance is not None and distance > 1e-7:
            distances.append(distance)
    if distances:
        depth = min(depth, float(np.percentile(distances, 75)))
                                                                          
    co = np.empty((len(mesh.vertices), 3), np.float32)
    mesh.vertices.foreach_get('co', co.ravel())
    rows = effect_groups.face_vertices(mesh, mask)
    points = co[rows]
    mat = np.asarray(transform)
    points = points @ mat[:3, :3].T + mat[:3, 3]
    spans = np.sort(np.ptp(points, axis=0))
    radius = min(radius, max(float(spans[-2])*.5, depth*2))
    return max(radius, 1e-5), max(depth, 1e-6), True


def setup_existing(context, source):
    """Size the selected inner layer and reuse the existing openings."""
    from . import boolean_break, params

    p = source.project_damage
    variant = boolean_break.variant(p)
    if variant not in SUPPORTED:
        return
    radius, depth, _ = existing_damage_size(context, source)
    unit = params.unit_factor(context) / max(.01, p.global_scale / 100.)
    size, deep = radius * unit, depth * unit
    previous = live._APPLYING
    live._APPLYING = True
    try:
        if variant in {'PIPES', 'CABLES'}:
            prefix = 'pipe' if variant == 'PIPES' else 'cable'
            scale = max(.01, getattr(p, 'boolean_' + prefix + '_scale') / 100.)
            setattr(p, prefix + '_axis', 'AUTO')
            setattr(p, prefix + '_side', 'POS' if variant == 'PIPES' else 'AUTO')
            setattr(p, prefix + '_cover', deep * .06 / scale)
            diameter = min(getattr(p, prefix + '_diameter'), size * .16 / scale, deep * .35 / scale)
            setattr(p, prefix + '_diameter', max(.05, diameter))
            if variant == 'PIPES':
                p.pipe_wall = min(p.pipe_wall, p.pipe_diameter * .25)
            return

        p.rebar_existing_damage = True
        if variant == 'CENTRAL':
            return
        p.rebar_mode = '0'
        p.rebar_fit = '2'
        p.rebar_layout = '0'
        p.rebar_axis = '0'
        p.rebar_axes_bits = 0
        p.rebar_side = '0'
        p.rebar_scale = 1.
        p.rebar_amount = 1.
        p.rebar_bars_u = True
        p.rebar_bars_v = True
        if p.rebar_fill == '2':
            if not p.is_property_set('boolean_wood_deepness'):
                p.boolean_wood_deepness = 12.
        elif p.rebar_fill == '5':
            p.boolean_wood_deepness = min(50., deep * .06)
        else:
            p.rebar_cover = deep * .06
        p.rebar_radius = max(.001, min(size * .012, deep * .05))
        p.rebar_spacing_u = p.rebar_spacing_v = size * .28
        p.rebar_embed = size * .2
        p.rebar_min_exposed = 0.
        p.rebar_reach = 0.
        p.rebar_core = False
        p.rebar_show_all = False
        p.rebar_drop_loose = True
        if variant == 'BRICKS':
            p.rebar_brick_length = size * .42
            p.rebar_brick_height = size * .16
            p.rebar_brick_depth = min(deep * .45, size * .21)
            p.rebar_mortar = size * .016
            p.rebar_missing = 0.
        elif variant == 'PLANKS':
            p.rebar_plank_width = size * .28
            p.rebar_plank_thickness = min(deep * .25, size * .035)
            p.rebar_plank_gap = size * .012
        elif variant == 'FRAMING':
            p.boolean_frame_amount = 1.
            p.rebar_skin = min(p.rebar_skin, deep * .08)
            p.rebar_stud_depth = min(deep * .65, size * .2)
            p.rebar_joist_depth = p.rebar_stud_depth
            p.rebar_stud_width = min(deep * .4, size * .08)
            p.rebar_stud_spacing = size * .45
            p.rebar_noggin_spacing = size * .55
    finally:
        live._APPLYING = previous


def helper_of(source):
    for item in source.project_damage.breakers:
        obj = item.obj
        if obj is not None and obj.get(HELPER) and obj.parent == source:
            return obj
    return None


def surface_frame(context, source, anchor=None):
    """Choose an actual surface, not a guessed +/- side of the bounding box."""
    mesh = source.data
    if not mesh.polygons:
        raise ValueError('The source has no faces')
    verts = [v.co.copy() for v in mesh.vertices]
    tree = BVHTree.FromPolygons(verts, [tuple(f.vertices) for f in mesh.polygons])
    lo = Vector(tuple(min(v[i] for v in verts) for i in range(3)))
    hi = Vector(tuple(max(v[i] for v in verts) for i in range(3)))
    ext = hi-lo
    if ext.length < 1e-8:
        raise ValueError('The source has no usable size')
    inv = source.matrix_world.inverted_safe()
    if anchor is not None:
        probe = inv @ anchor.matrix_world.translation
        point, normal, face, dist = tree.find_nearest(probe)
    else:
        cursor = inv @ context.scene.cursor.location
        point, normal, face, dist = tree.find_nearest(cursor)
                                                                              
        if dist is None or dist > ext.length*.15:
            axis = min(range(3), key=lambda a: ext[a])
            if max(ext)-min(ext) < .05*max(ext):
                axis = 1                                                   
            normal = Vector((0.,0.,0.)); normal[axis] = -1. if axis==1 else 1.
            center = (lo+hi)*.5
            point, hitnormal, face, dist = tree.ray_cast(center+normal*ext.length, -normal, ext.length*2)
            if point is None:
                f = max(mesh.polygons,key=lambda f:f.area)
                point=f.center.copy();hitnormal=f.normal.copy()
            normal=hitnormal
    if point is None or normal.length < .5:
        raise ValueError('Could not find a surface for the helper')
    normal.normalize()
    up = inv.to_3x3() @ Vector((0,0,1))
    if abs(up.normalized().dot(normal))>.95:
        up=Vector((0,1,0)) if abs(normal.y)<.95 else Vector((1,0,0))
    u=up.cross(normal).normalized();v=normal.cross(u).normalized()
    span_u=max(q.dot(u) for q in verts)-min(q.dot(u) for q in verts)
    span_v=max(q.dot(v) for q in verts)-min(q.dot(v) for q in verts)
    radius=max(1e-5,.32*min(span_u,span_v))
    eps=max(ext.length*1e-6,1e-7)
    back,_,_,distance=tree.ray_cast(point-normal*eps,-normal,ext.length*2)
    thickness=distance+eps if back is not None else max(eps*10,min(ext))
    depth=max(eps*4,min(radius*.75,thickness*.65))
                                                                       
    return point,normal,u,v,radius,depth,thickness


# действие «Auto Setup Boolean Damage»
class PROJECT_DAMAGE_OT_boolean_auto_setup(bpy.types.Operator):
    bl_idname='project_damage.boolean_auto_setup'
    bl_label='Auto Setup Boolean Damage'
    bl_description = 'Fit the inner layer to existing damage. Requires an opening from an active damage effect or an assigned breaker'
    bl_options={'REGISTER','UNDO'}
    @classmethod
    def poll(cls,context):
        from . import boolean_break
        ob=ownership.context_source(context)
        return context.mode=='OBJECT' and ob is not None and ob.type=='MESH' and boolean_break.variant(ob.project_damage) in SUPPORTED
    def execute(self,context):
        from . import build, background_compute
        source = ownership.context_source(context)
        try:
            background_compute.cancel(source)
            setup_existing(context, source)
            result, reports = build.rebuild_object(context, source)
            for kind, message in reports:
                self.report(kind, message)
            if result is None:
                self.report({'WARNING'}, 'Inner layer configured. Update after resolving the reported mesh error.')
        except (ValueError, RuntimeError) as exc:
            self.report({'ERROR'}, str(exc))
            return {'CANCELLED'}
        return {'FINISHED'}


class PROJECT_DAMAGE_OT_boolean_auto_select(bpy.types.Operator):
    bl_idname='project_damage.boolean_auto_select'
    bl_label='Select Damage Helper'
    bl_description='Select the Helper to move or scale the damage zone. Project Damage settings stay on its source'
    bl_options={'UNDO'}
    def execute(self,context):
        src=ownership.context_source(context);helper=helper_of(src) if src else None
        if helper is None:return {'CANCELLED'}
        for obj in context.selected_objects:obj.select_set(False)
        helper.hide_set(False);helper.select_set(True);context.view_layer.objects.active=helper
        return {'FINISHED'}


_CLASSES=(PROJECT_DAMAGE_OT_boolean_auto_setup,PROJECT_DAMAGE_OT_boolean_auto_select)
# подключаем свойства и действия этого модуля к Blender
def register():
    for cls in _CLASSES:
        if not getattr(cls,'is_registered',False):bpy.utils.register_class(cls)
# убираем подключения модуля при выключении аддона
def unregister():
    for cls in reversed(_CLASSES):
        if getattr(cls,'is_registered',False):bpy.utils.unregister_class(cls)
