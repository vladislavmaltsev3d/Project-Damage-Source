# переводим геометрию между Blender и расчётом вместе с UV и материалами
# ProjectDamage for Blender - worn edge damage cut into mesh objects.
# Copyright (C) 2026 Cornelius Dammrich
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <https://www.gnu.org/licenses/>.
"""Blender mesh <-> the core's arrays: the read side (points/polys, UVs,
mask weights, avoid polygons), the result mesh, the damage-class face
attributes and the result UV layer.
"""
from .blender_compat import enable_nodes
import bpy
import numpy as np
from mathutils import Vector


                                                                   
                                                                                                      
                                                      
CLASS_NAMES = ("chips_P", "L1_P", "L2_P", "L3_P", "holes_P", "buildup_P",
               "surface_P", "break_P")
BASE_CLASS = 6
DAMAGE_MATERIAL_NAMES = ("ProjectDamage_Chips", "ProjectDamage_Cracks_L1", "ProjectDamage_Cracks_L2", "ProjectDamage_Cracks_L3",
                         "ProjectDamage_Surface_Damage", "ProjectDamage_Buildup", "ProjectDamage_Base", "ProjectDamage_Break")


def read_mesh(mesh):
    """(points (N,3) float64, polys (M,4) int32) in the core's ProjectDamagePoly
    convention - c == d marks a triangle. Raises ValueError on an ngon.

    Local coordinates, deliberately: the C4D generator works in its own space
    too, and the result object carries the source's transform."""
    n_verts = len(mesh.vertices)
    n_polys = len(mesh.polygons)
    if n_verts == 0 or n_polys == 0:
        raise ValueError("the mesh has no geometry")

    points = np.empty(n_verts * 3, dtype=np.float32)
    mesh.vertices.foreach_get("co", points)

    loop_total = np.empty(n_polys, dtype=np.int32)
    mesh.polygons.foreach_get("loop_total", loop_total)
    n_ngons = int(np.count_nonzero(loop_total > 4))
    if n_ngons:
        raise ValueError(
            "%d of %d faces have more than 4 corners. ProjectDamage takes triangles and "
            "quads only - triangulate the ngons first (Face > Triangulate "
            "Faces, or a Triangulate modifier)" % (n_ngons, n_polys))
    if np.any(loop_total < 3):
        raise ValueError("the mesh has faces with fewer than 3 corners")

    loop_start = np.empty(n_polys, dtype=np.int32)
    mesh.polygons.foreach_get("loop_start", loop_start)
    loop_verts = np.empty(len(mesh.loops), dtype=np.int32)
    mesh.loops.foreach_get("vertex_index", loop_verts)

    polys = np.empty((n_polys, 4), dtype=np.int32)
    quad = loop_total == 4
    tri = ~quad
    if np.any(quad):
        polys[quad] = loop_verts[loop_start[quad][:, None] + np.arange(4)]
    if np.any(tri):
        polys[tri, :3] = loop_verts[loop_start[tri][:, None] + np.arange(3)]
        polys[tri, 3] = polys[tri, 2]
    return points.reshape(-1, 3).astype(np.float64), polys


def read_uvs(mesh, layer):
    """`layer`'s per-loop UVs -> (M,4,2) float64, in EXACTLY the same row/
    corner order `read_mesh` builds `polys` in (loop_start/loop_total, vertex
    order preserved). A triangle's unused 4th corner is written as (0, 0) -
    the same sentinel `DumpParamsJson`'s UV block uses for it
    (cpp/source/project_damage_adapter.cpp, `DumpParamsJson`) rather than NaN or a
    duplicate of the 3rd corner: the module's UV INPUT path never reads that
    slot for a triangle (`BuildBoxManifold`'s `cp.c == cp.d` skip), so the
    value is inert either way, but matching the dump's own convention keeps
    the two hosts' inputs comparable byte for byte."""
    n_polys = len(mesh.polygons)
    loop_total = np.empty(n_polys, dtype=np.int32)
    mesh.polygons.foreach_get("loop_total", loop_total)
    loop_start = np.empty(n_polys, dtype=np.int32)
    mesh.polygons.foreach_get("loop_start", loop_start)
    flat = np.empty(len(mesh.loops) * 2, dtype=np.float32)
    layer.uv.foreach_get("vector", flat)
    loop_uv = flat.reshape(-1, 2)

    uvs = np.zeros((n_polys, 4, 2), dtype=np.float64)
    quad = loop_total == 4
    tri = ~quad
    if np.any(quad):
        uvs[quad] = loop_uv[loop_start[quad][:, None] + np.arange(4)]
    if np.any(tri):
        uvs[tri, :3] = loop_uv[loop_start[tri][:, None] + np.arange(3)]
                                                               
    return uvs


