# готовим исходный меш к расчёту и повторно используем неизменённые данные
# ProjectDamage for Blender - non-destructive input preparation (Blender side).
# GPL-3.0-or-later; see LICENSE.
"""Reads the source (authored or evaluated with modifiers) into a numpy
snapshot, asks mesh_prep for core-ready arrays, and maps the result back:
materials, UVs, vertex groups and boolean face attributes (Avoid).

The source object and its mesh are never written to. The original code path
in build.py stays in charge whenever `plan()` returns None - a closed,
edge-manifold triangle/quad mesh read without modifiers.

Register AFTER panels (the panel is a child of OBJECT_PT_project_damage):
    _MODULES = (core_loader, props, breaker, build, manual, presets, panels,
                input_prep, gizmo, live)
"""
import bpy
from . import ownership
import bmesh
import numpy as np
from mathutils import Vector
from mathutils.bvhtree import BVHTree
from mathutils.interpolate import poly_3d_calc

from . import live, mesh_prep
from .mesh_io import CLASS_NAMES, read_mesh


# здесь хранятся значения, которые сохраняются вместе с объектом
class ProjectDamageInputProps(bpy.types.PropertyGroup):
    mode: bpy.props.EnumProperty(
        name="Mesh Preparation", default='AUTO', update=live.touch_prop,
        items=[('AUTO', "Auto", "Closed solids take the original path; n-gons are triangulated, "
                                "split vertices welded, open or non-manifold surfaces handled in surface mode"),
               ('CLOSED', "Closed Only", "Only triangulate n-gons; anything that is not a closed solid "
                                         "passes through unchanged (the original behaviour)"),
               ('SURFACE', "Surface", "Always prepare the mesh using Open Mesh Method, then build a temporary "
                                      "solid, apply damage, and remove helper faces")])
    holes: bpy.props.EnumProperty(name='Open Mesh Method',default='AUTO',update=live.touch_prop,
        items=(('AUTO','Auto','Cap small holes to make solid volumes; thicken sheet-like surfaces'),
               ('FILL','Fill Holes','Cap boundary loops when a valid closed volume can be made'),
               ('SHELL','Thin Shell','Keep the surface as a thin temporary shell instead of filling holes')))
    thickness_logic: bpy.props.EnumProperty(name="Thickness Logic",default="IMPROVED",update=live.touch_prop,items=(("IMPROVED","Improved","Local glass thickness and smoothed shell offsets"),("LEGACY","Legacy","Previous glass thickness and raw-normal shell offsets")))
    use_modifiers: bpy.props.BoolProperty(
        name="Use Modifiers", default=False, update=live.touch_prop,
        description="Damage the evaluated mesh (with the modifier stack) instead of the base mesh")
    weld_distance: bpy.props.FloatProperty(
        name="Weld Distance (cm)", default=0.001, min=0.0, soft_max=1.0, precision=4,
        update=live.touch_prop, description="Vertices closer than this are merged before repair")
    surface_thickness: bpy.props.FloatProperty(
        name="Surface Thickness (cm)", default=1.0, min=0.001, soft_max=50.0, update=live.touch_prop,
        description="Thickness of the temporary solid behind an open surface; damage deeper than this cuts through")
    surface_flip: bpy.props.BoolProperty(
        name="Damage Back Side", default=False, update=live.touch_prop,
        description="Thicken toward the front instead, so the damage is cut into the surface's back side")
    restore_ngons: bpy.props.BoolProperty(
        name="Restore N-gons", default=True, update=live.touch_prop,
        description="Dissolve untouched triangulated n-gons back into the original faces")
    transfer_data: bpy.props.BoolProperty(
        name="Transfer Groups and Attributes", default=True, update=live.touch_prop,
        description="Copy vertex groups and boolean face attributes (Avoid) onto the prepared result")


class _Defaults:
    mode = 'AUTO'
    use_modifiers = False
    holes = 'AUTO'
    thickness_logic = 'IMPROVED'
    weld_distance = 0.001
    surface_thickness = 1.0
    surface_flip = False
    restore_ngons = True
    transfer_data = True


def settings(obj):
    """The object's preparation settings; defaults when this module's
    property group is not registered."""
    return getattr(obj, "project_damage_input", None) or _Defaults()


