# перестраиваем сетку и переносим на неё материалы и UV
# ProjectDamage for Blender - Retopology for damaged results.
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
"""Retopology of a damaged result, with all of its data carried over.

Field-aligned remeshers (Instant Meshes, QuadriFlow, Smart Remesh's Organic
half, voxel remesh) do NOT keep the input surface: they lay a new even grid
over it and project it back. That is why this is a separate step on a COPY,
never part of the damage build (the exact in-pipeline options stay
Smart Remesh / Quad Remesh in build.py).

What this module guarantees about the damage:
- every loose part (fragment, shard, debris piece) is remeshed on its own, so
  pieces never fuse; parts below `keep_below` faces are copied unchanged;
- new vertices are fitted back onto the original surface of their own part;
- region data never bleeds across a damage border: a new face takes ALL of its
  face data (material, chips_P/L1_P/.../break_P, wallpaper/brick/bevel tags,
  effect id, sharp_face) from one original face, and its corners take UVs and
  other corner data only from the same UV island / region, so seams do not
  tear;
- point data (heat, rust, edge_wear, any float/int/colour attribute, vertex
  groups) is interpolated with barycentric weights on the original triangle;
- sharp edges are rebuilt on region borders and above the smoothing angle.

Backends:
- INSTANT_MESHES: the official BSD-licensed binary, run as a separate process
  (found next to an installed Smart Remesh extension, in /Applications, or at
  a path you set). Not bundled here.
- QUADRIFLOW: Blender's built-in quad remesher.
- VOXEL: Blender's Remesh modifier, voxel mode.
- Any other remesher (Smart Remesh, Quad Remesher, ...): run it yourself, then
  use Transfer Damage Data from the original to its result.

numpy + mathutils only; no code from other add-ons.
"""

import os
import shutil
import subprocess
import sys
import tempfile

import bpy
import numpy as np
from mathutils import Vector
from mathutils.bvhtree import BVHTree

RETOPO_SOURCE = "project_damage_retopo_source"
SKIP_ATTRS = {"position", ".select_vert", ".select_edge", ".select_poly",
              ".hide_vert", ".hide_edge", ".hide_poly", "sharp_edge",
              "custom_normal", "material_index", ".edge_verts", ".corner_vert",
              ".corner_edge", "UVMap_internal"}
NUMERIC = {"FLOAT", "INT", "INT8", "FLOAT_VECTOR", "FLOAT_COLOR", "BYTE_COLOR",
           "FLOAT2", "INT32_2D", "BOOLEAN", "QUATERNION", "FLOAT4X4", "INT16_2D"}
COMPONENTS = {"FLOAT": 1, "INT": 1, "INT8": 1, "BOOLEAN": 1, "FLOAT2": 2,
              "INT32_2D": 2, "INT16_2D": 2, "FLOAT_VECTOR": 3, "FLOAT_COLOR": 4,
              "BYTE_COLOR": 4, "QUATERNION": 4, "FLOAT4X4": 16}
DISCRETE = {"INT", "INT8", "BOOLEAN", "INT32_2D", "INT16_2D"}


                                                                             
              
def _value_key(dt):
    if dt in {"FLOAT_COLOR", "BYTE_COLOR"}:
        return "color"
    if dt in {"FLOAT_VECTOR", "FLOAT2", "INT32_2D", "INT16_2D"}:
        return "vector"
    if dt == "QUATERNION":
        return "value"
    return "value"


def _read_attr(attr, count):
    n = COMPONENTS[attr.data_type]
    dtype = np.int32 if attr.data_type in {"INT", "INT8", "INT32_2D", "INT16_2D"} else (
        bool if attr.data_type == "BOOLEAN" else np.float32)
    buf = np.empty(count * n, dtype=dtype)
    attr.data.foreach_get(_value_key(attr.data_type), buf)
    return buf.reshape(count, n) if n > 1 else buf


def _write_attr(mesh, name, dt, domain, values):
    attr = mesh.attributes.get(name)
    if attr is not None and (attr.data_type != dt or attr.domain != domain):
        mesh.attributes.remove(attr)
        attr = None
    if attr is None:
        attr = mesh.attributes.new(name, dt, domain)
    attr.data.foreach_set(_value_key(dt), np.ascontiguousarray(values).ravel())


def _is_uv(mesh, name):
    return name in mesh.uv_layers


