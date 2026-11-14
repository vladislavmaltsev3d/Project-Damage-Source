# разделяем сглаживание повреждения и нетронутой поверхности
"""Optional Damage Shading v2 final-topology adapter.

No source edits. Normals are captured before preparing the source, and
recomputed after remesh / n-gon restoration rather than reusing stale rows.
SPDX-License-Identifier: GPL-3.0-or-later
"""
import numpy as np
from .mesh_io import SHADING_REGION_ATTR


def loop_normals(mesh):
    out = np.empty(len(mesh.loops) * 3, dtype=np.float32)
    mesh.corner_normals.foreach_get('vector', out)
    return out.reshape(-1, 3).astype(np.float64)


def source_normals(mesh, snap=None, prep=None):
    if prep is not None:
        loops = prep.corner_loop.copy()
        normals = snap.corner_normals
        polys, points = prep.polys, prep.points
    else:
        starts = np.empty(len(mesh.polygons), dtype=np.int32)
        totals = np.empty(len(mesh.polygons), dtype=np.int32)
        mesh.polygons.foreach_get('loop_start', starts)
        mesh.polygons.foreach_get('loop_total', totals)
        loops = starts[:, None] + np.minimum(np.arange(4), totals[:, None] - 1)
        return np.ascontiguousarray(loop_normals(mesh)[loops].reshape(-1, 12))
    out = normals[np.maximum(loops, 0)].copy()
    # Temporary back sheets and walls have no authored loop normal.
    face = np.cross(points[polys[:, 1]] - points[polys[:, 0]],
                    points[polys[:, 2]] - points[polys[:, 0]])
    face /= np.maximum(np.linalg.norm(face, axis=1), 1e-30)[:, None]
    missing = loops < 0
    out[missing] = np.broadcast_to(face[:, None], out.shape)[missing]
    tri = polys[:, 2] == polys[:, 3]
    out[tri, 3] = out[tri, 2]
    return np.ascontiguousarray(out.reshape(-1, 12))


def face_mask(mesh, name):
    out = np.zeros(len(mesh.polygons), dtype=bool)
    attr = mesh.attributes.get(name)
    if attr is not None and attr.domain == 'FACE':
        attr.data.foreach_get('value', out)
    return out


def _rim_blend(normals, loop_face, loop_vert, loop_edge, edges, face_normal, groups, base_angle):
    """Base Smooth Angle: a damage face that meets the untouched surface at
    less than `base_angle` deg blends into it - the damage corners at that rim
    take the surface's own corner normal, so the rim reads soft instead of
    cut. Steeper rims stay crisp; 0 keeps every rim crisp. Only DAMAGE
    corners change: the untouched side always keeps the source's normals.
    A damage fan (the corners at one vertex that share a normal) moves as a
    whole, otherwise the fan would split into a new crease of its own."""
    if base_angle <= 0.0 or not len(loop_edge):
        return normals
    order = np.argsort(loop_edge, kind="stable")
    e = loop_edge[order]
    pair = np.flatnonzero(e[:-1] == e[1:])
    la, lb = order[pair], order[pair + 1]
    fa, fb = loop_face[la], loop_face[lb]
    ga, gb = groups[fa], groups[fb]
    rim = ((ga == 0) & (gb == 1)) | ((ga == 1) & (gb == 0))
    if not np.any(rim):
        return normals
    cos = np.einsum("ij,ij->i", face_normal[fa], face_normal[fb])
    rim &= cos >= np.cos(np.radians(min(180.0, base_angle)))
    if not np.any(rim):
        return normals
    base_f = np.where(ga[rim] == 0, fa[rim], fb[rim])
    dmg_f = np.where(ga[rim] == 0, fb[rim], fa[rim])
    edge_verts = edges[e[pair][rim]]
    span = int(loop_vert.max()) + 1
    lookup = dict(zip((loop_face.astype(np.int64) * span + loop_vert).tolist(), range(len(loop_vert))))
    out = normals.copy()
    target = {}
    for bf, df, vs in zip(base_f.tolist(), dmg_f.tolist(), edge_verts.tolist()):
        for v in vs:
            lb_ = lookup.get(bf * span + v)
            ld_ = lookup.get(df * span + v)
            if lb_ is None or ld_ is None:
                continue
            fan = (v,) + tuple(np.round(normals[ld_], 5).tolist())
            target.setdefault(fan, []).append(normals[lb_])
    if not target:
        return normals
    dmg_loops = np.flatnonzero(groups[loop_face] == 1)
    for l in dmg_loops.tolist():
        fan = (int(loop_vert[l]),) + tuple(np.round(normals[l], 5).tolist())
        t = target.get(fan)
        if t is not None:
            n = np.sum(t, axis=0)
            out[l] = n / max(np.linalg.norm(n), 1e-30)
    return out


