# упрощаем раскладку граней после расчёта
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
"""Quad Remesh: the core's result as all quads (or even triangles) with the
SAME surface. No face ever leaves the plane of the core face it came from,
so every damage form stays exactly as the core cut it.

1. Smart Remesh (with an exact flat test) turns the cut fans on the base
   surface into even, graded triangles.
2. Flat patches (faces in one plane with the same data) are remeshed in
   that plane, their outline kept vertex for vertex (quad_planar):
   - long straight edges get points about one quad apart, where the
     lattice of the biggest flat patch beside them crosses them;
   - a patch with four corners and matching sides becomes a structured
     grid (a plain side, a bevel strip), with holes cut out where damage
     sits inside it;
   - a large patch gets a lattice fitted to its outline, cut back from
     the damage rims;
   - what is left between lattice and outline becomes constrained
     Delaunay triangles, refined with off-centres to even shapes.
3. Neighbouring triangles that lie in one plane and carry the same data
   (class, source face, selections, continuous UVs) are paired into quads,
   best-shaped pairs first.
4. Every face is split at its edge midpoints and its centre: a triangle
   into 3 quads, a quad into 4. Midpoints lie on the edges and centres in
   the face's plane, so the surface is unchanged and the mesh is all quads.
   Both sides of every edge are split the same way, so it stays closed.
   The damage faces themselves are only split, never moved.
5. New points inside a flat patch are smoothed in that plane. A move is
   kept only where it improves the worst corner of the quads around it.
   UVs follow the patch's own affine UV map, and point data (heat) is
   re-read from the faces the point moved into.
6. Output Triangles: every quad split on its shorter diagonal.

Works on the core's `out` dict, like Smart Remesh, so materials, damage
selections, UVs, region lists and heat ride along the normal build path."""
import numpy as np

from .smart_remesh import (smart_remesh, Settings as SmartSettings, _classify, _edge_health, _face_rows,
                           _uv_modes, BIG, UV_NONE, UV_AFFINE, UV_FIXED)
from . import quad_planar


class Settings:
    output = 'QUADS'                   
    quad_size = 1.0                                                          
    density = 1.0                                                                    
    grade = 0.6                                   
    relax = 4                                                 
    grid = True                                                            
    flat_angle = 1e-5                                                               
    min_pair_angle = 45.0                                                           


def _ukey(a, b):
    return np.minimum(a, b) * BIG + np.maximum(a, b)


def _signature(out, kinds, M):
    cols = [np.zeros(M, dtype=np.int64)]
    for key, kind in kinds.items():
        if kind == 'face' and key != 'uvs':
            arr = np.asarray(out[key]).reshape(M, -1)
            if arr.dtype.kind not in 'iub':
                arr = np.round(arr.astype(np.float64), 9)
            _u, inv = np.unique(arr, axis=0, return_inverse=True)
            cols.append(inv.ravel().astype(np.int64))
        elif kind == 'index':
            m = np.zeros(M, dtype=np.int64)
            m[np.asarray(out[key])] = 1
            cols.append(m)
    _u, sig = np.unique(np.stack(cols, axis=1), axis=0, return_inverse=True)
    return sig.ravel().astype(np.int64)


def _normals(P, rows, n):
    """Face normals of rows (k corners used, k = n per face)."""
    A, B, C = P[rows[:, 0]], P[rows[:, 1]], P[rows[:, 2]]
    v = np.cross(B - A, C - A)
    quad = n == 4
    if quad.any():
        D = P[rows[quad, 3]]
        v[quad] = np.cross(C[quad] - A[quad], D - B[quad])
    return v / np.maximum(np.linalg.norm(v, axis=1), 1e-300)[:, None]


def _corner_angles(P, rows, n):
    """(F,4) corner angles in degrees; unused 4th corner of a triangle = 60."""
    out = np.full((len(rows), 4), 60.0)
    for k in range(4):
        prv = np.where(n == 4, rows[:, (k - 1) % 4], rows[:, (k - 1) % 3 if k < 3 else 0])
        cur = rows[:, k]
        nxt = np.where(n == 4, rows[:, (k + 1) % 4], rows[:, (k + 1) % 3 if k < 3 else 0])
        a = P[prv] - P[cur]
        b = P[nxt] - P[cur]
        c = np.einsum('ij,ij->i', a, b) / np.maximum(np.linalg.norm(a, axis=1) * np.linalg.norm(b, axis=1), 1e-300)
        ang = np.degrees(np.arccos(np.clip(c, -1, 1)))
        use = (n == 4) | (k < 3)
        out[use, k] = ang[use]
    return out