class Source:
    """Everything the transfer reads from the original mesh, as arrays."""

    def __init__(self, mesh):
        mesh.calc_loop_triangles()
        self.mesh = mesh
        nv, nf, nl = len(mesh.vertices), len(mesh.polygons), len(mesh.loops)
        self.co = np.empty(nv * 3, np.float64)
        mesh.vertices.foreach_get("co", self.co)
        self.co = self.co.reshape(nv, 3)
        self.loop_start = np.empty(nf, np.int64)
        self.loop_total = np.empty(nf, np.int64)
        mesh.polygons.foreach_get("loop_start", self.loop_start)
        mesh.polygons.foreach_get("loop_total", self.loop_total)
        self.loop_vert = np.empty(nl, np.int64)
        mesh.loops.foreach_get("vertex_index", self.loop_vert)
        nt = len(mesh.loop_triangles)
        self.tri_verts = np.empty(nt * 3, np.int64)
        self.tri_loops = np.empty(nt * 3, np.int64)
        self.tri_poly = np.empty(nt, np.int64)
        mesh.loop_triangles.foreach_get("vertices", self.tri_verts)
        mesh.loop_triangles.foreach_get("loops", self.tri_loops)
        mesh.loop_triangles.foreach_get("polygon_index", self.tri_poly)
        self.tri_verts = self.tri_verts.reshape(nt, 3)
        self.tri_loops = self.tri_loops.reshape(nt, 3)
        self.material = np.zeros(nf, np.int32)
        mesh.polygons.foreach_get("material_index", self.material)

        self.attrs = {"POINT": {}, "FACE": {}, "CORNER": {}, "EDGE": {}}
        for attr in mesh.attributes:
            if attr.name in SKIP_ATTRS or attr.name.startswith(".") or attr.data_type not in NUMERIC:
                continue
            count = {"POINT": nv, "FACE": nf, "CORNER": nl, "EDGE": len(mesh.edges)}.get(attr.domain)
            if count is None:
                continue
            self.attrs[attr.domain][attr.name] = (attr.data_type, _read_attr(attr, count))
        self.active_uv = mesh.uv_layers.active.name if mesh.uv_layers.active else None
        self.render_uv = next((u.name for u in mesh.uv_layers if u.active_render), None)

        self.face_of_loop = np.repeat(np.arange(nf), self.loop_total)
        self.component = self._components()
        self.region = self._regions()
        self.islands = {name: self._uv_islands(vals) for name, (dt, vals)
                        in self.attrs["CORNER"].items() if _is_uv(mesh, name)}

                                     
    def _components(self):
        nv = len(self.co)
        label = np.arange(nv)
        a = self.loop_vert
        b = self.loop_vert[self._next_loop()]
        for _ in range(64):
            m = np.minimum(label[a], label[b])
            np.minimum.at(label, a, m)
            np.minimum.at(label, b, m)
            label = label[label]
            if np.array_equal(label[a], label[b]):
                break
        while True:
            nxt = label[label]
            if np.array_equal(nxt, label):
                break
            label = nxt
        return label[self.loop_vert[self.loop_start]] if len(self.loop_start) else np.zeros(0, np.int64)

    def _next_loop(self):
        idx = np.arange(len(self.loop_vert))
        start = np.repeat(self.loop_start, self.loop_total)
        total = np.repeat(self.loop_total, self.loop_total)
        return start + (idx - start + 1) % total

                                                                           
                                                         
    def _regions(self):
        cols = [self.material.astype(np.int64)]
        for name, (dt, vals) in self.attrs["FACE"].items():
            if dt in DISCRETE:
                v = vals.reshape(len(vals), -1).astype(np.int64)
                for c in range(v.shape[1]):
                    if len(np.unique(v[:, c])) <= 256:
                        cols.append(v[:, c])
        key = np.stack(cols, axis=1)
        _, inv = np.unique(key, axis=0, return_inverse=True)
        return inv.ravel()

                                                               
    def _uv_islands(self, uv):
        nf = len(self.loop_start)
        nxt = self._next_loop()
        va, vb = self.loop_vert, self.loop_vert[nxt]
        key = np.minimum(va, vb) * len(self.co) + np.maximum(va, vb)
        order = np.argsort(key, kind="stable")
        ks = key[order]
        same = ks[1:] == ks[:-1]
        l1, l2 = order[:-1][same], order[1:][same]
                                                               
        fwd = va[l1] == va[l2]
        u1a, u1b = uv[l1], uv[nxt[l1]]
        u2a = np.where(fwd[:, None], uv[l2], uv[nxt[l2]])
        u2b = np.where(fwd[:, None], uv[nxt[l2]], uv[l2])
        ok = (np.abs(u1a - u2a).max(1) < 1e-5) & (np.abs(u1b - u2b).max(1) < 1e-5)
        f1, f2 = self.face_of_loop[l1[ok]], self.face_of_loop[l2[ok]]
        label = np.arange(nf)
        for _ in range(512):
            m = np.minimum(label[f1], label[f2])
            np.minimum.at(label, f1, m)
            np.minimum.at(label, f2, m)
            label = label[label]
            if np.array_equal(label[f1], label[f2]):
                break
        return label

    def vertex_groups(self, obj):
        if not obj.vertex_groups:
            return None
        w = np.zeros((len(self.co), len(obj.vertex_groups)), np.float32)
        for v in self.mesh.vertices:
            for g in v.groups:
                w[v.index, g.group] = g.weight
        return w


                                                                             
                  
