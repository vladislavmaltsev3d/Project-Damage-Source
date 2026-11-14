# возвращаем исходное освещение граней там, где повреждения их не затронули
"""Source-normal restore for the surviving surface of a cut result.

SPDX-License-Identifier: GPL-3.0-or-later

A boolean cut re-triangulates every source face a crack passes through:
the crack mouth is surrounded by long, thin fan triangles that join the
new rim vertices to the old quad corners. Blender then derives the corner
normals of those faces from their own (sliver) geometry, and the encoded
custom normals copied from the source (`custom_normal`, INT16_2D) are
relative to lnor spaces that no longer exist. On a low-poly retopo whose
look comes from custom normals - or a sculpt shaded smooth - the result is
fans of dark/bright streaks running out of every crack.

The surviving surface never moved: its true shading is the source's own
corner normal at the same spot. `restore` interpolates it (barycentric, in
the source triangle that contains the corner) for every corner of a face
that still lies on the source surface, and keeps the current normal for
every damage corner. Geometry, UVs, materials and flags are not touched.
"""
import numpy as np
from mathutils.bvhtree import BVHTree

                                                                         
                                                                        
                                                       
ON_SURFACE_TOL = 1e-5
                                                                        
                                                                    
FACING_COS = 0.5
                                                                        
                                                                           
                                                 
INSET = 1e-3


class SourceNormals:
    """Read-only numpy/BVH snapshot of the source surface and its corner
    normals (custom normals included), in the result's coordinates."""

    def __init__(self, mesh):
        mesh.calc_loop_triangles()
        nv, nt = len(mesh.vertices), len(mesh.loop_triangles)
        co = np.empty(nv * 3, dtype=np.float64)
        mesh.vertices.foreach_get('co', co)
        self.co = co.reshape(-1, 3)
        tv = np.empty(nt * 3, dtype=np.int32)
        tl = np.empty(nt * 3, dtype=np.int32)
        mesh.loop_triangles.foreach_get('vertices', tv)
        mesh.loop_triangles.foreach_get('loops', tl)
        self.tri_verts = tv.reshape(-1, 3)
        self.tri_loops = tl.reshape(-1, 3)
        cn = np.empty(len(mesh.loops) * 3, dtype=np.float32)
        mesh.corner_normals.foreach_get('vector', cn)
        self.corner_normals = cn.reshape(-1, 3).astype(np.float64)
        a, b, c = (self.co[self.tri_verts[:, k]] for k in range(3))
        n = np.cross(b - a, c - a)
        self.tri_normal = n / np.maximum(np.linalg.norm(n, axis=1), 1e-30)[:, None]
        self.diag = float(np.linalg.norm(self.co.max(0) - self.co.min(0))) if nv else 0.0


    def lookup(self, points, facing, tol):
        """Nearest source triangle for each point, or -1 when it is farther
        than `tol` or faces away from `facing`."""
        tri = np.full(len(points), -1, dtype=np.int64)
        loc = np.zeros_like(points)
        for i, (p, f) in enumerate(zip(points.tolist(), facing.tolist())):
            hit, _n, idx, dist = self.bvh.find_nearest(p, tol)
            if hit is None:
                continue
            if self.tri_normal[idx] @ np.asarray(f) < FACING_COS:
                continue
            tri[i] = idx
            loc[i] = hit
        return tri, loc

    def interpolate(self, tri, loc):
        verts = self.tri_verts[tri]
        a, b, c = (self.co[verts[:, k]] for k in range(3))
        v0, v1, v2 = b - a, c - a, loc - a
        d00 = np.einsum('ij,ij->i', v0, v0)
        d01 = np.einsum('ij,ij->i', v0, v1)
        d11 = np.einsum('ij,ij->i', v1, v1)
        d20 = np.einsum('ij,ij->i', v2, v0)
        d21 = np.einsum('ij,ij->i', v2, v1)
        den = d00 * d11 - d01 * d01
        den = np.where(np.abs(den) > 1e-300, den, 1.0)
        w1 = np.clip((d11 * d20 - d01 * d21) / den, 0.0, 1.0)
        w2 = np.clip((d00 * d21 - d01 * d20) / den, 0.0, 1.0)
        w0 = np.clip(1.0 - w1 - w2, 0.0, 1.0)
        loops = self.tri_loops[tri]
        n = (w0[:, None] * self.corner_normals[loops[:, 0]]
             + w1[:, None] * self.corner_normals[loops[:, 1]]
             + w2[:, None] * self.corner_normals[loops[:, 2]])
        return n / np.maximum(np.linalg.norm(n, axis=1), 1e-30)[:, None]