def _pair(P, F, sig, uvs, cos_flat, min_angle):
    """Greedy pairing of coplanar, same-data triangles into convex quads.
    Returns (quads (Q,4) as (a, d, b, c), their UVs (Q,4,2) or None,
    parent triangle per quad, mask of triangles used)."""
    tri = np.flatnonzero(F[:, 2] == F[:, 3])
    if not len(tri):
        return np.zeros((0, 4), np.int64), None, np.zeros(0, np.int64), np.zeros(len(F), bool)
    T = F[tri, :3]
    he_a = T.ravel()
    he_b = T[:, [1, 2, 0]].ravel()
    he_c = T[:, [2, 0, 1]].ravel()
    he_t = np.repeat(np.arange(len(tri)), 3)
    he_k = np.tile(np.arange(3), len(tri))
    fwd = he_a * BIG + he_b
    rev = he_b * BIG + he_a
    order = np.argsort(fwd)
    pos = np.searchsorted(fwd[order], rev)
    pos = np.clip(pos, 0, len(fwd) - 1)
    twin = order[pos]
    has = fwd[twin] == rev
    i = np.flatnonzero(has & (np.arange(len(fwd)) < twin))
    j = twin[i]
    t, u = he_t[i], he_t[j]
    ft, fu = tri[t], tri[u]
    ok = sig[ft] == sig[fu]
    N = np.cross(P[T[:, 1]] - P[T[:, 0]], P[T[:, 2]] - P[T[:, 0]])
    N /= np.maximum(np.linalg.norm(N, axis=1), 1e-300)[:, None]
    ok &= np.einsum('ij,ij->i', N[t], N[u]) >= cos_flat
    a, b, c, d = he_a[i], he_b[i], he_c[i], he_c[j]
    quads = np.stack([a, d, b, c], axis=1)
    qn = np.full(len(quads), 4)
    ang = _corner_angles(P, quads, qn)
    ok &= (ang.min(1) >= min_angle) & (ang.max(1) <= 180.0 - min_angle)
    ok &= np.abs(ang.sum(1) - 360.0) < 1e-3                             
    if uvs is not None:
        def uv_of(face, vert):
            k = np.argmax(F[face, :3] == vert[:, None], axis=1)
            return uvs[face, k]
        ua_t, ub_t, uc_t = uv_of(ft, a), uv_of(ft, b), uv_of(ft, c)
        ua_u, ub_u, ud_u = uv_of(fu, a), uv_of(fu, b), uv_of(fu, d)
        both_nan = np.isnan(ua_t).all(1) & np.isnan(ua_u).all(1)
        same = (np.abs(np.nan_to_num(ua_t - ua_u, nan=1.0)).max(1) < 1e-7) & \
               (np.abs(np.nan_to_num(ub_t - ub_u, nan=1.0)).max(1) < 1e-7)
                                                                   
                                                    
        e1, e2, dd = P[b] - P[a], P[c] - P[a], P[d] - P[a]
        d00, d01, d11 = (e1 * e1).sum(1), (e1 * e2).sum(1), (e2 * e2).sum(1)
        den = np.where(np.abs(d00 * d11 - d01 * d01) > 1e-300, d00 * d11 - d01 * d01, 1e-300)
        w1 = (d11 * (dd * e1).sum(1) - d01 * (dd * e2).sum(1)) / den
        w2 = (d00 * (dd * e2).sum(1) - d01 * (dd * e1).sum(1)) / den
        pred = ua_t + w1[:, None] * (ub_t - ua_t) + w2[:, None] * (uc_t - ua_t)
        affine = np.abs(np.nan_to_num(pred - ud_u, nan=1.0)).max(1) < 1e-9 * (1 + np.abs(np.nan_to_num(ud_u)).max(1))
        ok &= (same & affine) | both_nan
        quv = np.stack([ua_t, ud_u, ub_t, uc_t], axis=1)
    score = 90.0 - np.abs(ang - 90.0).max(1)
    cand = np.flatnonzero(ok)
    cand = cand[np.argsort(-score[cand], kind='stable')]
    used = np.zeros(len(tri), bool)
    take = []
    for e in cand:
        if not used[t[e]] and not used[u[e]]:
            used[t[e]] = used[u[e]] = True
            take.append(e)
    take = np.array(take, np.int64)
    used_faces = np.zeros(len(F), bool)
    used_faces[tri[used]] = True
    return (quads[take], None if uvs is None else quv[take], ft[take], used_faces)