def _bary(p, a, b, c):
    """Unclamped barycentric weights of p on triangle abc's plane (rows)."""
    v0, v1, v2 = b - a, c - a, p - a
    d00 = (v0 * v0).sum(1); d01 = (v0 * v1).sum(1); d11 = (v1 * v1).sum(1)
    d20 = (v2 * v0).sum(1); d21 = (v2 * v1).sum(1)
    den = d00 * d11 - d01 * d01
    den = np.where(np.abs(den) < 1e-30, 1e-30, den)
    v = (d11 * d20 - d01 * d21) / den
    w = (d00 * d21 - d01 * d20) / den
    return np.stack([1 - v - w, v, w], axis=1)


class _Tree:
    def __init__(self, co, tris, ids):
        self.ids = ids
        self.bvh = BVHTree.FromPolygons([tuple(x) for x in co], [tuple(t) for t in tris[ids]],
                                        all_triangles=True) if len(ids) else None

    def nearest(self, pts):
        loc = np.zeros((len(pts), 3)); tri = np.full(len(pts), -1, np.int64)
        if self.bvh is None:
            return loc, tri
        for i, p in enumerate(pts):
            hit = self.bvh.find_nearest(Vector(p))
            if hit[0] is not None:
                loc[i] = hit[0]; tri[i] = self.ids[hit[2]]
        return loc, tri


                                                                             
                                                       
def _target_faces(V, F, edge):
    area = 0.0
    for f in F:
        p = V[list(f)]
        for i in range(1, len(f) - 1):
            area += 0.5 * np.linalg.norm(np.cross(p[i] - p[0], p[i + 1] - p[0]))
    return max(8, int(area / max(edge * edge, 1e-12)))


def _temp_object(V, F, name="pd_retopo_tmp"):
    me = bpy.data.meshes.new(name)
    me.from_pydata([tuple(v) for v in V], [], [tuple(f) for f in F])
    me.validate(clean_customdata=False)
    ob = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(ob)
    return ob


def _read_object_mesh(me):
    V = np.empty(len(me.vertices) * 3); me.vertices.foreach_get("co", V)
    F = [tuple(p.vertices) for p in me.polygons]
    return V.reshape(-1, 3), F


def _drop(ob):
    me = ob.data
    bpy.data.objects.remove(ob, do_unlink=True)
    if me.users == 0:
        bpy.data.meshes.remove(me)


def remesh_voxel(V, F, st):
    ob = _temp_object(V, F)
    try:
        mod = ob.modifiers.new("remesh", "REMESH")
        mod.mode = "VOXEL"
        mod.voxel_size = st.edge_length
        mod.adaptivity = 0.0
        mod.use_smooth_shade = True
        dg = bpy.context.evaluated_depsgraph_get()
        ev = ob.evaluated_get(dg)
        return _read_object_mesh(ev.data)
    finally:
        _drop(ob)


def _clean_for_solver(ob, size):
    """Boolean cuts leave sliver faces and near-duplicate points; QuadriFlow
    rejects such a mesh as non-manifold. Only the solver's copy is cleaned."""
    import bmesh
    bm = bmesh.new()
    bm.from_mesh(ob.data)
    eps = max(size * 1e-6, 1e-7)
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=eps)
    bmesh.ops.dissolve_degenerate(bm, edges=bm.edges, dist=eps)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    bm.to_mesh(ob.data)
    bm.free()