def read_color_mask(mesh, name, channel='LUMA'):
    """(N,) float64 per-vertex mask from a COLOR ATTRIBUTE (vertex paint), or
    None when the mesh has no such attribute. Corner-domain colours are
    averaged per vertex. `channel`: 'LUMA' (Rec.709 luminance of the linear
    colour), 'R', 'G', 'B' or 'A'. Empty `name` = the active colour."""
    attrs = mesh.color_attributes
    attr = attrs.get(name) if name else (attrs.active_color or (attrs[0] if len(attrs) else None))
    if attr is None:
        return None
    n = len(attr.data)
    rgba = np.empty(n * 4, dtype=np.float32)
    attr.data.foreach_get("color", rgba)
    rgba = rgba.reshape(-1, 4).astype(np.float64)
    if channel == 'LUMA':
        value = rgba[:, :3] @ np.array([0.2126, 0.7152, 0.0722])
    else:
        value = rgba[:, "RGBA".index(channel)]
    nv = len(mesh.vertices)
    if attr.domain == 'POINT':
        return np.clip(value[:nv], 0.0, 1.0)
    lv = np.empty(len(mesh.loops), dtype=np.int64)
    mesh.loops.foreach_get("vertex_index", lv)
    acc = np.zeros(nv); cnt = np.zeros(nv)
    np.add.at(acc, lv, value)
    np.add.at(cnt, lv, 1.0)
    return np.clip(np.where(cnt > 0, acc / np.maximum(cnt, 1.0), 0.0), 0.0, 1.0)


def read_mask_weights(mesh, group_index):
    """(N,) float64 per-vertex weights for vertex group `group_index` - a
    vertex not assigned to the group weighs 0.0. That is a REAL zero (the
    group covers it at 0%), a different case from the module's own
    out-of-range fallback (weight 1.0 for a sample that misses the mesh
    entirely, project_damage_field.h's "a mask that cannot be read must never erase
    damage" rule) - this function can never produce that case, every input
    vertex is on the mesh by construction.

    ponytail: a plain per-vertex Python loop over `v.groups` - O(N x
    groups-per-vertex). Blender has no foreach_get for vertex-group weights
    (a vertex belongs to a variable number of groups, so there is no
    fixed-stride buffer to walk); fine at addon-scale meshes, revisit if a
    numpy-friendly group-weight API ever lands in bpy."""
    n = len(mesh.vertices)
    w = np.zeros(n, dtype=np.float64)
    for i, v in enumerate(mesh.vertices):
        for g in v.groups:
            if g.group == group_index:
                w[i] = g.weight
                break
    return w


def read_avoid_polys(mesh, attr):
    """`attr`'s per-face booleans -> (M,) bool, one entry per polygon, the
    shape CoreInput.avoidPoly expects."""
    n = len(mesh.polygons)
    out = np.empty(n, dtype=bool)
    attr.data.foreach_get("value", out)
    return out


def read_guide_splines(depsgraph, obj, links, min_points=2):
    """Fracture Guide Splines (C4D id 1191) -> one float64 (P,3) array per
    curve SPLINE, in `obj`'s LOCAL space - the same shape the C4D host hands
    the core, so the module needs no host-specific branch.

    Evaluated through the dependency graph, so modifiers and generated curves
    arrive the way the viewport shows them. That is also what applies the
    curve's resolution: Blender's control points are no more the curve than
    C4D's are (GetLineObject at lod 1.0 is the C4D half of the same problem),
    so a Bezier circle read from its four handles would cut a DIAMOND.
    """
    out = []
    inv = obj.matrix_world.inverted()
    seen = set()
    for link in links:
        src = link.obj
        if src is None or src.type != 'CURVE' or src.name in seen:
            continue
        seen.add(src.name)
        ev = src.evaluated_get(depsgraph)
                                                                          
                                                                             
                     
        try:
            me = ev.to_mesh()
        except RuntimeError:
            continue
        if me is None:
            continue
        try:
            rel = inv @ ev.matrix_world
            co = np.empty(len(me.vertices) * 3, dtype=np.float64)
            me.vertices.foreach_get("co", co)
            co = co.reshape(-1, 3)
            edges = np.empty(len(me.edges) * 2, dtype=np.int32)
            me.edges.foreach_get("vertices", edges)
            if min_points == 1:
                linked = set(int(v) for v in edges)
                for i in range(len(co)):
                    if i not in linked:
                        out.append(np.array([tuple(rel @ Vector(co[i]))], dtype=np.float64))
            for run in _edge_runs(edges.reshape(-1, 2), len(co)):
                if len(run) < 2:
                    continue
                pts = np.array([tuple(rel @ Vector(co[i])) for i in run],
                               dtype=np.float64)
                out.append(pts)
        finally:
            ev.to_mesh_clear()
    return out


def _edge_runs(edges, nverts):
    """Split an edge list into connected vertex runs (one per spline). A closed
    spline comes back with its first vertex repeated at the end, matching what
    the C4D side does for a closed segment."""
    adj = {}
    for a, b in edges:
        adj.setdefault(int(a), []).append(int(b))
        adj.setdefault(int(b), []).append(int(a))
    runs = []
    visited = set()
                                                                          
    starts = [v for v in sorted(adj) if len(adj[v]) == 1]
    starts += [v for v in sorted(adj) if v not in starts]
    for s in starts:
        if s in visited:
            continue
        run = [s]
        visited.add(s)
        cur = s
        while True:
            nxt = next((n for n in adj[cur] if n not in visited), None)
            if nxt is None:
                                                                
                if len(run) > 2 and s in adj[cur]:
                    run.append(s)
                break
            visited.add(nxt)
            run.append(nxt)
            cur = nxt
        if len(run) >= 2:
            runs.append(run)
    return runs


                                                              
                                                                                      
                                                                            
                                                                            
                                                 
PHONG_LIMIT = np.cos(np.radians(25.0))
                                                                           
                                                                 
                                                                             
