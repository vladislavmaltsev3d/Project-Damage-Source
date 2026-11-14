# готовим тонкую ткань к разрезанию, сохраняя её исходную форму
# ProjectDamage for Blender - Fabric Solidify input preparation.
# GPL-3.0-or-later; see LICENSE.
"""Open cloth -> a closed temporary shell for Fabric (Solidify).

Fabric Solidify used to hand the mesh to the core in mode 'CLOSED', so any
garment that was not already a closed solid (an open shirt, a plane, a
double-layer garment whose two layers are not joined at the hems) passed
through unchanged with "the mesh is not a closed solid".

`prepare()` here closes the cloth piece by piece instead:

1. loose vertices / edges are dropped (only face corners are kept),
2. edges shared by more than two faces are split (each fan of faces that is
   joined through ordinary two-face edges keeps its own copy of the vertex),
3. duplicate vertices are welded ONLY on open boundaries, with a tolerance
   relative to the local edge length - never a global merge, which would
   fuse the two layers of a double-layer garment,
4. n-gons are triangulated (by _faces_from_source),
5. normals are made consistent per piece,
6. double-layer pairs (two open pieces with the same face count lying less
   than PAIR_DIST x thickness apart) keep one layer; that layer is shelled
   with the measured gap, so the result keeps the garment's own thickness,
7. every other open piece is shelled with the fabric thickness,
8. every piece is checked with is_closed_manifold; a piece that still fails
   is left out of the core input and appended back unchanged to the result
   (FabricPrepared.finish), so one bad piece no longer blocks the garment.

Closed pieces (buttons) are only made outward-facing.

The back sheet and the rim walls are NOT helper faces: they map to the front
face they were offset from (material, UVs, groups), and they stay in the
result - Fabric Solidify returns a cloth with thickness.

Nothing here imports bpy; every step is testable outside Blender.
"""
import numpy as np

from . import mesh_prep as _mp

WELD_REL = 0.02         # boundary weld tolerance as a share of the median edge length
PAIR_DIST = 2.0         # double layer: mean gap below PAIR_DIST x thickness
PAIR_SAMPLES = 256      # sampled vertices per layer when measuring the gap
PAIR_MAX_GROUP = 12     # candidate pieces with the same face count tested pairwise
UNTOUCHED_CLASS = 6     # tri_class of untouched source faces (see Prepared.ngon_groups)


