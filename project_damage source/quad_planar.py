# собираем удобную сетку на плоских участках
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
"""Flat patches for Quad Remesh, meshed in 2D (numpy only).

A flat patch is a set of faces in one plane with the same data. Its
outline (damage rims, edges to other patches, points added on long edges)
is kept exactly, vertex for vertex. Inside, in this order of preference:
- four corners with matching sides: a structured grid by transfinite
  interpolation, holes cut out of it where there is damage inside;
- a lattice fitted to the patch's bounding box, snapped to the outline
  points that sit on it and cut back from the rest of the outline;
- nothing.
What the grid leaves is triangulated (ear clipping, constrained Delaunay
flips) and refined with off-centres (Ungor) and smoothing, so the band
between grid and outline gets even triangles. Every new point lies in the
patch's plane, so the surface does not change."""
import numpy as np


                                                                              

def _cross(o, a, b):
    return (a[..., 0] - o[..., 0]) * (b[..., 1] - o[..., 1]) - (a[..., 1] - o[..., 1]) * (b[..., 0] - o[..., 0])


def _area(loop_xy):
    x, y = loop_xy[:, 0], loop_xy[:, 1]
    return 0.5 * float(np.sum(x * np.roll(y, -1) - np.roll(x, -1) * y))


def _inside(pts, A, B):
    """Even-odd point-in-region test against segments A->B (all loops)."""
    px, py = pts[:, 0:1], pts[:, 1:2]
    ay, by = A[None, :, 1], B[None, :, 1]
    ax, bx = A[None, :, 0], B[None, :, 0]
    up = (ay <= py) != (by <= py)
    t = (py - ay) / np.where(by != ay, by - ay, 1.0)
    xi = ax + t * (bx - ax)
    return (up & (xi > px)).sum(1) % 2 == 1


def _seg_dist(pts, A, B):
    out = np.full(len(pts), np.inf)
    for i in range(0, len(A), 256):
        a, b = A[i:i + 256], B[i:i + 256]
        ab = b - a
        t = ((pts[:, None, :] - a[None]) * ab[None]).sum(2) / np.maximum((ab * ab).sum(1), 1e-300)[None]
        t = np.clip(t, 0, 1)
        q = a[None] + t[..., None] * ab[None]
        out = np.minimum(out, np.linalg.norm(pts[:, None, :] - q, axis=2).min(1))
    return out


def _loops(edges):
    """Directed boundary edges -> list of vertex loops, or None when a vertex
    has more than one way out (the patch touches itself at a point)."""
    nxt = {}
    for a, b in edges:
        if a in nxt:
            return None
        nxt[a] = b
    loops = []
    seen = set()
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


                                                                                

def _bridge(outer, holes, xy):
    """Join holes into the outer loop (Eberly's visible-vertex bridge)."""
    poly = list(outer)
    for h in sorted(holes, key=lambda h: -xy[h].max(axis=0)[0]):
        hi = int(np.argmax(xy[h][:, 0]))
        m = xy[h[hi]]
        P = xy[poly]
        best, bd = None, np.inf
                                                                 
        d = np.linalg.norm(P - m, axis=1)
        A = xy[np.array(poly)]
        B = xy[np.roll(np.array(poly), -1)]
        allA = np.concatenate([A] + [xy[np.array(hh)] for hh in holes])
        allB = np.concatenate([B] + [xy[np.roll(np.array(hh), -1)] for hh in holes])
        for k in np.argsort(d):
            q = P[k]
            if _segment_clear(m, q, allA, allB):
                best = k
                break
        if best is None:
            return None
        hl = list(h[hi:]) + list(h[:hi]) + [h[hi]]
        poly = poly[:best + 1] + hl + poly[best:]
    return poly


