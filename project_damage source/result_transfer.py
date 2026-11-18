# переносим материалы и UV исходной поверхности на результат
"""Non-destructive source-data transfer to generated ProjectDamage geometry.

Exact face provenance wins over spatial sampling. New geometry uses closest
surface barycentrics; UVs are CORNER data, never averaged across a UV seam.
Internal Blender attributes and ProjectDamage's generated damage masks are reserved.
"""
from .blender_compat import enable_nodes
import bpy
import numpy as np
from mathutils import Vector
from mathutils.bvhtree import BVHTree
from mathutils.kdtree import KDTree
from .mesh_io import CLASS_NAMES, BASE_CLASS, SHADING_REGION_ATTR, SourceShading

WALLPAPER_REGIONS = (
    ('wallpaper_front_polys', 'project_damage_wallpaper_front', 'wallpaper_material'),
    ('wallpaper_back_polys', 'project_damage_wallpaper_back', 'wallpaper_back_material'),
    ('wallpaper_wall_polys', 'project_damage_wallpaper_wall', 'wall_material'),
)
BRICK_REGIONS = tuple(('brick_' + name + '_polys', 'project_damage_brick_' + name,
                       'brick_' + name + '_material') for name in ('front', 'break', 'seam', 'behind', 'render'))
EXTRA_REGIONS = tuple((key+'_polys','project_damage_'+key,key+'_material') for key in ('bevel','wear','scratch_groove','scratch_scuff'))
MATERIAL_REGIONS = WALLPAPER_REGIONS + BRICK_REGIONS + EXTRA_REGIONS
RESERVED = {'project_damage_'+k for k in ('edge_wear','bevel_damage','scratch','metal_dent','metal_crease','metal_contact')} | set(CLASS_NAMES) | {row[1] for row in MATERIAL_REGIONS} | {'project_damage_wallpaper_flap'} | {SHADING_REGION_ATTR, 'position', 'material_index', 'sharp_face',
    'sharp_edge', 'normal', 'project_damage_rust', 'project_damage_rust_streak', 'project_damage_heat_exposure', 'project_damage_heat_char', 'project_damage_heat_char_depth'}

def apply_wallpaper_materials(mesh, out, props):
    apply_region_materials(mesh, out, props, WALLPAPER_REGIONS)
    from . import wallpaper_flaps
    wallpaper_flaps.write_face_mask(mesh, out)


def apply_brick_materials(mesh, out, props):
    apply_region_materials(mesh, out, props, BRICK_REGIONS)


def apply_region_materials(mesh, out, props, regions):
    """Apply precise native face tags before any host topology remapping."""
    count = len(mesh.polygons)
    for key, attr_name, prop in regions:
        indices = np.asarray(out.get(key, ()), dtype=np.int64)
        if not len(indices):
            continue
        if indices.min() < 0 or indices.max() >= count:
            raise ValueError('Material region face indices are out of range')
        mask = np.zeros(count, dtype=bool)
        mask[indices] = True
        attr = mesh.attributes.get(attr_name) or mesh.attributes.new(attr_name, 'BOOLEAN', 'FACE')
        attr.data.foreach_set('value', mask)
        material = getattr(props, prop)
        from . import material_roles
        if prop in material_roles.ROLES:
            material = material_roles.resolve(props, prop, material_roles.region_material(mesh, indices))
        if material is None and prop in {'brick_front_material','brick_break_material','brick_seam_material'}:
            from .material_defaults import get
            material=get('mortar' if prop=='brick_seam_material' else 'brick')
        if material is None and prop in {'brick_behind_material','brick_render_material'}:
            material=props.surface_damage_material
            if material is None:
                name='ProjectDamage_Brick_Render' if prop=='brick_render_material' else 'ProjectDamage_Brick_Backing'
                material=bpy.data.materials.get(name)
                if material is None:
                    material=bpy.data.materials.new(name);enable_nodes(material)
                    material.diffuse_color=(.42,.40,.36,1.)
                    shader=material.node_tree.nodes.get('Principled BSDF')
                    shader.inputs['Base Color'].default_value=material.diffuse_color
                    shader.inputs['Roughness'].default_value=.9
        if material is None:
            continue
        slot = next((i for i, m in enumerate(mesh.materials) if m == material), None)
        if slot is None:
            slot = len(mesh.materials)
            mesh.materials.append(material)
        for face in indices:
            mesh.polygons[int(face)].material_index = slot