def remesh_quadriflow(V, F, st):
    ob = _temp_object(V, F)
    try:
        _clean_for_solver(ob, float(np.ptp(V, axis=0).max()) if len(V) else 1.0)
        faces = min(st.max_faces, _target_faces(V, F, st.edge_length))
        with bpy.context.temp_override(active_object=ob, object=ob, selected_objects=[ob],
                                       selected_editable_objects=[ob]):
            res = bpy.ops.object.quadriflow_remesh(
                mode="FACES", target_faces=faces, use_preserve_sharp=True,
                use_preserve_boundary=True, use_mesh_symmetry=False, smooth_normals=False,
                seed=st.seed)
        if "FINISHED" not in res:
            raise RuntimeError("QuadriFlow did not finish")
        return _read_object_mesh(ob.data)
    finally:
        _drop(ob)


IM_NAMES = ("Instant Meshes", "Instant Meshes.exe")


def find_instant_meshes(explicit=""):
    cands = []
    if explicit:
        cands.append(bpy.path.abspath(explicit))
    cands += ["/Applications/Instant Meshes.app/Contents/MacOS/Instant Meshes",
              os.path.expanduser("~/Applications/Instant Meshes.app/Contents/MacOS/Instant Meshes")]
                                                                             
    for base in bpy.utils.script_paths(subdir="extensions") + bpy.utils.script_paths(subdir="addons"):
        for root in (base, *[os.path.join(base, d) for d in (os.listdir(base) if os.path.isdir(base) else ())]):
            for n in IM_NAMES:
                cands.append(os.path.join(root, "smart_remesh", "bin", n))
    for n in IM_NAMES:
        w = shutil.which(n)
        if w:
            cands.append(w)
    for c in cands:
        if c and os.path.isfile(c):
            return c
    return None


def _write_obj(path, V, F):
    with open(path, "w") as fh:
        for x, y, z in V:
            fh.write("v %.9g %.9g %.9g\n" % (x, y, z))
        for f in F:
            fh.write("f " + " ".join(str(i + 1) for i in f) + "\n")


def _read_obj(path):
    V, F = [], []
    with open(path) as fh:
        for line in fh:
            if line.startswith("v "):
                V.append([float(x) for x in line.split()[1:4]])
            elif line.startswith("f "):
                F.append(tuple(int(t.split("/")[0]) - 1 for t in line.split()[1:]))
    return np.asarray(V, np.float64).reshape(-1, 3), F


def remesh_instant_meshes(V, F, st):
    exe = find_instant_meshes(st.instant_meshes_path)
    if exe is None:
        raise RuntimeError("Instant Meshes not found: set its path, install the official build "
                           "in /Applications, or install Smart Remesh")
    if sys.platform == "darwin":
        try:
            if "com.apple.quarantine" in os.listxattr(exe):
                raise RuntimeError('macOS blocks the Instant Meshes binary (quarantine). In Terminal: '
                                   'xattr -d com.apple.quarantine "%s"' % exe)
        except (OSError, AttributeError):
            pass
    with tempfile.TemporaryDirectory(prefix="pd_retopo_") as tmp:
        src, dst = os.path.join(tmp, "in.obj"), os.path.join(tmp, "out.obj")
        _write_obj(src, V, F)
        faces = min(st.max_faces, _target_faces(V, F, st.edge_length))
        cmd = [exe, src, "-o", dst, "-f", str(faces), "-r", "4", "-p", "4",
               "-c", "%g" % st.crease_angle, "-S", str(st.smooth_iterations), "-b", "-d"]
        if st.quad_dominant:
            cmd.append("-D")
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=st.timeout)
        if proc.returncode != 0 or not os.path.isfile(dst):
            raise RuntimeError("Instant Meshes failed: " + (proc.stderr or proc.stdout)[-400:])
        return _read_obj(dst)


BACKENDS = {"INSTANT_MESHES": remesh_instant_meshes, "QUADRIFLOW": remesh_quadriflow,
            "VOXEL": remesh_voxel}


                                                                             
                       
