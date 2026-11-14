# ускоренные операции подготовки меша
# ProjectDamage for Blender - non-destructive input preparation (pure numpy).
# GPL-3.0-or-later; see LICENSE.
"""Turn an arbitrary polygon mesh into input the core accepts, without
touching the source, and map the core's result back onto the source's
polygons, corners and vertices.

Nothing here imports bpy, so every step is testable outside Blender; the
Blender reading/writing side lives in input_prep.py.

Paths (chosen by `prepare`):
  legacy       - closed, edge-manifold, triangles/quads only and no modifiers
                 asked for: the caller keeps its ORIGINAL code path unchanged
                 (`needs_preparation` returns False).
  triangulated - n-gons split into triangles (Blender's own loop triangles
                 when given, ear clipping otherwise); quads stay quads.
  repaired     - coincident vertices welded, degenerate and duplicate faces
                 dropped, until the mesh is a closed edge-manifold.
  surface      - open or non-manifold input. The core's manifold check is
                 KEPT; it is given a valid solid instead: components are
                 consistently oriented, non-manifold edges/vertices split,
                 open components thickened into closed shells (back sheet +
                 side walls). Helper faces are marked Avoid and removed from
                 the result again by the core's base-polygon map, so the
                 result is the original open surface with the damage cut in.
"""
import numpy as np

MODES = ("AUTO", "CLOSED", "SURFACE")


                                                                             
               

def _poly_rows(loop_verts, loop_start, loop_total, idx):
    """Corner vertex ids of polygon `idx` (Python list)."""
    s = int(loop_start[idx])
    return [int(v) for v in loop_verts[s:s + int(loop_total[idx])]]


def _newell_normal(pts):
    n = np.zeros(3)
    m = len(pts)
    for i in range(m):
        a, b = pts[i], pts[(i + 1) % m]
        n[0] += (a[1] - b[1]) * (a[2] + b[2])
        n[1] += (a[2] - b[2]) * (a[0] + b[0])
        n[2] += (a[0] - b[0]) * (a[1] + b[1])
    return n


def ear_clip(pts):
    """Triangulate one simple polygon (k,3), concave allowed. Returns a list of
    (i, j, l) corner indices, winding preserved. Falls back to a fan for
    degenerate input."""
    k = len(pts)
    if k == 3:
        return [(0, 1, 2)]
    n = _newell_normal(pts)
    nl = np.linalg.norm(n)
    if nl < 1e-18:
        return [(0, i, i + 1) for i in range(1, k - 1)]
    n /= nl
    axis = int(np.argmax(np.abs(n)))
    u, v = [a for a in range(3) if a != axis]
    p2 = pts[:, [u, v]].astype(np.float64)
    if n[axis] < 0:                                                         
        p2[:, 0] = -p2[:, 0]
    idx = list(range(k))
    tris = []

    def cross(o, a, b):
        return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])

    guard = 0
    while len(idx) > 3 and guard < 4 * k * k:
        guard += 1
        found = False
        m = len(idx)
        for t in range(m):
            i0, i1, i2 = idx[(t - 1) % m], idx[t], idx[(t + 1) % m]
            a, b, c = p2[i0], p2[i1], p2[i2]
            if cross(a, b, c) <= 1e-14:
                continue                                        
            inside = False
            for j in idx:
                if j in (i0, i1, i2):
                    continue
                q = p2[j]
                if cross(a, b, q) >= -1e-14 and cross(b, c, q) >= -1e-14 and cross(c, a, q) >= -1e-14:
                    inside = True
                    break
            if inside:
                continue
            tris.append((i0, i1, i2))
            idx.pop(t)
            found = True
            break
        if not found:
            break
    if len(idx) == 3:
        tris.append(tuple(idx))
    elif len(idx) > 3:                                                        
        tris += [(idx[0], idx[i], idx[i + 1]) for i in range(1, len(idx) - 1)]
    return tris


def edge_report(polys):
    """(boundary_edges, nonmanifold_edges, misoriented_edges) for (M,4)
    ProjectDamagePoly rows. Manifold for the core = every edge used by exactly two
    faces, in opposite directions."""
    a = polys
    tri = a[:, 2] == a[:, 3]
    src = np.concatenate([a[:, 0], a[:, 1], np.where(tri, a[:, 2], a[:, 2]), a[~tri, 3]])
    dst = np.concatenate([a[:, 1], a[:, 2], np.where(tri, a[:, 0], a[:, 3]), a[~tri, 0]])
    lo, hi = np.minimum(src, dst), np.maximum(src, dst)
    key = lo.astype(np.int64) * (int(max(hi.max(initial=0), lo.max(initial=0))) + 1) + hi
    forward = (src < dst).astype(np.int64)
    order = np.argsort(key, kind="stable")
    key_s, fwd_s = key[order], forward[order]
    uniq, start, count = np.unique(key_s, return_index=True, return_counts=True)
    fwd_sum = np.add.reduceat(fwd_s, start) if len(start) else np.zeros(0, np.int64)
    boundary = int(np.sum(count == 1))
    nonmanifold = int(np.sum(count > 2))
    misoriented = int(np.sum((count == 2) & (fwd_sum != 1)))
    return boundary, nonmanifold, misoriented


def is_closed_manifold(polys):
    if len(polys) == 0:
        return False
    if np.any((polys[:, 0] == polys[:, 1]) | (polys[:, 1] == polys[:, 2]) | (polys[:, 0] == polys[:, 2])):
        return False
    b, n, m = edge_report(polys)
    return b == 0 and n == 0 and m == 0


def needs_preparation(loop_total, polys_or_none, mode, use_modifiers):
    """False = the caller's original (legacy) path applies unchanged: closed
    edge-manifold triangles/quads read from the authored mesh."""
    if use_modifiers or mode == "SURFACE":
        return True
    if np.any(np.asarray(loop_total) > 4):
        return True
    if mode == "CLOSED":
        return False                                                            
    return polys_or_none is None or not is_closed_manifold(polys_or_none)


                                                                             
                   

