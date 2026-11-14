# убираем мелкие свободные части и подрезаем выступы результата
# GPL-3.0-or-later. Algorithm from Lazy3D Clean Floating Parts, by Lazy3D.
"""Manual post-build cleanup. Never changes the ProjectDamage source mesh."""
import bpy
import bmesh
import numpy as np
from mathutils import Vector
from mathutils.bvhtree import BVHTree
from . import build, live

# здесь хранятся значения, которые сохраняются вместе с объектом
class ProjectDamageCleanSettings(bpy.types.PropertyGroup):
    min_part_percent: bpy.props.FloatProperty(
        name="Min Part Size", subtype='PERCENTAGE', default=2.0, min=0.0, max=100.0,
        description="Parts smaller than this share of the largest part are removed "
                    "(volume for closed parts, area for open ones)")
    keep_largest_only: bpy.props.BoolProperty(
        name="Keep Largest Only", default=False,
        description="Remove every part except the largest one, whatever its size")
    report_only: bpy.props.BoolProperty(
        name="Report Only", default=False,
        description="Count what would be removed without changing the mesh")
    reference: bpy.props.PointerProperty(
        name="Source Shape", type=bpy.types.Object, poll=lambda self, o: o.type == 'MESH',
        description="Closed reference mesh; empty uses each result's ProjectDamage source")
    allowance: bpy.props.FloatProperty(
        name="Allowance", subtype='DISTANCE', default=0.0, min=0.0, soft_max=0.1,
        description="Allowed protrusion distance in result-local mesh units")
    use_reference_modifiers: bpy.props.BoolProperty(
        name="Source Modifiers", default=False,
        description="Trim against evaluated source; match ProjectDamage Use Modifiers when enabled")
    flip_open_side: bpy.props.BoolProperty(
        name="Reverse Open Surface Side", default=False,
        description="Open references: reverse which side of their normals is trimmed")