def _segment_clear(p, q, A, B):
    """Segment p-q crosses none of A->B (touching at its own ends allowed)."""
    eps = 1e-12 * max(1.0, float(np.abs(np.concatenate([A, B])).max()))
    d1 = _cross(A, B, np.broadcast_to(p, A.shape))
    d2 = _cross(A, B, np.broadcast_to(q, A.shape))
    d3 = _cross(np.broadcast_to(p, A.shape), np.broadcast_to(q, A.shape), A)
    d4 = _cross(np.broadcast_to(p, A.shape), np.broadcast_to(q, A.shape), B)
    shares = (np.linalg.norm(A - p, axis=1) < eps) | (np.linalg.norm(B - p, axis=1) < eps) | \
             (np.linalg.norm(A - q, axis=1) < eps) | (np.linalg.norm(B - q, axis=1) < eps)
    hit = (d1 * d2 < 0) & (d3 * d4 < 0) & ~shares
    return not hit.any()


def _earclip(poly, xy):
    """Ear clipping of a simple (bridged) CCW polygon given as vertex ids
    (ids may repeat at bridges). Returns triangles or None."""
    idx = list(range(len(poly)))
    P = xy[np.array(poly)]
    eps = 1e-10 * float(np.ptp(P, axis=0).max()) ** 2                                                 
    tris = []
    guard = 0
    while len(idx) > 3:
        guard += 1
        if guard > 4 * len(poly) * len(poly) + 10:
            return None
        n = len(idx)
        found = False
        Pi = P[idx]
        prv = np.roll(Pi, 1, axis=0)
        nxt = np.roll(Pi, -1, axis=0)
        convex = _cross(prv, Pi, nxt) > eps
                                               
        order = np.flatnonzero(convex)
        if not len(order):
            return None
        a1 = Pi[order] - prv[order]
        a2 = nxt[order] - Pi[order]
        a3 = prv[order] - nxt[order]
        def ang(u, v):
            c = -(u * v).sum(1) / np.maximum(np.linalg.norm(u, axis=1) * np.linalg.norm(v, axis=1), 1e-300)
            return np.arccos(np.clip(c, -1, 1))
        q = np.minimum(np.minimum(ang(a1, a2), ang(a2, a3)), ang(a3, a1))
        for k in order[np.argsort(-q)]:
            a, b, c = Pi[k - 1], Pi[k], Pi[(k + 1) % n]
            others = np.ones(n, bool)
            others[[k - 1, k, (k + 1) % n]] = False
            Q = Pi[others]
            ids = np.array(poly)[np.array(idx)][others]
            same = (ids == poly[idx[k - 1]]) | (ids == poly[idx[k]]) | (ids == poly[idx[(k + 1) % n]])
            Q = Q[~same]
            if len(Q):
                inside = (_cross(a, b, Q) >= 0) & (_cross(b, c, Q) >= 0) & (_cross(c, a, Q) >= 0)
                if inside.any():
                    continue
            tris.append((poly[idx[k - 1]], poly[idx[k]], poly[idx[(k + 1) % n]]))
            del idx[k]
            found = True
            break
        if not found:
            return None
    if _cross(P[idx[0]], P[idx[1]], P[idx[2]]) <= 0:
        return None
    tris.append(tuple(poly[i] for i in idx))
    return tris