class Prepared:
    """Arrays handed to the core plus maps back to the source mesh.

    points     (N,3) float64  core input points
    polys      (M,4) int32    ProjectDamagePoly rows (c == d: triangle)
    poly_src   (M,)  int32    source polygon index, -1 for a helper face
    corner_loop(M,4) int32    source loop index per corner, -1 if none
    point_src  (N,)  int32    source vertex each point copies
    helper     (M,)  bool     back sheet / side wall faces (surface path)
    path       str            "triangulated" | "repaired" | "surface"
    notes      list[str]      what was done, for the report
    """

    def __init__(self, points, polys, poly_src, corner_loop, point_src, path, notes):
        self.points = np.asarray(points, dtype=np.float64)
        self.polys = np.asarray(polys, dtype=np.int32)
        self.poly_src = np.asarray(poly_src, dtype=np.int32)
        self.corner_loop = np.asarray(corner_loop, dtype=np.int32)
        self.point_src = np.asarray(point_src, dtype=np.int32)
        self.helper = self.poly_src < 0
        self.path = path
        self.notes = list(notes)

                                                                         

    def point_values(self, per_source_vertex):
        """Per-source-vertex array (weights) -> per prepared point."""
        return np.asarray(per_source_vertex)[self.point_src]

    def avoid_polys(self, per_source_poly):
        """Per-source-polygon Avoid -> per prepared polygon; helper faces are
        always avoided so no damage starts on the back sheet or the walls.
        `per_source_poly` may be None (no user Avoid)."""
        out = np.ones(len(self.polys), dtype=bool)
        src = ~self.helper
        if per_source_poly is None:
            out[src] = False
        else:
            out[src] = np.asarray(per_source_poly, dtype=bool)[self.poly_src[src]]
        return out

    def uvs(self, loop_uv):
        """Per-loop UVs (L,2) -> (M,4,2) in the core's corner order; unused
        corners (triangle 4th, helper faces) are (0, 0)."""
        out = np.zeros((len(self.polys), 4, 2), dtype=np.float64)
        ok = self.corner_loop >= 0
        out[ok] = np.asarray(loop_uv, dtype=np.float64)[self.corner_loop[ok]]
        return out

    def material_index(self, per_source_poly):
        return np.asarray(per_source_poly)[np.maximum(self.poly_src, 0)]

                                                                         

    POLY_KEYS = ("polys", "tri_class", "uvs", "base_poly", "effect_ids")

    def finish(self, out, point_keys=()):
        """The core's result dict -> the same dict for the SOURCE mesh:
        helper faces (traced to back sheet / walls) removed, points
        compacted, every per-point array in `point_keys` remapped, and
        base_poly translated from prepared to SOURCE polygon indices (so Keep
        Selections and the n-gon restore address the source)."""
        if out.get("passthrough") or "polys" not in out:
            return out
        polys = np.asarray(out["polys"])
        base = out.get("base_poly")
        base = None if base is None else np.asarray(base)
        keep = np.ones(len(polys), dtype=bool)
        if self.helper.any():
            if base is None or len(base) != len(polys):
                raise ValueError("surface mode needs the core's base polygon map")
            traced = base >= 0
            keep[traced] = ~self.helper[base[traced]]
        res = dict(out)
                                                                     
                                                                         
        face_remap = -np.ones(len(polys), dtype=np.int64)
        face_remap[keep] = np.arange(int(keep.sum()))
        region_keys = ['wallpaper_' + k + '_polys' for k in ('front', 'back', 'wall', 'base')]
        region_keys += ['brick_' + k + '_polys' for k in ('front', 'break', 'seam', 'behind', 'render')]
        region_keys += [k + '_polys' for k in ('bevel', 'wear', 'scratch_groove', 'scratch_scuff')]
        for key in region_keys:
            if key in out:
                indices = np.asarray(out[key], dtype=np.int64)
                mapped = face_remap[indices]
                res[key] = mapped[mapped >= 0].astype(np.int32)
        for k in self.POLY_KEYS:
            v = res.get(k)
            if v is not None and len(np.asarray(v)) == len(polys):
                res[k] = np.asarray(v)[keep]
        kept = res["polys"]
        n_pts = len(np.asarray(out["points"]))
        used = np.zeros(n_pts, dtype=bool)
        used[kept.ravel()] = True
        remap = -np.ones(n_pts, dtype=np.int64)
        remap[used] = np.arange(int(used.sum()))
        res["polys"] = remap[kept].astype(np.int32)
        res["points"] = np.asarray(out["points"])[used]
        for k in point_keys:
            v = out.get(k)
            if v is not None and len(np.asarray(v)) == n_pts:
                res[k] = np.asarray(v)[used]
        if base is not None and res.get("base_poly") is not None:
            b = np.asarray(res["base_poly"]).copy()
            t = b >= 0
            b[t] = self.poly_src[b[t]]
            res["base_poly"] = b
        if "counts" in res and "tri_class" in res:
            n_chip = int(np.sum(np.asarray(res["tri_class"]) == 0))
            res["counts"] = (len(res["points"]), len(res["polys"]), n_chip)
        res["removed_helper_faces"] = int((~keep).sum())
        return res

    def ngon_groups(self, out):
        """Result faces that are an UNTOUCHED, complete triangulation of one
        source n-gon: {source_poly: [result face, ...]} - the caller may
        dissolve each group back into the original n-gon. Uses the
        source-indexed base_poly from finish()."""
        base = out.get("base_poly")
        tri_class = out.get("tri_class")
        if base is None or tri_class is None:
            return {}
        base = np.asarray(base)
        expected = {}
        for p in self.poly_src[(self.poly_src >= 0)]:
            expected[int(p)] = expected.get(int(p), 0) + 1
        groups = {}
        for f in np.flatnonzero((base >= 0) & (np.asarray(tri_class) == 6)):
            groups.setdefault(int(base[f]), []).append(int(f))
        return {p: fs for p, fs in groups.items()
                if expected.get(p, 0) > 1 and len(fs) == expected[p]}


                                                                             
                   

