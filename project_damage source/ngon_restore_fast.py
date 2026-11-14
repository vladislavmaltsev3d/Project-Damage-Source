# возвращаем исходные многоугольники там, где они остались целыми
# SPDX-License-Identifier: GPL-3.0-or-later
"""Batched n-gon restore from PD-perf-v2; used only when explicitly enabled."""
import bmesh
import numpy as np

def restore_ngons(mesh, groups):
    """Dissolve each group of result faces (an untouched triangulated n-gon)
    back into one face. UVs and attributes ride along in bmesh.

    Performance: one bmesh operator call walks the flags of the WHOLE mesh,
    so one call per group was quadratic (902 n-gons on a 500k-triangle wall:
    ~250 s). Groups that share no edge are dissolved together - each
    edge-connected region still becomes its own face, exactly as before - so
    a handful of calls (a greedy colouring of the groups) does it all. A
    batch that fails falls back to one call per group."""
    if not groups:
        return 0
    group_faces = [list(faces) for faces in groups.values()]
                                                          
    ls = np.empty(len(mesh.polygons), dtype=np.int64); mesh.polygons.foreach_get("loop_start", ls)
    lt = np.empty(len(mesh.polygons), dtype=np.int64); mesh.polygons.foreach_get("loop_total", lt)
    lv = np.empty(len(mesh.loops), dtype=np.int64); mesh.loops.foreach_get("vertex_index", lv)
    edge_groups = {}
    for gi, faces in enumerate(group_faces):
        for fi in faces:
            a0, n = int(ls[fi]), int(lt[fi])
            ring = lv[a0:a0 + n].tolist()
            for k in range(n):
                a, b = ring[k], ring[(k + 1) % n]
                edge_groups.setdefault((a, b) if a < b else (b, a), set()).add(gi)
    neighbours = [set() for _ in group_faces]
    for gs in edge_groups.values():
        if len(gs) > 1:
            for gi in gs:
                neighbours[gi].update(gs - {gi})
    colour = [-1] * len(group_faces)
    for gi in range(len(group_faces)):
        used = {colour[o] for o in neighbours[gi] if colour[o] >= 0}
        c = 0
        while c in used:
            c += 1
        colour[gi] = c
    batches = {}
    for gi, c in enumerate(colour):
        batches.setdefault(c, []).append(gi)
    bm = bmesh.new()
    try:
        bm.from_mesh(mesh)
        bm.faces.ensure_lookup_table()
        sets = [[bm.faces[i] for i in faces] for faces in group_faces]
        done = 0
        for c in sorted(batches):
            ok_groups = [gi for gi in batches[c] if all(f.is_valid for f in sets[gi])]
            if not ok_groups:
                continue
            try:
                bmesh.ops.dissolve_faces(bm, faces=[f for gi in ok_groups for f in sets[gi]], use_verts=False)
                done += len(ok_groups)
            except Exception:
                for gi in ok_groups:                                                       
                    if all(f.is_valid for f in sets[gi]):
                        bmesh.ops.dissolve_faces(bm, faces=sets[gi], use_verts=False)
                        done += 1
        bm.to_mesh(mesh)
    finally:
        bm.free()
    mesh.update()
    return done