def _delaunay_flips(T, xy, fixed):
    """Constrained Delaunay: flip non-fixed edges whose opposite angles sum
    to more than 180 degrees, as long as both new triangles stay CCW."""
    if not len(T):
        return [list(t) for t in T]
    T = np.array(T, dtype=np.int64)
    used = np.unique(T)
    eps = 1e-10 * float(np.ptp(xy[used], axis=0).max()) ** 2
    W = int(len(xy)) + 1
    fk = np.array([min(a, b) * W + max(a, b) for a, b in (tuple(e) for e in fixed if len(e) == 2)], np.int64)

    def ang(p, u, v):
        x, y = u - p, v - p
        c = (x * y).sum(1) / np.maximum(np.linalg.norm(x, axis=1) * np.linalg.norm(y, axis=1), 1e-300)
        return np.arccos(np.clip(c, -1, 1))
    for _ in range(500):
        n = len(T)
        ha, hb, hc = T.ravel(), T[:, [1, 2, 0]].ravel(), T[:, [2, 0, 1]].ravel()
        ht = np.repeat(np.arange(n), 3)
        key = ha * W + hb
        order = np.argsort(key)
        rk = hb * W + ha
        pos = np.clip(np.searchsorted(key[order], rk), 0, len(key) - 1)
        tw = order[pos]
        i = np.flatnonzero((key[tw] == rk) & (ha < hb))
        if len(fk):
            i = i[~np.isin(np.minimum(ha[i], hb[i]) * W + np.maximum(ha[i], hb[i]), fk)]
        if not len(i):
            break
        j = tw[i]
        a, b, c, d = ha[i], hb[i], hc[i], hc[j]
        pa, pb, pc, pd = xy[a], xy[b], xy[c], xy[d]
        s_ = ang(pc, pa, pb) + ang(pd, pa, pb)
        ok = (s_ > np.pi + 1e-9) & (_cross(pc, pd, pb) > eps) & (_cross(pd, pc, pa) > eps)
        cand = np.flatnonzero(ok)
        if not len(cand):
            break
        cand = cand[np.argsort(-s_[cand], kind='stable')]
        busy = np.zeros(n, bool)
        flips = 0
        for k in cand:
            ti, tj = ht[i[k]], ht[j[k]]
            if busy[ti] or busy[tj]:
                continue
            busy[ti] = busy[tj] = True
            T[ti] = (c[k], d[k], b[k])
            T[tj] = (d[k], c[k], a[k])
            flips += 1
        if not flips:
            break
    return T.tolist()


def _circ(X, T):
    """Circumcentres, circumradii and shortest edges of triangles T."""
    a, b, c = X[T[:, 0]], X[T[:, 1]], X[T[:, 2]]
    d = 2.0 * (a[:, 0] * (b[:, 1] - c[:, 1]) + b[:, 0] * (c[:, 1] - a[:, 1]) + c[:, 0] * (a[:, 1] - b[:, 1]))
    d = np.where(np.abs(d) > 1e-300, d, 1e-300)
    a2, b2, c2 = (a * a).sum(1), (b * b).sum(1), (c * c).sum(1)
    ux = (a2 * (b[:, 1] - c[:, 1]) + b2 * (c[:, 1] - a[:, 1]) + c2 * (a[:, 1] - b[:, 1])) / d
    uy = (a2 * (c[:, 0] - b[:, 0]) + b2 * (a[:, 0] - c[:, 0]) + c2 * (b[:, 0] - a[:, 0])) / d
    cc = np.stack([ux, uy], 1)
    R = np.linalg.norm(a - cc, axis=1)
    lmin = np.minimum(np.minimum(np.linalg.norm(b - a, axis=1), np.linalg.norm(c - b, axis=1)),
                      np.linalg.norm(a - c, axis=1))
    return cc, R, lmin


def _offcentre(X, T, cc, R, lmin):
    """Ungor's off-centres: on the bisector of the shortest edge, no further
    from it than an equilateral apex. Near a short boundary edge this lands
    inside the band, where the circumcentre would fall on the edge's own
    diametral circle. Returns points and the scale each one sets."""
    a, b, c = X[T[:, 0]], X[T[:, 1]], X[T[:, 2]]
    E = np.stack([np.linalg.norm(b - a, axis=1), np.linalg.norm(c - b, axis=1), np.linalg.norm(a - c, axis=1)], 1)
    k = E.argmin(1)
    V = np.stack([a, b, c], 1)
    r = np.arange(len(T))
    p, q = V[r, k], V[r, (k + 1) % 3]
    m = 0.5 * (p + q)
    d = q - p
    nrm = np.stack([-d[:, 1], d[:, 0]], 1)
    nrm /= np.maximum(np.linalg.norm(nrm, axis=1), 1e-300)[:, None]                            
    dist_cc = np.einsum('ij,ij->i', cc - m, nrm)
    dist = np.minimum(np.sqrt(3.0) / 2.0 * lmin, dist_cc)
    use_off = dist < dist_cc
    pts = np.where(use_off[:, None], m + nrm * dist[:, None], cc)
    scale = np.where(use_off, lmin, R)
    return pts, scale