def _faces_from_source(points, loop_verts, loop_start, loop_total, loop_tris):
    """Source polygons -> face rows (tris/quads kept, n-gons triangulated).
    Returns (faces (M,4), poly_src (M,), corner_loop (M,4), n_ngons)."""
    loop_total = np.asarray(loop_total)
    loop_start = np.asarray(loop_start)
    loop_verts = np.asarray(loop_verts)
    n = len(loop_total)
    small = np.flatnonzero(loop_total <= 4)
    if np.any(loop_total < 3):
        raise ValueError("the mesh has faces with fewer than 3 corners")
    rows, src, corners = [], [], []
                                                      
    cl = -np.ones((len(small), 4), dtype=np.int64)
    tot = loop_total[small]
    for c in range(4):
        has = tot > c
        cl[has, c] = loop_start[small][has] + c
    cl[tot == 3, 3] = cl[tot == 3, 2]
    face = loop_verts[cl]
    rows.append(face)
    src.append(small)
    corners.append(np.where(tot[:, None] == 3, np.concatenate([cl[:, :3], -np.ones((len(cl), 1), np.int64)], 1), cl))
    ngons = np.flatnonzero(loop_total > 4)
    if len(ngons):
        by_poly = {}
        if loop_tris is not None:
            for p, la, lb, lc in loop_tris:
                if loop_total[p] > 4:
                    by_poly.setdefault(int(p), []).append((int(la), int(lb), int(lc)))
        nr, ns, nc = [], [], []
        for p in ngons:
            s = int(loop_start[p])
            loops = list(range(s, s + int(loop_total[p])))
            tris = by_poly.get(int(p))
            if not tris:
                pts = points[loop_verts[loops]]
                tris = [(loops[i], loops[j], loops[k]) for i, j, k in ear_clip(pts)]
            for la, lb, lc in tris:
                nr.append([loop_verts[la], loop_verts[lb], loop_verts[lc], loop_verts[lc]])
                ns.append(p)
                nc.append([la, lb, lc, -1])
        rows.append(np.array(nr, dtype=np.int64))
        src.append(np.array(ns, dtype=np.int64))
        corners.append(np.array(nc, dtype=np.int64))
    faces = np.concatenate(rows).astype(np.int32)
    poly_src = np.concatenate(src).astype(np.int32)
    corner_loop = np.concatenate(corners).astype(np.int32)
                                                             
    order = np.argsort(poly_src, kind="stable")
    return faces[order], poly_src[order], corner_loop[order], len(ngons)


def _weld(points, faces, tol):
    """Merge points closer than ~tol (two offset grids, label propagation).
    Returns (labels (N,) representative index per point)."""
    n = len(points)
    labels = np.arange(n)
    if tol <= 0 or n == 0:
        return labels
    grids = [np.floor(points / tol).astype(np.int64), np.floor(points / tol + 0.5).astype(np.int64)]
    groups = []
    for q in grids:
        _, inv = np.unique(q, axis=0, return_inverse=True)
        groups.append(inv.ravel())
    for _ in range(16):
        before = labels.copy()
        for inv in groups:
            m = np.full(inv.max() + 1, n, dtype=np.int64)
            np.minimum.at(m, inv, labels)
            labels = np.minimum(labels, m[inv])
        labels = labels[labels]
        if np.array_equal(labels, before):
            break
                                                                  
    far = np.linalg.norm(points - points[labels], axis=1) > 2.0 * tol
    labels[far] = np.arange(n)[far]
    return labels


def _clean_faces(faces, poly_src, corner_loop):
    """Drop collapsed faces, turn quads with one repeated corner into
    triangles, drop exact duplicates and opposite-facing pairs (internal
    double walls). Returns filtered arrays and counts."""
    keep_rows, dropped_degenerate = [], 0
    out_f, out_s, out_c = [], [], []
    seen = {}
    for f, s, c in zip(faces.tolist(), poly_src.tolist(), corner_loop.tolist()):
        tri = f[2] == f[3]
        verts = f[:3] if tri else f
        corners = c[:3] if tri else c
        uniq, uc = [], []
        for v, l in zip(verts, corners):
            if not uniq or uniq[-1] != v:
                uniq.append(v); uc.append(l)
        if len(uniq) > 1 and uniq[0] == uniq[-1]:
            uniq.pop(); uc.pop()
        if len(uniq) < 3 or len(set(uniq)) < len(uniq):
            dropped_degenerate += 1
            continue
        if len(uniq) == 3:
            row, crow = [uniq[0], uniq[1], uniq[2], uniq[2]], [uc[0], uc[1], uc[2], -1]
        else:
            row, crow = uniq, uc
        key = tuple(sorted(set(uniq)))
        if key in seen:
            seen[key].append(len(out_f))
        else:
            seen[key] = [len(out_f)]
        out_f.append(row); out_s.append(s); out_c.append(crow)
    drop = set()
    duplicates = 0
    pairs = []
    for key, idx in seen.items():
        if len(idx) < 2:
            continue
                                                                      
                                                      
        def cyc(r):
            r = r[:3] if r[2] == r[3] else r
            m = r.index(min(r))
            return tuple(r[m:] + r[:m])
        windings = {}
        for i in idx:
            windings.setdefault(cyc(out_f[i]), []).append(i)
        for ids in windings.values():
            for extra in ids[1:]:
                drop.add(extra); duplicates += 1
        if len(windings) == 2:
            pair = []
            for ids in windings.values():
                drop.add(ids[0]); duplicates += 1
                pair.append((out_f[ids[0]], out_s[ids[0]], out_c[ids[0]]))
            pairs.append(pair)
    keep = [i for i in range(len(out_f)) if i not in drop]
    f = np.array([out_f[i] for i in keep], dtype=np.int32).reshape(-1, 4)
    s = np.array([out_s[i] for i in keep], dtype=np.int32)
    c = np.array([out_c[i] for i in keep], dtype=np.int32).reshape(-1, 4)
    return f, s, c, dropped_degenerate, duplicates, pairs


