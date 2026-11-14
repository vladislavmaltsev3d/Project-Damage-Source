# подставляем выбранную модель кирпича вместо обычного кирпичного блока
# ProjectDamage - 3D bricks (EXPERIMENTAL) - GPL-3.0-or-later; see LICENSE.
#
# With break_rebar.brick_3d on, the core lays the bricks as usual (no mortar)
# but emits no brick geometry; it returns one frame per kept brick instead:
#   res["rebar_brick_frame"]  (N, 12)  column-major 3x4: length axis x full
#                                      length, height axis x full height,
#                                      outward axis x full depth, centre -
#                                      it maps the unit cube [-0.5, 0.5]^3
#                                      onto the brick (mesh space)
#   res["rebar_brick_key"]    (N,)     stable random key per brick
#   res["rebar_brick_cut"]    (N, 8)   up to two planes (n.xyz, off) in the
#                                      unit cube, keep dot(n, u) >= off;
#                                      n = 0: no cut (a whole brick)
# This module puts a high-poly brick model from bricks_mat.blend in every
# frame: scaled to the brick on each axis, turned like it, and cut like it
# where the brick broke.

import os

import bmesh
import bpy
from mathutils import Matrix, Vector

BLEND_NAME = "bricks_mat.blend"
_cache = {}                                           


def default_blend_path():
    """bricks_mat.blend in the addon's blends folder."""
    folder = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'blends')
    models = os.path.join(folder, 'bricks_models.blend')
    return models if os.path.isfile(models) else os.path.join(folder, BLEND_NAME)


def library_path(props):
    return bpy.path.abspath(props.rebar_brick_library) if props.rebar_brick_library else default_blend_path()


def validate_library(props):
    if props.rebar_brick_collection is not None:
        if not collection_objects(props.rebar_brick_collection, props.id_data):
            raise RuntimeError('Brick Collection has no usable mesh models (source and generated outputs are excluded)')
        return
    path = default_blend_path()
    if not os.path.isfile(path):
        raise RuntimeError('Brick library not found. Select a .blend file in Brick Models')
    if not brick_models(path):
        raise RuntimeError('No brick models found. The library needs mesh objects named brick (not generated damage outputs)')


def collection_objects(collection, source=None):
    from .ownership import STAMPS
    return sorted((obj for obj in collection.all_objects
        if obj.type == 'MESH' and obj != source and len(obj.data.vertices) and len(obj.data.polygons)
        and not any(obj.get(stamp) for stamp in STAMPS)
        and not getattr(getattr(obj, 'project_damage_breaker', None), 'is_breaker', False)),
        key=lambda obj: obj.name)


def _is_model(name):
    low = name.lower()
                                                                       
    return 'brick' in low and not any(s in low for s in ('project_damage', 'edgy', 'boolean_bricks'))


class BrickModel:
    """A brick model: its mesh with the library object's rotation / scale
    baked in (a rotated, non-uniformly scaled model would otherwise need a
    sheared object matrix, which Blender cannot store), and the name."""
    def __init__(self, obj, world=False):
        self.name = obj.name
        self.data = obj.data.copy()
        self.data.name = f"{obj.name}_brick3d"
        self.data.transform((obj.matrix_world if world else obj.matrix_basis).to_3x3().to_4x4())
        self.data.update()


def brick_models(blend_path):
    """The mesh objects named *brick* in blend_path (appended once, cached),
    as BrickModel (rotation / scale baked into a copy of the mesh)."""
    mtime = os.path.getmtime(blend_path)
    hit = _cache.get(blend_path)
    if hit and hit[0] == mtime:
        try:
            if all(bpy.data.meshes.get(m.data.name) == m.data for m in hit[1]):
                return hit[1]
        except ReferenceError:
            pass                                                           
    with bpy.data.libraries.load(blend_path, link=False) as (src, dst):
        dst.objects = [n for n in src.objects if _is_model(n)]
    objs = [o for o in dst.objects if o is not None and o.type == 'MESH']
    objs.sort(key=lambda o: o.name)
    models = [BrickModel(o) for o in objs]
    for o in objs:                                                                    
        bpy.data.objects.remove(o)
    _cache[blend_path] = (mtime, models)
    return models


def model_normalizer(model):
    """Matrix mapping the model's (baked) mesh coordinates onto the unit cube
    with its longest extent on X (brick length), shortest on Y (height),
    middle on Z (depth): a signed axis permutation and a scale only, so the
    brick's object matrix is always rotation x scale. A permutation that
    would mirror the model flips the height axis instead."""
    pts = [v.co for v in model.data.vertices]
    if not pts:
        return Matrix.Identity(4)
    lo = Vector((min(p[i] for p in pts) for i in range(3)))
    hi = Vector((max(p[i] for p in pts) for i in range(3)))
    ext = hi - lo
    ctr = (hi + lo) * 0.5
    order = sorted(range(3), key=lambda i: ext[i])                               
    to_axis = {order[2]: 0, order[0]: 1, order[1]: 2}                                  
    P = Matrix.Identity(3) * 0.0
    for src_axis, dst_axis in to_axis.items():
        P[dst_axis][src_axis] = 1.0 / max(ext[src_axis], 1e-12)
    if P.determinant() < 0.0:
        P[1] = -P[1]                                                                           
    N = P.to_4x4()
    N.translation = -(P @ ctr)
    return N