class FabricPrepared(_mp.Prepared):
    """Prepared plus the pieces that could not be closed (passed through)
    and the per-face role of the shell (front / back / wall)."""

    def __init__(self, *args, role=None, passthrough=None, stats=None, **kwargs):
        super().__init__(*args, **kwargs)
        self.role = np.zeros(len(self.polys), np.int8) if role is None else np.asarray(role, np.int8)
        self.passthrough = passthrough
        self.stats = dict(stats or {})
        self._loop_uv = None
        self._front_result = None

    def uvs(self, loop_uv):
        self._loop_uv = np.asarray(loop_uv, dtype=np.float64)
        return super().uvs(loop_uv)

    def finish(self, out, point_keys=()):
        if out.get("passthrough") or "polys" not in out:
            return out
        base = out.get("base_poly")
        front = None
        if base is not None and len(np.asarray(base)) == len(np.asarray(out["polys"])):
            b = np.asarray(base)
            front = np.ones(len(b), dtype=bool)
            t = (b >= 0) & (b < len(self.role))
            front[t] = self.role[b[t]] == 0
        res = super().finish(out, point_keys)
        if self.passthrough is not None and len(self.passthrough["rows"]):
            res = self._append_passthrough(res, point_keys)
            if front is not None:
                front = np.concatenate([front, np.ones(len(self.passthrough["rows"]), dtype=bool)])
        self._front_result = front
        return res

    def _append_passthrough(self, res, point_keys):
        pt = self.passthrough
        rows, src, corners, pts = pt["rows"], pt["poly_src"], pt["corner_loop"], pt["points"]
        n_pts = len(np.asarray(res["points"]))
        n_polys = len(np.asarray(res["polys"]))
        k = len(rows)
        res = dict(res)
        res["points"] = np.vstack([np.asarray(res["points"], dtype=np.float64).reshape(-1, 3), pts])
        res["polys"] = np.vstack([np.asarray(res["polys"]).reshape(-1, 4), rows + n_pts]).astype(np.int32)
        for key in self.POLY_KEYS:
            if key == "polys" or res.get(key) is None:
                continue
            arr = np.asarray(res[key])
            if len(arr) != n_polys:
                continue
            if key == "base_poly":
                extra = src.astype(arr.dtype)
            elif key == "tri_class":
                extra = np.full(k, UNTOUCHED_CLASS, dtype=arr.dtype)
            elif key == "uvs" and self._loop_uv is not None:
                extra = np.zeros((k,) + arr.shape[1:], dtype=arr.dtype)
                ok = corners >= 0
                uv = np.zeros((k, 4, 2))
                uv[ok] = self._loop_uv[corners[ok]]
                extra = uv.reshape((k,) + arr.shape[1:]).astype(arr.dtype)
            else:
                extra = np.zeros((k,) + arr.shape[1:], dtype=arr.dtype)
            res[key] = np.concatenate([arr, extra])
        for key in point_keys:
            v = res.get(key)
            if v is not None and len(np.asarray(v)) == n_pts:
                v = np.asarray(v)
                res[key] = np.concatenate([v, np.zeros((len(pts),) + v.shape[1:], dtype=v.dtype)])
        if "counts" in res and "tri_class" in res:
            n_chip = int(np.sum(np.asarray(res["tri_class"]) == 0))
            res["counts"] = (len(res["points"]), len(res["polys"]), n_chip)
        res["passthrough_pieces"] = int(pt["pieces"])
        return res

    def ngon_groups(self, out):
        """Only FRONT faces are dissolved back into n-gons - the back sheet
        traces to the same source polygon and must stay separate."""
        base = out.get("base_poly")
        tri_class = out.get("tri_class")
        if base is None or tri_class is None:
            return {}
        base = np.asarray(base)
        front = self._front_result
        if front is None or len(front) != len(base):
            return {}
        expected = {}
        for p in self.poly_src[(self.poly_src >= 0) & (self.role == 0)]:
            expected[int(p)] = expected.get(int(p), 0) + 1
        if self.passthrough is not None:
            for p in self.passthrough["poly_src"]:
                expected[int(p)] = expected.get(int(p), 0) + 1
        groups = {}
        for f in np.flatnonzero((base >= 0) & (np.asarray(tri_class) == UNTOUCHED_CLASS) & front):
            groups.setdefault(int(base[f]), []).append(int(f))
        return {p: fs for p, fs in groups.items()
                if expected.get(p, 0) > 1 and len(fs) == expected[p]}


# --------------------------------------------------------------------------
# topology helpers

def edge_table(faces):
    """Per face-edge occurrence: (lo, hi, face index, unique edge id, faces
    per unique edge). Rows (n, 4); row[2] == row[3] is a triangle."""
    f = np.asarray(faces, dtype=np.int64).reshape(-1, 4)
    tri = f[:, 2] == f[:, 3]
    a = np.concatenate([f[:, 0], f[:, 1], f[:, 2], f[~tri, 3]])
    b = np.concatenate([f[:, 1], f[:, 2], np.where(tri, f[:, 0], f[:, 3]), f[~tri, 0]])
    fi = np.concatenate([np.arange(len(f))] * 3 + [np.flatnonzero(~tri)])
    lo, hi = np.minimum(a, b), np.maximum(a, b)
    key = lo * (int(hi.max(initial=0)) + 1) + hi
    _uniq, inv, cnt = np.unique(key, return_inverse=True, return_counts=True)
    return lo, hi, fi, inv.ravel(), cnt


def topology_report(faces):
    """(open edges, non-manifold edges) of rows - for the user messages."""
    if len(faces) == 0:
        return 0, 0
    _lo, _hi, _fi, _inv, cnt = edge_table(faces)
    return int(np.sum(cnt == 1)), int(np.sum(cnt > 2))