class Snapshot:
    """Plain numpy copy of the mesh ProjectDamage reads - valid after to_mesh_clear()."""
    evaluated = False
    points = loop_verts = loop_start = loop_total = None
    loop_tris = None
    uv = None                                      
    uv_active = None
    material_index = None
    materials = ()
    groups = None                                          
    group_names = None                             
    face_bools = None                                      


def _snapshot(me, obj, evaluated):
    s = Snapshot()
    s.evaluated = evaluated
    n_v, n_p, n_l = len(me.vertices), len(me.polygons), len(me.loops)
    if n_v == 0 or n_p == 0:
        raise ValueError("the mesh has no geometry")
    co = np.empty(n_v * 3,np.float32)
    me.vertices.foreach_get("co", co)
    s.points = co.reshape(-1, 3).astype(np.float64)
    s.loop_start = np.empty(n_p, dtype=np.int32)
    s.loop_total = np.empty(n_p, dtype=np.int32)
    me.polygons.foreach_get("loop_start", s.loop_start)
    me.polygons.foreach_get("loop_total", s.loop_total)
    s.loop_verts = np.empty(n_l, dtype=np.int32)
    me.loops.foreach_get("vertex_index", s.loop_verts)
    if np.any(s.loop_total > 4):
                                                                          
                                   
        me.calc_loop_triangles()
        n_t = len(me.loop_triangles)
        tp = np.empty(n_t, dtype=np.int32)
        tl = np.empty(n_t * 3, dtype=np.int32)
        me.loop_triangles.foreach_get("polygon_index", tp)
        me.loop_triangles.foreach_get("loops", tl)
        tl = tl.reshape(-1, 3)
        ng = s.loop_total[tp] > 4
        s.loop_tris = [(int(p), int(a), int(b), int(c)) for p, (a, b, c) in zip(tp[ng], tl[ng])]
    s.uv = {}
    for layer in me.uv_layers:
        flat = np.empty(n_l * 2,np.float32)
        layer.uv.foreach_get("vector",flat)
        s.uv[layer.name] = flat.reshape(-1, 2)
    s.uv_active = me.uv_layers.active.name if me.uv_layers.active else None
    s.material_index = np.empty(n_p, dtype=np.int32)
    me.polygons.foreach_get("material_index", s.material_index)
    from .material_ids import original as original_material
    s.materials = tuple(original_material(obj.material_slots[i].material
                        if i < len(obj.material_slots) and obj.material_slots[i].link == 'OBJECT'
                        else mat) for i, mat in enumerate(me.materials))
                                                                          
    s.group_names = {vg.index: vg.name for vg in obj.vertex_groups}
    s.groups = {i: np.zeros(n_v) for i in s.group_names}
    if s.groups:
        for i, v in enumerate(me.vertices):
            for g in v.groups:
                if g.group in s.groups:
                    s.groups[g.group][i] = g.weight
    s.face_bools = {}
    for attr in me.attributes:
        if attr.domain == 'FACE' and attr.data_type == 'BOOLEAN' and not attr.name.startswith("."):
            arr = np.empty(n_p, dtype=bool)
            attr.data.foreach_get("value", arr)
            s.face_bools[attr.name] = arr
    from . import result_transfer
    result_transfer.capture(me, s)
    from .damage_shading import loop_normals
    s.corner_normals = loop_normals(me) if obj.project_damage.damage_shading else None
    return s


def read_source(context, obj, use_modifiers):
    if not use_modifiers:
        return _snapshot(obj.data, obj, False)
    depsgraph = context.evaluated_depsgraph_get()
    ev = obj.evaluated_get(depsgraph)
    me = ev.to_mesh(preserve_all_data_layers=True, depsgraph=depsgraph)
    try:
        return _snapshot(me, obj, True)
    finally:
        ev.to_mesh_clear()


_PREP_CACHE = {}                                                                          
_PREP_CACHE_MAX = 8
_PREP_CACHE_BYTES = 256 * 1024 * 1024


def clear_prep_cache(obj=None):
    if obj is None:
        _PREP_CACHE.clear()
    else:
        _PREP_CACHE.pop(obj.as_pointer(), None)