def _refine(T, xy, fixed, h, SA, SB, min_angle=26.0, rounds=16, floor=0.2):
    """Delaunay refinement inside the region bounded by segments SA->SB:
    the circumcentres of triangles that are too large (circumradius over
    0.7 h) or too sharp (a corner under min_angle) are inserted, unless the
    centre falls outside, inside the diametral circle of a fixed segment
    (fixed segments are never split) or next to a vertex already there.
    Returns (triangles, all 2D points)."""
    X = np.asarray(xy, dtype=np.float64)
    T = [list(t) for t in T]
    if not T:
        return T, X
    q_lim = 1.0 / (2.0 * np.sin(np.radians(min_angle)))
    mid = 0.5 * (SA + SB)
    half = 0.5 * np.linalg.norm(SB - SA, axis=1)
    cap = len(X) + int(8 * sum(abs(_cross(X[t[0]], X[t[1]], X[t[2]])) for t in T) / (h * h)) + 64
    for _ in range(rounds):
        Tn = np.array(T)
        cc, R, lmin = _circ(X, Tn)
                                                                               
                                                                         
        bad = (R > 0.7 * h) | ((R > q_lim * lmin) & (lmin > floor * h))
        if not bad.any():
            break
        cand = np.flatnonzero(bad)
        cand = cand[np.argsort(-R[cand], kind='stable')]
        C, sc = _offcentre(X, Tn[cand], cc[cand], R[cand], lmin[cand])
        ok = _inside(C, SA, SB)
        for i in range(0, len(C), 256):
            dd = np.linalg.norm(C[i:i + 256, None, :] - mid[None], axis=2)
            ok[i:i + 256] &= ~(dd < half[None] * (1 + 1e-9)).any(1)
        picked = []
        for k in np.flatnonzero(ok):
            p, r = C[k], sc[k]
            if np.linalg.norm(X - p, axis=1).min() < 0.5 * r:
                continue
            if picked and np.linalg.norm(np.array([C[j] for j in picked]) - p, axis=1).min() < r:
                continue
            picked.append(k)
        if not picked or len(X) + len(picked) > cap:
            break
                                                     
        Tn = np.array(T)
        a, b, c = X[Tn[:, 0]], X[Tn[:, 1]], X[Tn[:, 2]]
        area2 = _cross(a, b, c)
        taken = set()
        added = []
        for k in picked:
            p = C[k]
            w0 = _cross(b, c, p[None]) / area2
            w1 = _cross(c, a, p[None]) / area2
            w2 = 1.0 - w0 - w1
            wm = np.minimum(np.minimum(w0, w1), w2)
            t = int(np.argmax(wm))
            if wm[t] < 1e-6 or t in taken:
                continue
            taken.add(t)
            pid = len(X) + len(added)
            added.append(p)
            i, j, l = T[t]
            T[t] = [i, j, pid]
            T.append([j, l, pid])
            T.append([l, i, pid])
        if not added:
            break
        X = np.concatenate([X, np.array(added)])
        T = _delaunay_flips(T, X, fixed)
    return T, X


def _smooth(T, X, n_fixed, rounds=3):
    """Laplacian smoothing of the inserted points (ids >= n_fixed), a move
    kept only if no triangle around the point turns over or gets worse."""
    Tn = np.array(T)
    if len(X) <= n_fixed or not len(Tn):
        return X
    X = X.copy()
    nb = {}
    star = {}
    for ti, t in enumerate(Tn):
        for k in range(3):
            v = t[k]
            if v >= n_fixed:
                nb.setdefault(v, set()).update((t[(k + 1) % 3], t[(k + 2) % 3]))
                star.setdefault(v, []).append(ti)
    for _ in range(rounds):
        for v, ns in nb.items():
            st = Tn[star[v]]
            old = X[v].copy()
            before = _min_ang(X, st)
            X[v] = X[list(ns)].mean(0)
            a, b, c = X[st[:, 0]], X[st[:, 1]], X[st[:, 2]]
            if (_cross(a, b, c) <= 0).any() or _min_ang(X, st) <= before:
                X[v] = old
    return X


