# подготовка сетки с переносом данных поверхности
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
"""Smart remesh: an optional post-step on the core's result dict.

It evens out the long thin triangles and fans that the Boolean cuts leave,
on the damaged faces and on the base faces the cuts touched (the rim), and
nothing else. Untouched faces keep their rows, corners and order. Every
per-face array (tri_class, base_poly, uvs, region lists such as
wallpaper_front_polys) and every per-point array (heat_*) is carried onto
the new faces and points, so materials, selections, UVs, attribute transfer
and the scoped shading downstream see a normal core result.

Pure numpy, no bpy: call it on `out` right after core.build_worn (and after
prep.finish when there is a prep), before _result_mesh. See INTEGRATION.md.

Steps, all inside the remesh region, repeated `iterations` times:
  1. split edges longer than 4/3 of the local target size at the midpoint
     (shared midpoints, so the mesh stays closed). The target is the
     damage's own edge length at the damage and grows with the distance
     from it, so a big wall face does not explode
  2. merge away free vertices on edges shorter than 4/5 of the target
  3. flip edges between coplanar faces of the same kind (Delaunay test)
  4. slide free vertices toward their neighbours, in their plane, and flip
     again
"Free" means inside one flat patch of one kind whose UVs are one affine map
(or absent, as on the damage): there a moved point's UV is known exactly.
Edges shared with an untouched face are never split or flipped. Faces of
different damage class, source face or region are never merged by a flip,
and faces with any other UV layout are only split. If the result fails
the closed-mesh check, the input is returned unchanged with a note.
"""
import numpy as np

BASE_CLASS = 6
POINT_KEYS = ("heat_exposure", "heat_char", "heat_char_depth", "rust", "rust_streak",
              "rust_source", "rust_source_streak", "edge_wear", "scratch",
              "metal_dent", "metal_crease", "metal_contact")
FACE_KEYS = ("tri_class", "base_poly", "uvs", "effect_ids")
SKIP_KEYS = {"points", "polys", "counts", "passthrough", "input_valid", "removed_helper_faces"}
                                                                     
SKIP_PREFIXES = ("deposits_", "coating_", "rebar_", "lath_", "bricks_", "debris_", "rbf_", "cellf_")
BIG = np.int64(1) << np.int64(32)
UV_NONE, UV_AFFINE, UV_FIXED = 0, 1, 2


class Settings:
    """Defaults for smart_remesh. `density` 1 = the damage's own median edge;
    2 = half that size. `grade`: how fast the size grows per unit of distance
    from the damage (0.6 = 6 cm more per 10 cm away). `size` (core units)
    overrides the automatic base size."""
    density = 1.0
    grade = 0.6
    relax = 2                                           
    iterations = 4                                               
    size = None
    flat_angle = 1.0                                                         
    max_growth = 8.0                                                       
    max_faces = 400000


                                                                             
         

def _ukey(a, b):
    return np.minimum(a, b) * BIG + np.maximum(a, b)


def _tri_normals(P, tv):
    n = np.cross(P[tv[:, 1]] - P[tv[:, 0]], P[tv[:, 2]] - P[tv[:, 0]])
    area2 = np.linalg.norm(n, axis=1)
    return n / np.maximum(area2, 1e-300)[:, None], area2


def _min_angle(A, B, C):
    def ang(u, v):
        nu = np.linalg.norm(u, axis=1)
        nv = np.linalg.norm(v, axis=1)
        c = np.einsum('ij,ij->i', u, v) / np.maximum(nu * nv, 1e-300)
        return np.arccos(np.clip(c, -1.0, 1.0))
    return np.minimum(np.minimum(ang(B - A, C - A), ang(A - B, C - B)), ang(A - C, B - C))


def _face_rows(F):
    """Polygon rows (c == d marks a triangle) -> list of (face, a, b) edges."""
    tri = F[:, 2] == F[:, 3]
    a = np.concatenate([F[:, 0], F[:, 1], F[:, 2], F[~tri, 3]])
    b = np.concatenate([F[:, 1], F[:, 2], np.where(tri, F[:, 0], F[:, 3]), F[~tri, 0]])
    face = np.concatenate([np.arange(len(F))] * 3 + [np.flatnonzero(~tri)])
    return face, a, b


def _edge_health(F):
    """(edges not used by exactly two faces, directed edges used twice)."""
    _f, a, b = _face_rows(F)
    _u, cnt = np.unique(_ukey(a, b), return_counts=True)
    _d, dcnt = np.unique(a * BIG + b, return_counts=True)
    return int(np.count_nonzero(cnt != 2)), int(np.count_nonzero(dcnt > 1))


def _source_mask(P, S):
    """True for result points that sit on a source point (within 1e-6 of the
    extent), tested on two offset grids so a point on a cell border still
    matches."""
    S = np.asarray(S, dtype=np.float64).reshape(-1, 3)
    if not len(S):
        return np.zeros(len(P), dtype=bool)
    ext = np.linalg.norm(np.ptp(np.vstack([P, S]), axis=0))
    tol = max(ext * 1e-6, 1e-12)
    hit = np.zeros(len(P), dtype=bool)
    for off in (0.0, 0.5):
        q = np.floor(np.vstack([P, S]) / tol + off).astype(np.int64)
        _u, inv = np.unique(q, axis=0, return_inverse=True)
        inv = inv.ravel()
        hit |= np.isin(inv[:len(P)], inv[len(P):])
    return hit