SHADING_REGION_ATTR = "project_damage_shading_region"
SHADING_REGION_GROUP = "ProjectDamage_ShadingRegion"
                                                                        
                                                                          
                                                                           
                                                      
GENERATED_REGION_KEYS = ("wallpaper_back_polys", "wallpaper_wall_polys",
                         "brick_break_polys", "brick_seam_polys", "brick_behind_polys",
                         "bevel_polys", "scratch_groove_polys")
                                                                            
                                                                         
                                                                            
                                                                              
                                                                     
                                                                           
                                                                        
                                                                                                                                     
                                            
DISPLACE_TOL = 1e-4
DISPLACE_TURN = 60.0
DISPLACE_COS = np.cos(np.radians(DISPLACE_TURN))
EDGE_MATCH_TOL = 1e-3


class SourceShading:
    """Plain numpy copy of the SOURCE mesh's shading plus the topology needed
    to map it onto a result whose edges no longer match the source's: face
    smooth flags, edge sharp flags, which source edge joins which two faces,
    and every face's triangles. Read-only towards the source and valid after
    the source mesh is freed (to_mesh_clear)."""

    def __init__(self, points, loop_start, loop_total, loop_edge, edges,
                 smooth, edge_sharp, tris, tri_face, loop_vert=None):
        self.points = np.asarray(points, dtype=np.float64).reshape(-1, 3)
        self.loop_start = np.asarray(loop_start, dtype=np.int64)
        self.loop_total = np.asarray(loop_total, dtype=np.int64)
        self.loop_edge = np.asarray(loop_edge, dtype=np.int64)
        self.edges = np.asarray(edges, dtype=np.int64).reshape(-1, 2)
        self.smooth = np.asarray(smooth, dtype=bool)
        self.edge_sharp = np.asarray(edge_sharp, dtype=bool)
        self.tris = np.asarray(tris, dtype=np.int64).reshape(-1, 3)
        self.tri_face = np.asarray(tri_face, dtype=np.int64)
        self.n_polys = len(self.loop_total)
        self.diag = float(np.linalg.norm(np.ptp(self.points, axis=0))) if len(self.points) else 0.0
        self._bvh = None

                                                                            
                                                                         
                                                                             
        loop_face = np.repeat(np.arange(self.n_polys, dtype=np.int64), self.loop_total)
        order = np.argsort(self.loop_edge, kind="stable")
        e, f = self.loop_edge[order], loop_face[order]
        pair = np.flatnonzero((e[:-1] == e[1:]) & (f[:-1] != f[1:]))
        lo = np.minimum(f[pair], f[pair + 1])
        hi = np.maximum(f[pair], f[pair + 1])
        keys = lo * self.n_polys + hi
        keys, first, count = np.unique(keys, return_index=True, return_counts=True)
        self.pair_keys = keys
        self.pair_edge = np.where(count == 1, e[pair][first], -1)

                                                                        
                                                                         
                                                                  
        if loop_vert is not None and len(self.loop_total):
            loop_vert = np.asarray(loop_vert, dtype=np.int64)
            quad = np.flatnonzero(self.loop_total == 4)
            if len(quad):
                q = loop_vert[self.loop_start[quad][:, None] + np.arange(4)]
                extra = np.concatenate([q[:, [0, 1, 2]], q[:, [0, 2, 3]], q[:, [0, 1, 3]], q[:, [1, 2, 3]]])
                keep = np.ones(self.n_polys, dtype=bool)
                keep[quad] = False
                keep_t = keep[self.tri_face]
                self.tris = np.concatenate([self.tris[keep_t], extra])
                self.tri_face = np.concatenate([self.tri_face[keep_t], np.tile(quad, 4)])
                                                                             
                                                                             
        self.tri_order = np.argsort(self.tri_face, kind="stable")
        self.tri_offsets = np.empty(self.n_polys + 1, dtype=np.int64)
        self.tri_offsets[0] = 0
        np.cumsum(np.bincount(self.tri_face, minlength=self.n_polys),
                  out=self.tri_offsets[1:])
        p = self.points[self.tris]
        n = np.cross(p[:, 1] - p[:, 0], p[:, 2] - p[:, 0])
        ln = np.linalg.norm(n, axis=1)
        self.tri_ok = ln > 1e-30
        self.tri_normal = n / np.where(self.tri_ok, ln, 1.0)[:, None]
        self.tri_origin = p[:, 0]
        self.tri_points = p

    @classmethod
    def from_mesh(cls, mesh, raw=False):
        """Snapshot `mesh` (the source, or the evaluated mesh the core read).
        Nothing is written to it."""
        n_v, n_p, n_l, n_e = len(mesh.vertices), len(mesh.polygons), len(mesh.loops), len(mesh.edges)
        co = np.empty(n_v * 3,np.float32)
        mesh.vertices.foreach_get("co", co)
        loop_start = np.empty(n_p, dtype=np.int32)
        loop_total = np.empty(n_p, dtype=np.int32)
        mesh.polygons.foreach_get("loop_start", loop_start)
        mesh.polygons.foreach_get("loop_total", loop_total)
        loop_edge = np.empty(n_l, dtype=np.int32)
        mesh.loops.foreach_get("edge_index", loop_edge)
        loop_vert = np.empty(n_l, dtype=np.int32)
        mesh.loops.foreach_get("vertex_index", loop_vert)
        edges = np.empty(n_e * 2, dtype=np.int32)
        mesh.edges.foreach_get("vertices", edges)
        smooth = np.empty(n_p, dtype=bool)
        mesh.polygons.foreach_get("use_smooth", smooth)
        sharp = np.empty(n_e, dtype=bool)
        mesh.edges.foreach_get("use_edge_sharp", sharp)
        mesh.calc_loop_triangles()
        n_t = len(mesh.loop_triangles)
        tris = np.empty(n_t * 3, dtype=np.int32)
        tri_face = np.empty(n_t, dtype=np.int32)
        mesh.loop_triangles.foreach_get("vertices", tris)
        mesh.loop_triangles.foreach_get("polygon_index", tri_face)
        args=(co, loop_start, loop_total, loop_edge, edges, smooth, sharp, tris, tri_face, loop_vert)
        return args if raw else cls(*args)

    def poly_edges(self, face):
        s = int(self.loop_start[face])
        return self.loop_edge[s:s + int(self.loop_total[face])]

    def nearest_faces(self, centers, normals):
        """Source face nearest to each point, preferring the sheet that faces
        the same way (the same rule Transfer uses), -1 where none."""
        from mathutils.bvhtree import BVHTree
        if self._bvh is None:
            self._bvh = BVHTree.FromPolygons([Vector(v) for v in self.points],
                                             self.tris.tolist(), all_triangles=True)
        eps = max(1e-7, self.diag * 1e-5)
        out = np.full(len(centers), -1, dtype=np.int64)
        for i, (c, nrm) in enumerate(zip(centers, normals)):
            p, n = Vector(c), Vector(nrm)
            _loc, normal, tri, dist = self._bvh.find_nearest(p)
            if tri is None:
                continue
            if normal.dot(n) < .5:
                near = self._bvh.find_nearest_range(p, dist + eps)
                if near:
                    tri = max(near, key=lambda q: q[1].dot(n))[2]
            out[i] = self.tri_face[tri]
        return out

    def _triangle_pairs(self, face_src, batch_size=65536):
        """Yield (query, triangle) indices for each query's actual source face.

        Flatten ragged ranges in bounded batches: both work and temporary
        memory follow real comparisons, never faces x largest n-gon. Queries
        with no triangles produce no pairs; degenerate triangles are skipped.
        """
        starts = self.tri_offsets[face_src]
        ends = np.cumsum(self.tri_offsets[face_src + 1] - starts)
        if not len(ends):
            return
        for first in range(0, int(ends[-1]), batch_size):
            pair = np.arange(first, min(first + batch_size, int(ends[-1])), dtype=np.int64)
            query = np.searchsorted(ends, pair, side="right")
            before = np.where(query > 0, ends[np.maximum(query - 1, 0)], 0)
            tri = self.tri_order[starts[query] + pair - before]
            valid = self.tri_ok[tri]
            yield query[valid], tri[valid]

    def deformed(self, face_src, loop_face, loop_xyz, normals):
        """Faces (src >= 0) that no longer lie on their source polygon: some
        corner left its surface (true distance to its triangles, not to their
        infinite planes), or the face turned away from all of them."""
        n_faces = len(face_src)
        has = face_src >= 0
        if not np.any(has) or not len(self.tris):
            return np.zeros(n_faces, dtype=bool)
        tol = max(1e-9, DISPLACE_TOL * self.diag)
        loop_ids = np.flatnonzero(has[loop_face])
        dist = np.full(len(loop_face), np.inf)
        for query, tri in self._triangle_pairs(face_src[loop_face[loop_ids]]):
            ids = loop_ids[query]
            d = _triangle_distance(loop_xyz[ids], self.tri_points[tri])
            np.minimum.at(dist, ids, d)
        face_ids = np.flatnonzero(has)
        best_dot = np.full(n_faces, -np.inf)
        for query, tri in self._triangle_pairs(face_src[face_ids]):
            ids = face_ids[query]
            dots = np.einsum("ij,ij->i", normals[ids], self.tri_normal[tri])
            np.maximum.at(best_dot, ids, dots)
        far = np.zeros(n_faces, dtype=bool)
        np.logical_or.at(far, loop_face[loop_ids], dist[loop_ids] > tol)
        return has & (far | (best_dot < DISPLACE_COS))

    def sharp_between(self, sa, sb, p0, p1):
        """Sharp flag of the source edge an output edge lies on, for output
        edges between faces traced to source faces sa and sb. Returns
        (sharp, matched). Same source face on both sides = the inside of
        one source polygon, never sharp. Output edges are matched to source
        edges by face pair first, then geometrically against the edges of
        both faces, because cuts, retriangulation and welds split and
        renumber edges."""
        m = len(sa)
        sharp = np.zeros(m, dtype=bool)
        matched = sa == sb
        if not len(self.pair_keys):
            todo = np.flatnonzero(~matched)
        else:
            key = np.minimum(sa, sb) * self.n_polys + np.maximum(sa, sb)
            pos = np.minimum(np.searchsorted(self.pair_keys, key), len(self.pair_keys) - 1)
            edge = np.where(self.pair_keys[pos] == key, self.pair_edge[pos], -1)
            cand = (~matched) & (edge >= 0)
            ci = np.flatnonzero(cand)
            if len(ci):
                q = self.points[self.edges[edge[ci]]]
                d = np.maximum(_segment_distance(p0[ci], q[:, 0], q[:, 1]),
                               _segment_distance(p1[ci], q[:, 0], q[:, 1]))
                good = d <= max(1e-9, EDGE_MATCH_TOL * self.diag)
                sharp[ci[good]] = self.edge_sharp[edge[ci[good]]]
                matched[ci[good]] = True
            todo = np.flatnonzero(~matched)
        tol = max(1e-9, EDGE_MATCH_TOL * self.diag)
        for i in todo:
            cands = np.unique(np.concatenate([self.poly_edges(sa[i]), self.poly_edges(sb[i])]))
            q = self.points[self.edges[cands]]
            a = np.repeat(p0[i][None], len(cands), axis=0)
            b = np.repeat(p1[i][None], len(cands), axis=0)
            d = np.maximum(_segment_distance(a, q[:, 0], q[:, 1]),
                           _segment_distance(b, q[:, 0], q[:, 1]))
            j = int(np.argmin(d))
            if d[j] <= tol:
                sharp[i] = self.edge_sharp[cands[j]]
                matched[i] = True
        return sharp, matched