class CleanAlgorithm:
                                                                               

    @staticmethod
    def mesh_arrays(mesh):
        n_v, n_p, n_l = len(mesh.vertices), len(mesh.polygons), len(mesh.loops)
        co = np.empty(n_v * 3)
        mesh.vertices.foreach_get("co", co)
        loop_start = np.empty(n_p, dtype=np.int64)
        loop_total = np.empty(n_p, dtype=np.int64)
        mesh.polygons.foreach_get("loop_start", loop_start)
        mesh.polygons.foreach_get("loop_total", loop_total)
        loop_verts = np.empty(n_l, dtype=np.int64)
        mesh.loops.foreach_get("vertex_index", loop_verts)
        return co.reshape(-1, 3), loop_start, loop_total, loop_verts

    @staticmethod
    def loop_edges(loop_start, loop_total, loop_verts):
        """Per loop: (face index, undirected edge key)."""
        loop_start = np.asarray(loop_start, dtype=np.int64)
        loop_total = np.asarray(loop_total, dtype=np.int64)
        face = np.repeat(np.arange(len(loop_start)), loop_total)
        first = loop_start[face]
                                                                            
        pos0 = np.concatenate([[0], np.cumsum(loop_total)[:-1]]).astype(np.int64)
        local = np.arange(len(face)) - pos0[face]
        idx = first + local
        nxt = first + (local + 1) % loop_total[face]
        a, b = np.asarray(loop_verts)[idx], np.asarray(loop_verts)[nxt]
        lo, hi = np.minimum(a, b), np.maximum(a, b)
        n = int(max(hi.max(initial=0), 0)) + 1
        return face, lo * n + hi

    @classmethod
    def analyse(cls, points, loop_start, loop_total, loop_verts):
        """Face labels of parts connected through MANIFOLD edges (shared by
        exactly two faces) and per-part volume, area and closedness."""
        face, key = cls.loop_edges(loop_start, loop_total, loop_verts)
        n_f = len(loop_start)
        order = np.argsort(key, kind="stable")
        k_s, f_s = key[order], face[order]
        uniq, start, count = np.unique(k_s, return_index=True, return_counts=True)
        pairs = start[count == 2]
        a, b = f_s[pairs], f_s[pairs + 1]
        parent = np.arange(n_f)

        def find(x):
            root = x
            while parent[root] != root:
                root = parent[root]
            while parent[x] != root:
                parent[x], x = root, parent[x]
            return root
        for x, y in zip(a.tolist(), b.tolist()):
            rx, ry = find(x), find(y)
            if rx != ry:
                parent[rx] = ry
        roots = np.array([find(i) for i in range(n_f)])
        _, labels = np.unique(roots, return_inverse=True)
        n_c = int(labels.max()) + 1
                                       
        loop_start = np.asarray(loop_start); loop_total = np.asarray(loop_total); loop_verts = np.asarray(loop_verts)
        tri_face = np.repeat(np.arange(n_f), np.maximum(loop_total - 2, 0))
        n_tri = np.maximum(loop_total - 2, 0)
        tri_pos0 = np.concatenate([[0], np.cumsum(n_tri)[:-1]]).astype(np.int64)
        offs = np.arange(len(tri_face)) - tri_pos0[tri_face] + 1
        v0 = loop_verts[loop_start[tri_face]]
        v1 = loop_verts[loop_start[tri_face] + offs]
        v2 = loop_verts[loop_start[tri_face] + offs + 1]
        p0, p1, p2 = points[v0], points[v1], points[v2]
        cr = np.cross(p1 - p0, p2 - p0)
        vol = np.einsum("ij,ij->i", p0, np.cross(p1, p2)) / 6.0
        area = 0.5 * np.linalg.norm(cr, axis=1)
        part_vol = np.bincount(labels[tri_face], weights=vol, minlength=n_c)
        part_area = np.bincount(labels[tri_face], weights=area, minlength=n_c)
                                                                          
        pk = labels[face].astype(np.int64) * (int(key.max(initial=0)) + 1) + key
        _, pinv, pcount = np.unique(pk, return_inverse=True, return_counts=True)
        open_edge = pcount[pinv] != 2
        part_open = np.bincount(labels[face], weights=open_edge.astype(float), minlength=n_c) > 0
        return labels, {"volume": part_vol, "area": part_area, "closed": ~part_open}

    @staticmethod
    def choose(stats, min_part_percent, keep_largest_only):
        """True per part = remove. The main body is the part with the largest
        |volume| among closed parts (largest area if none is closed)."""
        vol, area, closed = stats["volume"], stats["area"], stats["closed"]
        n = len(vol)
        if n <= 1:
            return np.zeros(n, dtype=bool)
        if closed.any():
            size = np.where(closed, np.abs(vol), -1.0)
        else:
            size = area.copy()
        main = int(np.argmax(size))
        remove = np.ones(n, dtype=bool) if keep_largest_only else np.zeros(n, dtype=bool)
        if not keep_largest_only:
            frac = min_part_percent / 100.0
            ref_vol = abs(vol[main]) if closed[main] else 0.0
            ref_area = area[main]
            small_closed = closed & (np.abs(vol) < frac * ref_vol)
            small_open = ~closed & (area < frac * ref_area)
            remove = small_closed | small_open
        remove[main] = False
        return remove

    @classmethod
    def open_edges(cls, loop_start, loop_total, loop_verts, keep_faces):
        face, key = cls.loop_edges(loop_start, loop_total, loop_verts)
        k = key[np.asarray(keep_faces)[face]]
        _, count = np.unique(k, return_counts=True)
        return int(np.sum(count == 1))

    @staticmethod
    def delete_faces(mesh, face_indices):
        """Delete whole faces with bmesh: UVs, materials, attributes and
        vertex groups of everything kept stay as they are."""
        bm = bmesh.new()
        try:
            bm.from_mesh(mesh)
            bm.faces.ensure_lookup_table()
            bmesh.ops.delete(bm, geom=[bm.faces[i] for i in face_indices], context='FACES')
            bm.to_mesh(mesh)
        finally:
            bm.free()
        mesh.update()


def target_of(obj):
    if obj is None or obj.type != 'MESH':
        return None
    if obj.get(build.RESULT_STAMP):
        return obj
    return build.result_of(obj)

def targets(context):
    from . import ownership
    pinned=ownership.pinned_source(context)
    if pinned is not None:
        target=target_of(build.result_of(pinned) or pinned)
        return [target] if target is not None else []
    found = {}
    for obj in context.selected_objects:
        target = target_of(obj)
        if target is not None:
            found[target.as_pointer()] = target
    target = target_of(context.object)
    if target is not None:
        found[target.as_pointer()] = target
    return list(found.values())