def _misoriented(faces, lo, hi, inv, cnt):
    """Per face-edge occurrence: True on a two-face edge whose faces run it
    in the SAME direction (orientation conflict)."""
    f = np.asarray(faces, dtype=np.int64).reshape(-1, 4)
    tri = f[:, 2] == f[:, 3]
    a = np.concatenate([f[:, 0], f[:, 1], f[:, 2], f[~tri, 3]])
    fwd = (a == lo).astype(np.int64)
    s = np.zeros(len(cnt), np.int64)
    np.add.at(s, inv, fwd)
    return (cnt[inv] == 2) & (s[inv] != 1)


def split_nonmanifold_edges(points, faces, point_src, include_misoriented=False):
    """Split every vertex of an edge used by more than two faces (and, with
    include_misoriented, of a two-face edge whose faces disagree on its
    direction) into one copy per fan of faces joined through ordinary
    two-face edges. Returns (points, faces, point_src, split edge count, new
    vertex count, set of vertices that were split or created - excluded
    from the boundary weld)."""
    faces = np.asarray(faces, dtype=np.int64).reshape(-1, 4).copy()
    lo, hi, fi, inv, cnt = edge_table(faces)
    per = cnt[inv]
    bad = per > 2
    if include_misoriented:
        bad = bad | _misoriented(faces, lo, hi, inv, cnt)
    if not bad.any():
        return points, faces.astype(np.int32), point_src, 0, 0, set()
    n_edges = len(np.unique(inv[bad]))
    verts = set(np.concatenate([lo[bad], hi[bad]]).tolist())
    edge_faces = {}
    incident = {}
    for l, h, f, c, b in zip(lo.tolist(), hi.tolist(), fi.tolist(), per.tolist(), bad.tolist()):
        if l in verts or h in verts:
            if c == 2 and not b:
                edge_faces.setdefault((l, h), []).append(f)
            if l in verts:
                incident.setdefault(l, set()).add(f)
            if h in verts:
                incident.setdefault(h, set()).add(f)
    new_points = [np.asarray(points, dtype=np.float64)]
    new_src = list(np.asarray(point_src).tolist())
    n0 = len(points)
    added = 0
    touched = set(verts)
    for v, fs in incident.items():
        fs = sorted(fs)
        parent = {f: f for f in fs}

        def find(x):
            while parent[x] != x:
                parent[x] = parent[parent[x]]
                x = parent[x]
            return x
        for (l, h), pair in edge_faces.items():
            if v != l and v != h:
                continue
            if len(pair) == 2 and pair[0] in parent and pair[1] in parent:
                parent[find(pair[0])] = find(pair[1])
        groups = {}
        for f in fs:
            groups.setdefault(find(f), []).append(f)
        if len(groups) < 2:
            continue
        for group in list(groups.values())[1:]:
            nv = n0 + added
            new_points.append(np.asarray(points[v], dtype=np.float64).reshape(1, 3))
            new_src.append(new_src[v])
            added += 1
            touched.add(nv)
            for f in group:
                row = faces[f]
                faces[f] = np.where(row == v, nv, row)
    pts = np.vstack(new_points)
    return pts, faces.astype(np.int32), np.asarray(new_src, dtype=np.int32), n_edges, added, touched


def median_edge_length(points, faces):
    lo, hi, _fi, _inv, _cnt = edge_table(faces)
    if len(lo) == 0:
        return 0.0
    return float(np.median(np.linalg.norm(points[lo] - points[hi], axis=1)))


def weld_boundary(points, faces, tol, exclude=()):
    """Labels (N,) merging only open-boundary vertices closer than ~tol."""
    labels = np.arange(len(points))
    if tol <= 0 or len(faces) == 0:
        return labels, 0
    lo, hi, _fi, inv, cnt = edge_table(faces)
    b = cnt[inv] == 1
    bv = np.unique(np.concatenate([lo[b], hi[b]]))
    if exclude:
        bv = bv[~np.isin(bv, np.fromiter(exclude, dtype=np.int64))]
    if len(bv) < 2:
        return labels, 0
    sub = _mp._weld(points[bv], None, tol)
    labels[bv] = bv[sub]
    return labels, int(np.sum(labels != np.arange(len(points))))