def remesh_parts(src, st, report=print):
    """Return (V, faces, kept_mask) for the whole object, part by part."""
    backend = BACKENDS[st.backend]
    face_comp = src.component
    out_V, out_F, kept = [], [], []
    offset = 0
    comps, counts = np.unique(face_comp, return_counts=True)
    failed = 0
    for comp, count in zip(comps, counts):
        faces = np.nonzero(face_comp == comp)[0]
        loops = [src.loop_vert[src.loop_start[f]:src.loop_start[f] + src.loop_total[f]] for f in faces]
        used = np.unique(np.concatenate(loops))
        remap = np.full(len(src.co), -1, np.int64); remap[used] = np.arange(len(used))
        V = src.co[used]; F = [tuple(remap[l]) for l in loops]
        keep = count < st.keep_below
        if not keep:
            try:
                nV, nF = backend(V, F, st)
                if len(nF) == 0:
                    raise RuntimeError("empty result")
                                                                               
                def topology(faces):
                    edges={};adj={}
                    for face in faces:
                        if len(set(face))!=len(face):raise RuntimeError("degenerate output face")
                        for a,b in zip(face,face[1:]+face[:1]):
                            key=tuple(sorted((int(a),int(b))));edges[key]=edges.get(key,0)+1
                            adj.setdefault(int(a),set()).add(int(b));adj.setdefault(int(b),set()).add(int(a))
                    seen=set();stack=[next(iter(adj))] if adj else []
                    while stack:
                        v=stack.pop()
                        if v in seen:continue
                        seen.add(v);stack.extend(adj[v]-seen)
                    return len(seen)==len(adj),bool(edges) and all(n==2 for n in edges.values())
                connected,closed=topology([tuple(f) for f in nF])
                _,was_closed=topology(F)
                if not connected or (was_closed and not closed):
                    raise RuntimeError("remesher changed fragment topology; original part retained")
            except Exception as exc:                                         
                failed += 1
                report("part %d kept unchanged: %s" % (comp, exc))
                keep = True
        if keep:
            nV, nF = V, F
        out_V.append(nV)
        out_F += [tuple(int(i) + offset for i in f) for f in nF]
        kept += [keep] * len(nF)
        offset += len(nV)
    V = np.concatenate(out_V) if out_V else np.zeros((0, 3))
    return V, out_F, np.asarray(kept, bool), failed


                                                                             
          
def _inner_points(mesh, co, nf):
    """A point inside every face: the centre of its largest triangle. The
    average of an n-gon's corners can fall outside a concave cut face and
    pick up the data of the face next to it."""
    mesh.calc_loop_triangles()
    nt = len(mesh.loop_triangles)
    tv = np.empty(nt * 3, np.int64); mesh.loop_triangles.foreach_get("vertices", tv); tv = tv.reshape(nt, 3)
    tp = np.empty(nt, np.int64); mesh.loop_triangles.foreach_get("polygon_index", tp)
    a, b, c = co[tv[:, 0]], co[tv[:, 1]], co[tv[:, 2]]
    area = np.linalg.norm(np.cross(b - a, c - a), axis=1)
    order = np.lexsort((area, tp))                                           
    last = np.ones(nt, bool); last[:-1] = tp[order][1:] != tp[order][:-1]
    best = order[last]
    out = np.zeros((nf, 3)); out[tp[best]] = (a[best] + b[best] + c[best]) / 3.0
    return out