def _triangle_distance(p, tri):
    """Distance from points p (N,3) to triangles tri (N,3,3), row by row."""
    a, b, c = tri[:, 0], tri[:, 1], tri[:, 2]
    n = np.cross(b - a, c - a)
    nn = np.einsum("ij,ij->i", n, n)
    ok = nn > 1e-30
    n = n / np.sqrt(np.where(ok, nn, 1.0))[:, None]
    plane = np.einsum("ij,ij->i", p - a, n)
    q = p - plane[:, None] * n
    inside = ok.copy()
    for u, v in ((a, b), (b, c), (c, a)):
        inside &= np.einsum("ij,ij->i", np.cross(v - u, q - u), n) >= 0.0
    edge = np.minimum(np.minimum(_segment_distance(p, a, b), _segment_distance(p, b, c)),
                      _segment_distance(p, c, a))
    return np.where(inside, np.abs(plane), edge)


def _segment_distance(p, a, b):
    ab = b - a
    t = np.einsum("ij,ij->i", p - a, ab) / np.maximum(np.einsum("ij,ij->i", ab, ab), 1e-30)
    c = a + np.clip(t, 0.0, 1.0)[:, None] * ab
    return np.linalg.norm(p - c, axis=1)


class ShadingProvenance:
    """What apply_phong needs to keep the 25 deg limit on the damage.

    tri_class  per result face damage class (BASE_CLASS = surviving base).
    base_poly  per result face source polygon index (-1 = untraced), in the
               SAME polygon numbering as `source`.
    source     SourceShading of the mesh the core read (snapshot or obj.data).
    extra      index arrays of faces to treat as damaged in addition.
    generated  the whole mesh is new geometry with no source surface (the
               deposits and coating objects): every edge gets the limit, on
               purpose, not as a fallback."""

    def __init__(self, tri_class=None, base_poly=None, source=None, extra=(), generated=False):
        self.tri_class = None if tri_class is None else np.asarray(tri_class)
        self.base_poly = None if base_poly is None else np.asarray(base_poly)
        self.source = source
        self.extra = tuple(np.asarray(x, dtype=np.int64) for x in extra if x is not None)
        self.generated = generated

    @classmethod
    def generated_object(cls):
        return cls(generated=True)

    @classmethod
    def from_result(cls, out, source):
        return cls(out.get("tri_class"), out.get("base_poly"), source,
                   [out.get(k) for k in GENERATED_REGION_KEYS])