def _classify(out, K, M):
    """{key: 'point' | 'face' | 'index'} for the arrays smart_remesh must
    carry, or (None, reason) when a key cannot be told apart."""
    kinds = {}
    for key, value in out.items():
        if key in SKIP_KEYS or key.startswith(SKIP_PREFIXES) or value is None:
            continue
        if isinstance(value, (bool, int, float, str, tuple, dict)):
            continue
        try:
            arr = np.asarray(value)
        except Exception:
            continue
        if arr.ndim == 0 or arr.dtype == object or not len(arr):
            continue
        n = len(arr)
        if key in POINT_KEYS and n == K:
            kinds[key] = 'point'
        elif key in FACE_KEYS and n == M:
            kinds[key] = 'face'
        elif key.endswith("_polys") and arr.ndim == 1 and arr.dtype.kind in 'iu':
            if arr.min() < 0 or arr.max() >= M:
                return None, "%s has face indices out of range" % key
            kinds[key] = 'index'
        elif n == M and arr.ndim >= 2 and arr.shape[1] == 4:
                                                                       
                                                                      
            return None, "%s looks like per-corner data" % key
        elif n == M and n != K:
            kinds[key] = 'face'
        elif n == K and n != M:
            kinds[key] = 'point'
        elif n == K == M:
            return None, "cannot tell whether %s is per point or per face" % key
    return kinds, None


def _uv_modes(P, tv, tuv, tsig, nsig):
    """Per signature: UV_NONE (no UVs, e.g. damage), UV_AFFINE (one affine
    map from position, fitted here) or UV_FIXED (anything else: those faces
    are only split, never flipped or moved, so their UVs stay exact)."""
    mode = np.full(nsig, UV_NONE, dtype=np.int8)
    maps = np.zeros((nsig, 4, 2))
    if tuv is None:
        return mode, maps
    corner_sig = np.repeat(tsig, 3)
    X = P[tv.ravel()]
    U = tuv.reshape(-1, 2)
    fin = np.isfinite(U).all(axis=1)
    order = np.argsort(corner_sig, kind='stable')
    bounds = np.searchsorted(corner_sig[order], np.arange(nsig + 1))
    for sg in range(nsig):
        idx = order[bounds[sg]:bounds[sg + 1]]
        if not len(idx):
            continue
        f = fin[idx]
        if not f.any():
            continue
        if not f.all():
            mode[sg] = UV_FIXED
            continue
        A = np.concatenate([X[idx], np.ones((len(idx), 1))], axis=1)
        M, *_ = np.linalg.lstsq(A, U[idx], rcond=None)
        err = np.abs(A @ M - U[idx]).max()
        ext = np.ptp(U[idx], axis=0).max()
        if err <= 1e-6 * max(ext, 1e-12) + 1e-9:
            mode[sg] = UV_AFFINE
            maps[sg] = M
        else:
            mode[sg] = UV_FIXED
    return mode, maps


def _uv_at(st, sigs, pos):
    """UVs at `pos` from the affine map of each signature (NaN for UV_NONE)."""
    out = np.einsum('nj,njk->nk', np.concatenate([pos, np.ones((len(pos), 1))], axis=1), st['uvmap'][sigs])
    out[st['uvmode'][sigs] == UV_NONE] = np.nan
    return out


def _graph_dist(P, tv, seeds, iters=400):
    e = np.unique(_ukey(tv[:, [0, 1, 2]].ravel(), tv[:, [1, 2, 0]].ravel()))
    e0, e1 = e // BIG, e % BIG
    L = np.linalg.norm(P[e0] - P[e1], axis=1)
    d = np.where(seeds, 0.0, np.inf)
    for _ in range(iters):
        nd = d.copy()
        np.minimum.at(nd, e1, d[e0] + L)
        np.minimum.at(nd, e0, d[e1] + L)
        if np.array_equal(nd, d):
            break
        d = nd
    return d