def capture(mesh, snap):
    mesh.calc_loop_triangles()
    n = len(mesh.loop_triangles)
    loops = np.empty(n * 3, np.int32)
    faces = np.empty(n, np.int32)
    mesh.loop_triangles.foreach_get('loops', loops)
    mesh.loop_triangles.foreach_get('polygon_index', faces)
    snap.transfer_tris = loops.reshape(-1, 3)
    snap.transfer_faces = faces
    edge = np.empty(len(mesh.edges) * 2, np.int32)
    mesh.edges.foreach_get('vertices', edge)
    snap.transfer_edges = edge.reshape(-1, 2)
                                                                            
                                                                          
                                                                      
                                                                          
    loop_edge=np.empty(len(mesh.loops),np.int32);mesh.loops.foreach_get('edge_index',loop_edge)
    smooth=np.empty(len(mesh.polygons),bool);mesh.polygons.foreach_get('use_smooth',smooth)
    sharp=np.empty(len(mesh.edges),bool);mesh.edges.foreach_get('use_edge_sharp',sharp)
    snap.shading=(snap.points,snap.loop_start,snap.loop_total,loop_edge,snap.transfer_edges,smooth,sharp,
                  snap.loop_verts[snap.transfer_tris],faces,snap.loop_verts)
    snap.uv_render = next((u.name for u in mesh.uv_layers if u.active_render), None)
    snap.color_active = mesh.color_attributes.active_color_name
    snap.color_render = mesh.color_attributes.render_color_index
    snap.transfer_attrs = {}
    snap.transfer_skipped = []
    for attr in mesh.attributes:
        if attr.name.startswith('.') or attr.name in RESERVED or attr.name in snap.uv:
            continue
        if attr.domain not in {'POINT', 'FACE', 'CORNER', 'EDGE'}:
            snap.transfer_skipped.append(attr.name)
            continue
        if not len(attr.data):
            continue
        item = attr.data[0]
        field = next((p for p in ('value', 'vector', 'color') if hasattr(item, p)), None)
        if field is None:
            snap.transfer_skipped.append(attr.name)
            continue
        if attr.data_type == 'STRING':
            values = [getattr(x, field) for x in attr.data]
            width = 1
        else:
            sample = np.asarray(getattr(item, field))
            width = max(1, sample.size)
            dtype = bool if attr.data_type == 'BOOLEAN' else np.int32 if sample.dtype.kind in 'iu' else np.float32
            values = np.empty(len(attr.data) * width, dtype=dtype)
            try:
                attr.data.foreach_get(field, values)
            except (TypeError, RuntimeError):
                snap.transfer_skipped.append(attr.name)
                continue
            values = values.reshape(len(attr.data), width)
        snap.transfer_attrs[attr.name] = (attr.domain, attr.data_type, field, width, values)

def barycentric(points, triangles):
    a, b, c = triangles[:, 0], triangles[:, 1], triangles[:, 2]
    ab, ac, ap = b-a, c-a, points-a
    d00 = np.einsum('ij,ij->i', ab, ab)
    d01 = np.einsum('ij,ij->i', ab, ac)
    d11 = np.einsum('ij,ij->i', ac, ac)
    d20 = np.einsum('ij,ij->i', ap, ab)
    d21 = np.einsum('ij,ij->i', ap, ac)
    den = d00*d11-d01*d01
    safe = np.where(np.abs(den) > 1e-25, den, 1)
    v, w = (d11*d20-d01*d21)/safe, (d00*d21-d01*d20)/safe
    out = np.stack([1-v-w, v, w], axis=1)
    out[np.abs(den) <= 1e-25] = [1, 0, 0]
    # Outside a source triangle: clamp to its boundary. This is an appearance
    # projection for newly created interiors, not a new authored UV unwrap.
    out = np.maximum(out, 0)
    return out / np.maximum(out.sum(axis=1, keepdims=True), 1e-30)