def _prep_fingerprint(snap, factor, backend, kwargs):
    import hashlib, struct
    h = hashlib.blake2b(digest_size=16)
    h.update(backend.__name__.encode())
    h.update(struct.pack('<d', float(factor)))
    for array in (snap.points, snap.loop_verts, snap.loop_start, snap.loop_total, kwargs.get('loop_tris')):
        if array is None:
            h.update(b'none;');continue
        a = np.ascontiguousarray(array)
        h.update(str((a.dtype.str, a.shape)).encode())
        if a.size:
            h.update(memoryview(a).cast('B'))
    h.update(repr(sorted((k, v) for k, v in kwargs.items() if k != 'loop_tris')).encode())
    return h.digest()


def _prepare_cached(obj,snap,factor,backend,**kwargs):
    from . import background_compute as bg
    return bg.immediate(_prepare_steps(obj,snap,factor,backend,**kwargs))

def _prepare_steps(obj, snap, factor, backend, **kwargs):
    from . import background_compute as bg
    import copy
    from . import incremental
    enabled = obj is not None and obj.project_damage.perf_prep_cache and incremental.cache_enabled()
    slot = obj.as_pointer() if obj is not None else 0
    if not enabled:
        _PREP_CACHE.pop(slot, None)
        return (yield from bg.task('Preparing mesh',backend.prepare,snap.points * factor, snap.loop_verts, snap.loop_start, snap.loop_total, **kwargs))
    key = _prep_fingerprint(snap, factor, backend, kwargs)
    hit = _PREP_CACHE.pop(slot, None)
    if hit is not None and hit[0] == key:
        _PREP_CACHE[slot] = hit             
        prepared = hit[1]
        if getattr(bpy.context.window_manager, 'project_damage_core_profiler', False):
            print('[pd-prep-cache] HIT: '+obj.name, flush=True)
    else:
        if getattr(bpy.context.window_manager, 'project_damage_core_profiler', False):
            print('[pd-prep-cache] MISS: '+obj.name, flush=True)
        prepared = (yield from bg.task('Preparing mesh',backend.prepare,snap.points * factor, snap.loop_verts, snap.loop_start, snap.loop_total, **kwargs))
        size = sum(a.nbytes for a in vars(prepared).values() if isinstance(a, np.ndarray))
        if size <= _PREP_CACHE_BYTES:
            _PREP_CACHE[slot] = (key, prepared, size)
            while (len(_PREP_CACHE) > _PREP_CACHE_MAX or
                   sum(v[2] for v in _PREP_CACHE.values()) > _PREP_CACHE_BYTES):
                _PREP_CACHE.pop(next(iter(_PREP_CACHE)))
                                                                                
    result = copy.copy(prepared)
    result.notes = list(prepared.notes)
    return result


                                                                           
                                                                          
                                                                            
                                                                            
                                                                           
                                                                         
FRACTURE_SHELL_MARGIN = 1.35                                                   
FISSURE_SHELL_MARGIN = 1.1                             


def fracture_shell_thickness(pd, thickness):
    if pd is None or not getattr(pd, 'fract_enable', False) or getattr(pd, 'fract_through_enable', False):
        return thickness
    if getattr(pd, 'all_off', False):
        return thickness
    clamp = lambda v: min(4.0, max(0.1, v))
    scale = clamp(pd.fract_scale / 100.0) * clamp(pd.global_scale / 100.0)
    mult = max(0.01, pd.fract_depth_mult / 100.0)
    deepest = 0.0
    for n in (1, 2, 3):
        if getattr(pd, 'fract_l%d_amount' % n, 0) > 0 or (n == 1 and pd.fract_mode == 'PAINT'):
            deepest = max(deepest, getattr(pd, 'fract_l%d_depth' % n) * mult,
                          getattr(pd, 'fract_l%d_chip_depth' % n) if getattr(pd, 'fract_l%d_chips' % n) else 0.0)
    if pd.fract_type == 'GLASS' or str(getattr(pd, 'fract_asset', '')).endswith('glass'):
        deepest = max(deepest, pd.fract_glass_depth * mult)
    if deepest <= 0.0:
        deepest = pd.fract_l1_depth * mult                                              
    deepest *= scale * FRACTURE_SHELL_MARGIN
    cap = pd.fract_thickness_cap / 100.0 if pd.fract_recipe == 'IMPROVED' else 0.0
    need = deepest / cap if cap > 0.0 else deepest
    return max(thickness, need)