def _sizing(st, h0, grade, lock_seg):
    """Target edge length per point: h0 at the damage, growing by `grade`
    per unit of distance from it, and never much smaller than a locked edge
    of the same patch close by (falling off twice as fast as the distance
    from that edge). Same patch only, so a long edge on the far side of a
    thin wall does not coarsen this side."""
    P, tv = st['P'], st['tv']
    seeds = st['seed'].copy()
    seeds[tv[st['tdmg'][st['tpar']]].ravel()] = True
    st['dist'] = _graph_dist(P, tv, seeds)
    floor = np.full(len(P), -np.inf)
    st['floor'] = floor
    h = h0 + grade * st['dist']
    if not len(lock_seg):
        return h
    seg, seg_sig = lock_seg
    a, b = P[seg[:, 0]], P[seg[:, 1]]
    ab = b - a
    L = np.linalg.norm(ab, axis=1)
    big = L > 3.0 * h0
    if not big.any():
        return h
    a, ab, L, seg_sig = a[big], ab[big], L[big], seg_sig[big]
    ll = np.maximum(np.einsum('ij,ij->i', ab, ab), 1e-300)
    vert_sig = np.unique(np.stack([tv.ravel(), np.repeat(st['tsig'], 3)], axis=1), axis=0)
    for sg in np.unique(seg_sig):
        verts = vert_sig[vert_sig[:, 1] == sg, 0]
        ss = np.flatnonzero(seg_sig == sg)
        step = max(1, 2000000 // len(ss))
        for i in range(0, len(verts), step):
            vv = verts[i:i + step]
            q = P[vv, None, :] - a[None, ss]
            t = np.clip(np.einsum('nsj,sj->ns', q, ab[ss]) / ll[ss], 0.0, 1.0)
            d = np.linalg.norm(q - t[..., None] * ab[None, ss], axis=2)
            floor[vv] = np.maximum(floor[vv], (0.35 * L[None, ss] - 2.0 * d).max(axis=1))
    return np.maximum(h, floor)


def _extend_points(pdata, a, b, w):
    """Append points interpolated between a and b (weight w on b)."""
    for key, arr in pdata.items():
        if arr.dtype.kind in 'fc':
            wb = w.reshape((-1,) + (1,) * (arr.ndim - 1))
            add = arr[a] * (1 - wb) + arr[b] * wb
        else:
            add = arr[a]
        pdata[key] = np.concatenate([arr, add.astype(arr.dtype)])


                                                                             
          

                                                                         
                           
_T1 = np.array([(0, 3, 2), (3, 1, 2)])
_T2A = np.array([(3, 1, 4), (0, 3, 2), (3, 4, 2)])                        
_T2B = np.array([(3, 1, 4), (0, 3, 4), (0, 4, 2)])                        
_T3 = np.array([(0, 3, 5), (3, 1, 4), (5, 4, 2), (3, 4, 5)])


CAP_ANGLE = np.radians(150.0)
THIN_ANGLE = np.radians(20.0)


def _split_pass(st, locked, cap):
    """Split the edges longer than 4/3 of the target at their midpoint. Both
    triangles of an edge see the same decision, so the mesh stays closed."""
    P, tv = st['P'], st['tv']
    ea = tv
    eb = tv[:, [1, 2, 0]]
    opp = tv[:, [2, 0, 1]]
    key = _ukey(ea, eb)
    length = np.linalg.norm(P[ea] - P[eb], axis=2)
    target = 0.5 * (st['h'][ea] + st['h'][eb])
    mark = (length > target * (4.0 / 3.0)) & ~np.isin(key, locked)
                                                                          
                                                                          
    thin = _min_angle(P[tv[:, 0]], P[tv[:, 1]], P[tv[:, 2]]) < THIN_ANGLE
    mark &= (~st['tdmg'][st['tpar']] | thin)[:, None]
    if not mark.any():
        return 0
                                                                     
                                                                        
                                                                 
    mp = 0.5 * (P[ea] + P[eb])
    close = np.linalg.norm(P[opp] - mp, axis=2) < 0.2 * length
    u = P[ea] - P[opp]
    v = P[eb] - P[opp]
    cosang = np.einsum('ijk,ijk->ij', u, v) / np.maximum(
        np.linalg.norm(u, axis=2) * np.linalg.norm(v, axis=2), 1e-300)
    close |= cosang < np.cos(CAP_ANGLE)
    cand = np.flatnonzero(mark.ravel())
    ck, inv = np.unique(key.ravel()[cand], return_inverse=True)
    inv = inv.ravel()
    veto = np.zeros(len(ck), dtype=bool)
    np.logical_or.at(veto, inv, close.ravel()[cand])
    ukeys = ck[~veto]
    if not len(ukeys):
        return 0
    mark = np.isin(key, ukeys)                                              
    cnt = mark.sum(axis=1)
    grow = int(np.count_nonzero(cnt == 1) + 2 * np.count_nonzero(cnt == 2) + 3 * np.count_nonzero(cnt == 3))
    if len(tv) + grow > cap:
        return -1
    lo, hi = ukeys // BIG, ukeys % BIG
    t = np.full(len(ukeys), 0.5)
    K = len(P)
    st['P'] = np.vstack([P, P[lo] + t[:, None] * (P[hi] - P[lo])])
                                                                          
                                                                       
                                                                         
                               
    L = np.linalg.norm(P[hi] - P[lo], axis=1)
    d_new = np.minimum(st['dist'][lo], st['dist'][hi]) + 0.5 * L
    f_new = np.maximum(st['floor'][lo], st['floor'][hi]) - L
    st['dist'] = np.concatenate([st['dist'], d_new])
    st['floor'] = np.concatenate([st['floor'], f_new])
    st['h'] = np.concatenate([st['h'], np.maximum(st['h0'] + st['grade'] * d_new, f_new)])
    st['orig'] = np.concatenate([st['orig'], np.zeros(len(lo), dtype=bool)])
    st['seed'] = np.concatenate([st['seed'], np.zeros(len(lo), dtype=bool)])
    _extend_points(st['pdata'], lo, hi, t)
    edge_ix = np.full(tv.shape, -1, dtype=np.int64)
    edge_ix[mark] = np.searchsorted(ukeys, key[mark])
    mid = np.where(mark, K + edge_ix, -1)

    U6 = None
    if st['tuv'] is not None:
                                                                        
                                                          
        tl = np.where(ea < eb, t[np.maximum(edge_ix, 0)], 1 - t[np.maximum(edge_ix, 0)])
        uv = st['tuv']
        muv = (1 - tl)[..., None] * uv + tl[..., None] * uv[:, [1, 2, 0]]
        U6 = np.concatenate([uv, muv], axis=1)

    keep = cnt == 0
    new_tv, new_uv, new_par = [tv[keep]], [], [st['tpar'][keep]]
    new_sig = [st['tsig'][keep]]
    if U6 is not None:
        new_uv.append(st['tuv'][keep])

    def emit(sel, r, tpl):
        if not len(sel):
            return
        rot = (np.arange(3)[None, :] + r[:, None]) % 3
        rot6 = np.concatenate([rot, rot + 3], axis=1)
        vr = np.take_along_axis(tv[sel], rot, axis=1)
        mr = np.take_along_axis(mid[sel], rot, axis=1)
        ids = np.concatenate([vr, mr], axis=1)
        new_tv.append(ids[:, tpl].reshape(-1, 3))
        new_par.append(np.repeat(st['tpar'][sel], len(tpl)))
        new_sig.append(np.repeat(st['tsig'][sel], len(tpl)))
        if U6 is not None:
            ur = np.take_along_axis(U6[sel], rot6[:, :, None], axis=1)
            new_uv.append(ur[:, tpl].reshape(-1, 3, 2))

    s1 = np.flatnonzero(cnt == 1)
    emit(s1, np.argmax(mark[s1], axis=1), _T1)
    s2 = np.flatnonzero(cnt == 2)
    if len(s2):
        um = np.argmin(mark[s2], axis=1)                                   
        r = (um + 1) % 3                                                     
        rot = (np.arange(3)[None, :] + r[:, None]) % 3
        vr = np.take_along_axis(tv[s2], rot, axis=1)
        mr = np.take_along_axis(mid[s2], rot, axis=1)
        P2 = st['P']
        da = np.linalg.norm(P2[mr[:, 0]] - P2[vr[:, 2]], axis=1)
        db = np.linalg.norm(P2[vr[:, 0]] - P2[mr[:, 1]], axis=1)
        pick = da <= db
        emit(s2[pick], r[pick], _T2A)
        emit(s2[~pick], r[~pick], _T2B)
    s3 = np.flatnonzero(cnt == 3)
    emit(s3, np.zeros(len(s3), dtype=np.int64), _T3)

    st['tv'] = np.concatenate(new_tv)
    st['tpar'] = np.concatenate(new_par)
    st['tsig'] = np.concatenate(new_sig)
    if U6 is not None:
        st['tuv'] = np.concatenate(new_uv)
    return len(lo)


                                                                             
         

def _uv_tol(*uvs):
    stack = np.nan_to_num(np.stack(uvs, axis=1))
    return 2e-3 * np.ptp(stack, axis=1).max(axis=-1) + 1e-7


def _same_uv(u, v, tol):
    nan_u = ~np.isfinite(u).all(axis=1)
    nan_v = ~np.isfinite(v).all(axis=1)
    close = np.linalg.norm(np.nan_to_num(u) - np.nan_to_num(v), axis=1) <= tol
    return np.where(nan_u | nan_v, nan_u & nan_v, close)


def _angle_at(p, q, r):
    u, w = q - p, r - p
    c = np.einsum('ij,ij->i', u, w) / np.maximum(np.linalg.norm(u, axis=1) * np.linalg.norm(w, axis=1), 1e-300)
    return np.arccos(np.clip(c, -1.0, 1.0))


def _flip_pass(st, locked, cos_flat):
    """One round of edge flips. Each test runs only on the edges that passed
    the ones before it, cheapest first."""
    P, tv = st['P'], st['tv']
    n = len(tv)
    a = tv.ravel()
    b = tv[:, [1, 2, 0]].ravel()
    c = tv[:, [2, 0, 1]].ravel()
    dkey = a * BIG + b
    order = np.argsort(dkey, kind='stable')
    sk = dkey[order]
    dup = np.zeros(len(sk), dtype=bool)
    dup[1:] |= sk[1:] == sk[:-1]
    dup[:-1] |= sk[1:] == sk[:-1]
    bad_dir = np.zeros(3 * n, dtype=bool)
    bad_dir[order[dup]] = True

    def lookup(keys):
        pos = np.minimum(np.searchsorted(sk, keys), len(sk) - 1)
        return np.where(sk[pos] == keys, order[pos], -1)

    twin = lookup(b * BIG + a)
    h = np.flatnonzero((twin > np.arange(3 * n)) & ~bad_dir)
    h = h[~bad_dir[twin[h]]]
    g = twin[h]
    t1, t2 = h // 3, g // 3
    keep = st['tsig'][t1] == st['tsig'][t2]
    if st['tuv'] is not None:
        keep &= st['uvmode'][st['tsig'][t1]] != UV_FIXED
    h, g, t1, t2 = h[keep], g[keep], t1[keep], t2[keep]
    A, B, C, D = a[h], b[h], c[h], c[g]
                                                                            
                                                                           
                                                                          
                             
    gain = _angle_at(P[C], P[A], P[B]) + _angle_at(P[D], P[A], P[B]) - np.pi
    keep = (gain > 1e-9) & (C != D)
    h, g, t1, t2, A, B, C, D, gain = (x[keep] for x in (h, g, t1, t2, A, B, C, D, gain))
    if not len(h):
        return 0
    N1, _ = _tri_normals(P, tv[t1])
    N2, _ = _tri_normals(P, tv[t2])
    keep = np.einsum('ij,ij->i', N1, N2) > cos_flat
                                                                    
    keep &= (lookup(C * BIG + D) < 0) & (lookup(D * BIG + C) < 0)
    keep &= ~np.isin(_ukey(A, B), locked) & ~np.isin(_ukey(C, D), locked)
                                                             
    n1 = np.cross(P[A] - P[C], P[D] - P[C])
    n2 = np.cross(P[B] - P[D], P[C] - P[D])
    l1 = np.linalg.norm(n1, axis=1)
    l2 = np.linalg.norm(n2, axis=1)
    keep &= (np.einsum('ij,ij->i', n1, N1) > 0.5 * l1) & (np.einsum('ij,ij->i', n2, N1) > 0.5 * l2)
    keep &= (l1 > 0) & (l2 > 0)
    h, g, t1, t2, A, B, C, D, gain = (x[keep] for x in (h, g, t1, t2, A, B, C, D, gain))
    tuv = st['tuv']
    if tuv is not None and len(h):
        k1, k2 = h % 3, g % 3
        ua, ub, uc = tuv[t1, k1], tuv[t1, (k1 + 1) % 3], tuv[t1, (k1 + 2) % 3]
        vb, va, vd = tuv[t2, k2], tuv[t2, (k2 + 1) % 3], tuv[t2, (k2 + 2) % 3]
        tol = _uv_tol(ua, ub, uc, vd)
        keep = _same_uv(ua, va, tol) & _same_uv(ub, vb, tol)
        h, t1, t2, A, B, C, D, gain = (x[keep] for x in (h, t1, t2, A, B, C, D, gain))
        ua, ub, uc, vd = ua[keep], ub[keep], uc[keep], vd[keep]
    if not len(h):
        return 0
                                                                            
    idx = np.arange(len(h))
    T = np.concatenate([t1, t2])
    Cn = np.concatenate([idx, idx])
    S = np.concatenate([gain, gain])
    o = np.lexsort((-S, T))
    first = np.ones(len(o), dtype=bool)
    first[1:] = T[o][1:] != T[o][:-1]
    winner = np.full(n, -1, dtype=np.int64)
    winner[T[o][first]] = Cn[o][first]
    acc = idx[(winner[t1] == idx) & (winner[t2] == idx)]
    if not len(acc):
        return 0
    tv[t1[acc]] = np.stack([C[acc], A[acc], D[acc]], axis=1)
    tv[t2[acc]] = np.stack([D[acc], B[acc], C[acc]], axis=1)
    if tuv is not None:
        UA, UB, UC, UD = ua[acc], ub[acc], uc[acc], vd[acc]
        tuv[t1[acc]] = np.stack([UC, UA, UD], axis=1)
        tuv[t2[acc]] = np.stack([UD, UB, UC], axis=1)
    return len(acc)


def _flip_all(st, locked, cos_flat, passes=100):
    total = 0
    for _ in range(passes):
        f = _flip_pass(st, locked, cos_flat)
        total += f
        if not f:
            break
    return total


                                                                             
          

def _collapse_pass(st, vlock, cos_flat, seed=7):
    """Merge a free vertex u into a neighbour v along an edge shorter than
    4/5 of the target. u must be one _relax could move (inside one flat patch
    of one kind), so the surface, the UV map and the patch borders do not
    change. Collapses are 2-ring independent, so their stars never overlap."""
    ok, _vn, e0, e1 = _eligible(st, vlock, cos_flat)
    P, tv, tuv = st['P'], st['tv'], st['tuv']
    K = len(P)
    L = np.linalg.norm(P[e0] - P[e1], axis=1)
    target = 0.5 * (st['h'][e0] + st['h'][e1])
    short = L < 0.8 * target
                                                    
    u = np.concatenate([e0[short], e1[short]])
    v = np.concatenate([e1[short], e0[short]])
    Lc = np.concatenate([L[short], L[short]])
    keep = ok[u]
    u, v, Lc = u[keep], v[keep], Lc[keep]
    if not len(u):
        return 0
    o = np.lexsort((Lc, u))
    first = np.ones(len(o), dtype=bool)
    first[1:] = u[o][1:] != u[o][:-1]
    u, v = u[o][first], v[o][first]
    rng = np.random.default_rng(seed + K)
    prio = np.full(K, -1.0)
    prio[u] = rng.random(len(u))
    ring1 = prio.copy()
    np.maximum.at(ring1, e0, prio[e1])
    np.maximum.at(ring1, e1, prio[e0])
    ring2 = ring1.copy()
    np.maximum.at(ring2, e0, ring1[e1])
    np.maximum.at(ring2, e1, ring1[e0])
                                                           
    win = prio[u] >= ring2[u]
    u, v = u[win], v[win]
    if not len(u):
        return 0
    flat = tv.ravel()
    tri_of = np.repeat(np.arange(len(tv)), 3)
    target_of = np.full(K, -1, dtype=np.int64)
    target_of[u] = v
                                                          
    ekeys = np.unique(_ukey(e0, e1))
    nb_u = np.concatenate([e0, e1])
    nb_w = np.concatenate([e1, e0])
    m = target_of[nb_u] >= 0
    cu, cw = nb_u[m], nb_w[m]
    cv = target_of[cu]
    common = (cw != cv) & np.isin(_ukey(cv, cw), ekeys)
    ncommon = np.bincount(cu[common], minlength=K)
    good = np.zeros(K, dtype=bool)
    good[u] = ncommon[u] == 2
                                                                                  
    has = target_of[flat] >= 0
    star_t = np.unique(tri_of[has])
    T0 = tv[star_t]
    T1 = T0.copy()
    rep = target_of[T1] >= 0
    T1[rep] = target_of[T1][rep]
    owner = np.where(rep, T0, -1).max(axis=1)
    dead = (T1[:, 0] == T1[:, 1]) | (T1[:, 1] == T1[:, 2]) | (T1[:, 2] == T1[:, 0])
    N0, _ = _tri_normals(P, T0)
    N1, a1 = _tri_normals(P, T1)
    _N, a0 = _tri_normals(P, T0)
    ang0 = _min_angle(P[T0[:, 0]], P[T0[:, 1]], P[T0[:, 2]])
    ang1 = _min_angle(P[T1[:, 0]], P[T1[:, 1]], P[T1[:, 2]])
    lmax = np.linalg.norm(P[T1] - P[T1[:, [1, 2, 0]]], axis=2).max(axis=1)
    tgt = st['h'][T1].mean(axis=1)
    bad = ~dead & ((np.einsum('ij,ij->i', N0, N1) < 0.9) | (a1 < 1e-12 * np.maximum(a0, 1e-300)) |
                   (lmax > tgt * (4.0 / 3.0)) | ((ang1 < 0.5 * ang0) & (ang1 < np.radians(20.0))))
    worst_old = np.full(K, np.inf)
    worst_new = np.full(K, np.inf)
    np.minimum.at(worst_old, owner, ang0)
    np.minimum.at(worst_new, owner, np.where(dead, np.inf, ang1))
    badv = np.zeros(K, dtype=bool)
    badv[owner[bad]] = True
    good &= ~badv & ((worst_new >= worst_old) | (worst_new > np.radians(25.0)))
    if not good.any():
        return 0
                                                                          
                                                 
    chosen = np.flatnonzero(good)
    corner_u = good[flat]
    if tuv is not None:
        cuv = tuv.reshape(-1, 2)
        v_uv = np.full((K, 2), np.nan)
                                                            
        uv_src = np.full(K, -1, dtype=np.int64)
        for k in range(3):
            uk = tv[:, k]
            for j in range(3):
                if j == k:
                    continue
                hit = good[uk] & (tv[:, j] == target_of[uk])
                uv_src[uk[hit]] = np.arange(len(tv))[hit] * 3 + j
        v_uv[chosen] = cuv[uv_src[chosen]]
        cuv[corner_u] = v_uv[flat[corner_u]]
    tv[good[tv]] = target_of[tv[good[tv]]]
    alive = ~((tv[:, 0] == tv[:, 1]) | (tv[:, 1] == tv[:, 2]) | (tv[:, 2] == tv[:, 0]))
    st['tv'] = tv[alive]
    st['tpar'] = st['tpar'][alive]
    st['tsig'] = st['tsig'][alive]
    if tuv is not None:
        st['tuv'] = tuv[alive]
    return len(chosen)


def _eligible(st, vlock, cos_flat):
    """Vertices that may slide or be merged away: not locked, inside one flat
    patch of one kind (same signature, normals within flat_angle), with a
    closed star, in a patch whose UVs are one affine map or absent."""
    P, tv, tuv = st['P'], st['tv'], st['tuv']
    K = len(P)
    N, _a2 = _tri_normals(P, tv)
    flat = tv.ravel()
    tri_of = np.repeat(np.arange(len(tv)), 3)
    cnt = np.bincount(flat, minlength=K)
    e = np.unique(_ukey(tv.ravel(), tv[:, [1, 2, 0]].ravel()))
    e0, e1 = e // BIG, e % BIG
    ecnt = np.bincount(np.concatenate([e0, e1]), minlength=K)
    ok = ~vlock & (cnt >= 3) & (ecnt == cnt)
    s = st['tsig'][tri_of]
    smin = np.full(K, np.iinfo(np.int64).max)
    smax = np.full(K, np.iinfo(np.int64).min)
    np.minimum.at(smin, flat, s)
    np.maximum.at(smax, flat, s)
    ok &= smin == smax
    vn = np.zeros((K, 3))
    np.add.at(vn, flat, N[tri_of])
    vn /= np.maximum(np.linalg.norm(vn, axis=1), 1e-300)[:, None]
    dmin = np.ones(K)
    np.minimum.at(dmin, flat, np.einsum('ij,ij->i', vn[flat], N[tri_of]))
    ok &= dmin > cos_flat
    if tuv is not None:
                                                                          
                                                             
        vsig = np.where(cnt > 0, smin, 0)
        ok &= st['uvmode'][np.clip(vsig, 0, len(st['uvmode']) - 1)] != UV_FIXED
    return ok, vn, e0, e1


def _relax(st, vlock, rounds, cos_flat, seed=1):
    """Slide vertices toward the middle of their neighbours, in their plane.
    Each round moves an independent set (no two neighbours), and a move is
    kept only if the worst angle around the vertex gets better."""
    rng = np.random.default_rng(seed)
    tuv = st['tuv']
    moved_total = 0
    for _ in range(rounds):
        ok, vn, e0, e1 = _eligible(st, vlock, cos_flat)
        if not ok.any():
            break
        for _sub in range(3):
            P, tv = st['P'], st['tv']
            K = len(P)
            flat = tv.ravel()
            tri_of = np.repeat(np.arange(len(tv)), 3)
            prio = np.where(ok, rng.random(K), -1.0)
            nbmax = np.full(K, -1.0)
            np.maximum.at(nbmax, e0, prio[e1])
            np.maximum.at(nbmax, e1, prio[e0])
            sel = ok & (prio > nbmax)
            if not sel.any():
                break
            nsum = np.zeros((K, 3))
            np.add.at(nsum, e0, P[e1])
            np.add.at(nsum, e1, P[e0])
            ncnt = np.bincount(np.concatenate([e0, e1]), minlength=K)
            lap = nsum / np.maximum(ncnt, 1)[:, None]
            N0, _ = _tri_normals(P, tv)
            ang0 = _min_angle(P[tv[:, 0]], P[tv[:, 1]], P[tv[:, 2]])
            star0 = np.full(K, np.inf)
            np.minimum.at(star0, flat, ang0[tri_of])
            accepted = np.zeros(K, dtype=bool)
            newP = P.copy()
            for alpha in (1.0, 0.5, 0.25):
                trial = sel & ~accepted
                if not trial.any():
                    break
                cand = P + alpha * (lap - P)
                cand -= np.einsum('ij,ij->i', cand - P, vn)[:, None] * vn
                Q = newP.copy()
                Q[trial] = cand[trial]
                N1, _ = _tri_normals(Q, tv)
                ang1 = _min_angle(Q[tv[:, 0]], Q[tv[:, 1]], Q[tv[:, 2]])
                ok_t = np.einsum('ij,ij->i', N1, N0) > 0.9
                star1 = np.full(K, np.inf)
                np.minimum.at(star1, flat, np.where(ok_t, ang1, -1.0)[tri_of])
                good = trial & (star1 > star0 + 1e-6)
                newP[good] = cand[good]
                accepted |= good
            if not accepted.any():
                continue
                                                                      
                                                          
            pick = accepted[flat]
            cv, ct = flat[pick], tri_of[pick]
            bar = _bary(newP[cv], P[tv[ct]])
            inside = (bar >= -1e-9).all(axis=1)
            cv, ct, bar = cv[inside], ct[inside], bar[inside]
            _u, first = np.unique(cv, return_index=True)
            cv, ct, bar = cv[first], ct[first], bar[first]
            lost = accepted.copy()
            lost[cv] = False
            newP[lost] = P[lost]
            if not len(cv):
                continue
            if tuv is not None:
                nuv = _uv_at(st, st['tsig'][ct], newP[cv])
                loc = np.full(K, -1, dtype=np.int64)
                loc[cv] = np.arange(len(cv))
                corner = loc[flat]
                hit = corner >= 0
                tuv.reshape(-1, 2)[hit] = nuv[corner[hit]]
            for key, arr in st['pdata'].items():
                if arr.dtype.kind in 'fc':
                    wv = bar.reshape((-1, 3) + (1,) * (arr.ndim - 1))
                    arr[cv] = (arr[tv[ct]] * wv).sum(axis=1).astype(arr.dtype)
            st['P'] = newP
            moved_total += len(cv)
    return moved_total


def _bary(p, tri):
    a, b, c = tri[:, 0], tri[:, 1], tri[:, 2]
    e1, e2, q = b - a, c - a, p - a
    d00 = np.einsum('ij,ij->i', e1, e1)
    d01 = np.einsum('ij,ij->i', e1, e2)
    d11 = np.einsum('ij,ij->i', e2, e2)
    d20 = np.einsum('ij,ij->i', q, e1)
    d21 = np.einsum('ij,ij->i', q, e2)
    den = d00 * d11 - d01 * d01
    good = np.abs(den) > 1e-300
    den = np.where(good, den, 1.0)
    v = (d11 * d20 - d01 * d21) / den
    w = (d00 * d21 - d01 * d20) / den
    out = np.stack([1 - v - w, v, w], axis=1)
    out[~good] = -1.0
    return out


                                                                             
             

def smart_remesh(out, source_points=None, settings=None):
    """`out`: the core's result dict (after prep.finish when there is a prep).
    `source_points`: the points the core was given, in the same units as
    out["points"]; with them the rim is exactly the base faces the cuts
    touched. Without them it is the base faces next to the damage and the
    other pieces of their source faces.

    Returns (new_out, notes). new_out is a shallow copy; `out` is not
    changed. On any failure new_out is `out` itself and notes say why."""
    s = settings or Settings()
    notes = []
    if out.get("passthrough") or "polys" not in out or "points" not in out:
        return out, notes
    P0 = np.asarray(out["points"], dtype=np.float64)
    F = np.asarray(out["polys"]).astype(np.int64)
    K, M = len(P0), len(F)
    if not M:
        return out, notes
    kinds, why = _classify(out, K, M)
    if kinds is None:
        return out, ["smart remesh skipped: " + why]

    tri_class = out.get("tri_class")
    damage = None
    if tri_class is not None and len(tri_class) == M:
        damage = np.asarray(tri_class) != BASE_CLASS
    orig = _source_mask(P0, source_points) if source_points is not None else None
    if damage is None and orig is None:
        return out, ["smart remesh skipped: the result has no damage classes"]
    if damage is None:
        damage = np.zeros(M, dtype=bool)
    is_tri = F[:, 2] == F[:, 3]
    corner_ok = np.ones((M, 4), dtype=bool)
    corner_ok[is_tri, 3] = False
    if orig is not None:
        touched = ~damage & ((~orig[F]) & corner_ok).any(axis=1)
    else:
        dv = np.zeros(K, dtype=bool)
        dv[F[damage].ravel()] = True
        touched = ~damage & dv[F].any(axis=1)
        base = out.get("base_poly")
        if base is not None and len(base) == M:
            base = np.asarray(base)
            pieces = np.unique(base[touched & (base >= 0)])
            touched |= ~damage & np.isin(base, pieces) & (base >= 0)
        orig = ~dv
                                                                        
                                                                           
                                                                       
    Fq = np.where(corner_ok, F, F[:, :1])
    Pc = P0[F[:, [0, 1, 2]]]
    thin = damage & (_min_angle(Pc[:, 0], Pc[:, 1], Pc[:, 2]) < THIN_ANGLE)
    near = np.zeros(K, dtype=bool)
    near[Fq[touched | thin].ravel()] = True
    region = touched | (damage & near[Fq].any(axis=1))
    if not region.any():
        return out, notes

                                                                             
                                                                           
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
    sig = sig.ravel().astype(np.int64)

    uvs = out.get("uvs") if kinds.get("uvs") == 'face' else None
    uvs = None if uvs is None else np.asarray(uvs, dtype=np.float64)

                                                                  
    rf = np.flatnonzero(region)
    rt = rf[is_tri[rf]]
    rq = rf[~is_tri[rf]]
    Pq = P0[F[rq]]
    short02 = np.linalg.norm(Pq[:, 0] - Pq[:, 2], axis=1) <= np.linalg.norm(Pq[:, 1] - Pq[:, 3], axis=1)
    qa, qb = rq[short02], rq[~short02]
    pieces = [(rt, [0, 1, 2]), (qa, [0, 1, 2]), (qa, [0, 2, 3]), (qb, [0, 1, 3]), (qb, [1, 2, 3])]
    tv = np.concatenate([F[f][:, c] for f, c in pieces])
    tpar = np.concatenate([f for f, _c in pieces])
    tuv = None if uvs is None else np.concatenate([uvs[f][:, c] for f, c in pieces])
    n0 = len(tv)

                                                                            
                                            
    fo, ea, eb = _face_rows(F[~region])
    locked = np.unique(_ukey(ea, eb))
    rk, rc = np.unique(_ukey(tv.ravel(), tv[:, [1, 2, 0]].ravel()), return_counts=True)
    locked = np.union1d(locked, rk[rc != 2])

    pdata = {k: np.array(out[k], copy=True) for k, kind in kinds.items() if kind == 'point'}
    seeds = np.zeros(K, dtype=bool)
    seeds[F[damage].ravel()] = True
    if not seeds.any():
        seeds[tv.ravel()] = ~orig[tv.ravel()]
    st = dict(P=P0.copy(), tv=tv, tpar=tpar, tsig=sig[tpar], tuv=tuv,
              orig=orig.copy(), seed=seeds, tdmg=damage, pdata=pdata)
    st['uvmode'], st['uvmap'] = _uv_modes(P0, tv, tuv, sig[tpar], int(sig.max()) + 1)
                                                                        
                                                                             
    rk_all = _ukey(tv.ravel(), tv[:, [1, 2, 0]].ravel())
    lk, first = np.unique(rk_all, return_index=True)
    on = np.isin(lk, locked)
    lk, first = lk[on], first[on]
    lock_seg = (np.stack([lk // BIG, lk % BIG], axis=1), sig[tpar][first // 3])

    if s.size:
        h0 = float(s.size)
    else:
        dt = tv[damage[tpar]] if damage[tpar].any() else tv
        L = np.linalg.norm(P0[dt] - P0[dt[:, [1, 2, 0]]], axis=2).ravel()
        h0 = float(np.median(L[L > 0])) if np.any(L > 0) else 0.0
        h0 /= max(float(s.density), 1e-6)
    if not h0 > 0:
        return out, ["smart remesh skipped: no size to aim for"]
    cap = min(int(s.max_faces), max(int(s.max_growth * n0), 20000))
    cos_flat = float(np.cos(np.radians(s.flat_angle)))

    splits = flips = collapses = moved = 0
    vlock = st['orig'].copy()
    vlock[np.unique(F[~region].ravel())] = True
    grade = float(s.grade)
    st['h0'], st['grade'] = h0, grade
    full = False
    for _it in range(int(s.iterations)):
        st['h'] = _sizing(st, h0, grade, lock_seg)
        for _ in range(8):
            r = 0 if full else _split_pass(st, locked, cap)
            if r < 0:
                notes.append("smart remesh stopped splitting at %d triangles (the limit)" % len(st['tv']))
                full = True
                break
            if r == 0:
                break
            splits += r
        vlock = np.concatenate([vlock, st['orig'][len(vlock):]])
        for _ in range(6):
            c = _collapse_pass(st, vlock, cos_flat)
            collapses += c
            if not c:
                break
        flips += _flip_all(st, locked, cos_flat)
        if s.relax:
            moved += _relax(st, vlock, int(s.relax), cos_flat)
            flips += _flip_all(st, locked, cos_flat)

                                                                           
                                                                
    keep = np.flatnonzero(~region)
    tv = st['tv']
    rows = np.concatenate([F[keep], np.concatenate([tv, tv[:, 2:3]], axis=1)])
    par = np.concatenate([keep, st['tpar']])
    order = np.argsort(par, kind='stable')
    rows, par = rows[order], par[order]

    before = _edge_health(F)
    after = _edge_health(rows)
    if after[0] > before[0] or after[1] > before[1]:
        return out, ["smart remesh undone: the result was not closed (%d/%d open or doubled edges, was %d/%d)"
                     % (after + before)]

                                                                           
                                                                      
    used = np.zeros(len(st['P']), dtype=bool)
    used[:K] = True
    used[rows.ravel()] = True
    remap = np.cumsum(used) - 1
    rows = remap[rows]
    for key in st['pdata']:
        st['pdata'][key] = st['pdata'][key][used]
    res = dict(out)
    src_dtype = np.asarray(out["polys"]).dtype
    res["points"] = st['P'][used].astype(np.asarray(out["points"]).dtype)
    res["polys"] = rows.astype(src_dtype)
    for key, kind in kinds.items():
        arr = np.asarray(out[key])
        if kind == 'point':
            res[key] = st['pdata'][key]
        elif kind == 'face' and key == 'uvs':
            U = np.empty((len(rows), 4, 2), dtype=arr.dtype)
            n_keep = len(keep)
            U_un = np.concatenate([arr[keep], np.concatenate(
                [st['tuv'], np.full((len(tv), 1, 2), np.nan)], axis=1).astype(arr.dtype)])
            U[:] = U_un[order]
            res[key] = U
        elif kind == 'face':
            res[key] = arr[par]
        else:
            m = np.zeros(M, dtype=bool)
            m[arr] = True
            res[key] = np.flatnonzero(m[par]).astype(arr.dtype)
    counts = out.get("counts")
    if isinstance(counts, tuple) and len(counts) == 3:
        res["counts"] = (len(res["points"]), len(rows), counts[2])
    res["smart_remesh"] = dict(region_faces=int(region.sum()), triangles_before=n0,
                               triangles_after=len(tv), splits=splits, collapses=collapses,
                               flips=flips, moved=moved, size=h0)
    return res, notes
