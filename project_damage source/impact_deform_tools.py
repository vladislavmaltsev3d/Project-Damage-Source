# точки удара и защищённые области для деформации
# SPDX-License-Identifier: GPL-3.0-or-later
"""Impact Deform helpers (2026-10-26).

Quick setup: Add Impact / Add Origin / Add Box create the empties (and their
collections) in one click; the new empty is selected and keeps the sidebar on
its source, so it can be moved and scaled right away.

Origin: Auto (the new default) gives every impact its own origin behind the
surface under it, so a dent always goes straight in - on a wall, a panel or
a closed object alike, with no origin empty to place. Empty and Object Origin
are the two older behaviours. Old scenes are switched to the one they used,
once, on load (deform_origin_mode is still unset in them).

Protection: a painted vertex group (1 = never moves, 0.5 = half the dent) is
the main way now; Paint Protection shows the original mesh in Weight Paint
and Done Painting goes back to the result. Protection boxes keep working for
older scenes and can still be added.

Smoothness / Noise Scale are core parameters (impact_deform.cpp); with an
older core they are skipped and the panel says so."""
import hashlib
import struct
import sys

import bpy
import numpy as np
from bpy.app.handlers import persistent

from . import live, ownership

PROTECT_GROUP = 'Impact Protection'
ORIGIN_INDEX = {'AUTO': 0, 'EMPTY': 1, 'OBJECT': 2}


def core_supports_new():
    """The loaded native core knows Smoothness / Noise Scale / own origins."""
    from . import core_loader
    core = core_loader.loaded()
    if core is None:
        return True                                          
    try:
        return hasattr(core.ImpactDeformParams(), 'smoothness')
    except Exception:
        return False


# ---------------------------------------------------------------------------
# Surface of the authored mesh (object space): BVH + whether it is open.

_SURF = {}


def _mesh_arrays(me):
    co = np.empty(len(me.vertices) * 3, dtype=np.float32)
    me.vertices.foreach_get('co', co)
    return co.reshape(-1, 3)


def surface(obj):
    """(bvh, is_open) of obj.data in object space, cached on the geometry."""
    from mathutils.bvhtree import BVHTree
    me = obj.data
    co = _mesh_arrays(me)
    key = (me.as_pointer(), len(me.vertices), len(me.polygons), len(me.loops), hash(co.tobytes()))
    hit = _SURF.get(obj.as_pointer())
    if hit is not None and hit[0] == key:
        return hit[1], hit[2]
    lv = np.empty(len(me.loops), dtype=np.int32)
    me.loops.foreach_get('vertex_index', lv)
    starts = np.empty(len(me.polygons), dtype=np.int32)
    me.polygons.foreach_get('loop_start', starts)
    totals = np.empty(len(me.polygons), dtype=np.int32)
    me.polygons.foreach_get('loop_total', totals)
    polys = [lv[s:s + t].tolist() for s, t in zip(starts.tolist(), totals.tolist())]
    bvh = BVHTree.FromPolygons(co.tolist(), polys, all_triangles=False) if polys else None
    le = np.empty(len(me.loops), dtype=np.int32)
    me.loops.foreach_get('edge_index', le)
    counts = np.bincount(le, minlength=len(me.edges)) if len(le) else np.zeros(0)
    is_open = bool((counts == 1).any())
    if len(_SURF) > 8:
        _SURF.clear()
    _SURF[obj.as_pointer()] = (key, bvh, is_open)
    return bvh, is_open


def auto_origin(bvh, is_open, c, radius, depth):
    """Origin behind the surface under an impact at `c` (object space,
    Blender units): the nearest surface point pushed back along the averaged
    surface normal by depth x radius. Open meshes (a wall plane) take the
    normal facing the impact; closed ones keep their outward normals, so an
    impact sunk into the surface still dents inward. None if no surface."""
    if bvh is None:
        return None
    loc, nrm, _idx, dist = bvh.find_nearest(c)
    if loc is None:
        return None
    n = nrm.normalized()
                                                                               
    if is_open and dist > radius * 1e-4 and (c - loc).dot(n) < 0.0:
        n = -n
                                                                          
                                                       
    total = n * 1e-6
    reach = max(1e-9, radius * 0.5)
    for _l, fn, _i, d in bvh.find_nearest_range(loc, reach):
        fn = fn.normalized()
        if is_open and fn.dot(n) < 0.0:
            fn = -fn
        if fn.dot(n) > 0.3:
            total += fn * (1.0 - d / reach)
    if total.length > 1e-12:
        n = total.normalized()
    return loc - n * (radius * depth)


                                                                             
                                                     