def _scoped_shading(points, normals, loop_total, loop_verts, loop_edge, edges,
                    smooth, sharp, prov):
    """The numpy core of apply_phong. Returns (smooth, sharp, region, notes):
    the new face/edge flags, the faces treated as damaged and any messages
    the caller should report. `smooth`/`sharp` come in as the mesh's current
    flags; whatever is not classified keeps them."""
    n_polys, n_edges = len(loop_total), len(edges)
    smooth, sharp = smooth.copy(), sharp.copy()
    notes = []
    loop_face = np.repeat(np.arange(n_polys, dtype=np.int64), loop_total)
    centers = np.zeros((n_polys, 3))
    np.add.at(centers, loop_face, points[loop_verts])
    centers /= np.maximum(loop_total, 1)[:, None]

    region = np.zeros(n_polys, dtype=bool)
    src = np.full(n_polys, -1, dtype=np.int64)
    source = None if prov is None else prov.source
    if isinstance(source,tuple): source=SourceShading(*source)
    if prov is not None and prov.generated:
        region[:] = True
    else:
        tri_class = None if prov is None else prov.tri_class
        if tri_class is not None and len(tri_class) != n_polys:
            notes.append("damage classes do not match the result (%d vs %d faces)"
                         % (len(tri_class), n_polys))
            tri_class = None
        if tri_class is None and source is None:
            notes.append("no damage classes or source shading were available, so "
                         "the result's shading was left untouched")
            return smooth, sharp, region, notes
        base_face = np.ones(n_polys, dtype=bool) if tri_class is None else tri_class == BASE_CLASS
        region |= ~base_face
        for idx in (prov.extra if prov is not None else ()):
            idx = idx[(idx >= 0) & (idx < n_polys)]
            region[idx] = True
        base_face &= ~region
        if source is not None:
            base = prov.base_poly
            if base is not None and len(base) == n_polys:
                ok = base_face & (base >= 0) & (base < source.n_polys)
                src[ok] = base[ok]
            missing = np.flatnonzero(base_face & (src < 0))
            if len(missing):
                src[missing] = source.nearest_faces(centers[missing], normals[missing])
            if tri_class is None:
                notes.append("no damage classes: the damaged region was taken as "
                             "the faces that left the source surface")
            region |= source.deformed(src, loop_face, points[loop_verts], normals)
            unresolved = base_face & (src < 0)
            if np.any(unresolved):
                region |= unresolved
                notes.append("%d surviving faces could not be traced to the source and "
                             "were shaded as damage" % int(unresolved.sum()))
        else:
            notes.append("no source shading was available: only the damaged faces "
                         "were shaded, the others were left as created")
                                                                             
                                                                            
    edge_src = src.copy()
    src[region] = -1

                                                                           
                                                 
    order = np.argsort(loop_edge, kind="stable")
    e, p = loop_edge[order], loop_face[order]
    pair = np.flatnonzero(e[:-1] == e[1:])
    ea, fa, fb = e[pair], p[pair], p[pair + 1]

    smooth[region] = True
    keep = src >= 0
    if np.any(keep):
        smooth[keep] = source.smooth[src[keep]]

    rim = region[fa] | region[fb]
    cos = np.einsum("ij,ij->i", normals[fa], normals[fb])
    val = rim & (cos < PHONG_LIMIT)
    both = (edge_src[fa] >= 0) & (edge_src[fb] >= 0)
    if np.any(both):
        bi = np.flatnonzero(both)
        ev = edges[ea[bi]]
        s_sharp, matched = source.sharp_between(edge_src[fa[bi]], edge_src[fb[bi]],
                                                points[ev[:, 0]], points[ev[:, 1]])
        val[bi] |= s_sharp
                                                                          
        lost = int(np.count_nonzero(~matched & ~rim[bi]))
        if lost:
            notes.append("%d surviving edges had no matching source edge and were "
                         "left smooth" % lost)
    touched = rim | both
    sharp[ea[touched]] = False
    np.logical_or.at(sharp, ea[touched], val[touched])
    return smooth, sharp, region, notes