# действие «Clean Floating Parts»
class PROJECT_DAMAGE_OT_clean_floating_parts(bpy.types.Operator):
    """Remove detached parts of ProjectDamage results; the original source stays unchanged"""
    bl_idname = "project_damage.clean_floating_parts"
    bl_label = "Clean Floating Parts"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return context.mode == 'OBJECT' and not live._APPLYING and bool(targets(context))

    def execute(self, context):
        from . import ownership,python_jobs
        source=ownership.context_source(context)
        if source is not None and python_jobs.queue(context,source,'CLEAN'):
            self.report({'INFO'},'Calculation queued in background')
            return {'FINISHED'}
        settings = context.scene.project_damage_clean_floating
        removed = parts = 0
        live._APPLYING = True
        try:
            for obj in targets(context):
                mesh = obj.data
                if not len(mesh.polygons):
                    continue
                points, ls, lt, lv = CleanAlgorithm.mesh_arrays(mesh)
                labels, stats = CleanAlgorithm.analyse(points, ls, lt, lv)
                remove = CleanAlgorithm.choose(stats, settings.min_part_percent,
                                               settings.keep_largest_only)
                faces = np.flatnonzero(remove[labels])
                parts += len(remove)
                removed += int(remove.sum())
                if settings.report_only or not len(faces):
                    continue
                if mesh.shape_keys:
                    self.report({'WARNING'}, obj.name + ": shape keys present; cleanup skipped")
                    removed -= int(remove.sum())
                    continue
                before = CleanAlgorithm.open_edges(ls, lt, lv, ~remove[labels])
                                                                                       
                candidate = mesh.copy()
                obj.data = candidate
                committed = False
                try:
                    CleanAlgorithm.delete_faces(candidate, faces)
                    _, ls, lt, lv = CleanAlgorithm.mesh_arrays(candidate)
                    after = CleanAlgorithm.open_edges(ls, lt, lv, np.ones(len(lt), dtype=bool))
                    if after != before:
                        self.report({'ERROR'}, obj.name + ": boundary changed; cleanup discarded")
                        removed -= int(remove.sum())
                        continue
                    obj.data = candidate
                    committed = True
                finally:
                    if not committed:
                        obj.data = mesh
                    if candidate.users == 0:
                        bpy.data.meshes.remove(candidate)
                if mesh.users == 0:
                    bpy.data.meshes.remove(mesh)
            verb = "would remove" if settings.report_only else "removed"
            self.report({'INFO'}, "Clean Floating Parts: %s %d of %d parts" % (verb, removed, parts))
        finally:
            live._APPLYING = False
        return {'FINISHED'}