def _min_ang(X, T):
    a, b, c = X[T[:, 0]], X[T[:, 1]], X[T[:, 2]]
    out = np.inf
    for p, q, r in ((a, b, c), (b, c, a), (c, a, b)):
        u, v = q - p, r - p
        cs = (u * v).sum(1) / np.maximum(np.linalg.norm(u, axis=1) * np.linalg.norm(v, axis=1), 1e-300)
        out = min(out, float(np.degrees(np.arccos(np.clip(cs, -1, 1))).min()))
    return out


def _clean_cells(cell):
    """No two cells meeting at one corner only, no cell with one neighbour
    or none (in place)."""
    nx, ny = cell.shape
    for _ in range(nx * ny):
        changed = False
        pad = np.pad(cell, 1)
        a, b, c, d = pad[:-1, :-1], pad[1:, :-1], pad[1:, 1:], pad[:-1, 1:]                             
        pinch = (a & c & ~b & ~d) | (b & d & ~a & ~c)
        if pinch.any():
            i, j = np.argwhere(pinch)[0]
            cands = [(i - 1, j - 1), (i, j)] if a[i, j] else [(i, j - 1), (i - 1, j)]
            cands = [(x, y) for x, y in cands if 0 <= x < nx and 0 <= y < ny and cell[x, y]]
            x, y = cands[0]
            cell[x, y] = False
            changed = True
        nb = np.zeros_like(cell, dtype=int)
        nb[1:] += cell[:-1]
        nb[:-1] += cell[1:]
        nb[:, 1:] += cell[:, :-1]
        nb[:, :-1] += cell[:, 1:]
        lone = cell & (nb <= 1)
        if lone.any():
            cell &= ~lone
            changed = True
        if not changed:
            break
    return cell


def lattice_params(xy_pts, H, inherit=(None, None)):
    """A lattice fitted to a patch's bounding box in its frame: whole cells
    of about H across it, so its rows and columns run along the sides of a
    rectangular patch. An axis can instead inherit (offset, spacing) from
    the points a bigger neighbour put on a shared edge.
    Returns (x0, y0, hx, hy)."""
    lo, hi = xy_pts.min(0), xy_pts.max(0)
    out = []
    for ax in range(2):
        if inherit[ax] is not None:
            g0, hg = inherit[ax]
            out.append((float(g0), float(hg)))
        else:
            w = hi[ax] - lo[ax]
            out.append((float(lo[ax]), float(w / max(1, int(round(w / H))))))
    return out[0][0], out[1][0], out[0][1], out[1][1]


def _axis_lattice(xy, verts, bnd, lat, H):
    """Nodes of the patch's lattice over its bounding box, snapped to the
    boundary points that sit on them. Returns (nodes, ids) or None."""
    x0, y0, hx, hy = lat
    lo, hi = xy[verts].min(0), xy[verts].max(0)
    i0, i1 = int(np.floor((lo[0] - x0) / hx + 1e-9)), int(np.ceil((hi[0] - x0) / hx - 1e-9))
    j0, j1 = int(np.floor((lo[1] - y0) / hy + 1e-9)), int(np.ceil((hi[1] - y0) / hy - 1e-9))
    if i1 - i0 < 2 or j1 - j0 < 2:
        return None
    GX, GY = np.meshgrid(x0 + hx * np.arange(i0, i1 + 1), y0 + hy * np.arange(j0, j1 + 1), indexing='ij')
    N = np.stack([GX, GY], axis=2)
    ids = -np.ones(GX.shape, np.int64)
    bv = np.unique(np.array(bnd).ravel())
    ri = np.rint((xy[bv, 0] - x0) / hx).astype(np.int64) - i0
    rj = np.rint((xy[bv, 1] - y0) / hy).astype(np.int64) - j0
    inr = (ri >= 0) & (ri < N.shape[0]) & (rj >= 0) & (rj < N.shape[1])
    bv, ri, rj = bv[inr], ri[inr], rj[inr]
    close = np.linalg.norm(xy[bv] - N[ri, rj], axis=1) < 1e-6 * H
    ids[ri[close], rj[close]] = bv[close]
    N[ri[close], rj[close]] = xy[bv[close]]
    return N, ids