def apply_phong(mesh,provenance):
    from . import background_compute as bg
    return bg.immediate(phong_steps(mesh,provenance))

def phong_steps(mesh, provenance):
    from . import background_compute as bg
    """Give the result the C4D result's look ON THE DAMAGE ONLY: damaged faces
    smooth, every edge inside the damage or on its rim (damage face against
    surviving face) marked sharp where the faces meet at more than 25 deg.
    Without it a chip rim (median dihedral 62 deg) smooths into the surface
    it was cut out of and the damage reads as a dent in a blob.

    Surviving faces keep the smooth/flat flag of the source face they came
    from, and edges between them keep the source's sharp flag, matched
    through the face provenance (output edges do not map 1:1 to source
    edges). The 25 deg limit never touches them, and an intentional sharp
    edge is never cleared. `provenance` is a ShadingProvenance;
    ShadingProvenance.generated_object() for a mesh that is all new geometry.

    Fallbacks, never a global angle pass: an untraced surviving face finds
    its source face by nearest surface; a face that cannot be traced at all
    is shaded as damage (local); without damage classes the damage is the
    faces that left the source surface; without any provenance nothing is
    changed. Each fallback is returned as a message for the caller's report.
    The source mesh is only ever read. Returns the list of messages."""
    n_polys, n_loops, n_edges = len(mesh.polygons), len(mesh.loops), len(mesh.edges)
    co = np.empty(len(mesh.vertices) * 3,np.float32)
    mesh.vertices.foreach_get("co", co)
    nrm = np.empty(n_polys * 3,np.float32)
    mesh.polygons.foreach_get("normal", nrm)
    total = np.empty(n_polys, dtype=np.int32)
    mesh.polygons.foreach_get("loop_total", total)
    loop_verts = np.empty(n_loops, dtype=np.int32)
    mesh.loops.foreach_get("vertex_index", loop_verts)
    loop_edge = np.empty(n_loops, dtype=np.int32)
    mesh.loops.foreach_get("edge_index", loop_edge)
    edges = np.empty(n_edges * 2, dtype=np.int32)
    mesh.edges.foreach_get("vertices", edges)
    smooth = np.empty(n_polys, dtype=bool)
    mesh.polygons.foreach_get("use_smooth", smooth)
    sharp = np.empty(n_edges, dtype=bool)
    mesh.edges.foreach_get("use_edge_sharp", sharp)

    smooth, sharp, region, notes = yield from bg.task('Calculating surface shading',_scoped_shading,
        co.reshape(-1, 3).astype(np.float64), nrm.reshape(-1, 3).astype(np.float64), total, loop_verts, loop_edge,
        edges.reshape(-1, 2), smooth, sharp, provenance)

    mesh.polygons.foreach_set("use_smooth", smooth)
    mesh.edges.foreach_set("use_edge_sharp", sharp)
    attr = mesh.attributes.get(SHADING_REGION_ATTR) or mesh.attributes.new(SHADING_REGION_ATTR, 'BOOLEAN', 'FACE')
    attr.data.foreach_set("value", region)
    mesh.update()
    return notes