class TrimAlgorithm:
    @staticmethod
    def clamp_open_surface(mesh, reference, allowance, flip=False):
        """One-sided nearest-surface projection, with unchanged mesh topology.

        q,n = nearest reference triangle point and unit face normal.
        d = dot(p-q,n); p' = p - max(0,d-allowance)*n.
        Negative-side damage cavities are retained. Boundary triangles extend
        their tangent half-space; this is not a volume intersection.
        """
        reference.calc_loop_triangles()
        points, _, _, _ = CleanAlgorithm.mesh_arrays(reference)
        triangles = np.empty(len(reference.loop_triangles) * 3, np.int32)
        reference.loop_triangles.foreach_get("vertices", triangles)
        triangles = triangles.reshape(-1, 3)
        if not len(triangles):
            raise ValueError("source has no usable surface triangles")
        cross = np.cross(points[triangles[:, 1]] - points[triangles[:, 0]],
                         points[triangles[:, 2]] - points[triangles[:, 0]])
        scale = float(np.linalg.norm(np.ptp(points, axis=0)))
        valid = np.linalg.norm(cross, axis=1) > max(1e-30, scale * scale * 1e-12)
        if not valid.any():
            raise ValueError("source contains only degenerate triangles")
        tree = BVHTree.FromPolygons([Vector(p) for p in points],
                                   triangles[valid].tolist(), all_triangles=True)
        coords = CleanAlgorithm.mesh_arrays(mesh)[0]
        if not np.isfinite(coords).all() or not np.isfinite(points).all():
            raise ValueError("non-finite mesh coordinates")
        moved = 0
        maximum = 0.0
        eps = max(1e-9, float(np.linalg.norm(np.ptp(points, axis=0))) * 1e-8)
        for i, point in enumerate(coords):
            nearest, normal, triangle, _ = tree.find_nearest(Vector(point))
            if triangle is None:
                continue
            if flip:
                normal = -normal
            distance = float((Vector(point) - nearest).dot(normal))
            excess = distance - allowance
            if excess > eps:
                coords[i] -= np.asarray(normal) * excess
                moved += 1
                maximum = max(maximum, excess)
        if moved:
            mesh.vertices.foreach_set("co", coords.ravel())
            mesh.update()
        return moved, maximum

    @staticmethod
    def project_damage_source(obj):
        """The ProjectDamage source of a result object (its stamp matches the source's
        project_damage_uid), or None."""
        uid = obj.get("project_damage_result_of")
        if uid is None:
            return None
        return next((o for o in bpy.data.objects if o.type == 'MESH' and o.get("project_damage_uid") == uid), None)

    @staticmethod
    def offset_points(points, loop_start, loop_total, loop_verts, distance):
        """Move each vertex out along its angle-weighted normal, mitred so flat
        faces move by exactly `distance` (as a Solidify with even thickness)."""
        points = np.asarray(points, dtype=np.float64)
        vn = np.zeros_like(points)
        face_n = []
        for st, tot in zip(np.asarray(loop_start).tolist(), np.asarray(loop_total).tolist()):
            ids = np.asarray(loop_verts)[st:st + tot]
            p = points[ids]
            n = np.zeros(3)
            for i in range(tot):                                      
                a, b = p[i], p[(i + 1) % tot]
                n += ((a[1] - b[1]) * (a[2] + b[2]), (a[2] - b[2]) * (a[0] + b[0]), (a[0] - b[0]) * (a[1] + b[1]))
            ln = np.linalg.norm(n)
            n = n / ln if ln > 1e-18 else n
            face_n.append((ids, n))
            for i in range(tot):
                e1 = p[(i + 1) % tot] - p[i]
                e2 = p[(i - 1) % tot] - p[i]
                l1, l2 = np.linalg.norm(e1), np.linalg.norm(e2)
                if l1 > 1e-18 and l2 > 1e-18:
                    vn[ids[i]] += n * np.arccos(np.clip(np.dot(e1, e2) / (l1 * l2), -1.0, 1.0))
        ln = np.linalg.norm(vn, axis=1)
        ok = ln > 1e-18
        vn[ok] /= ln[ok, None]
        min_cos = np.ones(len(points))
        for ids, n in face_n:
            np.minimum.at(min_cos, ids, vn[ids] @ n)
        return points + vn * (distance / np.maximum(0.35, min_cos))[:, None]

    @staticmethod
    def volume(mesh):
        bm = bmesh.new()
        try:
            bm.from_mesh(mesh)
            return bm.calc_volume(signed=False)
        finally:
            bm.free()

    @staticmethod
    def intersect(context, obj, ref_mesh):
        """Return obj.data INTERSECT ref_mesh with the Exact solver. Other
        modifiers are muted during the evaluation, so only the Boolean is
        baked; UVs, materials and attributes ride along."""
        cutter = bpy.data.objects.new("_trim_source", ref_mesh)
        context.scene.collection.objects.link(cutter)
        cutter.matrix_world = obj.matrix_world.copy()
        cutter.hide_render = True
        muted = [m for m in obj.modifiers if m.show_viewport]
        for m in muted:
            m.show_viewport = False
        mod = obj.modifiers.new("_trim_to_source", 'BOOLEAN')
        try:
            mod.operation = 'INTERSECT'
            mod.solver = 'EXACT'
            mod.object = cutter
            mod.use_self = True
            mod.use_hole_tolerant = True
            depsgraph = context.evaluated_depsgraph_get()
            depsgraph.update()
            new_mesh = bpy.data.meshes.new_from_object(obj.evaluated_get(depsgraph),
                                                       preserve_all_data_layers=True, depsgraph=depsgraph)
        finally:
            obj.modifiers.remove(mod)
            for m in muted:
                m.show_viewport = True
            bpy.data.objects.remove(cutter, do_unlink=True)
        return new_mesh