def fill(props, core, obj, p, factor):
    """Impact Deform inputs in object space (cm): the impacts (each with its
    own origin in Auto), the shared origin, the protection boxes, Smoothness
    and Noise Scale; a signature of all of them goes into the cache key.
    The painted protection is per input point and comes later
    (fill_protect_weights, once the input mesh is read)."""
    from mathutils import Vector
    inv = obj.matrix_world.inverted_safe()
    d = p.deform
    new_core = hasattr(d, 'smoothness')
    sig = hashlib.blake2b(digest_size=4)
    mode = props.deform_origin_mode
    if mode == 'EMPTY' and props.deform_origin is None:
        mode = 'AUTO'                                                         
    sig.update(mode.encode())
    depth = max(0.01, props.deform_origin_depth)
    surf = None
    impacts, auto_origins = [], []
    if props.deform_collection is not None:
        for e in sorted(props.deform_collection.all_objects, key=lambda o: o.name):
            if e.type != 'EMPTY' or e == obj:
                continue
            m = inv @ e.matrix_world
            c = m.translation * factor
            r = e.empty_display_size * max(abs(s) for s in e.matrix_world.to_scale()) * factor
            im = core.DeformImpact()
            im.x, im.y, im.z, im.radius = c.x, c.y, c.z, r
            im.strength = max(0.0, float(e.get('project_damage_deform_strength', 1.0)))
            sig.update(struct.pack('5d', c.x, c.y, c.z, r, im.strength))
            if mode == 'AUTO':
                if surf is None:
                    surf = surface(obj)
                o = auto_origin(surf[0], surf[1], m.translation, r / factor, depth)
                if o is not None:
                    o = o * factor
                    auto_origins.append(o)
                    if new_core:
                        im.own_origin = True
                        im.origin_x, im.origin_y, im.origin_z = o.x, o.y, o.z
                    sig.update(struct.pack('3d', o.x, o.y, o.z))
            impacts.append(im)
    d.impacts = impacts
    if mode == 'EMPTY':
        o = (inv @ props.deform_origin.matrix_world.translation) * factor
    elif mode == 'AUTO' and not new_core and auto_origins:
                                                                         
        o = sum(auto_origins, Vector()) / len(auto_origins)
    else:
        o = Vector()
    d.origin_x, d.origin_y, d.origin_z = o.x, o.y, o.z
    sig.update(struct.pack('3d', o.x, o.y, o.z))
    boxes = []
    if props.deform_protection is not None:
        for e in sorted(props.deform_protection.all_objects, key=lambda o: o.name):
            if e.type != 'EMPTY' or e == obj:
                continue
            m = inv @ e.matrix_world
            s = e.empty_display_size
            pts = [m @ Vector((x * s, y * s, z * s)) * factor for x in (-1, 1) for y in (-1, 1) for z in (-1, 1)]
            b = core.DeformBox()
            b.min_x, b.min_y, b.min_z = (min(q[i] for q in pts) for i in range(3))
            b.max_x, b.max_y, b.max_z = (max(q[i] for q in pts) for i in range(3))
            boxes.append(b)
            sig.update(struct.pack('6d', b.min_x, b.min_y, b.min_z, b.max_x, b.max_y, b.max_z))
    d.protection = boxes
    if new_core:
        d.smoothness = max(0.0, min(1.0, props.deform_smoothness))
        d.noise_scale = max(0.0, props.deform_noise_scale)
        sig.update(struct.pack('2d', d.smoothness, d.noise_scale))
    d.signature = int.from_bytes(sig.digest(), 'little') & 0x7fffffff