def transfer_maps(source, points, lt, lv, normals, base, edge_data=None):
    from types import SimpleNamespace
    snap=SimpleNamespace(**source);self=SimpleNamespace()
    self.source_tri = snap.loop_verts[snap.transfer_tris]
    tri_xyz = snap.points[self.source_tri]
    tree = BVHTree.FromPolygons(np.asarray(snap.points, np.float64).tolist(),
                                    self.source_tri.tolist(), all_triangles=True)
    n = len(points)
    self.points = points
    # Plain Python lists in and out: the per-point cost is the BVH query itself,
    # not numpy row objects and Vector construction.
    find_nearest = tree.find_nearest
    point_rows = np.asarray(self.points, np.float64).tolist()
    tri_rows = [0] * n
    for i, p in enumerate(point_rows):
        loc, _normal, tri, _dist = find_nearest(p)
        if tri is not None:
            tri_rows[i] = tri
            point_rows[i] = loc[:]
    self.pt_tri = np.asarray(tri_rows, np.int32).reshape(n)
    locations = np.asarray(point_rows, self.points.dtype).reshape(self.points.shape)
    self.pt_bary = barycentric(locations, tri_xyz[self.pt_tri])
    self.loop_face = np.repeat(np.arange(len(lt)), lt)
    self.loop_vertex = lv
    centers = np.zeros((len(lt), 3))
    np.add.at(centers, self.loop_face, self.points[lv])
    centers /= lt[:, None]
    self.face_tri = np.zeros(len(lt), np.int32)
    eps = max(1e-7, np.linalg.norm(np.ptp(snap.points, axis=0))*1e-5)
    normal_rows = np.asarray(normals, np.float64).tolist()
    for i, p in enumerate(centers.tolist()):
        _loc, normal, tri, dist = find_nearest(p)
        if tri is None:
            continue
                                                                    
                                                                        
        wanted = normal_rows[i]
        if normal.dot(wanted) < .5:
            candidates = tree.find_nearest_range(p, dist+eps)
            if candidates:
                tri = max(candidates, key=lambda q: q[1].dot(wanted))[2]
        self.face_tri[i] = tri
    self.face_source = snap.transfer_faces[self.face_tri].copy()
    if base is not None and len(base) == len(lt):
        base = np.asarray(base)
        traced = (base >= 0) & (base < len(snap.loop_total))
        self.face_source[traced] = base[traced]
                                                                       
                                                            
    groups = [[] for _ in snap.loop_total]
    for tri, face in enumerate(snap.transfer_faces): groups[face].append(tri)
    count = max(map(len, groups), default=1)
    table = np.full((len(groups), count), -1, np.int32)
    for face, tris in enumerate(groups): table[face, :len(tris)] = tris
    src_face = self.face_source[self.loop_face]
    q = self.points[lv]
    self.loop_tri = self.face_tri[self.loop_face].copy()
    self.loop_bary = np.zeros((len(lv), 3))
    best = np.full(len(lv), np.inf)
    for k in range(count):
        tri = table[src_face, k]
        use = np.flatnonzero(tri >= 0)
        weights = barycentric(q[use], tri_xyz[tri[use]])
        projected = np.einsum('ij,ijk->ik', weights, tri_xyz[tri[use]])
        distance = np.einsum('ij,ij->i', q[use]-projected, q[use]-projected)
        take = distance < best[use]
        ix = use[take]
        best[ix] = distance[take]
        self.loop_tri[ix] = tri[ix]
        self.loop_bary[ix] = weights[take]
    self.edge_source = None
    if edge_data is not None:
        src_edges, dst_edges = edge_data
        mids = snap.points[src_edges].mean(axis=1)
        if len(mids):
            tree=KDTree(len(mids))
            for i,v in enumerate(mids):tree.insert(Vector(v),i)
            tree.balance()
            self.edge_source=np.fromiter((tree.find(Vector(v))[1] for v in self.points[dst_edges].mean(axis=1)),dtype=np.int32,count=len(dst_edges))

    return vars(self)