def fissure_shell_thickness(pd, thickness):
    if pd is None or not getattr(pd, 'fissure_enable', False) or getattr(pd, 'all_off', False):
        return thickness
    scale = min(4.0, max(0.01, pd.fissure_master_scale / 100.0)) * min(4.0, max(0.1, pd.global_scale / 100.0))
    deepest = pd.fissure_depth * (1.0 + max(0.0, pd.fissure_depth_random))
    if pd.fissure_branch_enable:
        deepest = max(deepest, pd.fissure_branch_depth)
    cap = min(0.98, max(0.0, pd.fissure_thickness_cap))
    need = deepest * FISSURE_SHELL_MARGIN / (cap if cap > 0.0 else 1.0)
    if pd.fissure_slab_enable:
                                                                              
        need = max(need, 2.5 * max(pd.fissure_slab_thick_min, pd.fissure_slab_thick_max))
    return max(thickness, need * scale)


def damage_shell_thickness(pd, thickness):
    from . import ground_depth
    return ground_depth.shell_thickness(pd, fracture_shell_thickness(pd, thickness))


def object_scale(obj):
    """Mean absolute world scale - thicknesses are entered in world cm, the
    mesh is read in object space (a Plane at scale 5 needs 1/5 of it)."""
    try:
        sc = obj.matrix_world.to_scale()
        value = (abs(sc.x) + abs(sc.y) + abs(sc.z)) / 3.0
    except (AttributeError, TypeError):
        return 1.0
    return value if value > 1e-9 else 1.0


class _FabricBackend:
    """fabric_prep behind the backend interface _prepare_steps expects (a
    `__name__` for the cache key and a module-level `prepare` function, which
    the background worker can import by name)."""
    def __init__(self, base):
        from . import fabric_prep
        fast = base.__name__.rsplit('.', 1)[-1] == 'mesh_prep_fast'
        self.__name__ = 'fabric_prep+' + base.__name__
        self.prepare = fabric_prep.prepare_with_mesh_prep_fast if fast else fabric_prep.prepare_with_mesh_prep


def fabric_backend(base):
    return _FabricBackend(base)


def plan(context,obj,factor,*,solid_damage=False):
    from . import background_compute as bg
    return bg.immediate(plan_steps(context,obj,factor,solid_damage=solid_damage))