def fill_protect_weights(obj, params, snap, prep):
    """Painted protection: the vertex group's weights per input point
    (after Mesh Preparation, like the damage mask), folded into the
    signature. Nothing when Impact Deform is off or no group is linked."""
    d = getattr(params, 'deform', None)
    props = obj.project_damage
    if d is None or not d.enable or not hasattr(d, 'protect_weights'):
        return
    vg = obj.vertex_groups.get(props.deform_protect_group) if props.deform_protect_group else None
    if vg is None:
        d.protect_weights = []
        return
    if snap is None:
        from .mesh_io import read_mask_weights
        w = read_mask_weights(obj.data, vg.index)
    else:
        w = prep.point_values(snap.groups[vg.index])
    w = np.clip(np.asarray(w, dtype=np.float32), 0.0, 1.0)
    if props.deform_protect_invert:
        w = 1.0 - w
    d.protect_weights = w.tolist()
    h = hashlib.blake2b(digest_size=4)
    h.update(struct.pack('i', d.signature))
    h.update(w.tobytes())
    d.signature = int.from_bytes(h.digest(), 'little') & 0x7fffffff


                                                                             
                                                          

def mark_new_setup(props):
    """A setup touched in this version keeps whatever Origin it shows: write
    the (maybe default) mode so the load migration never rewrites it."""
    try:
        if props.get('deform_origin_mode') is None:
            props['deform_origin_mode'] = ORIGIN_INDEX[props.deform_origin_mode]
    except (TypeError, AttributeError, RuntimeError):
        pass                


def touch(self, context):
    mark_new_setup(self)
    live.touch_prop(self, context)


def origin_picked(self, context):
    """Picking an origin empty means: use it."""
    mark_new_setup(self)
    if self.deform_origin is not None and self.deform_origin_mode != 'EMPTY':
        self.deform_origin_mode = 'EMPTY'                           
        return
    live.touch_prop(self, context)


@persistent
def migrate_origin_mode(_dummy=None):
    """Scenes from before Auto origin keep their look: an origin empty ->
    Empty, none -> Object Origin. Raw ID properties, so no update callbacks
    and no build start here."""
    objects = getattr(bpy.data, 'objects', None)
    if objects is None:
        return False                                                          
    for obj in objects:
        if obj.type != 'MESH':
            continue
        p = getattr(obj, 'project_damage', None)
        if p is None or p.get('deform_origin_mode') is not None:
            continue
        if p.deform_collection is None and p.deform_origin is None and p.deform_protection is None and not p.deform_enable:
            continue
        try:
            p['deform_origin_mode'] = ORIGIN_INDEX['EMPTY' if p.deform_origin is not None else 'OBJECT']
        except (TypeError, AttributeError, RuntimeError):
            pass
    return True


def _deferred_migration():
    return None if migrate_origin_mode() else 0.1


                                                                             
           

def _source(context):
    src = ownership.context_source(context)
    return src if src is not None and src.type == 'MESH' else None


def _collection(context, src, prop, suffix):
    p = src.project_damage
    coll = getattr(p, prop)
    if coll is not None and coll.library is not None:
        return None
    if coll is None:
        coll = bpy.data.collections.new(src.name + ' ' + suffix)
        context.scene.collection.children.link(coll)
        setattr(p, prop, coll)
    elif coll != context.scene.collection and coll not in context.scene.collection.children_recursive:
        context.scene.collection.children.link(coll)
    return coll


def _on_surface(src, world_point, radius):
    """The nearest point of the source's surface to world_point (world). On
    an open mesh (a wall plane) it stays 1 % of the radius on the cursor's
    side, so Auto origin knows which way the hit goes."""
    bvh, is_open = surface(src)
    if bvh is None:
        return world_point
    local = src.matrix_world.inverted_safe() @ world_point
    loc, nrm, _i, dist = bvh.find_nearest(local)
    if loc is None:
        return world_point
    if is_open and dist > 1e-9:
        side = 1.0 if (local - loc).dot(nrm) >= 0.0 else -1.0
        scale = max(1e-9, max(abs(v) for v in src.matrix_world.to_scale()))
        loc = loc + nrm.normalized() * side * radius * 0.01 / scale
    return src.matrix_world @ loc