def transfer(src, src_obj, dst_mesh, dst_obj=None, fit=True, smooth_angle=30.0, kept=None):
    """Write src's materials, UVs, attributes and vertex groups onto dst_mesh.

    dst_mesh must lie on (or near) src's surface, in the same object space.
    """
    nv, nf = len(dst_mesh.vertices), len(dst_mesh.polygons)
    co = np.empty(nv * 3); dst_mesh.vertices.foreach_get("co", co); co = co.reshape(nv, 3)
    ls = np.empty(nf, np.int64); lt = np.empty(nf, np.int64)
    dst_mesh.polygons.foreach_get("loop_start", ls); dst_mesh.polygons.foreach_get("loop_total", lt)
    lv = np.empty(len(dst_mesh.loops), np.int64); dst_mesh.loops.foreach_get("vertex_index", lv)
    face_of_loop = np.repeat(np.arange(nf), lt)

    tri_comp = src.component[src.tri_poly]
    all_tris = np.arange(len(src.tri_poly))
    whole = _Tree(src.co, src.tri_verts, all_tris)

                                                                                 
    cen = _inner_points(dst_mesh, co, nf)
    _, ctri = whole.nearest(cen)
    fcomp = tri_comp[ctri]
                                                                         
    trees = {c: _Tree(src.co, src.tri_verts, all_tris[tri_comp == c]) for c in np.unique(fcomp)}
    vcomp = np.full(nv, -1, np.int64); vcomp[lv] = fcomp[face_of_loop]

                                                        
    vloc = co.copy(); vtri = np.zeros(nv, np.int64)
    for c, tree in trees.items():
        ids = np.nonzero(vcomp == c)[0]
        loc, tri = tree.nearest(co[ids])
        vloc[ids] = loc; vtri[ids] = tri
    if fit:
        moved = vloc if kept is None else np.where(
            np.isin(np.arange(nv), lv[np.repeat(kept, lt)])[:, None], co, vloc)
        dst_mesh.vertices.foreach_set("co", moved.ravel())
        co = moved
        cen = _inner_points(dst_mesh, co, nf)
                                          
    ftri = np.zeros(nf, np.int64)
    for c, tree in trees.items():
        ids = np.nonzero(fcomp == c)[0]
        _, ftri[ids] = tree.nearest(cen[ids])
    fpoly = src.tri_poly[ftri]

                                                  
    dst_mesh.polygons.foreach_set("material_index", src.material[fpoly])
    for name, (dt, vals) in src.attrs["FACE"].items():
        _write_attr(dst_mesh, name, dt, "FACE", vals[fpoly])

                                                             
    tv = src.tri_verts[vtri]
    w = np.clip(_bary(co, src.co[tv[:, 0]], src.co[tv[:, 1]], src.co[tv[:, 2]]), 0, 1)
    w /= np.maximum(w.sum(1, keepdims=True), 1e-12)
    for name, (dt, vals) in src.attrs["POINT"].items():
        _write_attr(dst_mesh, name, dt, "POINT", _mix(dt, vals[tv], w))
                                                                              

                                                                               
                                                                               
                                                                               
    lco = co[lv]
    ltri = vtri[lv]
    fl_tri = ftri[face_of_loop]
    for name, (dt, vals) in src.attrs["CORNER"].items():
        label = src.islands.get(name, src.region)
        same = label[src.tri_poly[ltri]] == label[src.tri_poly[fl_tri]]
        same &= src.component[src.tri_poly[ltri]] == src.component[src.tri_poly[fl_tri]]
        use = np.where(same, ltri, fl_tri)
        tl = src.tri_loops[use]; tvv = src.tri_verts[use]
        wl = _bary(lco, src.co[tvv[:, 0]], src.co[tvv[:, 1]], src.co[tvv[:, 2]])
        wl = np.where(same[:, None], np.clip(wl, 0, 1), wl)
        wl /= np.where(np.abs(wl.sum(1, keepdims=True)) < 1e-12, 1, wl.sum(1, keepdims=True))
        val = _mix(dt, vals[tl], wl)
        if _is_uv(src.mesh, name):
            layer = dst_mesh.uv_layers.get(name) or dst_mesh.uv_layers.new(name=name)
            layer.uv.foreach_set("vector", np.ascontiguousarray(val, np.float32).ravel())
        else:
            _write_attr(dst_mesh, name, dt, "CORNER", val)
    if src.active_uv and src.active_uv in dst_mesh.uv_layers:
        dst_mesh.uv_layers.active = dst_mesh.uv_layers[src.active_uv]
    if src.render_uv and src.render_uv in dst_mesh.uv_layers:
        dst_mesh.uv_layers[src.render_uv].active_render = True

                                                                
    if dst_obj is not None and src_obj is not None and src_obj.vertex_groups:
        wsrc = src.vertex_groups(src_obj)
        dst_obj.vertex_groups.clear()
        vw = (wsrc[tv] * w[:, :, None]).sum(1)
        for gi, g in enumerate(src_obj.vertex_groups):
            ng = dst_obj.vertex_groups.new(name=g.name)
            col = vw[:, gi]
            for val in np.unique(np.round(col[col > 1e-4], 4)):
                ng.add(np.nonzero(np.round(col, 4) == val)[0].tolist(), float(val), "REPLACE")

                                                                     
    dst_mesh.update()
    region = src.region[fpoly]
    _rebuild_sharp(dst_mesh, region, smooth_angle)
    if dst_mesh.attributes.get("custom_normal"):
        dst_mesh.attributes.remove(dst_mesh.attributes["custom_normal"])
    return {"faces": nf, "verts": nv, "regions": int(len(np.unique(region)))}


def _mix(dt, vals, w):
    """vals (n, 3[, k]) mixed by w (n, 3). Discrete data takes the largest weight."""
    if dt in DISCRETE:
        return vals[np.arange(len(w)), w.argmax(1)]
    if vals.ndim == 2:
        return (vals * w).sum(1).astype(np.float32)
    return (vals * w[:, :, None]).sum(1).astype(np.float32)