def repair_degenerate_uv(uv, points, loop_vertex, loop_total, loop_tri, source_tri, source_points, source_uv_tris):
    """Give real UV area to generated faces whose transferred UVs collapsed.

    The transfer projects every corner onto its source face and clamps it to
    that face. Two kinds of new faces collapse to a line or a point in UV:
    walls that stand on the surface (hole rims, crater sides) and faces that
    lie past the border of their source face (bevels, chamfers).

    Only those faces are rewritten. Each one continues the UV map of its
    source triangle without clamping; the distance from the source plane is
    unfolded sideways at the same texel density, so a wall receives a strip of
    the texture that starts exactly at the rim. Faces that already have UV
    area are returned untouched.
    """
    faces = len(loop_total)
    if not faces or not len(uv):
        return uv, 0
    start = np.zeros(faces, np.int64)
    np.cumsum(loop_total[:-1], out=start[1:])
    loop_face = np.repeat(np.arange(faces), loop_total)
    nxt = np.arange(len(loop_vertex)) + 1
    last = start + loop_total - 1
    nxt[last] = start
    pos = points[loop_vertex]
    cross3 = np.cross(pos, pos[nxt])
    normal = np.zeros((faces, 3))
    np.add.at(normal, loop_face, cross3)
    area3 = np.linalg.norm(normal, axis=1) * .5
    def uv_area(coords):
        return np.bincount(loop_face, coords[:, 0] * coords[nxt, 1] - coords[nxt, 0] * coords[:, 1], faces) * .5
    area_uv = uv_area(uv)
    tri = loop_tri[start]
    corners = source_tri[tri]
    a = source_points[corners[:, 0]]
    e1 = source_points[corners[:, 1]] - a
    e2 = source_points[corners[:, 2]] - a
    tri_uv = source_uv_tris[tri]
    uv_a = tri_uv[:, 0]
    du1 = tri_uv[:, 1] - uv_a
    du2 = tri_uv[:, 2] - uv_a
    source_normal = np.cross(e1, e2)
    source_area = np.linalg.norm(source_normal, axis=1)
    source_uv_area = du1[:, 0] * du2[:, 1] - du1[:, 1] * du2[:, 0]
    usable = (source_area > 1e-20) & (np.abs(source_uv_area) > 1e-20)
    density = np.zeros(faces)
    density[usable] = np.abs(source_uv_area[usable]) / source_area[usable]
    extent = float(np.ptp(points, axis=0).max()) if len(points) else 0.0
    broken = usable & (area3 > (extent * 1e-7) ** 2) & (np.abs(area_uv) < area3 * density * 1e-3)
    if not broken.any():
        return uv, 0
    rows = np.flatnonzero(broken)
    loops = np.flatnonzero(broken[loop_face])
    face = loop_face[loops]
    a, e1, e2 = a[face], e1[face], e2[face]
    unit = source_normal[face] / source_area[face][:, None]
    offset = pos[loops] - a
    # in-plane coordinates on the unclamped source triangle
    g11 = np.einsum('ij,ij->i', e1, e1); g12 = np.einsum('ij,ij->i', e1, e2); g22 = np.einsum('ij,ij->i', e2, e2)
    det = g11 * g22 - g12 * g12
    det = np.where(np.abs(det) > 1e-30, det, 1.0)
    def planar(vector):
        d1 = np.einsum('ij,ij->i', vector, e1); d2 = np.einsum('ij,ij->i', vector, e2)
        u = (g22 * d1 - g12 * d2) / det; v = (g11 * d2 - g12 * d1) / det
        return u[:, None] * du1[face] + v[:, None] * du2[face]
    # Hinge each face flat into its source plane (rotation that takes the face
    # normal onto the source normal, about the corner nearest to that plane),
    # then read the source UV map there. Lengths on the face are preserved, the
    # texture is not mirrored and the rim corner keeps its surface UV.
    face_normal = normal[face] / np.maximum(np.linalg.norm(normal[face], axis=1), 1e-30)[:, None]
    height = np.abs(np.einsum('ij,ij->i', offset, unit))
    order = np.lexsort((height, face))
    first = np.ones(len(order), bool)
    first[1:] = face[order][1:] != face[order][:-1]
    pivot_loop = np.zeros(faces, np.int64)
    pivot_loop[face[order][first]] = loops[order][first]
    pivot = pos[pivot_loop[face]]
    cosine = np.einsum('ij,ij->i', face_normal, unit)
    axis = np.cross(face_normal, unit)
    turn = cosine > -0.999
    rel = pos[loops] - pivot
    rotated = (rel * cosine[:, None] + np.cross(axis, rel)
               + axis * (np.einsum('ij,ij->i', axis, rel) / np.maximum(1.0 + cosine, 1e-3))[:, None])
    rel = np.where(turn[:, None], rotated, rel)
    trial = uv.copy()
    trial[loops] = uv_a[face] + planar(pivot + rel - a)
    area = uv_area(trial)
    fixed = broken & np.isfinite(area) & (np.abs(area) > area3 * density * 1e-3)
    if not fixed.any():
        return uv, 0
    mask = fixed[loop_face]
    result = uv.copy()
    result[mask] = trial[mask]
    return result, int(fixed.sum())