def frame_matrix(row):
    """4x4 matrix from one rebar_brick_frame row (unit cube -> mesh space)."""
    c = [Vector(row[0:3]), Vector(row[3:6]), Vector(row[6:9]), Vector(row[9:12])]
    M = Matrix.Identity(4)
    for col in range(3):
        for r in range(3):
            M[r][col] = c[col][r]
    M.translation = c[3]
    return M


def _cut_mesh(model, N, planes, name):
    """A copy of the model's mesh cut by the brick's break planes (given in the
    unit cube); the cut faces are closed."""
    bm = bmesh.new()
    bm.from_mesh(model.data)
    N3 = N.to_3x3()
    for n_u, off in planes:
                                                                         
        n_m = N3.transposed() @ n_u
        off_m = off - n_u.dot(N.translation)
        L = n_m.length
        if L < 1e-12:
            continue
        n_m /= L
        off_m /= L
        geom = bm.verts[:] + bm.edges[:] + bm.faces[:]
        res = bmesh.ops.bisect_plane(bm, geom=geom, dist=1e-7, plane_co=n_m * off_m, plane_no=n_m,
                                     clear_inner=True, clear_outer=False)
        cut_edges = [e for e in res["geom_cut"] if isinstance(e, bmesh.types.BMEdge)]
        if cut_edges:
            bmesh.ops.holes_fill(bm, edges=cut_edges, sides=0)
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    for mat in model.data.materials:
        me.materials.append(mat)
    return me


def place_bricks(res, target_obj, blend_path=None, collection=None, join=True, name=None, model_collection=None):
    if model_collection is None:
        return _place_bricks(res, target_obj, blend_path, collection, join, name)
                                                                                      
    models = [BrickModel(obj, world=True) for obj in collection_objects(model_collection, target_obj)]
    try:
        if not models:
            raise RuntimeError('Brick Collection has no usable mesh models')
        return _place_bricks(res, target_obj, collection=collection, join=join, name=name, models=models)
    finally:
        for model in models:
            if model.data.users == 0:
                bpy.data.meshes.remove(model.data)


def _place_bricks(res, target_obj, blend_path=None, collection=None, join=True, name=None, models=None):
    """Put a high-poly brick model in every brick frame of `res` (the dict the
    core returned for `target_obj`). join=True: one mesh object (it replaces
    the generated ..._boolean_bricks object); False: one object per brick
    (whole bricks share the model's mesh), parented to an empty - ready for
    rigid bodies. Returns the new object (join) or the empty."""
    frames = res.get("rebar_brick_frame")
    if frames is None or len(frames) == 0:
        return None
    keys = res.get("rebar_brick_key")
    cuts = res.get("rebar_brick_cut")
    path = blend_path or default_blend_path()
    models = brick_models(path) if models is None else models
    if not models:
        raise RuntimeError(f"no brick models (objects named *brick*) in {path}")
    norms = [model_normalizer(m) for m in models]
    collection = collection or target_obj.users_collection[0]
    name = name or f"{target_obj.name}_project_damage_bricks_3d"
    bpy.context.view_layer.update()                                                            
    world = target_obj.matrix_world.copy()

    def planes_of(i):
        out = []
        if cuts is not None:
            row = cuts[i]
            for j in range(2):
                n = Vector(row[4 * j:4 * j + 3])
                if n.length > 0.0:
                    out.append((n, float(row[4 * j + 3])))
        return out

    if join:
        bm = bmesh.new()
        mats = []
        for i in range(len(frames)):
            k = int(keys[i]) % len(models) if keys is not None else i % len(models)
            model, N = models[k], norms[k]
            planes = planes_of(i)
            me = _cut_mesh(model, N, planes, "tmp_brick") if planes else model.data
                                                             
            remap = []
            for mat in me.materials:
                if mat not in mats:
                    mats.append(mat)
                remap.append(mats.index(mat))
            M = frame_matrix(frames[i]) @ N
            tmp = bmesh.new()
            tmp.from_mesh(me)
            bmesh.ops.transform(tmp, matrix=M, verts=tmp.verts)
            for f in tmp.faces:
                if remap:
                    f.material_index = remap[min(f.material_index, len(remap) - 1)]
            tmp_me = bpy.data.meshes.new("tmp_join")
            tmp.to_mesh(tmp_me)
            tmp.free()
            bm.from_mesh(tmp_me)
            bpy.data.meshes.remove(tmp_me)
            if planes:
                bpy.data.meshes.remove(me)
        out_me = bpy.data.meshes.new(name)
        bm.to_mesh(out_me)
        bm.free()
        for mat in mats:
            out_me.materials.append(mat)
        ob = bpy.data.objects.new(name, out_me)
        ob.matrix_world = world
        collection.objects.link(ob)
        return ob

    root = bpy.data.objects.new(name, None)
    root.matrix_world = world
    collection.objects.link(root)
    for i in range(len(frames)):
        k = int(keys[i]) % len(models) if keys is not None else i % len(models)
        model, N = models[k], norms[k]
        planes = planes_of(i)
        me = _cut_mesh(model, N, planes, f"{name}_{i:04d}") if planes else model.data
        ob = bpy.data.objects.new(f"{name}_{i:04d}", me)
        ob.parent = root
        ob.matrix_parent_inverse = Matrix.Identity(4)
        ob.matrix_basis = frame_matrix(frames[i]) @ N
        collection.objects.link(ob)
    bpy.context.view_layer.update()                                          
    return root