def _tfi(xy, loop, corner_turn=25.0):
    """A boundary loop with four corners and the same number of points on
    opposite sides: the nodes of a structured grid by transfinite
    interpolation (the way a modeller meshes a plain side or a bevel strip).
    Returns (nodes (nu+1, nv+1, 2), node ids: the loop's own ids on the
    rim, -1 inside) or None."""
    L = np.asarray(loop)
    X = xy[L]
    d_in = X - np.roll(X, 1, axis=0)
    d_out = np.roll(X, -1, axis=0) - X
    turn = np.degrees(np.arctan2(_cross(np.zeros_like(X), d_in, d_out), (d_in * d_out).sum(1)))
    corners = np.flatnonzero(np.abs(turn) > corner_turn)
    if len(corners) != 4 or (turn[corners] < 0).any():
        return None
    k = len(L)
    sides = []
    for c in range(4):
        a, b = corners[c], corners[(c + 1) % 4]
        sides.append([(a + i) % k for i in range(((b - a) % k) + 1)])
    if len(sides[0]) != len(sides[2]) or len(sides[1]) != len(sides[3]):
        return None
    nu, nv = len(sides[0]) - 1, len(sides[1]) - 1
    bottom, right = X[sides[0]], X[sides[1]]
    top, left = X[sides[2]][::-1], X[sides[3]][::-1]

    def param(c):
        s = np.concatenate([[0.0], np.cumsum(np.linalg.norm(np.diff(c, axis=0), axis=1))])
        return s / max(s[-1], 1e-300)
    u = 0.5 * (param(bottom) + param(top))[:, None, None]
    v = 0.5 * (param(left) + param(right))[None, :, None]
    c00, c10, c11, c01 = bottom[0], bottom[-1], top[-1], top[0]
    N = ((1 - v) * bottom[:, None] + v * top[:, None] + (1 - u) * left[None] + u * right[None]
         - ((1 - u) * (1 - v) * c00 + u * (1 - v) * c10 + u * v * c11 + (1 - u) * v * c01))
    ids = -np.ones((nu + 1, nv + 1), np.int64)
    ids[:, 0] = L[sides[0]]
    ids[nu, :] = L[sides[1]]
    ids[:, nv] = L[sides[2]][::-1]
    ids[0, :] = L[sides[3]][::-1]
    N[ids >= 0] = xy[ids[ids >= 0]]
    return N, ids


def _lattice_cells(N, ids, SA, SB, inside, H, delta, eps):
    """Cells of a lattice that are convex, inside the region and at least
    delta * H from the segments SA->SB, with none of their ends inside."""
    nu, nv = N.shape[0] - 1, N.shape[1] - 1
    if nu < 1 or nv < 1:
        return None
    G = N.reshape(-1, 2)
    ok = inside(G) | (ids.ravel() >= 0)
    if len(SA):
        ok &= (_seg_dist(G, SA, SB) >= delta * H) | (ids.ravel() >= 0)
    okg = ok.reshape(nu + 1, nv + 1)
    cell = okg[:-1, :-1] & okg[1:, :-1] & okg[1:, 1:] & okg[:-1, 1:]
    Q = np.stack([N[:-1, :-1], N[1:, :-1], N[1:, 1:], N[:-1, 1:]], axis=2)                       
    C = Q.mean(2).reshape(-1, 2)
    cok = inside(C)
    if len(SA):
        cok &= _seg_dist(C, SA, SB) >= delta * H
    cell &= cok.reshape(nu, nv)
    conv = _cross(np.roll(Q, 1, axis=2), Q, np.roll(Q, -1, axis=2)) > eps
    cell &= conv.all(2)
    if len(SA):
                                                                         
                                                                
        I4 = np.stack([ids[:-1, :-1], ids[1:, :-1], ids[1:, 1:], ids[:-1, 1:]], axis=2)
        both = (I4 >= 0) & (np.roll(I4, -1, axis=2) >= 0)
        if both.any():
            Mid = 0.5 * (Q + np.roll(Q, -1, axis=2))
            w = np.argwhere(both)
            dm = _seg_dist(Mid[w[:, 0], w[:, 1], w[:, 2]], SA, SB)
            badc = np.zeros((nu, nv), bool)
            badc[w[dm < 0.25 * H, 0], w[dm < 0.25 * H, 1]] = True
            cell &= ~badc
    if len(SA):
        pts = np.unique(np.concatenate([SA, SB]), axis=0)
        Qf = Q.reshape(-1, 4, 2)
        for i in range(0, len(pts), 64):
            p = pts[i:i + 64][:, None, None, :]
            inq = (_cross(Qf[None], np.roll(Qf, -1, axis=1)[None], p) > 0).all(2)                  
            cell &= ~inq.any(0).reshape(nu, nv)
    return _clean_cells(cell)