class _EdgeStats:
    """edge_report() kept up to date incrementally: per unordered edge its use
    count and forward count; total = boundary + non-manifold + misoriented."""
    def __init__(self, polys):
        src, dst, _ = _edge_arrays(polys)
        self.cnt, self.fwd = {}, {}
        for a, b in zip(src.tolist(), dst.tolist()):
            k = (min(a, b), max(a, b))
            self.cnt[k] = self.cnt.get(k, 0) + 1
            self.fwd[k] = self.fwd.get(k, 0) + (1 if a < b else 0)
        self.total = sum(self._bad(self.cnt[k], self.fwd[k]) for k in self.cnt)

    @staticmethod
    def _bad(c, f):
        return (1 if c == 1 else 0) + (1 if c > 2 else 0) + (1 if (c == 2 and f != 1) else 0)

    def _deltas(self, row):
        d = {}
        for a, b in _face_edges(list(row)):
            k = (min(a, b), max(a, b))
            c0, f0 = d.get(k, (self.cnt.get(k, 0), self.fwd.get(k, 0)))
            d[k] = (c0 + 1, f0 + (1 if a < b else 0))
        return d

    def gain(self, row):
        change = 0
        for k, (c1, f1) in self._deltas(row).items():
            c0, f0 = self.cnt.get(k, 0), self.fwd.get(k, 0)
            change += self._bad(c1, f1) - (self._bad(c0, f0) if c0 else 0)
        return change

    def add(self, row):
        for k, (c1, f1) in self._deltas(row).items():
            c0, f0 = self.cnt.get(k, 0), self.fwd.get(k, 0)
            self.total += self._bad(c1, f1) - (self._bad(c0, f0) if c0 else 0)
            self.cnt[k], self.fwd[k] = c1, f1


def MP_edge_gain(before, after):
    """True when `after` has fewer open + non-manifold + misoriented edges."""
    return sum(edge_report(after)) < sum(edge_report(before))


def _face_edges(row):
    r = row[:3] if row[2] == row[3] else row
    return [(r[i], r[(i + 1) % len(r)]) for i in range(len(r))]


def _edge_arrays(faces):
    """Directed edges of (m, 4) rows in scan order (face-major, the order of
    _face_edges): (src, dst, face) int64 arrays."""
    F = np.asarray(faces, dtype=np.int64).reshape(-1, 4)
    m = len(F)
    tri = F[:, 2] == F[:, 3]
    src = np.stack([F[:, 0], F[:, 1], F[:, 2], F[:, 3]], 1)
    dst = np.stack([F[:, 1], F[:, 2], np.where(tri, F[:, 0], F[:, 3]), F[:, 0]], 1)
    valid = np.ones((m, 4), dtype=bool)
    valid[tri, 3] = False
    fi = np.repeat(np.arange(m, dtype=np.int64), 4).reshape(m, 4)
    sel = valid.ravel()
    return src.ravel()[sel], dst.ravel()[sel], fi.ravel()[sel]