# действие «Trim to Source»
class PROJECT_DAMAGE_OT_trim_to_source(bpy.types.Operator):
    """Trim result: Exact intersection for solids, normal projection for open references"""
    bl_idname = "project_damage.trim_to_source"
    bl_label = "Trim to Source"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return PROJECT_DAMAGE_OT_clean_floating_parts.poll(context) and not context.scene.project_damage_clean_floating.report_only

    def execute(self, context):
        from . import ownership,python_jobs
        source=ownership.context_source(context)
        if source is not None and python_jobs.queue(context,source,'TRIM'):
            self.report({'INFO'},'Calculation queued in background')
            return {'FINISHED'}
        s = context.scene.project_damage_clean_floating
        done = 0
        live._APPLYING = True
        try:
            for obj in targets(context):
                ref = s.reference or TrimAlgorithm.project_damage_source(obj)
                if ref is None or ref == obj:
                    self.report({'WARNING'}, obj.name + ": choose a different Source Shape")
                    continue
                if obj.data.shape_keys:
                    self.report({'WARNING'}, obj.name + ": shape keys present; trim skipped")
                    continue
                ref_mesh = None
                candidate = None
                try:
                    depsgraph = context.evaluated_depsgraph_get()
                    depsgraph.update()
                    if s.use_reference_modifiers:
                        ref_mesh = bpy.data.meshes.new_from_object(
                            ref.evaluated_get(depsgraph), preserve_all_data_layers=True,
                            depsgraph=depsgraph)
                    else:
                        ref_mesh = ref.data.copy()
                    if not len(ref_mesh.polygons):
                        self.report({'WARNING'}, ref.name + ": empty source; trim skipped")
                        continue
                    points, ls, lt, lv = CleanAlgorithm.mesh_arrays(ref_mesh)
                    _, stats = CleanAlgorithm.analyse(points, ls, lt, lv)
                    open_reference = not stats["closed"].all()
                    if open_reference and not any(p.area > 0 for p in ref_mesh.polygons):
                        raise ValueError("source contains only degenerate faces")
                                                                                         
                    ref_mesh.transform(obj.matrix_world.inverted() @ ref.matrix_world)
                    if open_reference:
                        old = obj.data
                        candidate = old.copy()
                        moved, maximum = TrimAlgorithm.clamp_open_surface(
                            candidate, ref_mesh, s.allowance, s.flip_open_side)
                        if moved:
                            obj.data = candidate
                            candidate = None
                            if old.users == 0:
                                bpy.data.meshes.remove(old)
                        done += 1
                        self.report({'INFO'}, "%s: open-surface trim, %d vertices clamped (max %.4g)"
                                    % (obj.name, moved, maximum))
                        continue
                    if s.allowance > 0:
                        points = CleanAlgorithm.mesh_arrays(ref_mesh)[0]
                        grown = TrimAlgorithm.offset_points(points, ls, lt, lv, s.allowance)
                        ref_mesh.vertices.foreach_set("co", grown.ravel())
                        ref_mesh.update()
                    old = obj.data
                                                                               
                                                                               
                    from . import input_prep, result_transfer
                    from .mesh_io import BASE_CLASS
                    snap = input_prep._snapshot(old, obj, False)
                    for name in snap.transfer_attrs:
                        attr = ref_mesh.attributes.get(name)
                        if attr is not None:
                            ref_mesh.attributes.remove(attr)
                    ref_mesh.materials.clear()
                    for material in snap.materials:
                        ref_mesh.materials.append(material)
                    cap_data = {'tri_class': np.full(len(ref_mesh.polygons), BASE_CLASS)}
                    projection = result_transfer.Transfer(snap, ref_mesh, cap_data, 1.0)
                    projection.uv_and_attributes(cap_data, 1.0)
                    before = TrimAlgorithm.volume(old)
                    candidate = TrimAlgorithm.intersect(context, obj, ref_mesh)
                                                                                          
                    if len(old.polygons):
                        _, original_stats = CleanAlgorithm.analyse(*CleanAlgorithm.mesh_arrays(old))
                        if original_stats["closed"].all() and len(candidate.polygons):
                            _, new_stats = CleanAlgorithm.analyse(*CleanAlgorithm.mesh_arrays(candidate))
                            if not new_stats["closed"].all():
                                self.report({'ERROR'}, obj.name + ": trim is not closed; result discarded")
                                continue
                                                                          
                                                                           
                    for name in snap.transfer_attrs:
                        attr = candidate.attributes.get(name)
                        if attr is not None:
                            candidate.attributes.remove(attr)
                    weights = result_transfer.Transfer(snap, candidate, {}, 1.0)
                    saved_uv = snap.uv
                    snap.uv = {}
                    try:
                        weights.uv_and_attributes({}, 1.0)
                    finally:
                        snap.uv = saved_uv
                    after = TrimAlgorithm.volume(candidate)
                    candidate.name = old.name
                    obj.data = candidate
                    candidate = None
                                                                             
                                                                            
                    if snap.groups and len(obj.data.vertices):
                        for index, values in snap.groups.items():
                            vg = obj.vertex_groups.get(snap.group_names[index])
                            if vg is None:
                                vg = obj.vertex_groups.new(name=snap.group_names[index])
                            mapped = weights.sample(values[:, None],
                                weights.source_tri[weights.pt_tri], weights.pt_bary)[:, 0]
                            for vertex in obj.data.vertices:
                                if not any(g.group == vg.index for g in vertex.groups):
                                    value = float(np.clip(mapped[vertex.index], 0, 1))
                                    if value > 0:
                                        vg.add([vertex.index], value, 'REPLACE')
                    if old.users == 0:
                        bpy.data.meshes.remove(old)
                    done += 1
                    self.report({'INFO'}, "%s: trimmed %.4g of %.4g volume outside '%s'"
                                % (obj.name, before - after, before, ref.name))
                except (RuntimeError, ValueError) as exc:
                    self.report({'WARNING'}, obj.name + ": trim skipped: " + str(exc))
                finally:
                    if candidate is not None and candidate.users == 0:
                        bpy.data.meshes.remove(candidate)
                    if ref_mesh is not None and ref_mesh.users == 0:
                        bpy.data.meshes.remove(ref_mesh)
        finally:
            live._APPLYING = False
        return {'FINISHED'} if done else {'CANCELLED'}