def apply(mesh, core, source_points, source_polys, source_corner_normals, params, factor):
    from . import background_compute as bg
    return bg.immediate(apply_steps(mesh,core,source_points,source_polys,source_corner_normals,params,factor))

def apply_steps(mesh,core,source_points,source_polys,source_corner_normals,params,factor):
    from . import background_compute as bg
    """Shade the final mesh, including restored n-gons, without changing
    geometry. Custom normals cover the whole mesh (Blender has no partial
    custom normals), but only the damage region and its rim change: the
    untouched faces get the source's own corner normals and keep the
    smooth/flat flags and sharp edges apply_phong copied from the source."""
    if not mesh.polygons:
        return False
    mesh.calc_loop_triangles()
    count = len(mesh.loop_triangles)
    verts = np.empty(count * 3, dtype=np.int32)
    loops = np.empty(count * 3, dtype=np.int32)
    faces = np.empty(count, dtype=np.int32)
    mesh.loop_triangles.foreach_get('vertices', verts)
    mesh.loop_triangles.foreach_get('loops', loops)
    mesh.loop_triangles.foreach_get('polygon_index', faces)
    tris = verts.reshape(-1, 3)
    polys = np.ascontiguousarray(np.column_stack((tris, tris[:, 2])), dtype=np.int32)
    points = np.empty(len(mesh.vertices) * 3, dtype=np.float32)
    mesh.vertices.foreach_get('co', points)
    groups = face_mask(mesh, SHADING_REGION_ATTR).astype(np.uint8)
    for part in ('front', 'back', 'wall'):
        groups[face_mask(mesh, 'project_damage_wallpaper_' + part)] = 2
    angle = (min(params.damage_smooth_angle, 25.) if params.surf_type == 1 and params.surf_amount > 0
             else params.damage_smooth_angle)
    cn = yield from bg.native(core,'compute_damage_shading',
        np.ascontiguousarray(points.reshape(-1, 3),dtype=np.float64) * factor, polys,
        np.ascontiguousarray(groups[faces]), source_points, source_polys,
        source_corner_normals, angle, params.base_smooth_angle)
    cn = np.asarray(cn).reshape(-1, 4, 3)[:, :3].reshape(-1, 3)
    if not np.isfinite(cn).all():
        raise ValueError('Damage Shading returned non-finite normals')
                                                                            
                                                                           
    normals = np.zeros((len(mesh.loops), 3))
    np.add.at(normals, loops, cn)
    normals /= np.maximum(np.linalg.norm(normals, axis=1), 1e-30)[:, None]
    n_polys, n_loops = len(mesh.polygons), len(mesh.loops)
    total = np.empty(n_polys, dtype=np.int32)
    mesh.polygons.foreach_get('loop_total', total)
    loop_face = np.repeat(np.arange(n_polys, dtype=np.int64), total)
    loop_vert = np.empty(n_loops, dtype=np.int32)
    mesh.loops.foreach_get('vertex_index', loop_vert)
    loop_edge = np.empty(n_loops, dtype=np.int32)
    mesh.loops.foreach_get('edge_index', loop_edge)
    face_normal = np.empty(n_polys * 3,np.float32)
    mesh.polygons.foreach_get('normal', face_normal)
    edges = np.empty(len(mesh.edges) * 2, dtype=np.int32)
    mesh.edges.foreach_get('vertices', edges)
    normals = yield from bg.task('Blending damage normals',_rim_blend,normals, loop_face, loop_vert, loop_edge, edges.reshape(-1, 2),
                         face_normal.reshape(-1, 3),
                         groups, float(params.base_smooth_angle))
                                                                           
                                                                        
                                                                        
                                                                           
                                      
    smooth = np.empty(n_polys, dtype=bool)
    mesh.polygons.foreach_get('use_smooth', smooth)
    smooth[groups > 0] = True
    sharp = np.empty(len(mesh.edges), dtype=bool)
    mesh.edges.foreach_get('use_edge_sharp', sharp)
    sharp[loop_edge[groups[loop_face] > 0]] = False
    mesh.polygons.foreach_set('use_smooth', smooth)
    mesh.edges.foreach_set('use_edge_sharp', sharp)
    mesh.normals_split_custom_set(normals.tolist())
    mesh.update()
    return True
