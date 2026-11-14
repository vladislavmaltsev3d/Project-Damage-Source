# области влияния для свисающих полос обоев
# SPDX-License-Identifier: GPL-3.0-or-later
"""Wind zone for the Wallpaper Torn opening flaps.

The core lists the faces of every HANGING flap (automatic and Grease Pencil
tears; not the small lifted seam edges) in out['wallpaper_flap_polys'].
They are kept on the result as the boolean face attribute FACE_ATTR, and
refresh() turns them into the weighted vertex group GROUP:

  0 where the flap is fused to the wall and wherever the paper lies close to
  the wall, rising smoothly to 1 at the free part that hangs farthest out
  and lowest. Meant as the affected zone for a wind setup in Geometry Nodes
  (Named Attribute / vertex group -> multiply the wind offset).

Per flap (connected island of flap faces):
  wall   = smoothstep(0, falloff, d / d_max)   d = distance to the nearest
           non-flap face of the result (the paper or wall behind the flap)
  height = smoothstep(0, 1, drop / drop_max)   drop = how far the vertex
           hangs below its nearest root vertex, along the Wallpaper "up"
  w      = wall * (1 - k + k * height)         k = Height Influence
then a few Laplacian passes with the root pinned at 0, and normalised so
every flap reaches 1 at its freest point.
Only GROUP and FACE_ATTR are written; nothing else on the mesh changes.
"""
import numpy as np

GROUP = 'PD_Wallpaper Flaps'
FACE_ATTR = 'project_damage_wallpaper_flap'
OWNED = 'project_damage_flap_group'                                                      
SETTINGS = 'project_damage_flap_settings'                                                          


def write_face_mask(mesh, out):
    """Store out['wallpaper_flap_polys'] as FACE_ATTR. Call wherever the
    other wallpaper region masks are written (apply_wallpaper_materials)."""
    count = len(mesh.polygons)
    indices = np.asarray(out.get('wallpaper_flap_polys', ()), dtype=np.int64)
    attr = mesh.attributes.get(FACE_ATTR)
    if not len(indices):
        if attr is not None:
            mesh.attributes.remove(attr)
        return
    if indices.min() < 0 or indices.max() >= count:
        raise ValueError('Wallpaper flap face indices are out of range')
    if attr is not None and (attr.domain != 'FACE' or attr.data_type != 'BOOLEAN'):
        mesh.attributes.remove(attr)
        attr = None
    if attr is None:
        attr = mesh.attributes.new(FACE_ATTR, 'BOOLEAN', 'FACE')
    mask = np.zeros(count, dtype=bool)
    mask[indices] = True
    attr.data.foreach_set('value', mask)