def _split(P, rows, n, uvs, pdata):
    """Every face into n quads at edge midpoints and its centre."""
    K = len(P)
    nf = len(rows)
                        
    fa, fb, ff, fk = [], [], [], []
    for k in range(4):
        use = (n == 4) | (k < 3)
        nxt = np.where(n == 4, (k + 1) % 4, (k + 1) % 3)
        f = np.flatnonzero(use)
        fa.append(rows[f, k])
        fb.append(rows[f, nxt[f]])
        ff.append(f)
        fk.append(np.full(len(f), k))
    fa, fb, ff, fk = (np.concatenate(x) for x in (fa, fb, ff, fk))
    ek, inv = np.unique(_ukey(fa, fb), return_inverse=True)
    ea, eb = ek // BIG, ek % BIG
    mid = K + inv                                                                                
    cen = K + len(ek) + np.arange(nf)
    Pm = 0.5 * (P[ea] + P[eb])
    cnt = n.astype(float)[:, None]
    Pc = np.where(n[:, None] == 4, P[rows].sum(1), P[rows[:, :3]].sum(1)) / cnt
    newP = np.concatenate([P, Pm, Pc])
    for key, arr in list(pdata.items()):
        if arr.dtype.kind in 'fc':
            sh = (-1,) + (1,) * (arr.ndim - 1)
            am = 0.5 * (arr[ea] + arr[eb])
            ac = np.where((n == 4).reshape(sh), arr[rows].sum(1), arr[rows[:, :3]].sum(1)) / n.reshape(sh)
        else:
            am, ac = arr[ea], arr[rows[:, 0]]
        pdata[key] = np.concatenate([arr, am.astype(arr.dtype), ac.astype(arr.dtype)])
                                                                          
    m_of = np.zeros((nf, 4), np.int64)
    m_of[ff, fk] = mid
    out_rows, out_par, out_uv = [], [], []
    for k in range(4):
        f = np.flatnonzero((n == 4) | (k < 3))
        kk = np.full(len(f), k)
        prev = np.where(n[f] == 4, (kk - 1) % 4, (kk - 1) % 3)
        out_rows.append(np.stack([rows[f, k], m_of[f, k], cen[f], m_of[f, prev]], axis=1))
        out_par.append(f)
        if uvs is not None:
            nxt = np.where(n[f] == 4, (kk + 1) % 4, (kk + 1) % 3)
            U = uvs[f]
            u0 = U[np.arange(len(f)), kk]
            un = U[np.arange(len(f)), nxt]
            up = U[np.arange(len(f)), prev]
            uc = np.where((n[f] == 4)[:, None], U.sum(1), U[:, :3].sum(1)) / n[f][:, None]
            out_uv.append(np.stack([u0, 0.5 * (u0 + un), uc, 0.5 * (u0 + up)], axis=1))
    rows2 = np.concatenate(out_rows)
    par = np.concatenate(out_par)
    order = np.argsort(par, kind='stable')
    uv2 = None if uvs is None else np.concatenate(out_uv)[order]
    return newP, rows2[order], par[order], uv2


def _relax(P, Q, qsig, quv, pdata, movable, uvmode, uvmap, rounds, rng):
    """Smooth movable points in their plane, many at once: each pass moves
    a set of points that share no quad toward the mean of their
    neighbours, and keeps a move only where the worst corner of the quads
    around the point improves and none of them turns over or folds."""
    nq = len(Q)
    n4 = np.full(nq, 4)
    if not nq or not movable.any():
        return 0
    vq = np.repeat(np.arange(nq), 4)
    vc = np.tile(np.arange(4), nq)
    vv = Q.ravel()
    nb_a = np.concatenate([Q[:, 0], Q[:, 1], Q[:, 2], Q[:, 3]])
    nb_b = np.concatenate([Q[:, 1], Q[:, 2], Q[:, 3], Q[:, 0]])
    e = np.unique(np.stack([np.minimum(nb_a, nb_b), np.maximum(nb_a, nb_b)], 1), axis=0)
    cnt = np.zeros(len(P))
    np.add.at(cnt, e[:, 0], 1)
    np.add.at(cnt, e[:, 1], 1)
    qn = _normals(P, Q, n4)
    vn = np.zeros_like(P)
    vn[vv] = qn[vq]                                                                
    moved = np.zeros(len(P), bool)
    for _r in range(rounds):
        for _sub in range(4):
            ang = _corner_angles(P, Q, n4)
            qmin = ang.min(1)
            vmin = np.full(len(P), np.inf)
            np.minimum.at(vmin, vv, qmin[vq])
            acc = np.zeros_like(P)
            np.add.at(acc, e[:, 0], P[e[:, 1]])
            np.add.at(acc, e[:, 1], P[e[:, 0]])
            step = acc / np.maximum(cnt, 1)[:, None] - P
            step -= np.einsum('ij,ij->i', step, vn)[:, None] * vn
            cand = movable & (vmin < 85.0) & (np.einsum('ij,ij->i', step, step) > 0)
            if not cand.any():
                break
                                                                                     
            pri = np.where(cand, rng.random(len(P)), -1.0)
            qbest = np.full(nq, -1.0)
            np.maximum.at(qbest, vq, pri[vv])
            lose = np.zeros(len(P), bool)
            np.logical_or.at(lose, vv, qbest[vq] > pri[vv])
            S = cand & ~lose
            if not S.any():
                break
            old = P.copy()
            P[S] += step[S]
            ang2 = _corner_angles(P, Q, n4)
            qn2 = _normals(P, Q, n4)
            touched = S[vv]
            bad_q = np.zeros(nq, bool)
            bad_q |= np.einsum('ij,ij->i', qn2, qn) < 0.999
            bad_q |= np.abs(ang2.sum(1) - 360.0) > 1e-3
                                                                         
            after = np.full(len(P), np.inf)
            np.minimum.at(after, vv[touched], ang2.min(1)[vq[touched]])
            anybad = np.zeros(len(P), bool)
            np.logical_or.at(anybad, vv[touched], bad_q[vq[touched]])
            ok = S & ~anybad & (after > vmin + 1e-6)
            undo = S & ~ok
            P[undo] = old[undo]
            if not ok.any():
                continue
            moved |= ok
            inc = ok[vv]
            if quv is not None:
                q_i, c_i, v_i = vq[inc], vc[inc], vv[inc]
                aff = uvmode[qsig[q_i]] == UV_AFFINE
                X = np.concatenate([P[v_i[aff]], np.ones((aff.sum(), 1))], axis=1)
                quv[q_i[aff], c_i[aff]] = np.einsum('ij,ijk->ik', X, uvmap[qsig[q_i[aff]]])
            if pdata:
                _resample(np.flatnonzero(ok), old, P, Q, vq[inc], vv[inc], pdata)
    return int(moved.sum())