class Transfer:
    def __init__(self, snap, mesh, out, factor):
        from . import background_compute as bg
        bg.immediate(self.prepare(snap,mesh,out))

    def prepare(self,snap,mesh,out):
        from . import background_compute as bg
        self.snap,self.mesh=snap,mesh
        def array(items,field,width,dtype):
            data=np.empty(len(items)*width,dtype);items.foreach_get(field,data)
            return data.reshape(-1,width) if width>1 else data
        inputs={k:getattr(snap,k) for k in ('points','loop_verts','transfer_tris','transfer_faces','loop_total')}
        mapped=yield from bg.task('Mapping source UVs and attributes',transfer_maps,inputs,
            array(mesh.vertices,'co',3,np.float32).astype(np.float64),array(mesh.polygons,'loop_total',1,np.int32),
            array(mesh.loops,'vertex_index',1,np.int32),array(mesh.polygons,'normal',3,np.float32),out.get('base_poly'),
            (snap.transfer_edges,array(mesh.edges,'vertices',2,np.int32)) if any(x[0]=='EDGE' for x in snap.transfer_attrs.values()) else None)
        self.__dict__.update(mapped)

    def sample(self, values, indices, weights):
        if isinstance(values, list):
            return [values[int(i)] for i in indices[np.arange(len(indices)), weights.argmax(axis=1)]]
        if values.dtype.kind in 'fc':
            return np.einsum('ij,ijk->ik', weights, values[indices])
        return values[indices[np.arange(len(indices)), weights.argmax(axis=1)]]

    def uv_and_attributes(self, out, factor):
        from . import background_compute as bg
        return bg.immediate(self.attribute_steps(out,factor))

    def attribute_steps(self,out,factor):
        snap, mesh = self.snap, self.mesh
        skipped = list(snap.transfer_skipped)
        for name, values in snap.uv.items():
            uv = np.einsum('ij,ijk->ik', self.loop_bary, values[snap.transfer_tris[self.loop_tri]])
                                                                         
                                                                            
            base_uv = out.get('uvs')
            if name == snap.uv_active and base_uv is not None and len(mesh.polygons) and len(base_uv) == len(mesh.polygons):
                totals=np.empty(len(mesh.polygons),np.int32);mesh.polygons.foreach_get('loop_total',totals)
                a=np.asarray(base_uv);flat=a[np.arange(a.shape[1])[None,:] < totals[:,None]]
                good = np.isfinite(flat).all(axis=1)
                uv[good] = flat[good]
            try:
                totals = np.empty(len(mesh.polygons), np.int32); mesh.polygons.foreach_get('loop_total', totals)
                if len(uv) == int(totals.sum()) == len(self.loop_vertex):
                    uv, _ = repair_degenerate_uv(np.asarray(uv, np.float64), self.points, self.loop_vertex, totals,
                                                 self.loop_tri, self.source_tri, np.asarray(snap.points, np.float64),
                                                 np.asarray(values, np.float64)[snap.transfer_tris])
            except Exception:
                # the repair is cosmetic: never let it stop a build
                import traceback
                traceback.print_exc()
            layer = mesh.uv_layers.get(name) or mesh.uv_layers.new(name=name)
            layer.uv.foreach_set('vector',np.asarray(uv,np.float32).ravel())
            layer.active_render = name == snap.uv_render
            yield None
        if snap.uv_active and snap.uv_active in mesh.uv_layers:
            mesh.uv_layers.active_index = list(mesh.uv_layers.keys()).index(snap.uv_active)
        for name, (domain, kind, field, width, values) in snap.transfer_attrs.items():
            if name in RESERVED or mesh.attributes.get(name) is not None:
                continue
            if domain == 'POINT':
                mapped = self.sample(values, self.source_tri[self.pt_tri], self.pt_bary)
            elif domain == 'CORNER':
                mapped = self.sample(values, snap.transfer_tris[self.loop_tri], self.loop_bary)
            elif domain == 'FACE':
                mapped = [values[i] for i in self.face_source] if isinstance(values, list) else values[self.face_source]
            else:
                if self.edge_source is None:
                    src_mid = snap.points[snap.transfer_edges].mean(axis=1)
                    tree = KDTree(len(src_mid))
                    for i, mid in enumerate(src_mid): tree.insert(Vector(mid), i)
                    tree.balance()
                    edges = np.empty(len(mesh.edges)*2, np.int32)
                    mesh.edges.foreach_get('vertices', edges)
                    mid = self.points[edges.reshape(-1, 2)].mean(axis=1)
                    self.edge_source = np.array([tree.find(Vector(v))[1] for v in mid], np.int32)
                mapped = [values[i] for i in self.edge_source] if isinstance(values, list) else values[self.edge_source]
            try:
                attr = mesh.attributes.new(name=name, type=kind, domain=domain)
                if isinstance(mapped, list):
                    for item, val in zip(attr.data, mapped): setattr(item, field, val)
                else:
                    if kind == 'QUATERNION':
                        norm = np.linalg.norm(mapped, axis=1, keepdims=True)
                        mapped = mapped / np.maximum(norm, 1e-30)
                    attr.data.foreach_set(field, mapped.ravel())
            except (TypeError, ValueError, RuntimeError):
                attr = mesh.attributes.get(name)
                if attr is not None: mesh.attributes.remove(attr)
                skipped.append(name)
            yield None
        if snap.color_active and snap.color_active in mesh.color_attributes:
            mesh.color_attributes.active_color_name = snap.color_active
        if len(mesh.color_attributes):
            mesh.color_attributes.render_color_index = min(snap.color_render, len(mesh.color_attributes)-1)
                                                                           
                                                                             
        classes = out.get('tri_class')
        if classes is not None:
            base=np.asarray(classes)==BASE_CLASS
            ids=np.empty(len(base),np.int32);mesh.polygons.foreach_get('material_index',ids)
            ids[base]=snap.material_index[self.face_source[base]]
            mesh.polygons.foreach_set('material_index',ids)
        mesh.update()
        return skipped

    def groups(self, obj):
        snap = self.snap
        for group, values in snap.groups.items():
            mapped = np.clip(self.sample(values[:, None], self.source_tri[self.pt_tri], self.pt_bary)[:, 0], 0, 1)
            vg = obj.vertex_groups.get(snap.group_names[group]) or obj.vertex_groups.new(name=snap.group_names[group])
                                                                           
                                                                       
            vg.remove(list(range(len(obj.data.vertices))))
            levels = np.round(mapped*1000).astype(np.int32)
            for level in np.unique(levels[levels > 0]):
                vg.add(np.flatnonzero(levels == level).tolist(), float(level)/1000, 'REPLACE')