def _new_empty(context, src, coll, name, kind, size, location):
    e = bpy.data.objects.new(name, None)
    e.empty_display_type = kind
    e.empty_display_size = max(1e-4, size)
    e.location = location
    e['project_damage_paint_source'] = src                                                 
    coll.objects.link(e)
    for ob in tuple(context.selected_objects):
        ob.select_set(False)
    try:
        e.select_set(True)
        context.view_layer.objects.active = e
    except RuntimeError:
        pass                                             
    return e


def _size(src, share):
    return max(src.dimensions) * share if max(src.dimensions) > 0 else 0.25


class _SourceOp:
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return context.mode == 'OBJECT' and _source(context) is not None


# действие «Add Impact»
class PROJECT_DAMAGE_OT_deform_add_impact(_SourceOp, bpy.types.Operator):
    bl_idname = 'project_damage.deform_add_impact'
    bl_label = 'Add Impact'
    bl_description = ('Add an impact empty on the surface nearest the 3D cursor (Shift+Right-click the mesh to place '
                      'the cursor). Its size is the dent radius; Impact Points collection is made if needed')

    def execute(self, context):
        src = _source(context)
        coll = _collection(context, src, 'deform_collection', 'Impacts')
        if coll is None:
            self.report({'ERROR'}, 'Choose a local, editable Impact Points collection')
            return {'CANCELLED'}
        mark_new_setup(src.project_damage)
        src.project_damage.deform_enable = True
        radius = _size(src, 0.12)
        e = _new_empty(context, src, coll, src.name + ' Impact', 'SPHERE', radius,
                       _on_surface(src, context.scene.cursor.location.copy(), radius))
        e['project_damage_deform_strength'] = 1.0
        live._mark_dirty(src)
        self.report({'INFO'}, 'Impact added: scale it for the dent radius; Custom Properties > strength')
        return {'FINISHED'}


# действие «Add Origin»
class PROJECT_DAMAGE_OT_deform_add_origin(_SourceOp, bpy.types.Operator):
    bl_idname = 'project_damage.deform_add_origin'
    bl_label = 'Add Origin'
    bl_description = ('Find the centre of the mesh (volume centre of a closed mesh, bounding box centre otherwise), '
                      'put a Deformation Origin empty there and switch Origin to Empty. Moves the existing one')

    def execute(self, context):
        src = _source(context)
        p = src.project_damage
        mark_new_setup(p)
        centre, closed = mesh_centre(src)
        world = src.matrix_world @ centre
        e = p.deform_origin
        if e is None or e.library is not None:
            coll = src.users_collection[0] if src.users_collection else context.scene.collection
            if coll.library is not None:
                coll = context.scene.collection
            e = _new_empty(context, src, coll, src.name + ' Deform Origin', 'PLAIN_AXES', _size(src, 0.08), world)
            p.deform_origin = e
        else:
            e.location = world if e.parent is None else e.parent.matrix_world.inverted_safe() @ world
        if p.deform_origin_mode != 'EMPTY':
            p.deform_origin_mode = 'EMPTY'
        live._mark_dirty(src)
        if not closed:
            self.report({'WARNING'}, 'Open or flat mesh: the centre lies on it, so dents push sideways. Origin Auto works better here')
        return {'FINISHED'}


def mesh_centre(obj):
    """(centre, closed) in object space: the volume centroid of a closed,
    consistently oriented mesh, else the bounding box centre."""
    from mathutils import Vector
    me = obj.data
    me.calc_loop_triangles()
    co = _mesh_arrays(me).astype(np.float64)
    if not len(co):
        return Vector(), False
    lo, hi = co.min(axis=0), co.max(axis=0)
    bbox = Vector(((lo + hi) * 0.5).tolist())
    tri = np.empty(len(me.loop_triangles) * 3, dtype=np.int32)
    me.loop_triangles.foreach_get('vertices', tri)
    tri = tri.reshape(-1, 3)
    _bvh, is_open = surface(obj)
    if is_open or not len(tri):
        return bbox, False
    a, b, c = co[tri[:, 0]], co[tri[:, 1]], co[tri[:, 2]]
    vol = np.einsum('ij,ij->i', a, np.cross(b, c)) / 6.0
    total = vol.sum()
    if abs(total) < 1e-12:
        return bbox, False
    centre = ((a + b + c) / 4.0 * vol[:, None]).sum(axis=0) / total
    return Vector(centre.tolist()), True