def _smoothstep(e0, e1, x):
    t = np.clip((x - e0) / max(e1 - e0, 1e-12), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def _islands(n, a, b):
    """Connected component label per vertex (0..n-1) for edges a-b:
    min-label propagation with pointer jumping, vectorised."""
    label = np.arange(n)
    while True:
        m = np.minimum(label[a], label[b])
        new = label.copy()
        np.minimum.at(new, a, m)
        np.minimum.at(new, b, m)
        np.minimum.at(new, label, new)                                          
        new = new[new]
        while True:
            jumped = new[new]
            if np.array_equal(jumped, new):
                break
            new = jumped
        if np.array_equal(new, label):
            return label
        label = new


def flap_weights(co, totals, corners, flap, up, falloff=0.6, height=0.5, smooth=4,
                 wall_distance=None):
    """Weights for the flap vertices.

    co: (V, 3) vertex positions; totals: corners per face; corners: vertex
    index per face corner (faces stored contiguously, as in Blender); flap:
    boolean per face; up: the Wallpaper "up" (seam direction), object space.
    wall_distance(points, wall_faces) -> distance per point to the nearest
    face in wall_faces (a list of vertex-index lists); defaults to a
    mathutils BVH. Returns (vertex indices, weights in 0..1)."""
    co = np.asarray(co, dtype=np.float64).reshape(-1, 3)
    totals = np.asarray(totals, dtype=np.int64)
    corners = np.asarray(corners, dtype=np.int64)
    flap = np.asarray(flap, dtype=bool)
    empty = np.empty(0, dtype=np.int32), np.empty(0, dtype=np.float32)
    if not flap.any():
        return empty
    face_of = np.repeat(np.arange(len(totals)), totals)
    corner_flap = flap[face_of]
    verts = np.unique(corners[corner_flap])
    other = np.zeros(len(co), dtype=bool)
    other[corners[~corner_flap]] = True
    local = np.full(len(co), -1, dtype=np.int64)
    local[verts] = np.arange(len(verts))
    p = co[verts]

                                                                      
    starts = np.concatenate(([0], np.cumsum(totals)[:-1]))
    nxt = np.arange(len(corners)) + 1
    last = starts + totals - 1
    nxt[last] = starts
    ea = local[corners[corner_flap]]
    eb = local[corners[nxt[corner_flap]]]
    island = _islands(len(verts), ea, eb)

                                                                     
    if wall_distance is None:
        wall_distance = _bvh_distance
    lo, hi = p.min(axis=0), p.max(axis=0)
    margin = float(np.linalg.norm(hi - lo)) + 1e-6
    near = np.all((co >= lo - margin) & (co <= hi + margin), axis=1)
    face_near = np.zeros(len(totals), dtype=bool)
    np.logical_or.at(face_near, face_of, near[corners])
    wall_faces = np.flatnonzero(face_near & ~flap)
    if len(wall_faces):
        wall = [corners[starts[f]:starts[f] + totals[f]].tolist() for f in wall_faces.tolist()]
        d = np.asarray(wall_distance(co, wall, p), dtype=np.float64)
    else:
        d = np.zeros(len(p))

    up = np.asarray(up, dtype=np.float64)
    up = up / np.linalg.norm(up) if np.linalg.norm(up) > 1e-12 else np.array([0.0, 0.0, 1.0])
    root = other[verts]
    w = np.zeros(len(p))
    for k in np.unique(island):
        sel = island == k
        dk = d[sel]
        dmax = dk.max()
        if dmax <= 1e-9:
            continue
        rk = root[sel]
        if not rk.any():
                                                                          
            rk = dk <= 0.05 * dmax
            if not rk.any():
                # No vertex near the wall (the island was cut free): root at its nearest ones.
                rk = dk <= dk.min() + 1e-9
        wall_term = _smoothstep(0.0, max(falloff, 1e-3), dk / dmax)
        pk = p[sel]
        rp = pk[rk]
                                                                        
        drop = np.empty(len(pk))
        for s in range(0, len(pk), 2048):
            q = pk[s:s + 2048]
            j = np.argmin(((q[:, None, :] - rp[None, :, :]) ** 2).sum(-1), axis=1)
            drop[s:s + 2048] = (rp[j] - q) @ up
        drop = np.maximum(drop, 0.0)
        dm = drop.max()
        height_term = _smoothstep(0.0, 1.0, drop / dm) if dm > 1e-9 else np.ones(len(pk))
        wk = wall_term * (1.0 - height + height * height_term)
        wk[rk] = 0.0
        w[sel] = wk

                                                            
    if smooth > 0 and len(ea):
        a = np.concatenate((ea, eb))
        b = np.concatenate((eb, ea))
        deg = np.bincount(a, minlength=len(p)).astype(np.float64)
        for _ in range(int(smooth)):
            acc = np.bincount(a, weights=w[b], minlength=len(p))
            avg = np.where(deg > 0, acc / np.maximum(deg, 1.0), w)
            w = 0.5 * w + 0.5 * avg
            w[root] = 0.0
    for k in np.unique(island):
        sel = island == k
        m = w[sel].max()
        if m > 1e-9:
            w[sel] /= m
    return verts.astype(np.int32), np.clip(w, 0.0, 1.0).astype(np.float32)


def _bvh_distance(co, wall_faces, points):
    from mathutils import Vector
    from mathutils.bvhtree import BVHTree
    tree = BVHTree.FromPolygons([Vector(v) for v in co.tolist()], wall_faces, all_triangles=False)
    out = np.empty(len(points))
    for i, q in enumerate(points.tolist()):
        hit = tree.find_nearest(Vector(q))
        out[i] = hit[3] if hit[0] is not None else 0.0
    return out


def _face_mask(mesh):
    attr = mesh.attributes.get(FACE_ATTR)
    if attr is None or attr.domain != 'FACE' or not len(mesh.polygons):
        return None
    mask = np.empty(len(mesh.polygons), dtype=bool)
    attr.data.foreach_get('value', mask)
    return mask


def _remove(obj):
    name = obj.get(OWNED)
    group = obj.vertex_groups.get(name) if name else None
    if group is not None:
        obj.vertex_groups.remove(group)
    for key in (OWNED, SETTINGS):
        if key in obj:
            del obj[key]


def write(obj, settings):
    """(Re)write GROUP on obj from FACE_ATTR. settings: dict with up,
    falloff, height, smooth. Removes the group when there is no flap."""
    mesh = obj.data
    mask = _face_mask(mesh)
    if mask is None or not mask.any():
        _remove(obj)
        return 0
    co = np.empty(len(mesh.vertices) * 3, dtype=np.float64)
    mesh.vertices.foreach_get('co', co)
    totals = np.empty(len(mesh.polygons), dtype=np.int32)
    mesh.polygons.foreach_get('loop_total', totals)
    corners = np.empty(len(mesh.loops), dtype=np.int32)
    mesh.loops.foreach_get('vertex_index', corners)
    verts, weights = flap_weights(co, totals, corners, mask, settings['up'],
                                  settings['falloff'], settings['height'], settings['smooth'])
    name = obj.get(OWNED)
    group = obj.vertex_groups.get(name) if name else None
    if group is None:
        group = obj.vertex_groups.get(GROUP) or obj.vertex_groups.new(name=GROUP)
    elif len(mesh.vertices):
        group.remove(list(range(len(mesh.vertices))))
                                                           
    levels = np.round(weights * 1000.0).astype(np.int32)
    order = np.argsort(levels, kind='stable')
    levels, verts = levels[order], verts[order]
    cuts = np.flatnonzero(np.diff(levels)) + 1
    for chunk_v, chunk_l in zip(np.split(verts, cuts), np.split(levels, cuts)):
        if len(chunk_v):
            group.add(chunk_v.tolist(), float(chunk_l[0]) / 1000.0, 'REPLACE')
    obj[OWNED] = group.name
    obj[SETTINGS] = {k: (list(v) if k == 'up' else v) for k, v in settings.items()}
    return len(verts)


def settings_of(props):
    return {'up': [props.wallpaper_seam_dir_x, props.wallpaper_seam_dir_y, props.wallpaper_seam_dir_z],
            'falloff': float(props.wallpaper_flap_wall_falloff),
            'height': float(props.wallpaper_flap_height_influence),
            'smooth': int(props.wallpaper_flap_smooth)}


def refresh(source, result):
    """Called at the end of effect_groups.refresh."""
    p = source.project_damage
    if (p.all_off or not p.wallpaper_enable or not getattr(p, 'wallpaper_flap_group', True)):
        _remove(result)
        return 0
    return write(result, settings_of(p))


def rewrite(obj):
    """After effect_groups.resplit: same settings, new vertex indices."""
    settings = obj.get(SETTINGS)
    if settings is None:
        return 0
    s = {k: settings[k] for k in ('falloff', 'height', 'smooth')}
    s['up'] = list(settings['up'])
    return write(obj, s)