def _face_mask(mesh, name):
    out = np.zeros(len(mesh.polygons), dtype=bool)
    attr = mesh.attributes.get(name)
    if attr is not None and attr.domain == 'FACE' and attr.data_type == 'BOOLEAN':
        attr.data.foreach_get('value', out)
    return out


def restore_steps(mesh, source, base_faces):
    from . import background_compute as bg
    if not len(mesh.polygons) or source is None or source.diag <= 0:
        return 0, None
    def array(items, field, width, dtype):
        a=np.empty(len(items)*width,dtype=dtype)
        items.foreach_get(field,a)
        return a.reshape(-1,width) if width>1 else a
    data={
        'total':array(mesh.polygons,'loop_total',1,np.int32),
        'loop_vert':array(mesh.loops,'vertex_index',1,np.int32),
        'co':array(mesh.vertices,'co',3,np.float64),
        'fn':array(mesh.polygons,'normal',3,np.float64),
        'normals':array(mesh.corner_normals,'vector',3,np.float64),
    }
    normals,count,faces=yield from bg.task('Preserving source surface shading',compute_restore,vars(source),data,np.asarray(base_faces,dtype=bool))
    if count:
        mesh.normals_split_custom_set(normals.tolist())
        mesh.update()
    return count,faces


def restore(mesh, source, base_faces):
    from . import background_compute as bg
    return bg.immediate(restore_steps(mesh,source,base_faces))


def compute_restore(source_data, data, base_faces):
    source=SourceNormals.__new__(SourceNormals)
    source.__dict__.update(source_data)
    source.bvh=BVHTree.FromPolygons(source.co.tolist(),source.tri_verts.tolist())
    total=data['total'];loop_vert=data['loop_vert'];co=data['co'];fn=data['fn'];normals=data['normals']
    n_polys=len(total)
    loop_face=np.repeat(np.arange(n_polys),total)
    centre=np.zeros((n_polys,3))
    np.add.at(centre,loop_face,co[loop_vert])
    centre/=np.maximum(total,1)[:,None]
    cand=np.flatnonzero(base_faces[loop_face])
    if not len(cand):return normals,0,None
    tol=ON_SURFACE_TOL*source.diag
    faces=loop_face[cand];p=co[loop_vert[cand]]
    probe=p+(centre[faces]-p)*INSET
    tri,loc=source.lookup(probe,fn[faces],tol+np.linalg.norm(centre[faces]-p,axis=1).max()*INSET)
    ok=tri>=0
    valid=np.flatnonzero(ok)
    if len(valid):
        n=source.tri_normal[tri[valid]]
        a=source.co[source.tri_verts[tri[valid],0]]
        ok[valid]&=np.abs(np.einsum('ij,ij->i',p[valid]-a,n))<=tol
    face_ok=np.ones(n_polys,dtype=bool)
    np.logical_and.at(face_ok,faces,ok)
    use=ok&face_ok[faces]
    if not np.any(use):return normals,0,None
    normals[cand[use]]=source.interpolate(tri[use],loc[use])
    restored=np.zeros(n_polys,dtype=bool)
    restored[np.unique(faces)]=True
    restored&=face_ok
    return normals,int(use.sum()),restored


def surviving_faces(mesh, region_attr):
    """Faces apply_phong left as surviving source surface: every face NOT in
    the damage shading region (the region holds every non-base class, the
    generated-geometry tags and the base faces that left the source)."""
    region = _face_mask(mesh, region_attr)
    return ~region


def source_mesh(context, obj, evaluated):
    """(mesh, release) for the mesh the core read: the evaluated mesh when
    preparation used modifiers, otherwise the authored data."""
    if not evaluated:
        return obj.data, (lambda: None)
    ev = obj.evaluated_get(context.evaluated_depsgraph_get())
    mesh = ev.to_mesh()
    return mesh, ev.to_mesh_clear