def _compact(points, faces, point_src):
    used = np.unique(np.asarray(faces).ravel())
    remap = -np.ones(len(points), dtype=np.int64)
    remap[used] = np.arange(len(used))
    return points[used], remap[faces].astype(np.int32), np.asarray(point_src)[used], len(points) - len(used)


def _nearest_dist(a, b, chunk=64):
    """Distance from each row of a to its nearest row of b, and that row."""
    d_out = np.empty(len(a))
    i_out = np.empty(len(a), dtype=np.int64)
    for s in range(0, len(a), chunk):
        q = a[s:s + chunk]
        d2 = ((q[:, None, :] - b[None, :, :]) ** 2).sum(axis=2)
        i = np.argmin(d2, axis=1)
        i_out[s:s + chunk] = i
        d_out[s:s + chunk] = np.sqrt(d2[np.arange(len(q)), i])
    return d_out, i_out


def _sample(n, k=PAIR_SAMPLES):
    return np.unique(np.linspace(0, n - 1, min(n, k)).astype(np.int64))


def _flip_rows(rows, corners):
    rows = rows.copy(); corners = corners.copy()
    tri = rows[:, 2] == rows[:, 3]
    r, c = rows[tri], corners[tri]
    rows[tri] = np.stack([r[:, 0], r[:, 2], r[:, 1], r[:, 1]], 1)
    corners[tri] = np.stack([c[:, 0], c[:, 2], c[:, 1], -np.ones(len(c), c.dtype)], 1)
    rows[~tri] = rows[~tri][:, ::-1]
    corners[~tri] = corners[~tri][:, ::-1]
    return rows, corners


def _shell_piece(lpts, rows, src, corners, thickness, thickness_logic, backend):
    """One open piece in LOCAL indices -> (points, rows, src, corners, role)
    of the closed shell: front (role 0), back sheet (1), rim walls (2)."""
    n = len(lpts)
    if thickness_logic == "LEGACY":
        vn, min_cos = backend._vertex_normals_legacy(lpts, rows)
        inner = lpts - vn * (thickness / np.maximum(0.35, min_cos))[:, None]
    else:
        off = backend._shell_offsets(lpts, rows, thickness)
        inner = lpts.copy()
        for v, q in off.items():
            inner[v] = q
    back_rows, back_c = _flip_rows(rows + n, corners)
    lo, hi, fi, inv, cnt = edge_table(rows)
    tri = rows[:, 2] == rows[:, 3]
    walls, wsrc, wc = [], [], []
    # directed face edges in winding order with their corners
    for k in range(4):
        use = np.ones(len(rows), bool) if k < 3 else ~tri
        if k == 2:
            a, b = rows[:, 2], np.where(tri, rows[:, 0], rows[:, 3])
            ca, cb = corners[:, 2], np.where(tri, corners[:, 0], corners[:, 3])
        elif k == 3:
            a, b = rows[:, 3], rows[:, 0]
            ca, cb = corners[:, 3], corners[:, 0]
        else:
            a, b = rows[:, k], rows[:, k + 1]
            ca, cb = corners[:, k], corners[:, k + 1]
        a, b, ca, cb = a[use], b[use], ca[use], cb[use]
        fidx = np.flatnonzero(use)
        lo_k, hi_k = np.minimum(a, b), np.maximum(a, b)
        walls.append((a, b, ca, cb, fidx, lo_k, hi_k))
    key_all = lo * (int(hi.max(initial=0)) + 1) + hi
    open_keys = set(key_all[cnt[inv] == 1].tolist())
    mul = int(hi.max(initial=0)) + 1
    W, WS, WC = [], [], []
    for a, b, ca, cb, fidx, lo_k, hi_k in walls:
        m = np.isin(lo_k * mul + hi_k, np.fromiter(open_keys, dtype=np.int64)) if open_keys else np.zeros(len(a), bool)
        if not m.any():
            continue
        W.append(np.stack([a[m], a[m] + n, b[m] + n, b[m]], 1))
        WS.append(src[fidx[m]])
        WC.append(np.stack([ca[m], ca[m], cb[m], cb[m]], 1))
    W = np.vstack(W) if W else np.zeros((0, 4), np.int64)
    WS = np.concatenate(WS) if WS else np.zeros(0, np.int64)
    WC = np.vstack(WC) if WC else np.zeros((0, 4), np.int64)
    pts = np.vstack([lpts, inner])
    all_rows = np.vstack([rows, back_rows, W])
    all_src = np.concatenate([src, src, WS])
    all_c = np.vstack([corners, back_c, WC])
    role = np.concatenate([np.zeros(len(rows), np.int8), np.ones(len(rows), np.int8), np.full(len(W), 2, np.int8)])
    return pts, all_rows, all_src, all_c, role