def _orient_and_split(points, faces, poly_src, corner_loop, point_src):
    """Consistently orient each edge-connected component (flip faces where
    needed, majority keeps the source winding) and split vertices so faces
    meet only across edges shared by exactly two faces. Returns updated
    arrays plus (component id per face, flipped face count, split count).

    Performance: the edge table, face adjacency, flips and edge counts are
    built with numpy (the adjacency keeps the old insertion order, so the
    walk is the same); the per-vertex fan check runs only on the vertices a
    vectorized corner labelling finds non-manifold. Output identical to the
    old all-Python version."""
    m = len(faces)
    src, dst, efi = _edge_arrays(faces)
    nv_all = int(max(len(points), int(src.max(initial=-1)) + 1, int(dst.max(initial=-1)) + 1))
    lo, hi = np.minimum(src, dst), np.maximum(src, dst)
    key = lo * nv_all + hi
    order = np.argsort(key, kind="stable")
    key_s = key[order]
    uniq, first, count = np.unique(key_s, return_index=True, return_counts=True)
                                                                          
                                                                   
    two = count == 2
    i0 = order[first[two]]
    i1 = order[first[two] + 1]
    by_first = np.argsort(i0, kind="stable")
    i0, i1 = i0[by_first], i1[by_first]
    f0, f1 = efi[i0].tolist(), efi[i1].tolist()
    diff = ((src[i0] < dst[i0]) == (src[i1] < dst[i1])).tolist()
    adj = [[] for _ in range(m)]
    for x, y, d in zip(f0, f1, diff):
        adj[x].append((y, d))                                    
        adj[y].append((x, d))
    flip = np.zeros(m, dtype=bool)
    comp = -np.ones(m, dtype=np.int64)
    flipped_total = 0
    for seed in range(m):
        if comp[seed] >= 0:
            continue
        cid = seed
        comp[seed] = cid
        stack, members = [seed], [seed]
        while stack:
            f = stack.pop()
            for g, must_differ in adj[f]:
                want = flip[f] ^ must_differ
                if comp[g] < 0:
                    comp[g] = cid
                    flip[g] = want
                    stack.append(g); members.append(g)
        members = np.array(members)
        if flip[members].sum() * 2 > len(members):                                  
            flip[members] = ~flip[members]
        flipped_total += int(flip[members].sum())
    faces = np.array(faces, copy=True)
    corner_loop = np.array(corner_loop, copy=True)
    fl = np.flatnonzero(flip)
    if len(fl):
        rows, cs = faces[fl], corner_loop[fl]
        tri = rows[:, 2] == rows[:, 3]
        nrows, ncs = rows[:, ::-1].copy(), cs[:, ::-1].copy()
        nrows[tri] = np.stack([rows[tri, 0], rows[tri, 2], rows[tri, 1], rows[tri, 1]], 1)
        ncs[tri] = np.stack([cs[tri, 0], cs[tri, 2], cs[tri, 1], -np.ones(int(tri.sum()), dtype=cs.dtype)], 1)
        faces[fl], corner_loop[fl] = nrows, ncs
                                                                              
                                                                       
                                                                         
    F = faces.astype(np.int64)
    cf = np.repeat(np.arange(m, dtype=np.int64), 4)
    cv = F.ravel()
    ck = np.unique(cf * nv_all + cv)                                              
    lab = np.arange(len(ck), dtype=np.int64)
    ea, eb = lo[i0], hi[i0]                                                                
    ja = np.concatenate([np.searchsorted(ck, efi[i0] * nv_all + ea), np.searchsorted(ck, efi[i0] * nv_all + eb)])
    jb = np.concatenate([np.searchsorted(ck, efi[i1] * nv_all + ea), np.searchsorted(ck, efi[i1] * nv_all + eb)])
    for _ in range(256):
        before = lab.copy()
        mn = np.minimum(lab[ja], lab[jb])
        np.minimum.at(lab, ja, mn)
        np.minimum.at(lab, jb, mn)
        lab = lab[lab]                                                     
        if np.array_equal(lab, before):
            break
    cvert = ck % nv_all
    pairs_vl = np.unique(cvert * len(ck) + lab)
    nlab = np.bincount((pairs_vl // len(ck)).astype(np.int64), minlength=nv_all)
    flagged = set(np.flatnonzero(nlab > 1).tolist())
    new_points = points
    new_src = list(point_src)
    extra_points = []
    splits = 0
    remap = {}
    if flagged:
                                                                             
                                                                
        seen, walk = set(), []
        for row in faces.tolist():
            for v in set(row):
                if v in flagged and v not in seen:
                    seen.add(v); walk.append(v)
        fl_arr = np.array(sorted(flagged), dtype=np.int64)
        touch = np.isin(src, fl_arr) | np.isin(dst, fl_arr)
        edge_faces, edge_count = {}, {}
        for x, y, f in zip(src[touch].tolist(), dst[touch].tolist(), efi[touch].tolist()):
            k = (min(x, y), max(x, y))
            edge_faces.setdefault(k, []).append((f, x < y))
        for k, lst in edge_faces.items():
            edge_count[k] = len(lst)
        corner_rows = np.flatnonzero(np.isin(F, fl_arr).any(axis=1))
        incident = {}
        for fi in corner_rows.tolist():
            for v in set(faces[fi].tolist()):
                if v in flagged:
                    incident.setdefault(v, []).append(fi)
        for v in walk:
            fs = incident.get(v, [])
            if len(fs) == 1:
                continue
            parent = {f: f for f in fs}
            def find(x):
                while parent[x] != x:
                    parent[x] = parent[parent[x]]
                    x = parent[x]
                return x
            fset = set(fs)
            for f in fs:
                for a2, b2 in _face_edges(faces[f].tolist()):
                    if v not in (a2, b2):
                        continue
                    k = (min(a2, b2), max(a2, b2))
                    if edge_count.get(k) != 2:
                        continue
                    other = [g for g, _ in edge_faces.get(k, []) if g != f and g in fset]
                    for g in other:
                        parent[find(f)] = find(g)
            roots = {}
            for f in fs:
                roots.setdefault(find(f), []).append(f)
            if len(roots) == 1:
                continue
            for r, group in list(roots.items())[1:]:
                nv = len(points) + len(extra_points)
                extra_points.append(points[v])
                new_src.append(point_src[v])
                splits += 1
                for f in group:
                    remap[(f, v)] = nv
    for (f, v), nv in remap.items():
        row = faces[f]
        faces[f] = np.where(row == v, nv, row)
    if extra_points:
        new_points = np.vstack([np.asarray(points), np.asarray(extra_points, dtype=np.float64).reshape(-1, 3)])
    return (np.array(new_points), faces, poly_src, corner_loop, np.array(new_src, dtype=np.int32),
            comp, flipped_total, splits)


def _face_normals(points, faces):
    """Area-weighted normals (length = face area) of (n, 4) rows; a row with
    row[2] == row[3] is a triangle. Vectorized."""
    faces = np.asarray(faces, dtype=np.int64).reshape(-1, 4)
    a, b, c, d = (points[faces[:, k]] for k in range(4))
    n = 0.5 * np.cross(c - a, d - b)                                                   
    tri = faces[:, 2] == faces[:, 3]
    n[tri] = 0.5 * np.cross(b[tri] - a[tri], c[tri] - a[tri])
    return n


def _vertex_edges(faces):
    """Unique undirected edges (m, 2) of (n, 4) rows."""
    faces = np.asarray(faces, dtype=np.int64).reshape(-1, 4)
    tri = faces[:, 2] == faces[:, 3]
    e = np.concatenate([faces[:, [0, 1]], faces[:, [1, 2]],
                        np.where(tri[:, None], faces[:, [2, 0]], faces[:, [2, 3]]),
                        faces[~tri][:, [3, 0]]])
    e = np.sort(e, axis=1)
    e = e[e[:, 0] != e[:, 1]]
    return np.unique(e, axis=0)


def _vertex_normals(points, faces):
    """Unit area-weighted vertex normals, and per vertex the smallest cosine
    between that normal and its faces' normals."""
    faces = np.asarray(faces, dtype=np.int64).reshape(-1, 4)
    fn = _face_normals(points, faces)
    tri = faces[:, 2] == faces[:, 3]
    vn = np.zeros_like(points)
    for k in range(4):
        use = ~tri if k == 3 else np.ones(len(faces), bool)
        np.add.at(vn, faces[use, k], fn[use])
    vn = _unit_rows(vn)
    fu = _unit_rows(fn)
    min_cos = np.ones(len(points))
    for k in range(4):
        use = ~tri if k == 3 else np.ones(len(faces), bool)
        v = faces[use, k]
        np.minimum.at(min_cos, v, np.einsum("ij,ij->i", vn[v], fu[use]))
    return vn, min_cos


def _unit_rows(v, fallback=(0.0, 0.0, 1.0)):
    ln = np.linalg.norm(v, axis=1)
    ok = ln > 1e-18
    out = np.empty_like(v)
    out[ok] = v[ok] / ln[ok, None]
    out[~ok] = fallback
    return out


                                                                             
                                                                             
                                                                              
                                                                              
                                                                            
SHELL_SMOOTH = 2.0
SHELL_MITER = 1.5
SHELL_MIN = 0.1                                               
SHELL_FOLD_ITERS = 16


def _shell_offsets(points, faces, thickness):
    """Inner-wall positions for the vertices of one open component (rows
    (n, 4), face winding = outside). Returns {vertex: point}."""
    faces = np.asarray(faces, dtype=np.int64).reshape(-1, 4)
    verts = np.unique(faces.ravel())
    vn, min_cos = _vertex_normals(points, faces)
    edges = _vertex_edges(faces)
    if len(edges) == 0:
        return {int(v): points[v] - vn[v] * thickness for v in verts}
    elen = np.linalg.norm(points[edges[:, 0]] - points[edges[:, 1]], axis=1)
    mean_e = float(np.mean(elen)) if len(elen) else thickness
                                                                     
    iters = int(np.clip((SHELL_SMOOTH * thickness / max(mean_e, 1e-12)) ** 2, 0, 60))
    sn = vn.copy()
    for _ in range(iters):
        acc = sn.copy()
        np.add.at(acc, edges[:, 0], sn[edges[:, 1]])
        np.add.at(acc, edges[:, 1], sn[edges[:, 0]])
        sn = _unit_rows(acc)
                                                                          
    flip = np.einsum("ij,ij->i", sn, vn) < 0.2
    sn[flip] = vn[flip]
    cos = np.clip(np.einsum("ij,ij->i", sn, vn) * np.maximum(min_cos, 0.0), 1.0 / SHELL_MITER, 1.0)
    d = thickness / cos
                                                                          
                                                                       
                                                                             
    a, b = edges[:, 0], edges[:, 1]
    outer = points[b] - points[a]
    o2 = np.einsum("ij,ij->i", outer, outer)
    fo = _face_normals(points, faces)
    fo2 = np.einsum("ij,ij->i", fo, fo)
    for _ in range(SHELL_FOLD_ITERS):
        inner = points - sn * d[:, None]
        ie = inner[b] - inner[a]
        bad_e = np.einsum("ij,ij->i", ie, outer) < 0.25 * o2
        bad_f = np.einsum("ij,ij->i", _face_normals(inner, faces), fo) < 0.1 * fo2
        if not bad_e.any() and not bad_f.any():
            break
        shrink = np.ones(len(points))
        np.minimum.at(shrink, a[bad_e], 0.75)
        np.minimum.at(shrink, b[bad_e], 0.75)
        for k in range(4):
            np.minimum.at(shrink, faces[bad_f, k], 0.75)
        d *= shrink
    d = np.maximum(d, SHELL_MIN * thickness)
    inner = points - sn * d[:, None]
    return {int(v): inner[v] for v in verts}


def _vertex_normals_legacy(points, faces):
    """The v1 angle-weighted vertex normals (THICKNESS_LOGICS 'LEGACY')."""
    vn = np.zeros_like(points)
    fn_all = []
    for row in faces:
        r = row[:3] if row[2] == row[3] else row
        p = points[r]
        n = _newell_normal(p)
        ln = np.linalg.norm(n)
        n = n / ln if ln > 1e-18 else n
        fn_all.append(n)
        k = len(r)
        for i in range(k):
            a = p[(i + 1) % k] - p[i]
            b = p[(i - 1) % k] - p[i]
            la, lb = np.linalg.norm(a), np.linalg.norm(b)
            if la < 1e-18 or lb < 1e-18:
                continue
            ang = np.arccos(np.clip(np.dot(a, b) / (la * lb), -1.0, 1.0))
            vn[r[i]] += n * ang
    ln = np.linalg.norm(vn, axis=1)
    ok = ln > 1e-18
    vn[ok] /= ln[ok, None]
    vn[~ok] = (0, 0, 1)
    min_cos = np.ones(len(points))
    for row, n in zip(faces, fn_all):
        r = row[:3] if row[2] == row[3] else row
        for v in r:
            min_cos[v] = min(min_cos[v], float(np.dot(vn[v], n)))
    return vn, min_cos


def _signed_volume(points, faces):
    faces = np.asarray(faces, dtype=np.int64).reshape(-1, 4)
    a, b, c, d = (points[faces[:, k]] for k in range(4))
    v = np.einsum("ij,ij->i", a, np.cross(b, c))
    quad = faces[:, 2] != faces[:, 3]
    v[quad] += np.einsum("ij,ij->i", a[quad], np.cross(c[quad], d[quad]))
    return float(v.sum()) / 6.0


HOLE_MODES = ("AUTO", "FILL", "SHELL")
                                                
                                                                  
                                                                        
                                                                      
                                          
THICKNESS_LOGICS = ("IMPROVED", "LEGACY")
                                                                          
                                                                             
                                                                    
CAP_AREA_SHARE = 0.25


def _boundary_loops(rows):
    """Boundary edges of one component (rows in face winding) chained into
    vertex loops that follow the face winding. None when the boundary is not a
    set of simple loops (a vertex with two outgoing boundary edges)."""
    use = {}
    for row in rows:
        for a, b in _face_edges(row):
            k = (min(a, b), max(a, b))
            use[k] = use.get(k, 0) + 1
    nxt = {}
    for row in rows:
        for a, b in _face_edges(row):
            if use[(min(a, b), max(a, b))] == 1:
                if a in nxt:
                    return None
                nxt[a] = b
    loops, seen = [], set()
    for start in nxt:
        if start in seen:
            continue
        loop = [start]
        seen.add(start)
        v = nxt[start]
        while v != start:
            if v in seen or v not in nxt:
                return None
            loop.append(v)
            seen.add(v)
            v = nxt[v]
        loops.append(loop)
    return loops


def _face_area_sum(points, rows):
    rows = np.asarray(rows, dtype=np.int64)
    a, b, c, d = (points[rows[:, k]] for k in range(4))
    area = 0.5 * np.linalg.norm(np.cross(b - a, c - a), axis=1)
    quad = rows[:, 2] != rows[:, 3]
    area[quad] += 0.5 * np.linalg.norm(np.cross(c[quad] - a[quad], d[quad] - a[quad]), axis=1)
    return float(area.sum())


def _cap_component(points, rows, holes):
    """Cap faces (rows, plus new centre points) that close one open component,
    or None when it should be thickened instead. Small loops are ear-clipped,
    big ones fanned from their centroid; caps run against the face winding."""
    if holes == "SHELL":
        return None
    loops = _boundary_loops(rows)
    if not loops:
        return None
    cap_area = sum(0.5 * np.linalg.norm(_newell_normal(points[np.array(l)])) for l in loops)
    if holes == "AUTO" and cap_area > CAP_AREA_SHARE * _face_area_sum(points, rows):
        return None
    caps, centres = [], []
    base = len(points)
    edges = set()
    for row in rows:
        for a, b in _face_edges(row):
            edges.add((min(a, b), max(a, b)))
    for loop in loops:
        rev = loop[::-1]
        if len(rev) == 3 and (min(rev[0], rev[2]), max(rev[0], rev[2])) not in edges:
            caps.append([rev[0], rev[1], rev[2], rev[2]])
            continue
        if 3 < len(rev) <= 120:
            tris = ear_clip(points[np.array(rev)])
            rows_t = [[rev[i], rev[j], rev[k], rev[k]] for i, j, k in tris]
            chords = set()
            for row in rows_t:
                for a, b in _face_edges(row):
                    chords.add((min(a, b), max(a, b)))
            loop_edges = {(min(rev[i], rev[i - 1]), max(rev[i], rev[i - 1])) for i in range(len(rev))}
            if len(tris) == len(rev) - 2 and not ((chords - loop_edges) & edges):
                caps += rows_t
                continue
        ci = base + len(centres)
        centres.append((points[np.array(loop)].mean(axis=0), loop[0]))
        caps += [[ci, rev[i], rev[(i + 1) % len(rev)], rev[(i + 1) % len(rev)]] for i in range(len(rev))]
    closed = list(rows) + caps
    use = {}
    for row in closed:
        for a, b in _face_edges(row):
            k = (min(a, b), max(a, b))
            use[k] = use.get(k, 0) + 1
    if any(c != 2 for c in use.values()):
        return None
    return caps, centres, len(loops)



def _flip_rows(F, C, members):
    for fi in members:
        row, c = F[fi], C[fi]
        if row[2] == row[3]:
            F[fi] = [row[0], row[2], row[1], row[1]]
            C[fi] = [c[0], c[2], c[1], -1]
        else:
            F[fi] = row[::-1]
            C[fi] = c[::-1]


def _solidify(points, faces, poly_src, corner_loop, point_src, comp, thickness, flip_side,
              holes="AUTO", stats=None, thickness_logic="IMPROVED"):
    """Close every OPEN component: cap its holes when they are small next to
    its surface (`holes`, see HOLE_MODES), otherwise thicken it into a closed
    shell. Closed components are made outward-facing. Returns the assembled
    arrays; `stats` (a dict) counts capped holes and thickened components.

    Performance: components are grouped once (stable argsort) and each one is
    processed in LOCAL vertex indices (only its own points), so the work per
    component is proportional to its size - not to the whole mesh. The output
    is identical to the previous per-component full-mesh version (same order
    of points and faces), which was quadratic in the number of components
    (2,234 wall pieces: minutes and several GB)."""
    points = np.asarray(points, dtype=np.float64)
    n_points = len(points)
    new_pts, new_psrc = [], []
    psrc_base = np.asarray(point_src)
    F, S, C = faces.tolist(), poly_src.tolist(), corner_loop.tolist()
    stats = {} if stats is None else stats
    stats.setdefault("capped", 0)
    stats.setdefault("shelled", 0)

    def flip(fi):
        row, c = F[fi], C[fi]
        if row[2] == row[3]:
            F[fi] = [row[0], row[2], row[1], row[1]]
            C[fi] = [c[0], c[2], c[1], -1]
        else:
            F[fi] = row[::-1]
            C[fi] = c[::-1]

    comp = np.asarray(comp)
    order = np.argsort(comp, kind="stable")
    cuts = np.flatnonzero(np.diff(comp[order])) + 1
    for members in np.split(order, cuts):
        if len(members) == 0:
            continue
        sub = faces[members]
        verts = np.unique(sub[:, :4].ravel())
        lsub = np.searchsorted(verts, sub).astype(np.int64)                   
        lpts = points[verts]
        cap = _cap_component(lpts, lsub.tolist(), holes)
        if cap is not None:
            caps, centres, n_loops = cap
            base_global = n_points + len(new_pts)
            nl = len(verts)
            for centre, corner in centres:
                new_pts.append(centre)
                new_psrc.append(int(psrc_base[verts[corner]]) if corner < nl else int(new_psrc[corner - nl]))
            def to_global(i):
                return int(verts[i]) if i < nl else base_global + (i - nl)
            first_cap = len(F)
            for row in caps:
                F.append([to_global(int(i)) for i in row])
                S.append(-1)
                C.append([-1, -1, -1, -1])
            ext = np.vstack([lpts, np.array([c for c, _ in centres], dtype=np.float64).reshape(-1, 3)])
            all_rows = np.vstack([lsub, np.asarray(caps, dtype=np.int64).reshape(-1, 4)]) if caps else lsub
            if _signed_volume(ext, all_rows) < 0:
                _flip_rows(F, C, list(members) + list(range(first_cap, len(F))))
            stats["capped"] += n_loops
            continue
        edge_use = {}
        for row in sub.tolist():
            for a, b in _face_edges(row):
                k = (min(a, b), max(a, b))
                edge_use[k] = edge_use.get(k, 0) + 1
        open_edges = [k for k, c in edge_use.items() if c == 1]
        if not open_edges:
            if _signed_volume(lpts, lsub) < 0:                                        
                for fi in members:
                    flip(fi)
            continue
        stats["shelled"] += 1
        if flip_side:                                                                 
            for fi in members:
                flip(fi)
        cur = np.array([F[fi] for fi in members], dtype=np.int64)
        lcur = np.searchsorted(verts, cur).astype(np.int64)
        if thickness_logic == "LEGACY":
            vn, min_cos = _vertex_normals_legacy(lpts, lcur)
            inner = {int(v): lpts[li] - vn[li] * (thickness / max(0.35, min_cos[li])) for li, v in enumerate(verts)}
        else:
            linner = _shell_offsets(lpts, lcur, thickness)
            inner = {int(verts[li]): q for li, q in linner.items()}
        back = {}
        for v in verts:
            back[int(v)] = n_points + len(new_pts)
            new_pts.append(inner[int(v)])
            new_psrc.append(int(psrc_base[v]))
        for row in cur.tolist():
            if row[2] == row[3]:
                F.append([back[row[0]], back[row[2]], back[row[1]], back[row[1]]])
            else:
                F.append([back[row[3]], back[row[2]], back[row[1]], back[row[0]]])
            S.append(-1); C.append([-1, -1, -1, -1])
        for row in cur.tolist():
            for a, b in _face_edges(row):
                if edge_use[(min(a, b), max(a, b))] != 1:
                    continue
                F.append([a, back[a], back[b], b])                                              
                S.append(-1); C.append([-1, -1, -1, -1])
    all_pts = np.vstack([points, np.asarray(new_pts, dtype=np.float64).reshape(-1, 3)]) if new_pts else points.copy()
    all_psrc = np.concatenate([psrc_base.astype(np.int32), np.asarray(new_psrc, dtype=np.int32)])
    return (all_pts, np.array(F, dtype=np.int32), np.array(S, dtype=np.int32),
            np.array(C, dtype=np.int32), all_psrc)


def prepare(points, loop_verts, loop_start, loop_total, mode="AUTO", weld=1e-3,
            thickness=1.0, flip_side=False, loop_tris=None, holes="AUTO",
            thickness_logic="IMPROVED"):
    """Source arrays (core units) -> Prepared. Raises ValueError when no valid
    solid can be made (the caller reports it; the source is never touched)."""
    if mode not in MODES:
        raise ValueError("unknown preparation mode %r" % mode)
    if holes not in HOLE_MODES:
        raise ValueError("unknown hole mode %r" % holes)
    if thickness_logic not in THICKNESS_LOGICS:
        raise ValueError("unknown thickness logic %r" % thickness_logic)
    points = np.asarray(points, dtype=np.float64)
    if len(points) == 0 or len(loop_total) == 0:
        raise ValueError("the mesh has no geometry")
    faces, poly_src, corner_loop, n_ngons = _faces_from_source(points, loop_verts, loop_start,
                                                              loop_total, loop_tris)
    point_src = np.arange(len(points), dtype=np.int32)
    notes = []
    if n_ngons:
        notes.append("%d n-gons triangulated" % n_ngons)
    if mode == "CLOSED" or (mode == "AUTO" and is_closed_manifold(faces)):
        return Prepared(points, faces, poly_src, corner_loop, point_src, "triangulated", notes)

                          
    labels = _weld(points, faces, weld)
    merged = int(np.sum(labels != np.arange(len(points))))
    used = np.unique(labels)
    remap = -np.ones(len(points), dtype=np.int64)
    remap[used] = np.arange(len(used))
    pts = points[used]
    psrc = point_src[used]
    f = remap[labels[faces]].astype(np.int32)
    f, s, c, degenerate, duplicates, pairs = _clean_faces(f, poly_src, corner_loop)
                                                                          
                                                                        
                                                                        
    restored = 0
    if pairs:
                                                                           
                                                                          
                                                                   
        stats_e = _EdgeStats(f)
        for pair in pairs[:64]:
            if stats_e.total == 0:
                break
            for row, src_i, crow in pair:
                if stats_e.gain(row) < 0:
                    stats_e.add(row)
                    f = np.vstack([f, np.array([row], dtype=np.int32)])
                    s = np.append(s, np.int32(src_i))
                    c = np.vstack([c, np.array([crow], dtype=np.int32)])
                    restored += 1
                    duplicates -= 1
                    break
    if merged:
        notes.append("%d coincident vertices welded" % merged)
    if degenerate:
        notes.append("%d degenerate faces dropped" % degenerate)
    if duplicates:
        notes.append("%d duplicate / double-wall faces dropped" % duplicates)
    if len(f) == 0:
        raise ValueError("nothing is left after removing degenerate faces")
    if mode == "AUTO" and is_closed_manifold(f):
        return Prepared(pts, f, s, c, psrc, "repaired", notes)

                                                                      
    pts, f, s, c, psrc, comp, flipped, splits = _orient_and_split(pts, f, s, c, psrc)
    if flipped:
        notes.append("%d faces re-oriented for consistent normals" % flipped)
    if splits:
        notes.append("%d vertices split at non-manifold edges/vertices" % splits)
    if thickness <= 0:
        raise ValueError("surface mode needs a positive thickness")
    stats = {}
    pts, f, s, c, psrc = _solidify(pts, f, s, c, psrc, comp, thickness, flip_side,
                                   holes=holes, stats=stats, thickness_logic=thickness_logic)
    if not is_closed_manifold(f):
        b, nm, mo = edge_report(f)
        raise ValueError("could not build a closed solid from this surface "
                         "(%d open, %d non-manifold, %d misoriented edges remain)" % (b, nm, mo))
    if stats["capped"]:
        notes.append("%d holes capped to make a solid (cap faces are removed again)" % stats["capped"])
    if stats["shelled"]:
        notes.append("open surface thickened by %.3g cm (helper faces are removed again)" % thickness)
    return Prepared(pts, f, s, c, psrc, "surface", notes)