# действие «Add Box»
class PROJECT_DAMAGE_OT_deform_add_box(_SourceOp, bpy.types.Operator):
    bl_idname = 'project_damage.deform_add_box'
    bl_label = 'Add Box'
    bl_description = ('Add a cube empty at the 3D cursor to the Protection Boxes collection (made if needed): '
                      'vertices inside never move and nothing is pushed into it')

    def execute(self, context):
        src = _source(context)
        coll = _collection(context, src, 'deform_protection', 'Protection Boxes')
        if coll is None:
            self.report({'ERROR'}, 'Choose a local, editable Protection Boxes collection')
            return {'CANCELLED'}
        mark_new_setup(src.project_damage)
        _new_empty(context, src, coll, src.name + ' Protection Box', 'CUBE', _size(src, 0.1),
                   context.scene.cursor.location.copy())
        live._mark_dirty(src)
        return {'FINISHED'}


# действие «Paint Protection»
class PROJECT_DAMAGE_OT_deform_paint_protection(bpy.types.Operator):
    bl_idname = 'project_damage.deform_paint_protection'
    bl_label = 'Paint Protection'
    bl_description = ('Show the original mesh in Weight Paint on the protection group (made if needed). '
                      'Weight 1 never moves, 0.5 moves half; press Done Painting to see the result')
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        src = _source(context)
        return src is not None and src.library is None and src.data.library is None and context.mode in {'OBJECT', 'PAINT_WEIGHT'}

    def execute(self, context):
        from . import build
        src = _source(context)
        p = src.project_damage
        mark_new_setup(p)
        vg = src.vertex_groups.get(p.deform_protect_group) if p.deform_protect_group else None
        if vg is None:
            vg = src.vertex_groups.get(PROTECT_GROUP) or src.vertex_groups.new(name=PROTECT_GROUP)
            p.deform_protect_group = vg.name
        src.vertex_groups.active_index = vg.index
        if context.mode != 'OBJECT':
            bpy.ops.object.mode_set(mode='OBJECT')
        result = build.result_of(src)
        if result is not None and result.name in context.view_layer.objects:
            result.hide_set(True)
        try:
            src.hide_set(False)
        except RuntimeError:
            self.report({'ERROR'}, 'The original mesh is not in this view layer')
            return {'CANCELLED'}
        for ob in tuple(context.selected_objects):
            ob.select_set(False)
        src.select_set(True)
        context.view_layer.objects.active = src
        bpy.ops.object.mode_set(mode='WEIGHT_PAINT')
        return {'FINISHED'}


# действие «Done Painting»
class PROJECT_DAMAGE_OT_deform_paint_done(bpy.types.Operator):
    bl_idname = 'project_damage.deform_paint_done'
    bl_label = 'Done Painting'
    bl_description = 'Leave Weight Paint, hide the original again and update the damage'
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return context.mode == 'PAINT_WEIGHT' and _source(context) is not None

    def execute(self, context):
        from . import build
        src = _source(context)
        bpy.ops.object.mode_set(mode='OBJECT')
        result = build.result_of(src)
        if result is not None and result.name in context.view_layer.objects:
            src.hide_set(True)
            result.hide_set(False)
            src.select_set(False)
            result.select_set(True)
            context.view_layer.objects.active = result
        live._mark_dirty(src)
        return {'FINISHED'}


CLASSES = (PROJECT_DAMAGE_OT_deform_add_impact, PROJECT_DAMAGE_OT_deform_add_origin,
           PROJECT_DAMAGE_OT_deform_add_box, PROJECT_DAMAGE_OT_deform_paint_protection,
           PROJECT_DAMAGE_OT_deform_paint_done)


                                                                             
                                                  