def _resample(vs, old, P, Q, inc_q, inc_v, pdata):
    """Point data of moved points vs, re-read at their new place from the
    quads around them as they were before the move."""
                                                              
    T = np.concatenate([Q[inc_q][:, [0, 1, 2]], Q[inc_q][:, [0, 2, 3]]])
    tv = np.concatenate([inc_v, inc_v])
    A, B, C = old[T[:, 0]], old[T[:, 1]], old[T[:, 2]]
    p = P[tv]
    e1, e2, d = B - A, C - A, p - A
    d00, d01, d11 = (e1 * e1).sum(1), (e1 * e2).sum(1), (e2 * e2).sum(1)
    den = d00 * d11 - d01 * d01
    den = np.where(np.abs(den) > 1e-300, den, 1e-300)
    b1 = (d11 * (d * e1).sum(1) - d01 * (d * e2).sum(1)) / den
    b2 = (d00 * (d * e2).sum(1) - d01 * (d * e1).sum(1)) / den
    W = np.stack([1 - b1 - b2, b1, b2], 1)
    out = -W.min(1)                                               
    order = np.lexsort((out, tv))
    first = order[np.r_[True, tv[order][1:] != tv[order][:-1]]]
    first = first[out[first] <= 1e-9]
    Wf = W[first]
    Tf = T[first]
    for key, arr in pdata.items():
        if arr.dtype.kind in 'fc':
            vals = arr[Tf].astype(np.float64)
            arr[tv[first]] = np.einsum('ij,ij...->i...', Wf, vals).astype(arr.dtype)


def _patches(P, F, sig, cos_flat):
    """Connected groups of faces with one signature in one plane."""
    M = len(F)
    tri = F[:, 2] == F[:, 3]
    n = np.where(tri, 3, 4)
    N = _normals(P, F, n)
    fa, fb, ff = [], [], []
    for k in range(4):
        use = ~tri | (k < 3)
        f = np.flatnonzero(use)
        nxt = np.where(tri[f], (k + 1) % 3, (k + 1) % 4)
        fa.append(F[f, k]); fb.append(F[f, nxt]); ff.append(f)
    fa, fb, ff = (np.concatenate(x) for x in (fa, fb, ff))
    key = _ukey(fa, fb)
    o = np.argsort(key, kind='stable')
    ks, fs = key[o], ff[o]
    pair = np.flatnonzero(ks[:-1] == ks[1:])
    a, b = fs[pair], fs[pair + 1]
    join = (sig[a] == sig[b]) & (np.einsum('ij,ij->i', N[a], N[b]) >= cos_flat)
    parent = np.arange(M)

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x
    for x, y in zip(a[join], b[join]):
        rx, ry = find(x), find(y)
        if rx != ry:
            parent[rx] = ry
    roots = np.array([find(i) for i in range(M)])
    return roots, n