def draw_controls(layout, context):
    box = layout.box()
    box.label(text="Clean Floating Parts")
    s = context.scene.project_damage_clean_floating
    box.label(text="Protrusions above the source surface")
    box.prop(s, "reference")
    box.prop(s, "allowance")
    box.prop(s, "use_reference_modifiers")
    box.prop(s, "flip_open_side")
    box.operator("project_damage.trim_to_source", icon='MOD_BOOLEAN')
    box.separator()
    box.label(text="Detached parts")
    box.prop(s, "min_part_percent")
    box.prop(s, "keep_largest_only")
    box.prop(s, "report_only")
    box.operator("project_damage.clean_floating_parts", icon='BRUSH_DATA')
    box.label(text="Result only. Run again after Update.", icon='INFO')
    box.label(text="Trim first, then Clean. Open sources supported.")

class OBJECT_PT_project_damage_clean_result(bpy.types.Panel):
    bl_idname = "OBJECT_PT_project_damage_clean_result"
    bl_label = "ProjectDamage — Result Cleanup"
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "object"

    @classmethod
    def poll(cls, context):
        return context.object is not None and bool(context.object.get(build.RESULT_STAMP))

    def draw(self, context):
        draw_controls(self.layout, context)

_CLASSES = (ProjectDamageCleanSettings, PROJECT_DAMAGE_OT_clean_floating_parts, PROJECT_DAMAGE_OT_trim_to_source,
            OBJECT_PT_project_damage_clean_result)

# подключаем свойства и действия этого модуля к Blender
def register():
    for cls in _CLASSES[:-1]:
        bpy.utils.register_class(cls)
    bpy.types.Scene.project_damage_clean_floating = bpy.props.PointerProperty(type=ProjectDamageCleanSettings)

# убираем подключения модуля при выключении аддона
def unregister():
    del bpy.types.Scene.project_damage_clean_floating
    for cls in reversed(_CLASSES[:-1]):
        bpy.utils.unregister_class(cls)