# собираем видимые настройки этого раздела
def draw(layout, context):
    from . import material_controls
    src = ownership.context_source(context)
    p = src.project_damage
    layout.use_property_split = False
    if not core_supports_new():
        warn = layout.box()
        warn.label(text='Rebuild the native core for Smoothness,', icon='ERROR')
        warn.label(text='Noise Scale, painted protection and Auto origin')

    box = layout.box()
    material_controls.centered_heading(box, 'Impact Points', icon='EMPTY_AXIS')
    row = box.row(align=True)
    row.scale_y = 1.4
    row.operator('project_damage.deform_add_impact', text='Add Impact', icon='ADD')
    box.prop(p, 'deform_collection', text='Collection')
    box.label(text='Places it at the 3D cursor, on the surface', icon='INFO')
    box.label(text='Empty size = dent radius')

    box = layout.box()
    material_controls.centered_heading(box, 'Deformation Origin', icon='ORIENTATION_GLOBAL')
    row = box.row(align=True)
    row.prop(p, 'deform_origin_mode', expand=True)
    if p.deform_origin_mode == 'AUTO':
        box.prop(p, 'deform_origin_depth', text='Origin Depth')
        box.label(text='Each impact pushes straight into the surface', icon='INFO')
    elif p.deform_origin_mode == 'EMPTY':
        row = box.row(align=True)
        row.prop(p, 'deform_origin', text='')
        row.operator('project_damage.deform_add_origin', text='Find Centre' if p.deform_origin else 'Add Origin',
                     icon='PIVOT_MEDIAN')
        if p.deform_origin is None:
            box.label(text='No empty yet: Auto is used', icon='INFO')
    else:
        box.label(text='Vertices are pushed toward the object origin', icon='INFO')

    box = layout.box()
    material_controls.centered_heading(box, 'Basic Settings', icon='MOD_MESHDEFORM')
    col = box.column(align=True)
    col.prop(p, 'deform_strength', text='Strength')
    col.prop(p, 'deform_max_deform', text='Maximum Deformation')
    col.prop(p, 'deform_smoothness', text='Smoothness', slider=True)

    box = layout.box()
    material_controls.centered_heading(box, 'Noisify', icon='MOD_NOISE')
    col = box.column(align=True)
    col.prop(p, 'deform_noise_factor', text='Noise Strength')
    sub = col.column(align=True)
    sub.active = p.deform_noise_factor > 0
    sub.prop(p, 'deform_total_amplitude', text='Noise Amplitude')
    sub.prop(p, 'deform_noise_scale', text='Noise Scale')

    box = layout.box()
    material_controls.centered_heading(box, 'Protection Zones', icon='MOD_VERTEX_WEIGHT')
    row = box.row(align=True)
    row.scale_y = 1.4
    if context.mode == 'PAINT_WEIGHT':
        row.operator('project_damage.deform_paint_done', text='Done Painting', icon='CHECKMARK')
    else:
        row.operator('project_damage.deform_paint_protection', text='Paint Protection', icon='BRUSH_DATA')
    row = box.row(align=True)
    row.prop_search(p, 'deform_protect_group', src, 'vertex_groups', text='')
    row.prop(p, 'deform_protect_invert', text='', icon='ARROW_LEFTRIGHT')
    box.label(text='Weight 1 never moves, 0.5 moves half', icon='INFO')
    sub = box.box()
    head = sub.row(align=True)
    head.label(text='Protection Boxes', icon='CUBE')
    head.operator('project_damage.deform_add_box', text='Add Box', icon='ADD')
    sub.prop(p, 'deform_protection', text='')

    box = layout.box()
    material_controls.centered_heading(box, 'Search Grid', icon='MESH_GRID')
    row = box.row(align=True)
    row.prop(p, 'deform_groups_x', text='X')
    row.prop(p, 'deform_groups_y', text='Y')
    row.prop(p, 'deform_groups_z', text='Z')


def register_handlers():
    unregister_handlers()
    bpy.app.handlers.load_post.append(migrate_origin_mode)
    if not migrate_origin_mode() and not bpy.app.timers.is_registered(_deferred_migration):
        bpy.app.timers.register(_deferred_migration, first_interval=0.1)


def unregister_handlers():
    for callback in tuple(bpy.app.handlers.load_post):
        if getattr(callback, '__name__', None) == 'migrate_origin_mode' and \
                getattr(callback, '__module__', '').endswith('impact_deform_tools'):
            bpy.app.handlers.load_post.remove(callback)
    if bpy.app.timers.is_registered(_deferred_migration):
        bpy.app.timers.unregister(_deferred_migration)