def _remesh_patches(P, F, sig, uvs, pdata, base, cos_flat, s):
    """Remesh flat patches in their plane (see quad_planar): large ones get
    an aligned grid bridged to their exact boundary, and long straight
    edges between patches get points every quad length, on the edge, so
    no face has to reach along them. Returns the new (P, F, sig, uvs),
    the old row each new row came from, and counts."""
    M = len(F)
    roots, n = _patches(P, F, sig, cos_flat)
    tri = F[:, 2] == F[:, 3]
    tv = np.concatenate([F[:, [0, 1, 2]], F[~tri][:, [0, 2, 3]]])
    tpar = np.concatenate([np.arange(M), np.flatnonzero(~tri)])
    tuv = None if uvs is None else np.concatenate([uvs[:, [0, 1, 2]], uvs[~tri][:, [0, 2, 3]]])
    uvmode, uvmap = _uv_modes(P, tv, tuv, sig[tpar], int(sig.max()) + 1)
    nothing = (P, F, sig, uvs, np.arange(M), 0, 0)
                                                                           
    dmg = np.asarray(base["tri_class"]) != 6 if "tri_class" in base else np.zeros(M, bool)
    sel = tv[dmg[tpar]] if dmg.any() else tv
    L = np.linalg.norm(P[sel] - P[sel[:, [1, 2, 0]]], axis=2).ravel()
    h0 = float(np.median(L[L > 0])) if np.any(L > 0) else 0.0
    H = 2.0 * h0 * float(s.quad_size)
    if not H > 0:
        return nothing
    A = np.linalg.norm(np.cross(P[tv[:, 1]] - P[tv[:, 0]], P[tv[:, 2]] - P[tv[:, 0]]), axis=1) / 2
    parea = np.zeros(M)
    np.add.at(parea, roots[tpar], A)
                                                                          
                                
    nrm = _normals(P, F, n)
    dev = np.zeros(M)
    for k in range(4):
        dev = np.maximum(dev, np.abs(np.einsum('ij,ij->i', P[F[:, k]] - P[F[:, 0]], nrm)))
    diag = np.linalg.norm(P[F[:, 2]] - P[F[:, 0]], axis=1)
    bad = ~tri & (dev > 1e-7 * diag)
    if uvs is not None:
        bad |= uvmode[sig] == UV_FIXED
    blocked = set(np.unique(roots[bad]).tolist())
                                        
    fa, fb, ff = [], [], []
    for k in range(4):
        use = ~tri | (k < 3)
        f = np.flatnonzero(use)
        nxt = np.where(tri[f], (k + 1) % 3, (k + 1) % 4)
        fa.append(F[f, k]); fb.append(F[f, nxt]); ff.append(f)
    fa, fb, ff = (np.concatenate(x) for x in (fa, fb, ff))
    key = _ukey(fa, fb)
    ek, inv, cnt = np.unique(key, return_inverse=True, return_counts=True)
    inv = inv.ravel()
    elo, ehi = ek // BIG, ek % BIG
    elen = np.linalg.norm(P[ehi] - P[elo], axis=1)
    rmin = np.full(len(ek), M)
    rmax = np.full(len(ek), -1)
    np.minimum.at(rmin, inv, roots[ff])
    np.maximum.at(rmax, inv, roots[ff])
    border = (cnt == 1) | ((cnt == 2) & (rmin != rmax))
    long_e = np.flatnonzero(border & (elen > 1.5 * H))
    e_roots = {}
    for e, r in zip(inv, roots[ff]):
        e_roots.setdefault(int(e), set()).add(int(r))
    big = set(np.unique(roots[parea[roots] >= 12 * H * H]).tolist())
    members = {}
    for f, r in enumerate(roots):
        members.setdefault(int(r), []).append(f)
                                                                           
                                                                           
    frames, lats = {}, {}
    p_long = {}
    for e in long_e:
        for r in e_roots[int(e)]:
            p_long.setdefault(r, []).append(e)
    for r in sorted(big - blocked, key=lambda r: (-parea[r], r)):
        faces0 = [list(F[f, :n[f]]) for f in members[r]]
        fr = quad_planar.patch_frame(P, faces0)
        if fr is None:
            continue
        o_, u_, v_ = fr
                                                                         
                                                                     
        inherit = [None, None]
        for e in sorted(p_long.get(r, []), key=lambda e: -elen[e]):
            own = [(parea[q], -q, q) for q in e_roots[int(e)] if q in lats]
            if not own:
                continue
            a, b = elo[e], ehi[e]
            t, hg = _edge_params(P[a], P[b], own, frames, lats, H)
            if hg is None or not len(t):
                continue
            pa = np.array([(P[a] - o_) @ u_, (P[a] - o_) @ v_])
            d = np.array([(P[b] - P[a]) @ u_, (P[b] - P[a]) @ v_])
            for ax in range(2):
                if inherit[ax] is None and abs(d[1 - ax]) <= 1e-9 * elen[e]:
                    inherit[ax] = (pa[ax] + t[0] * d[ax], hg)
        vs = np.unique(np.concatenate(faces0))
        frames[r] = fr
        lats[r] = quad_planar.lattice_params(np.stack([(P[vs] - o_) @ u_, (P[vs] - o_) @ v_], 1), H, inherit)

    noedge = set()                                                                      
    for _attempt in range(6):
        sub = [e for e in long_e if not (e_roots[int(e)] & (blocked | noedge))] if _attempt < 5 else []
                                           
        e_ids = {}
        eP = [np.zeros((0, 3))]
        nP = len(P)
        pd = {k: v.copy() for k, v in pdata.items()}
        for e in sub:
            a, b = elo[e], ehi[e]
            t, _hg = _edge_params(P[a], P[b], [(parea[r], -r, r) for r in e_roots[int(e)] if r in lats],
                                  frames, lats, H)
            if not len(t):
                continue
            k = len(t) + 1
            eP.append(P[a] + t[:, None] * (P[b] - P[a]))
            e_ids[int(ek[e])] = nP + np.arange(k - 1)
            nP += k - 1
            for name, arr in pd.items():
                if arr.dtype.kind in 'fc':
                    sh = (-1,) + (1,) * (arr.ndim - 1)
                    val = arr[a] + t.reshape(sh) * (arr[b].astype(np.float64) - arr[a])
                else:
                    val = np.repeat(arr[a:a + 1], k - 1, axis=0)
                pd[name] = np.concatenate([arr, val.astype(arr.dtype)])
        Pe = np.concatenate([P] + eP)
        touched = set()
        for e in sub:
            if int(ek[e]) in e_ids:
                touched |= e_roots[int(e)]
        todo = sorted((big - blocked) | touched)
        Pn = [Pe]
        nP = len(Pe)
        drop = np.zeros(M, bool)
        add_rows, add_par = [], []
        done, failed = [], set()
        for r in todo:
            fs = members[r]
            faces = []
            for f in fs:
                vs = list(F[f, :n[f]])
                poly = []
                for i in range(len(vs)):
                    a, b = int(vs[i]), int(vs[(i + 1) % len(vs)])
                    poly.append(a)
                    ids = e_ids.get(int(min(a, b) * BIG + max(a, b)))
                    if ids is not None:
                        poly += list(ids if a < b else ids[::-1])
                faces.append(poly)
            res = quad_planar.mesh_patch(Pe, faces, H, grid=r in big, frame=frames.get(r), lattice=lats.get(r))
            if res is None and r in big:
                res = quad_planar.mesh_patch(Pe, faces, H, grid=False)
            if res is not None:
                done.append((r, res))
            elif r in touched:
                failed.add(r)                                                            
        if failed:
            noedge |= failed
            continue
        break
    count = 0
    for r, (newP, newF) in done:
        fs = members[r]
        shift = nP - len(Pe)
        for f in newF:
            g = [x if x < len(Pe) else x + shift for x in f]
            add_rows.append(g + [g[2]] if len(g) == 3 else g)
            add_par.append(fs[0])
        Pn.append(newP)
        if pd and len(newP):
                                                                             
            _interp_points(Pe, [list(F[f, :n[f]]) for f in fs], newP, pd)
        nP += len(newP)
        drop[fs] = True
        count += 1
    if not count:
        return nothing
    P2 = np.concatenate(Pn)
    rows = np.array(add_rows, np.int64)
    par = np.array(add_par, np.int64)
    keep = np.flatnonzero(~drop)
    F2 = np.concatenate([F[keep], rows])
    sig2 = np.concatenate([sig[keep], sig[par]])
    par2 = np.concatenate([keep, par])
    uv2 = None
    if uvs is not None:
        nu = np.full((len(rows), 4, 2), np.nan)
        aff = uvmode[sig[par]] == UV_AFFINE
        X = np.concatenate([P2[rows], np.ones((len(rows), 4, 1))], axis=2)
        mapped = np.einsum('fcj,fjk->fck', X, uvmap[sig[par]])
        nu[aff] = mapped[aff]
        t3 = rows[:, 2] == rows[:, 3]
        nu[t3, 3] = np.nan
        uv2 = np.concatenate([uvs[keep], nu])
    pdata.clear()
    pdata.update(pd)
    return P2, F2, sig2, uv2, par2, count, sum(len(e_ids[k]) for k in e_ids)