def face_islands(faces, n_points):
    """Island id per face: faces connected through shared vertices."""
    f = np.asarray(faces, dtype=np.int64).reshape(-1, 4)
    lab = np.arange(n_points, dtype=np.int64)
    for _ in range(10000):
        fm = lab[f].min(axis=1)
        new = lab.copy()
        for k in range(4):
            np.minimum.at(new, f[:, k], fm)
        new = new[new]
        new = new[new]
        if np.array_equal(new, lab):
            break
        lab = new
    return lab[f[:, 0]]


def _drop_double_layers(pts, faces, poly_src, corner_loop, thickness, n_source_polys):
    """Pairs of source islands with the same face and vertex count lying less
    than PAIR_DIST x thickness apart are two layers of one garment panel:
    the outer one (farther from the garment's centre) is kept. Returns the
    remaining faces, the pair count, the measured gap per SOURCE polygon
    (NaN when unpaired), {kept island: points of the dropped layer} and the
    island id per source polygon."""
    isl = face_islands(faces, len(pts))
    uniq, inv = np.unique(isl, return_inverse=True)
    inv = inv.ravel()
    centre = pts[np.unique(faces.ravel())].mean(axis=0)
    info = []
    for k in range(len(uniq)):
        fidx = np.flatnonzero(inv == k)
        verts = np.unique(faces[fidx].ravel())
        info.append(dict(f=fidx, v=verts))
    by_count = {}
    for k, it in enumerate(info):
        by_count.setdefault((len(it["f"]), len(it["v"])), []).append(k)
    gap_poly = np.full(n_source_polys, np.nan)
    island_of_poly = -np.ones(n_source_polys, dtype=np.int64)
    island_of_poly[poly_src] = inv
    drop = np.zeros(len(faces), dtype=bool)
    partner_of = {}
    pairs = 0
    for group in by_count.values():
        if len(group) < 2 or len(group) > PAIR_MAX_GROUP:
            continue
        free = set(group)
        for ia in group:
            if ia not in free:
                continue
            A = pts[info[ia]["v"]]
            sa = _sample(len(A))
            best = None
            for ib in group:
                if ib == ia or ib not in free:
                    continue
                B = pts[info[ib]["v"]]
                d, _ = _nearest_dist(A[sa], B)
                if float(np.mean(d)) < PAIR_DIST * thickness and (best is None or np.mean(d) < best[0]):
                    best = (float(np.mean(d)), ib, float(np.median(d)))
            if best is None:
                continue
            _mean, ib, med_gap = best
            if med_gap <= 0:
                continue
            free.discard(ia); free.discard(ib)
            ra = np.linalg.norm(pts[info[ia]["v"]] - centre, axis=1).mean()
            rb = np.linalg.norm(pts[info[ib]["v"]] - centre, axis=1).mean()
            keep, gone = (ia, ib) if ra >= rb else (ib, ia)
            drop[info[gone]["f"]] = True
            gap_poly[poly_src[info[keep]["f"]]] = med_gap
            partner_of[int(keep)] = pts[info[gone]["v"]]
            pairs += 1
    keep = ~drop
    return (faces[keep], poly_src[keep], corner_loop[keep], pairs, gap_poly, partner_of, island_of_poly)