def plan_steps(context, obj, factor, *, solid_damage=False):
    from . import background_compute as bg
    """None = the original path applies unchanged. Otherwise (snapshot,
    Prepared) with core-unit points. Raises ValueError with a user message."""
    s = settings(obj)
    pd = getattr(obj, 'project_damage', None)
    ground_surface = bool(not solid_damage and pd is not None and not pd.all_off and (pd.ground_enable or pd.fissure_enable))
                                                                                  
    if pd is not None and pd.perf_mesh_preparation:
        from . import mesh_prep_fast as mesh_prep
    else:
        from . import mesh_prep
    if not solid_damage and pd is not None and pd.fabric_solid_enable:
        # Fabric Solidify: an open cloth (a shirt, a plane, a double-layer
        # garment) is closed piece by piece into a temporary shell instead of
        # being forced through mode 'CLOSED' (which passed every open cloth
        # through unchanged). Closed garments keep the original path.
        snap = read_source(context, obj, True)
        thickness = damage_shell_thickness(pd, s.surface_thickness) / object_scale(obj)
        prep = yield from _prepare_steps(obj, snap, factor, fabric_backend(mesh_prep),
                                 weld=s.weld_distance, thickness=thickness,
                                 flip_side=s.surface_flip, loop_tris=snap.loop_tris,
                                 thickness_logic=s.thickness_logic)
        return snap, prep
    if not solid_damage and pd is not None and getattr(pd, 'fabric_enable', False):
                                                                             
                                                                    
        if not s.use_modifiers:
            me = obj.data
            total = np.empty(len(me.polygons), dtype=np.int32)
            me.polygons.foreach_get("loop_total", total)
            if len(total) and not np.any(total > 4):
                return None
        snap = read_source(context, obj, s.use_modifiers)
        prep = yield from _prepare_steps(obj, snap, factor, mesh_prep,
                                 mode='CLOSED', weld=s.weld_distance, thickness=s.surface_thickness,
                                 flip_side=s.surface_flip, loop_tris=snap.loop_tris, holes=s.holes, thickness_logic=s.thickness_logic)
        return snap, prep
    if not s.use_modifiers and s.mode != 'SURFACE':
        me = obj.data
        total = np.empty(len(me.polygons), dtype=np.int32)
        me.polygons.foreach_get("loop_total", total)
        if len(total) and not np.any(total > 4):
            if s.mode == 'CLOSED' and not ground_surface:
                return None
            _points, polys = read_mesh(me)
            if (yield from bg.task('Checking source topology',mesh_prep.is_closed_manifold,polys)):
                return None
    snap = read_source(context, obj, s.use_modifiers)
    mode = 'AUTO' if ground_surface and s.mode == 'CLOSED' else s.mode
    thickness = s.surface_thickness if solid_damage else damage_shell_thickness(pd, s.surface_thickness)
    try:
        prep = yield from _prepare_steps(obj, snap, factor, mesh_prep,
                                 mode=mode, weld=s.weld_distance, thickness=thickness,
                                 flip_side=s.surface_flip, loop_tris=snap.loop_tris, holes=s.holes,
                                 thickness_logic=s.thickness_logic)
    except ValueError as exc:
        if mode not in ('AUTO', 'SURFACE') or 'closed solid' not in str(exc):
            raise
        prep = yield from _prepare_steps(obj, snap, factor, fabric_backend(mesh_prep),
                                 weld=s.weld_distance,
                                 thickness=thickness if ground_surface else thickness / object_scale(obj),
                                 flip_side=s.surface_flip, loop_tris=snap.loop_tris,
                                 thickness_logic=s.thickness_logic, detect_double_layer=False,
                                 path_label="surface", helpers=True)
        prep.notes.insert(0, "regular preparation failed (%s); per-piece fallback used" % exc)
    if s.use_modifiers:
        prep.notes.insert(0, "modifiers applied (evaluated mesh)")
    return snap, prep


def source_mesh_copy(context, obj, snap):
    from .material_ids import normalize_mesh
    if snap is not None and snap.evaluated:
        depsgraph = context.evaluated_depsgraph_get()
        mesh=bpy.data.meshes.new_from_object(obj.evaluated_get(depsgraph),
                                            preserve_all_data_layers=True,depsgraph=depsgraph)
    else:mesh=obj.data.copy()
    return normalize_mesh(mesh)


def copy_materials(mesh, snap):
    from .material_ids import original
    for mat in snap.materials:
        mesh.materials.append(original(mat))
    return len(snap.materials)


def preserved_ngons(prep,base,classes):
    return prep.ngon_groups({'base_poly':base,'tri_class':classes})

def restore_ngons(mesh, groups, performance=False):
    """Dissolve each group of result faces (an untouched triangulated n-gon)
    back into one face. UVs and attributes ride along in bmesh."""
    if performance:
        from .ngon_restore_fast import restore_ngons as optimized
        return optimized(mesh, groups)
    if not groups:
        return 0
    bm = bmesh.new()
    try:
        bm.from_mesh(mesh)
        bm.faces.ensure_lookup_table()
        sets = [[bm.faces[i] for i in faces] for faces in groups.values()]
        done = 0
        for faces in sets:
            if all(f.is_valid for f in faces):
                bmesh.ops.dissolve_faces(bm, faces=faces, use_verts=False)
                done += 1
        bm.to_mesh(mesh)
    finally:
        bm.free()
    mesh.update()
    return done