def _fill(xy, bnd, n0, H, delta, lattice, near, inside, eps):
    """Quads from the lattice's kept cells, triangles (refined) for the rest
    of the patch. Returns (points beyond n0, quads, triangles) or None."""
    quads, cells_xy = [], []
    X = xy
    edges = list(bnd)
    if lattice is not None:
        N, ids = lattice
        SA, SB = near
        cell = _lattice_cells(N, ids, SA, SB, inside, H, delta, eps)
        if cell is None or cell.sum() < 4:
            return None
        nu, nv = cell.shape
        used = np.zeros((nu + 1, nv + 1), bool)
        for di, dj in ((0, 0), (1, 0), (1, 1), (0, 1)):
            used[di:nu + di, dj:nv + dj] |= cell
        gid = ids.copy()
        new = used & (ids < 0)
        gid[new] = n0 + np.arange(new.sum())
        X = np.concatenate([xy, N[new]])
        for i, j in np.argwhere(cell):
            quads.append([gid[i, j], gid[i + 1, j], gid[i + 1, j + 1], gid[i, j + 1]])
        qde = set()
        for q in quads:
            for k in range(4):
                qde.add((q[k], q[(k + 1) % 4]))
        edges += [(b, a) for a, b in qde if (b, a) not in qde]
                                                                      
    es = set(edges)
    gap = [(a, b) for a, b in edges if (b, a) not in es]
    tris = []
    fixed = {frozenset(e) for e in gap}
    if gap:
        loops = _loops(gap)
        if not loops:
            return None
        areas = [_area(X[np.array(l)]) for l in loops]
        outers = [i for i, ar in enumerate(areas) if ar > 0]
        holes = [i for i, ar in enumerate(areas) if ar < 0]
        region_holes = {i: [] for i in outers}
        for h in holes:
            p = X[loops[h][0]]
            best, ba_ = None, np.inf
            for o in outers:
                lp = X[np.array(loops[o])]
                if _inside(p[None], lp, np.roll(lp, -1, axis=0))[0] and areas[o] < ba_:
                    best, ba_ = o, areas[o]
            if best is None:
                return None
            region_holes[best].append(h)
        for o, hs in region_holes.items():
            poly = _bridge(loops[o], [loops[h] for h in hs], X)
            if poly is None:
                return None
            t = _earclip(poly, X)
            if t is None:
                return None
            tris += t
        tris = _delaunay_flips(tris, X, fixed)
        GA = X[np.array([a for a, b in gap])]
        GB = X[np.array([b for a, b in gap])]
        n_fix = len(X)
        tris, X = _refine(tris, X, fixed, H, GA, GB)
        if len(X) > n_fix:
            X = _smooth(tris, X, n_fix)
            tris = _delaunay_flips(tris, X, fixed)
    area = 0.0
    if tris:
        Tn = np.array(tris)
        ta = _cross(X[Tn[:, 0]], X[Tn[:, 1]], X[Tn[:, 2]])
        if (ta <= eps).any():
            return None
        area += 0.5 * float(ta.sum())
    if quads:
        Qn = X[np.array(quads)]
        area += sum(_area(q) for q in Qn)
    return X[n0:], quads, tris, area


                                                                                