def shading_region_group(obj, empty=False):
    """A vertex group holding the vertices of the faces apply_phong shaded as
    damage, for limiting the Weighted Normal modifier to the damage (set
    `modifier.vertex_group` to the returned name). Always created, even
    empty: an EMPTY group name on the modifier would mean every vertex."""
    mesh = obj.data
    vg = obj.vertex_groups.get(SHADING_REGION_GROUP) or obj.vertex_groups.new(name=SHADING_REGION_GROUP)
    vg.remove(list(range(len(mesh.vertices))))
    attr = mesh.attributes.get(SHADING_REGION_ATTR)
    if not empty and attr is not None and len(mesh.polygons):
        region = np.empty(len(mesh.polygons), dtype=bool)
        attr.data.foreach_get("value", region)
        total = np.empty(len(mesh.polygons), dtype=np.int32)
        mesh.polygons.foreach_get("loop_total", total)
        lv = np.empty(len(mesh.loops), dtype=np.int32)
        mesh.loops.foreach_get("vertex_index", lv)
        verts = np.unique(lv[np.repeat(region, total)])
        if len(verts):
            vg.add(verts.tolist(), 1.0, 'REPLACE')
    return vg.name


def _result_mesh(name, points, polys, shading):
    """A Blender mesh from the core's (K,3)/(K2,4) output. `shading` is the
    ShadingProvenance for apply_phong, or None when the caller applies it
    later, once the damage classes and source mapping are known. Required on
    purpose: no call site falls back to a global angle pass by omission."""
                                                                                     
    p=np.asarray(polys,dtype=np.int32).reshape(-1,4)
    totals=np.where(p[:,2]==p[:,3],3,4).astype(np.int32)
    lv=p[np.arange(4)[None,:]<totals[:,None]]
    mesh=bpy.data.meshes.new(name)
    mesh.vertices.add(len(points));mesh.vertices.foreach_set('co',np.asarray(points,np.float32).ravel())
    mesh.loops.add(len(lv));mesh.loops.foreach_set('vertex_index',lv)
    mesh.polygons.add(len(p));mesh.polygons.foreach_set('loop_start',(np.cumsum(totals,dtype=np.int32)-totals))
    mesh.polygons.foreach_set('loop_total',totals)
    mesh.update(calc_edges=True)
                                                  
    if len(p):
        attr=mesh.attributes.get('sharp_face') or mesh.attributes.new('sharp_face','BOOLEAN','FACE')
        attr.data.foreach_set('value',np.ones(len(p),dtype=np.bool_))
    if shading is not None:
        apply_phong(mesh, shading)
    return mesh


def copy_input_materials(mesh, src_mesh):
    """The source's material slots, in order, onto the result (Keep
    Selections). Returns how many were copied, which is the offset every
    damage class slot then shifts by."""
    for mat in src_mesh.materials:
        mesh.materials.append(mat)
    return len(src_mesh.materials)


def _apply_classes(mesh, tri_class, base_poly=None, src_mesh=None, offset=0, src_index=None, damage_materials=None, shared_cracks=False, skip_base_material=False):
    """One boolean FACE attribute per PRESENT damage class, named exactly like
    the C4D polygon selection defaults, plus one material slot each so the
    classes are addressable by material the way the C4D selections are.

    With `base_poly` and `src_mesh` given (Keep Selections on, after
    copy_input_materials put the source's own slots in front), an UNDAMAGED
    face keeps the material index of the INPUT face it came from, and the
    class slots start at `offset` instead of 0. This is the same sentence the
    C4D half says: base faces keep what they had, cut faces belong to ProjectDamage's
    own classes. A face the core could not trace back (-1: a cutter made it,
    or a coplanarity merge joined two different input faces) falls back to
    slot 0, which is where it would have landed before the feature.

    `src_index` (optional): per-source-polygon material indices, used instead
    of reading `src_mesh` - the prepared path passes the indices of the mesh
    it actually read (an evaluated mesh has no persistent datablock)."""
    present = [c for c in range(len(CLASS_NAMES)) if np.any(tri_class == c)]
    slot_of = np.zeros(len(CLASS_NAMES), dtype=np.int32)
    slot = 0
    crack_slot = None
    for cls in present:
        name = CLASS_NAMES[cls]
        attr = mesh.attributes.new(name, 'BOOLEAN', 'FACE')
        attr.data.foreach_set("value", tri_class == cls)
        if cls == BASE_CLASS and skip_base_material:
            slot_of[cls]=0
            continue
        if shared_cracks and cls in (1,2,3) and crack_slot is not None:
            slot_of[cls] = crack_slot
            continue
        mat = ((damage_materials or {}).get(cls)
               or bpy.data.materials.get(DAMAGE_MATERIAL_NAMES[cls])
               or bpy.data.materials.get("project_damage_" + name))
        if shared_cracks and cls in (1,2,3):
            mat = (next((m for m in ((damage_materials or {}).get(c) for c in (1,2,3)) if m),None)
                   or bpy.data.materials.get('ProjectDamage_Cracks')
                   or bpy.data.materials.get(DAMAGE_MATERIAL_NAMES[1]))
        if mat is None:
            mat = bpy.data.materials.new('ProjectDamage_Cracks' if shared_cracks and cls in (1,2,3) else DAMAGE_MATERIAL_NAMES[cls])
            enable_nodes(mat)
            shader = mat.node_tree.nodes.get("Principled BSDF")
            shader.inputs["Roughness"].default_value = .8
            color = (.38, .33, .27, 1) if cls == 0 else (.18, .15, .12, 1)
            mat.diffuse_color = color
            shader.inputs["Base Color"].default_value = color
        mesh.materials.append(mat)
        slot_of[cls] = slot + offset
        if shared_cracks and cls in (1,2,3):
            crack_slot = slot + offset
        slot += 1
    if not present:
        return present
    index = slot_of[tri_class]
    if base_poly is not None and (src_mesh is not None or src_index is not None) and len(base_poly):
        if src_index is None:
            n_src = len(src_mesh.polygons)
            src_index = np.empty(n_src, dtype=np.int32)
            src_mesh.polygons.foreach_get("material_index", src_index)
        else:
            src_index = np.asarray(src_index, dtype=np.int32)
            n_src = len(src_index)
                                                                             
                                                                             
        undamaged = tri_class == BASE_CLASS
        traced = undamaged & (base_poly >= 0) & (base_poly < n_src)
        index[traced] = src_index[base_poly[traced]]
        index[undamaged & ~traced] = 0
    mesh.polygons.foreach_set("material_index", index)
    return present