def _edge_params(pa3, pb3, owners, frames, lats, H):
    """Where to put points on a long edge, as fractions from a to b, and the
    lattice spacing they follow: where the lattice lines of its biggest
    gridded patch cross it when it runs along one of that lattice's axes,
    else evenly about H apart (spacing None)."""
    if owners:
        r = max(owners)[2]
        o_, u_, v_ = frames[r]
        x0, y0, hx, hy = lats[r]
        pa = np.array([(pa3 - o_) @ u_, (pa3 - o_) @ v_])
        pb = np.array([(pb3 - o_) @ u_, (pb3 - o_) @ v_])
        d = pb - pa
        L = float(np.linalg.norm(d))
        for ax, g0, hg in ((0, x0, hx), (1, y0, hy)):
            if abs(d[1 - ax]) <= 1e-9 * L and abs(d[ax]) > 0 and hg > 0:
                lo, hi = sorted((pa[ax], pb[ax]))
                ks = np.arange(np.ceil((lo - g0) / hg - 1e-9), np.floor((hi - g0) / hg + 1e-9) + 1)
                xs = g0 + ks * hg
                xs = xs[(xs - lo > 0.35 * hg) & (hi - xs > 0.35 * hg)]
                return np.sort((xs - pa[ax]) / d[ax]), hg
    k = max(2, int(round(np.linalg.norm(pb3 - pa3) / H)))
    return np.arange(1, k) / k, None