def _rebuild_sharp(mesh, region, angle_deg):
    ne = len(mesh.edges)
    if ne == 0:
        return
    nl = len(mesh.loops)
    le = np.empty(nl, np.int64); mesh.loops.foreach_get("edge_index", le)
    lt = np.empty(len(mesh.polygons), np.int64); mesh.polygons.foreach_get("loop_total", lt)
    fol = np.repeat(np.arange(len(lt)), lt)
    nrm = np.empty(len(lt) * 3); mesh.polygons.foreach_get("normal", nrm); nrm = nrm.reshape(-1, 3)
    order = np.argsort(le, kind="stable"); e = le[order]; f = fol[order]
    pair = e[1:] == e[:-1]
    ea, fa, fb = e[:-1][pair], f[:-1][pair], f[1:][pair]
    sharp = np.ones(ne, bool)                                 
    cosang = (nrm[fa] * nrm[fb]).sum(1)
    sharp[ea] = (region[fa] != region[fb]) | (cosang < np.cos(np.radians(angle_deg)))
    _write_attr(mesh, "sharp_edge", "BOOLEAN", "EDGE", sharp)
    sf = mesh.attributes.get("sharp_face")
    if sf is None:
        _write_attr(mesh, "sharp_face", "BOOLEAN", "FACE", np.zeros(len(lt), bool))


                                                                             
              
def retopo_object(context, obj, st, report=print):
    src = Source(obj.data)
    V, F, kept, failed = remesh_parts(src, st, report)
    me = bpy.data.meshes.new(obj.data.name + "_retopo")
    me.from_pydata([tuple(v) for v in V], [], F)
    marker=me.attributes.new("_pd_retopo_kept","BOOLEAN","FACE")
    marker.data.foreach_set("value",kept)
    me.validate(clean_customdata=False)
    kept=np.empty(len(me.polygons),bool)
    me.attributes["_pd_retopo_kept"].data.foreach_get("value",kept)
    me.attributes.remove(me.attributes["_pd_retopo_kept"])
    for m in obj.data.materials:
        me.materials.append(m)
    new = bpy.data.objects.new(obj.name + "_retopo", me)
    for coll in obj.users_collection:
        coll.objects.link(new)
    new.matrix_world = obj.matrix_world.copy()
    new[RETOPO_SOURCE] = obj.name
    try:
        info = transfer(src, obj, me, new, fit=st.fit_surface, smooth_angle=st.smooth_angle, kept=kept)
    except Exception:
        bpy.data.objects.remove(new,do_unlink=True)
        if me.users==0:bpy.data.meshes.remove(me)
        raise
    info["parts_kept"] = int(kept.sum())
    info["failed"] = failed
    return new, info


def transfer_objects(src_obj, dst_obj, fit=False, smooth_angle=30.0):
    """Transfer onto an object remeshed elsewhere (Smart Remesh, Quad Remesher...)."""
    src = Source(src_obj.data)
    dst = dst_obj.data
    if dst.users > 1:
        dst = dst.copy(); dst_obj.data = dst
                                     
    m = src_obj.matrix_world.inverted() @ dst_obj.matrix_world
    if m != m.Identity(4):
        dst.transform(m)
        dst_obj.matrix_world = src_obj.matrix_world.copy()
    names = [m.name if m else None for m in dst.materials]
    if [m.name if m else None for m in src_obj.data.materials] != names:
        dst.materials.clear()
        for mat in src_obj.data.materials:
            dst.materials.append(mat)
    return transfer(src, src_obj, dst, dst_obj, fit=fit, smooth_angle=smooth_angle)


                                                                             
    