def _damage_uvs(points, polys, mask):
    """The numpy mirror of `ApplyDamageUVs` (cpp/source/project_damage_result.cpp):
    a bounding-box-normalized cube projection picked by
    each polygon's dominant face-normal axis, tile 1002 (u + 1.0). `points`/
    `polys` are the core's OWN result arrays (core centimeters, pre the
    `/factor` conversion `rebuild_object` applies for the Blender-side mesh)
    - the projection is a ratio of differences over the same bbox, which is
    scale-invariant, so computing it before or after that division gives the
    identical answer; using the core's own arrays avoids a second copy.
    `mask` is a boolean array over ALL polygons selecting which rows to
    project (the damage ones); returns (mask.sum(), 4, 2) float64, one row
    per True entry of `mask`, in polygon order.

    Corner order (a, b, c, d) and the axis-vs-(u, w) assignment are copied
    from the C4D source line for line:
        u = axis == 0 ? nrm(z) : nrm(x)
        w = axis == 1 ? nrm(z) : nrm(y)
        (1.0 + u, w)              # +1.0 = UDIM tile 1002
    A triangle's unused 4th corner (c == d) reuses corner c's projection,
    matching `proj(cp.c != cp.d ? cp.d : cp.c)`."""
    a, b, c, d = polys[mask, 0], polys[mask, 1], polys[mask, 2], polys[mask, 3]
    pa, pb, pc = points[a], points[b], points[c]
    n = np.cross(pb - pa, pc - pa)
    absn = np.abs(n)
    axis0 = (absn[:, 0] >= absn[:, 1]) & (absn[:, 0] >= absn[:, 2])
    axis1 = (~axis0) & (absn[:, 1] >= absn[:, 2])
    bbmin = points.min(axis=0)
    ext = points.max(axis=0) - points.min(axis=0)

    def nrm(v, axis):
        if ext[axis] > 1e-9:
            return (v - bbmin[axis]) / ext[axis]
        return np.full_like(v, 0.5)

    def proj(idx):
        p = points[idx]
        nx, ny, nz = nrm(p[:, 0], 0), nrm(p[:, 1], 1), nrm(p[:, 2], 2)
        u = np.where(axis0, nz, nx)
        w = np.where(axis1, nz, ny)
        return np.stack([1.0 + u, w], axis=-1)

    out = np.empty((int(mask.sum()), 4, 2), dtype=np.float64)
    out[:, 0] = proj(a)
    out[:, 1] = proj(b)
    out[:, 2] = proj(c)
    out[:, 3] = proj(np.where(c != d, d, c))
    return out


def _write_result_uvs(mesh, layer_name, base_uvs, points, polys, tri_class):
    """One Blender UV layer, named after the SOURCE's own active layer,
    combining the core's own base-polygon UVs (`base_uvs` - out["uvs"], NaN
    on every damage polygon and on a triangle's unused 4th corner - the
    module's contract) with `_damage_uvs` for the damage rows. `points`/
    `polys` are the core's own output arrays, the same ones `_damage_uvs`
    documents. Loop order matches `_result_mesh`'s face construction
    (a, b, c[, d]) exactly - the same convention `read_uvs` uses on the
    input side."""
    uvs = base_uvs.copy()
    dmg = tri_class != BASE_CLASS
    if np.any(dmg):
        uvs[dmg] = _damage_uvs(points, polys, dmg)

    layer = mesh.uv_layers.new(name=layer_name)
    n_polys = len(mesh.polygons)
    loop_total = np.empty(n_polys, dtype=np.int32)
    mesh.polygons.foreach_get("loop_total", loop_total)
    loop_start = np.empty(n_polys, dtype=np.int32)
    mesh.polygons.foreach_get("loop_start", loop_start)
    flat = np.empty((len(mesh.loops), 2), dtype=np.float64)
    quad = loop_total == 4
    tri = ~quad
    if np.any(quad):
        flat[loop_start[quad][:, None] + np.arange(4)] = uvs[quad]
    if np.any(tri):
        flat[loop_start[tri][:, None] + np.arange(3)] = uvs[tri, :3]
    layer.data.foreach_set("uv", flat.ravel())
    mesh.update()