def _interp_points(P, faces, newP, pdata):
    """Append point data for newP, read from the triangles of `faces`."""
    T = []
    for f in faces:
        for k in range(1, len(f) - 1):
            T.append((f[0], f[k], f[k + 1]))
    T = np.array(T)
    A, B, C = P[T[:, 0]], P[T[:, 1]], P[T[:, 2]]
    best = np.full(len(newP), np.inf)
    W = np.zeros((len(newP), 3))
    ti = np.zeros(len(newP), np.int64)
    for i in range(0, len(newP), 256):
        p = newP[i:i + 256][:, None, :]
        e1, e2, q = (B - A)[None], (C - A)[None], p - A[None]
        d00, d01, d11 = (e1 * e1).sum(2), (e1 * e2).sum(2), (e2 * e2).sum(2)
        d20, d21 = (q * e1).sum(2), (q * e2).sum(2)
        den = np.where(np.abs(d00 * d11 - d01 * d01) > 1e-300, d00 * d11 - d01 * d01, 1e-300)
        v = (d11 * d20 - d01 * d21) / den
        w = (d00 * d21 - d01 * d20) / den
        u = 1 - v - w
        out = -np.minimum(np.minimum(u, v), w)                                
        k = out.argmin(1)
        r = np.arange(len(k))
        best[i:i + 256] = out[r, k]
        W[i:i + 256] = np.stack([u[r, k], v[r, k], w[r, k]], 1)
        ti[i:i + 256] = k
    W = np.clip(W, 0, None)
    W /= np.maximum(W.sum(1), 1e-300)[:, None]
    for key, arr in list(pdata.items()):
        if arr.dtype.kind in 'fc':
            vals = (arr[T[ti]] * W.reshape((-1, 3) + (1,) * (arr.ndim - 1))).sum(1)
        else:
            vals = arr[T[ti, 0]]
        pdata[key] = np.concatenate([arr, vals.astype(arr.dtype)])