def _piece_closed(rows):
    rows = np.asarray(rows, dtype=np.int64)
    return len(rows) > 0 and _mp.is_closed_manifold(rows)


# --------------------------------------------------------------------------

def prepare(points, loop_verts, loop_start, loop_total, thickness=0.1, flip_side=False,
            loop_tris=None, thickness_logic="IMPROVED", weld=0.0, backend=None,
            detect_double_layer=True, path_label="fabric shell", helpers=False):
    """Source arrays (core units, cm) -> FabricPrepared. `thickness` is the
    shell thickness in the same units (already divided by object scale).
    Raises ValueError (with counts) only when NO piece can be closed.

    helpers=True (the AUTO/SURFACE fallback) marks the back sheet and the rim
    walls as helper faces, removed again after the damage like the regular
    surface path; Fabric Solidify keeps them (helpers=False)."""
    backend = backend or _mp
    if thickness <= 0:
        raise ValueError("Fabric Solidify needs a positive thickness")
    points = np.asarray(points, dtype=np.float64)
    if len(points) == 0 or len(loop_total) == 0:
        raise ValueError("the mesh has no geometry")
    faces, poly_src, corner_loop, n_ngons = _mp._faces_from_source(points, loop_verts, loop_start,
                                                                   loop_total, loop_tris)
    stats = dict(ngons=int(n_ngons))
    notes = []
    open0, nm0 = topology_report(faces)
    stats.update(open_edges=open0, nonmanifold_edges=nm0)
    if open0 == 0 and nm0 == 0 and _mp.is_closed_manifold(faces):
        # already a closed solid (a garment with Solidify): the original path
        if n_ngons:
            notes.append("%d n-gons triangulated" % n_ngons)
        stats.update(pieces=0, shelled=0, failed=0, closed=-1, double_layers=0)
        return FabricPrepared(points, faces, poly_src, corner_loop, np.arange(len(points), dtype=np.int32),
                              "triangulated", notes, stats=stats)
    # 1. loose vertices / edges
    pts, f, psrc, loose = _compact(points, faces, np.arange(len(points), dtype=np.int32))
    stats["loose"] = int(loose)
    # 2. edges with more than two faces
    pts, f, psrc, n_split, n_new, touched = split_nonmanifold_edges(pts, f, psrc)
    stats.update(split_edges=n_split, split_verts=n_new)
    # 3. double layers, on the SOURCE islands (before any weld joins them)
    dropped_layers, gap_poly, partner_of, island_of_poly = 0, None, {}, None
    if detect_double_layer:
        (f, poly_src, corner_loop, dropped_layers, gap_poly,
         partner_of, island_of_poly) = _drop_double_layers(pts, f, poly_src, corner_loop, thickness, len(loop_total))
        used = np.unique(np.asarray(f).ravel())
        pts, f, psrc, _ = _compact(pts, f, psrc)
        kept = [t for t in touched if t <= used[-1] and used[min(np.searchsorted(used, t), len(used) - 1)] == t]
        touched = set(np.searchsorted(used, kept).tolist())
    stats["double_layers"] = dropped_layers
    gaps = gap_poly[np.isfinite(gap_poly)] if gap_poly is not None else np.zeros(0)
    # 4. boundary-only weld, size relative, always far below the layer gap
    med = median_edge_length(pts, f)
    tol = max(float(weld), 1e-6 * max(med, 1e-9))
    tol = min(tol, WELD_REL * med, 0.25 * thickness)
    if len(gaps):
        tol = min(tol, 0.25 * float(gaps.min()))
    labels, merged = weld_boundary(pts, f, tol, exclude=touched)
    stats["welded"] = int(merged)
    f = labels[f].astype(np.int32)
    f, s, c, degenerate, duplicates, _pairs = _mp._clean_faces(f, poly_src, corner_loop)
    stats.update(degenerate=int(degenerate), duplicates=int(duplicates))
    if len(f) == 0:
        raise ValueError("nothing is left after removing degenerate faces")
    pts, f, psrc, _ = _compact(pts, f, psrc)
    # 5. consistent normals per piece (also splits leftover non-manifold vertices)
    pts, f, s, c, psrc, comp, flipped, splits = backend._orient_and_split(pts, f, s, c, psrc)
    # orientation conflicts (a seam welded across a twist): cut them open and
    # orient again - the cut becomes a rim of the shell
    conflicts = 0
    for _ in range(3):
        _b, _n, mis = _mp.edge_report(np.asarray(f, dtype=np.int32))
        if not mis:
            break
        pts, f, psrc, n_cut, _added, _t = split_nonmanifold_edges(pts, f, psrc, include_misoriented=True)
        conflicts += n_cut
        pts, f, s, c, psrc, comp, fl2, sp2 = backend._orient_and_split(pts, np.asarray(f, np.int32), s, c, psrc)
        flipped += fl2; splits += sp2
    stats.update(flipped=int(flipped), vertex_splits=int(splits), orientation_cuts=int(conflicts))
    f = np.asarray(f, dtype=np.int64); s = np.asarray(s, dtype=np.int64); c = np.asarray(c, dtype=np.int64)
    comp = np.asarray(comp)
    order = np.argsort(comp, kind="stable")
    cuts = np.flatnonzero(np.diff(comp[order])) + 1
    pieces = []
    gap_of = {}
    for members in np.split(order, cuts):
        if len(members) == 0:
            continue
        rows = f[members]
        verts = np.unique(rows.ravel())
        lrows = np.searchsorted(verts, rows).astype(np.int64)
        lpts = pts[verts]
        lo, hi, _fi, inv, cnt = edge_table(lrows)
        piece = dict(rows=lrows, verts=verts, pts=lpts, src=s[members], corners=c[members],
                     open=int(np.sum(cnt == 1)), nonmanifold=int(np.sum(cnt > 2)), nf=len(members))
        if gap_poly is not None and piece["open"]:
            g = gap_poly[piece["src"]]
            g = g[np.isfinite(g)]
            if len(g):
                piece["gap"] = max(float(np.median(g)), 1e-6)
                gap_of[len(pieces)] = piece["gap"]
                # the dropped layer must end up behind the kept one (inside the shell)
                islands = np.unique(island_of_poly[piece["src"]])
                behind = [partner_of[i] for i in islands.tolist() if i in partner_of]
                if behind:
                    behind = np.vstack(behind)
                    vn, _mc = _mp._vertex_normals(lpts, lrows)
                    sk = _sample(len(lpts))
                    _d, j = _nearest_dist(lpts[sk], behind)
                    side = float(np.mean(np.einsum("ij,ij->i", behind[j] - lpts[sk], vn[sk])))
                    if side > 0:
                        piece["rows"], piece["corners"] = _flip_rows(lrows, piece["corners"])
                        piece["oriented"] = True
        pieces.append(piece)
    stats["pieces"] = len(pieces)
    # 7./8. shell open pieces, check every piece
    out_pts, out_rows, out_src, out_c, out_role, out_psrc = [], [], [], [], [], []
    pass_rows, pass_src, pass_c, pass_pts = [], [], [], []
    base = 0
    pass_base = 0
    shelled = closed_kept = failed = 0
    failed_open = failed_nm = 0
    for p in pieces:
        if p.get("dropped"):
            continue
        rows, src, corners, lpts = p["rows"], p["src"], p["corners"], p["pts"]
        psrc_local = psrc[p["verts"]]
        if p["open"] == 0 and p["nonmanifold"] == 0:
            if _mp._signed_volume(lpts, rows) < 0:
                rows, corners = _flip_rows(rows, corners)
            role = np.zeros(len(rows), np.int8)
            spts, srows, ssrc, sc, spsrc = lpts, rows, src, corners, psrc_local
            closed_kept += 1
        else:
            if flip_side and not p.get("oriented") and "gap" not in p:
                rows, corners = _flip_rows(rows, corners)
            t = p.get("gap", thickness)
            spts, srows, ssrc, sc, role = _shell_piece(lpts, rows, src, corners, t, thickness_logic, backend)
            spsrc = np.concatenate([psrc_local, psrc_local])
            shelled += 1
        if not _piece_closed(srows):
            failed += 1
            o, nm = topology_report(srows)
            failed_open += o; failed_nm += nm
            pass_rows.append(p["rows"] + pass_base)
            pass_src.append(p["src"]); pass_c.append(p["corners"]); pass_pts.append(p["pts"])
            pass_base += len(p["pts"])
            continue
        out_pts.append(spts); out_rows.append(srows + base); out_src.append(ssrc)
        out_c.append(sc); out_role.append(role); out_psrc.append(spsrc)
        base += len(spts)
    stats.update(shelled=shelled, closed=closed_kept, failed=failed,
                 failed_open_edges=failed_open, failed_nonmanifold_edges=failed_nm,
                 thickness=float(thickness))
    if not out_rows:
        raise ValueError(
            "Fabric Solidify could not close any piece of this cloth "
            "(%d open edges, %d non-manifold edges in the source; %d of %d pieces still open "
            "after thickening: %d open, %d non-manifold edges). Use Fabric Classic for this mesh"
            % (open0, nm0, failed, len(pieces) - dropped_layers, failed_open, failed_nm))
    P = np.vstack(out_pts)
    F = np.vstack(out_rows).astype(np.int32)
    S = np.concatenate(out_src).astype(np.int32)
    C = np.vstack(out_c).astype(np.int32)
    R = np.concatenate(out_role)
    PS = np.concatenate(out_psrc).astype(np.int32)
    if helpers:
        # general surface fallback: the shell is temporary (removed by finish)
        S[R > 0] = -1
        C[R > 0] = -1
    passthrough = None
    if pass_rows:
        passthrough = dict(rows=np.vstack(pass_rows).astype(np.int64), poly_src=np.concatenate(pass_src).astype(np.int64),
                           corner_loop=np.vstack(pass_c).astype(np.int64), points=np.vstack(pass_pts), pieces=failed)
    if n_ngons:
        notes.append("%d n-gons triangulated" % n_ngons)
    if loose:
        notes.append("%d loose vertices dropped" % loose)
    if n_split:
        notes.append("%d edges with more than two faces split" % n_split)
    if conflicts:
        notes.append("%d edges with an orientation conflict cut open" % conflicts)
    if merged:
        notes.append("%d boundary vertices welded (tolerance %.2g cm)" % (merged, tol))
    if degenerate:
        notes.append("%d degenerate faces dropped" % degenerate)
    if flipped:
        notes.append("%d faces re-oriented for consistent normals" % flipped)
    if dropped_layers:
        gaps = sorted(gap_of.values())
        notes.append("%d double-layer pairs found: one layer of each kept; %d pieces shelled with the measured gap (%.3g-%.3g cm)"
                     % (dropped_layers, len(gaps), gaps[0], gaps[-1]))
    n_gap = len(gap_of)
    if shelled > n_gap:
        notes.append("%d open pieces thickened by %.3g cm" % (shelled - n_gap, thickness))
    if closed_kept:
        notes.append("%d closed pieces kept" % closed_kept)
    if failed:
        notes.append("%d pieces could not be closed (%d open, %d non-manifold edges) and pass through undamaged"
                     % (failed, failed_open, failed_nm))
    prep = FabricPrepared(P, F, S, C, PS, path_label, notes, role=R, passthrough=passthrough, stats=stats)
    return prep


# Module-level entry points: the background worker (background_worker.py)
# resolves a task by module and function NAME, so the backend choice cannot
# travel as a bound method or a keyword holding a module.
def prepare_with_mesh_prep(points, loop_verts, loop_start, loop_total, **kwargs):
    return prepare(points, loop_verts, loop_start, loop_total, backend=_mp, **kwargs)


def prepare_with_mesh_prep_fast(points, loop_verts, loop_start, loop_total, **kwargs):
    from . import mesh_prep_fast
    return prepare(points, loop_verts, loop_start, loop_total, backend=mesh_prep_fast, **kwargs)