def transfer_face_bools(mesh, snap, base_poly):
    """Boolean face attributes (Avoid and friends) onto result faces that
    trace back to a source face; cut faces get False."""
    base = np.asarray(base_poly)
    traced = base >= 0
    for name, values in snap.face_bools.items():
        if name in CLASS_NAMES or mesh.attributes.get(name) is not None:
            continue
        out = np.zeros(len(mesh.polygons), dtype=bool)
        if len(base) == len(out):
            out[traced] = values[base[traced]]
        mesh.attributes.new(name, 'BOOLEAN', 'FACE').data.foreach_set("value", out)


def transfer_vertex_groups(res_obj, snap, prep, factor):
    """Every source vertex group onto the result object, interpolated from
    the nearest point of the prepared FRONT surface (helper faces excluded)."""
    if not snap.groups:
        return 0
    front = [list(row[:3] if row[2] == row[3] else row) for row, h in zip(prep.polys.tolist(), prep.helper) if not h]
    pts = prep.points / factor
    tree = BVHTree.FromPolygons([Vector(p) for p in pts], front, all_triangles=False)
    me = res_obj.data
    n = len(me.vertices)
    co = np.empty(n * 3)
    me.vertices.foreach_get("co", co)
    co = co.reshape(-1, 3)
    weights = {i: prep.point_values(w) for i, w in snap.groups.items()}
    out = {i: np.zeros(n) for i in weights}
    for vi in range(n):
        loc, _nrm, face, _dist = tree.find_nearest(Vector(co[vi]))
        if face is None:
            continue
        idx = front[face]
        bary = poly_3d_calc([Vector(pts[k]) for k in idx], loc)
        for gi, w in weights.items():
            out[gi][vi] = sum(b * w[k] for b, k in zip(bary, idx))
    for gi, values in out.items():
        name = snap.group_names[gi]
        vg = res_obj.vertex_groups.get(name) or res_obj.vertex_groups.new(name=name)
        q = np.round(np.clip(values, 0.0, 1.0) * 1000.0).astype(np.int64)                         
        for level in np.unique(q[q > 0]):
            vg.add(np.flatnonzero(q == level).tolist(), level / 1000.0, 'REPLACE')
    return len(out)


def copy_group_names(res_obj, src_obj):
    """Passthrough: the copied mesh already holds the weights (they live in
    the mesh); the group NAMES live on the object, in index order."""
    for vg in src_obj.vertex_groups:
        if res_obj.vertex_groups.get(vg.name) is None:
            res_obj.vertex_groups.new(name=vg.name)


# раздел «Mesh Preparation»
class OBJECT_PT_project_damage_input(bpy.types.Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "⚒ Project Damage"
    bl_parent_id = "OBJECT_PT_project_damage"
    bl_label = "Mesh Preparation"
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        obj = ownership.context_source(context)
        return (obj is not None and obj.type == 'MESH' and hasattr(obj, "project_damage_input")
                and not obj.get("project_damage_result_of") and not obj.get("project_damage_deposits_of") and not obj.get("project_damage_rebar_of")
                and not obj.get("project_damage_coating_of"))

    def draw(self, context):
        s = ownership.context_source(context).project_damage_input
        layout = self.layout
        column = layout.column()
        column.prop(s, "mode")
        column.prop(s, "use_modifiers")
        column.separator()

        repair = column.column()
        repair.active = s.mode == 'AUTO'
        repair.prop(s, "weld_distance")
        column.separator()

        surface = column.column()
        surface.active = s.mode != 'CLOSED'
        surface.label(text="Open / Non-manifold Surfaces")
        surface.prop(s,"holes")
        surface.prop(s, "surface_thickness")
        surface.prop(s, "surface_flip")
        column.separator()

        column.prop(s, "restore_ngons")
        column.prop(s, "thickness_logic")
        column.prop(s, "transfer_data")
        column.label(text="The source object is never modified.", icon='INFO')


_CLASSES = (ProjectDamageInputProps, OBJECT_PT_project_damage_input)


# подключаем свойства и действия этого модуля к Blender
def register():
    bpy.utils.register_class(ProjectDamageInputProps)
    bpy.types.Object.project_damage_input = bpy.props.PointerProperty(type=ProjectDamageInputProps)


# убираем подключения модуля при выключении аддона
def unregister():
    clear_prep_cache()
    del bpy.types.Object.project_damage_input
    bpy.utils.unregister_class(ProjectDamageInputProps)