# здесь хранятся значения, которые сохраняются вместе с объектом
class PD_RetopoSettings(bpy.types.PropertyGroup):
    backend: bpy.props.EnumProperty(name="Engine", default="QUADRIFLOW", items=(
        ("INSTANT_MESHES", "Instant Meshes", "Official Instant Meshes binary (BSD), run as a separate process"),
        ("QUADRIFLOW", "QuadriFlow", "Blender's built-in quad remesher"),
        ("VOXEL", "Voxel", "Blender's voxel remesh: even triangles/quads, closes small gaps")))
    edge_length: bpy.props.FloatProperty(name="Edge Length", default=0.02, min=1e-5, soft_max=1.0,
        subtype="DISTANCE", description="Target edge length of the new mesh, in object units")
    max_faces: bpy.props.IntProperty(name="Max Faces per Part", default=200000, min=8)
    keep_below: bpy.props.IntProperty(name="Keep Parts Below", default=200, min=0,
        description="Loose parts (shards, debris) with fewer faces are copied unchanged")
    fit_surface: bpy.props.BoolProperty(name="Fit to Original", default=True,
        description="Move new vertices back onto the original surface of their own part")
    smooth_angle: bpy.props.FloatProperty(name="Sharp Angle", default=30.0, min=0.0, max=180.0,
        description="Edges above this angle, and every border between damage regions, are marked sharp")
    crease_angle: bpy.props.FloatProperty(name="Crease Angle", default=30.0, min=0.0, max=180.0,
        description="Instant Meshes: dihedral angle above which an edge is followed as a crease")
    smooth_iterations: bpy.props.IntProperty(name="Smoothing", default=2, min=0, max=10)
    quad_dominant: bpy.props.BoolProperty(name="Quad Dominant", default=False,
        description="Instant Meshes: allow triangles instead of splitting every face into quads")
    seed: bpy.props.IntProperty(name="Seed", default=0, min=0)
    timeout: bpy.props.IntProperty(name="Timeout (s)", default=300, min=5)
    instant_meshes_path: bpy.props.StringProperty(name="Instant Meshes", subtype="FILE_PATH",
        description="Path to the Instant Meshes executable. Empty: look in /Applications and "
                    "in an installed Smart Remesh extension")


# действие «Retopology Copy»
class PD_OT_retopo(bpy.types.Operator):
    bl_idname = "project_damage.retopo"
    bl_label = "Retopology Copy"
    bl_description = ("Remesh a copy of the active mesh part by part and carry over materials, "
                      "UVs, attributes and vertex groups. The original is not changed")
    bl_options = {"REGISTER", "UNDO"}

    @classmethod
    def poll(cls, context):
        o = context.active_object
        return o is not None and o.type == "MESH" and len(o.data.polygons) > 0

    def execute(self, context):
        obj = context.active_object
        st = context.scene.project_damage_retopo
        notes = []
        try:
            new, info = retopo_object(context, obj, st, report=notes.append)
        except Exception as exc:
            self.report({"ERROR"}, str(exc))
            return {"CANCELLED"}
        for n in notes[:5]:
            self.report({"WARNING"}, n)
        new["pd_retopo_source_hide_render"]=obj.hide_render
        obj.hide_set(True)
        obj.hide_render=True
        for o in context.selected_objects:
            o.select_set(False)
        new.select_set(True)
        context.view_layer.objects.active = new
        self.report({"INFO"}, "Retopology: %d faces, %d regions, %d faces kept as is"
                    % (info["faces"], info["regions"], info["parts_kept"]))
        return {"FINISHED"}


# действие «Transfer Damage Data»
class PD_OT_transfer_damage_data(bpy.types.Operator):
    bl_idname = "project_damage.transfer_damage_data"
    bl_label = "Transfer Damage Data"
    bl_description = ("Copy materials, UVs, damage selections, attributes and vertex groups from the "
                      "selected original onto the active remeshed object (made with any remesher)")
    bl_options = {"REGISTER", "UNDO"}

    fit: bpy.props.BoolProperty(name="Fit to Original", default=False)

    @classmethod
    def poll(cls, context):
        a = context.active_object
        return (a is not None and a.type == "MESH"
                and any(o is not a and o.type == "MESH" for o in context.selected_objects))

    def execute(self, context):
        dst = context.active_object
        src = next(o for o in context.selected_objects if o is not dst and o.type == "MESH")
        st = context.scene.project_damage_retopo
        info = transfer_objects(src, dst, fit=self.fit, smooth_angle=st.smooth_angle)
        self.report({"INFO"}, "Transferred from %s: %d faces, %d regions" % (src.name, info["faces"], info["regions"]))
        return {"FINISHED"}


CLASSES = (PD_RetopoSettings, PD_OT_retopo, PD_OT_transfer_damage_data)


# подключаем свойства и действия этого модуля к Blender
def register():
    for c in CLASSES:
        bpy.utils.register_class(c)
    bpy.types.Scene.project_damage_retopo = bpy.props.PointerProperty(type=PD_RetopoSettings)


# убираем подключения модуля при выключении аддона
def unregister():
    del bpy.types.Scene.project_damage_retopo
    for c in reversed(CLASSES):
        bpy.utils.unregister_class(c)