def patch_frame(P, faces):
    """(origin, u, v) of a flat patch, u along its longest boundary edge,
    or None when it has no area."""
    de = set()
    for f in faces:
        for i in range(len(f)):
            de.add((f[i], f[(i + 1) % len(f)]))
    bnd = [(a, b) for a, b in de if (b, a) not in de]
    nrm = np.zeros(3)
    for f in faces:
        p = P[f]
        for k in range(1, len(f) - 1):
            nrm += np.cross(p[k] - p[0], p[k + 1] - p[0])
    if np.linalg.norm(nrm) == 0 or not bnd:
        return None
    nrm /= np.linalg.norm(nrm)
    ba = np.array([P[b] - P[a] for a, b in bnd])
    u = ba[np.argmax(np.linalg.norm(ba, axis=1))]
    u = u - (u @ nrm) * nrm
    u /= np.linalg.norm(u)
    return P[faces[0][0]].copy(), u, np.cross(nrm, u)


def mesh_patch(P, faces, H, delta=0.6, grid=True, frame=None, lattice=None):
    """Remesh one flat patch.

    P: (N,3) all points. faces: list of vertex-id lists (the patch's faces,
    CCW seen from their normal; extra points on their edges are allowed).
    H: target edge length. grid: allow an aligned grid of H cells.
    Tried in order: a structured grid over the whole patch when its outline
    has four corners (holes cut out with a refined band around them), an
    aligned grid with a refined band to the outline, refined triangles.
    Returns (new_points (n,3), new_faces as lists of ids where ids >= len(P)
    index into new_points), or None when the patch is not suitable."""
    de = set()
    for f in faces:
        for i in range(len(f)):
            de.add((f[i], f[(i + 1) % len(f)]))
    bnd = [(a, b) for a, b in de if (b, a) not in de]
    loops = _loops(bnd)
    if not loops:
        return None
    verts = np.unique(np.concatenate([np.asarray(f) for f in faces]))
    if frame is None:
        frame = patch_frame(P, faces)
        if frame is None:
            return None
    origin, u, v = frame
    xy = np.zeros((len(P), 2))
    xy[verts] = np.stack([(P[verts] - origin) @ u, (P[verts] - origin) @ v], axis=1)
    A = np.array([xy[a] for a, b in bnd])
    B = np.array([xy[b] for a, b in bnd])
    eps = 1e-10 * float(np.ptp(xy[verts], axis=0).max()) ** 2
    old = sum(abs(_area(xy[np.array(f)])) for f in faces)
    areas = [_area(xy[np.array(l)]) for l in loops]
    outer = [i for i, ar in enumerate(areas) if ar > 0]
    holes = [l for i, l in enumerate(loops) if areas[i] < 0]
    tries = []
    if len(outer) == 1:
        t = _tfi(xy, loops[outer[0]])
        if t is not None:
            if holes:
                HA = np.concatenate([xy[np.array(h)] for h in holes])
                HB = np.concatenate([xy[np.roll(np.array(h), -1)] for h in holes])
                tries.append((t, (HA, HB), lambda q, HA=HA, HB=HB: ~_inside(q, HA, HB)))
            else:
                tries.append((t, (np.zeros((0, 2)), np.zeros((0, 2))), lambda q: np.ones(len(q), bool)))
    if grid:
        if lattice is None:
            lattice = lattice_params(xy[verts], H)
        g = _axis_lattice(xy, verts, bnd, lattice, H)
        if g is not None:
            N, ids = g
                                                                            
            snapped = {}
            for (i, j) in np.argwhere(ids >= 0):
                snapped[int(ids[i, j])] = (i, j)
            near = [k for k, (a, b) in enumerate(bnd)
                    if not (a in snapped and b in snapped and
                            abs(snapped[a][0] - snapped[b][0]) + abs(snapped[a][1] - snapped[b][1]) == 1)]
            NA, NB = (A[near], B[near]) if near else (np.zeros((0, 2)), np.zeros((0, 2)))
            tries.append((g, (NA, NB), lambda q: _inside(q, A, B)))
    tries.append((None, None, None))
    for lattice, near, inside in tries:
        r = _fill(xy, bnd, len(P), H, delta, lattice, near, inside, eps)
        if r is None:
            continue
        pts, quads, tris, area = r
        if abs(area - old) > 1e-6 * max(old, 1e-12):
            continue
        newP = origin + pts[:, :1] * u + pts[:, 1:] * v
        return newP, [list(q) for q in quads] + [list(t) for t in tris]
    return None