def quad_remesh(out, source_points=None, settings=None):
    """`out`: the core's result dict (after prep.finish when there is one).
    Returns (new_out, notes); on any failure new_out is `out` itself and
    notes say why. `out` is never changed."""
    s = settings or Settings()
    notes = []
    if out.get("passthrough") or "polys" not in out or "points" not in out:
        return out, notes
    ss = SmartSettings()
    ss.density, ss.grade, ss.flat_angle = s.density, s.grade, s.flat_angle
    base, n_smart = smart_remesh(out, source_points=source_points, settings=ss)
    notes += n_smart
    P = np.asarray(base["points"], dtype=np.float64)
    F = np.asarray(base["polys"]).astype(np.int64)
    K, M = len(P), len(F)
    if not M:
        return out, notes
    kinds, why = _classify(base, K, M)
    if kinds is None:
        return out, notes + ["quad remesh skipped: " + why]
    sig = _signature(base, kinds, M)
    uvs = None
    if kinds.get("uvs") == 'face':
        uvs = np.asarray(base["uvs"], dtype=np.float64).copy()
    cos_flat = float(np.cos(np.radians(s.flat_angle)))

                                                                   
    pdata = {k: np.array(base[k], copy=True) for k, kind in kinds.items() if kind == 'point'}
    F_base, M_base = F, M
    bpar = np.arange(M)                                                
    n_grid = n_edge = 0
    if s.grid:
        P, F, sig, uvs, bpar, n_grid, n_edge = _remesh_patches(P, F, sig, uvs, pdata, base, cos_flat, s)
        M = len(F)

                                
    pq, puv, ppar, used = _pair(P, F, sig, uvs, cos_flat, float(s.min_pair_angle))
    keep = np.flatnonzero(~used)
    rows = np.concatenate([F[keep], pq])
    par = np.concatenate([keep, ppar])
    n = np.where(rows[:, 2] == rows[:, 3], 3, 4)
    n[len(keep):] = 4
    U = None
    if uvs is not None:
        U = np.concatenate([uvs[keep], puv]) if len(pq) else uvs[keep]
    o = np.argsort(par, kind='stable')
    rows, par, n = rows[o], par[o], n[o]
    if U is not None:
        U = U[o]

                                    
    P2, Q, qpar_local, quv = _split(P, rows, n, U, pdata)
    qpar = par[qpar_local]
    qsig = sig[qpar]

                                                                    
    nq = len(Q)
    qn = _normals(P2, Q, np.full(nq, 4))
    vq = np.repeat(np.arange(nq), 4)
    vv = Q.ravel()
    nv = len(P2)
    sig_min = np.full(nv, np.iinfo(np.int64).max)
    sig_max = np.full(nv, -1)
    np.minimum.at(sig_min, vv, qsig[vq])
    np.maximum.at(sig_max, vv, qsig[vq])
    ref = np.zeros((nv, 3))
    ref[vv] = qn[vq]
    dots = np.einsum('ij,ij->i', qn[vq], ref[vv])
    flat = np.ones(nv, bool)
    np.logical_and.at(flat, vv, dots >= cos_flat)
    tv = np.concatenate([Q[:, [0, 1, 2]], Q[:, [0, 2, 3]]])
    tsig = np.concatenate([qsig, qsig])
    tuv = None if quv is None else np.concatenate([quv[:, [0, 1, 2]], quv[:, [0, 2, 3]]])
    uvmode, uvmap = _uv_modes(P2, tv, tuv, tsig, int(sig.max()) + 1)
    movable = (np.arange(nv) >= len(np.asarray(out["points"]))) & (sig_min == sig_max) & flat
    movable &= uvmode[np.clip(sig_min, 0, len(uvmode) - 1)] != 2                           
    _e, ea, eb = _face_rows(Q)
    ek, ecnt = np.unique(_ukey(ea, eb), return_counts=True)
    movable[ek[ecnt != 2] // BIG] = False                                                           
    movable[ek[ecnt != 2] % BIG] = False
    moved = 0
    if s.relax:
        moved = _relax(P2, Q, qsig, quv, pdata, movable, uvmode, uvmap, int(s.relax),
                       np.random.default_rng(1))

                             
    if s.output == 'TRIS':
        d02 = np.linalg.norm(P2[Q[:, 0]] - P2[Q[:, 2]], axis=1)
        d13 = np.linalg.norm(P2[Q[:, 1]] - P2[Q[:, 3]], axis=1)
        a = d02 <= d13
        c1 = np.where(a[:, None], [0, 1, 2], [0, 1, 3])
        c2 = np.where(a[:, None], [0, 2, 3], [1, 2, 3])
        r = np.arange(nq)[:, None]
        t1, t2 = Q[r, c1], Q[r, c2]
        frows = np.concatenate([np.concatenate([t1, t1[:, 2:]], 1), np.concatenate([t2, t2[:, 2:]], 1)])
        fpar = np.concatenate([qpar, qpar])
        if quv is not None:
            nanc = np.full((nq, 1, 2), np.nan)
            fuv = np.concatenate([np.concatenate([quv[r, c1], nanc], 1), np.concatenate([quv[r, c2], nanc], 1)])
        oo = np.argsort(fpar, kind='stable')
        frows, fpar = frows[oo], fpar[oo]
        if quv is not None:
            fuv = fuv[oo]
    else:
        frows, fpar, fuv = Q, qpar, quv

    fpar = bpar[fpar]
    before = _edge_health(F_base)
    after = _edge_health(frows)
    if after[0] > before[0] or after[1] > before[1]:
        return out, notes + ["quad remesh undone: the result was not closed (%d/%d open or doubled edges, was %d/%d)"
                             % (after + before)]
                                                                     
    used = np.zeros(len(P2), bool)
    used[frows.ravel()] = True
    remap = np.cumsum(used) - 1
    frows = remap[frows]
    P2 = P2[used]
    for key in pdata:
        pdata[key] = pdata[key][used]
    res = dict(base)
    res["points"] = P2.astype(np.asarray(base["points"]).dtype)
    res["polys"] = frows.astype(np.asarray(base["polys"]).dtype)
    for key, kind in kinds.items():
        arr = np.asarray(base[key])
        if kind == 'point':
            res[key] = pdata[key]
        elif kind == 'face' and key == 'uvs':
            res[key] = fuv.astype(arr.dtype)
        elif kind == 'face':
            res[key] = arr[fpar]
        else:
            m = np.zeros(M_base, dtype=bool)
            m[arr] = True
            res[key] = np.flatnonzero(m[fpar]).astype(arr.dtype)
    counts = out.get("counts")
    if isinstance(counts, tuple) and len(counts) == 3:
        res["counts"] = (len(res["points"]), len(frows), counts[2])
    res["quad_remesh"] = dict(faces_in=len(np.asarray(out["polys"])), smart_faces=M_base,
                              patches=n_grid, edge_points=n_edge, paired=len(pq), quads=nq, faces=len(frows), moved=moved)
    return res, notes
