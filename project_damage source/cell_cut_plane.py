# плоскость задаёт настоящий разрез уже созданных осколков
import bpy
import bmesh
import numpy as np
from mathutils import Matrix, Vector

OWNER = 'project_damage_cell_plane_source'
HINT = ('Cut Plane is the most stable cutting mode: it uses an explicit mesh instead of projecting a drawing. '
        'Place the plane across the pieces, then edit or extrude its faces. Add Solidify for a wider cut. '
        'The cutter must cross a whole piece to split it. Closed, non-self-intersecting shards are required; '
        'a partial cut leaves the piece unchanged. Live Update follows edits; otherwise press Update.')


def _poll(props, obj):
    return obj.type == 'MESH' and obj != props.id_data and not obj.get('project_damage_cellf_of') and not obj.get('project_damage_result_of')


def register_props(cls):
    from . import live
    cls.__annotations__['cellf_cut_plane'] = bpy.props.PointerProperty(
        name='Cut Plane', type=bpy.types.Object, poll=_poll, update=live.touch_prop,
        description='Editable mesh cutting existing shards. A flat or extruded sheet makes a narrow split; Solidify sets a wider removed slice')


def mesh_arrays(context, source, factor=1.0, skin=None):
    """Read the evaluated helper, without adding temporary objects to the scene."""
    obj = source.project_damage.cellf_cut_plane
    if obj is None or obj.type != 'MESH' or obj == source:
        return None
    if obj.mode == 'EDIT':
        obj.update_from_editmode()
    deps = context.evaluated_depsgraph_get()
    evaluated = obj.evaluated_get(deps)
    mesh = evaluated.to_mesh()
    bm = bmesh.new()
    try:
        bm.from_mesh(mesh)
        if not bm.faces:
            return None
        bm.transform(Matrix.Scale(factor, 4) @ source.matrix_world.inverted_safe() @ evaluated.matrix_world)
        if skin is not None:
            # открытый лист получает лишь техническую толщину, сам helper не меняется
            if any(len(edge.link_faces) > 2 for edge in bm.edges):
                raise ValueError('Cut Plane contains edges shared by more than two faces')
            if any(edge.is_boundary for edge in bm.edges):
                bmesh.ops.recalc_face_normals(bm, faces=list(bm.faces))
                bmesh.ops.solidify(bm, geom=list(bm.faces), thickness=skin)
            if any(not edge.is_manifold for edge in bm.edges if edge.link_faces):
                raise ValueError('Cut Plane cannot form a closed knife; remove doubled or intersecting faces')
            bmesh.ops.recalc_face_normals(bm, faces=list(bm.faces))
        bmesh.ops.triangulate(bm, faces=list(bm.faces))
        bm.verts.ensure_lookup_table(); bm.verts.index_update()
        vertices = np.asarray([tuple(v.co) for v in bm.verts], dtype=np.float64).reshape(-1, 3)
        triangles = np.asarray([[v.index for v in face.verts] for face in bm.faces], dtype=np.int32).reshape(-1, 3)
        if not np.isfinite(vertices).all():
            raise ValueError('Cut Plane has non-finite coordinates')
        return np.ascontiguousarray(vertices), np.ascontiguousarray(triangles)
    finally:
        bm.free()
        evaluated.to_mesh_clear()


def hash_inputs(h, source):
    h.update(b'cell-cut-plane-v1')
    data = mesh_arrays(bpy.context, source)
    if data is not None:
        for array in data:
            h.update(str(array.shape).encode()); h.update(array.tobytes())


def cut_mesh(out, vertices, triangles, uv_scale):
    """The same native splitter as the drawing tools, with an explicit solid knife."""
    from . import cell_cuts
    if not len(vertices) or not len(triangles) or not len(out.get('cellf_polys', ())):
        return out, 0, 0
    points = np.asarray(out['cellf_points'], dtype=np.float64)
    faces = np.asarray(out['cellf_polys'], dtype=np.int32)
    ids = np.asarray(out['cellf_piece'], dtype=np.int32)
    # пробуем только осколки рядом с конечной геометрией резака
    lo, hi = vertices.min(axis=0), vertices.max(axis=0)
    corners = points[faces]
    hit = np.all(corners.max(axis=1) >= lo, axis=1) & np.all(corners.min(axis=1) <= hi, axis=1)
    targets = np.unique(ids[hit]).astype(np.int32)
    if not len(targets):
        return out, 0, 0
    D = lambda name: np.ascontiguousarray(out['cellf_' + name], np.float64)
    I = lambda name: np.ascontiguousarray(out['cellf_' + name], np.int32)
    metadata = (D('piece_center').reshape(-1, 3), D('piece_volume')) if 'cellf_piece_center' in out and 'cellf_piece_volume' in out else ()
    cut = cell_cuts.load().slice_cells(D('points'), I('polys'), I('piece'), I('base_poly'),
        D('uvs'), D('normals'), [vertices], [triangles], [targets], max(uv_scale, 1e-6), *metadata)
    return dict(out, **cut), int(cut['cut_cells']), int(cut['invalid_cells'])


def apply_steps(context, source, out, factor):
    from . import background_compute as bg
    source['project_damage_cell_plane_error'] = ''
    source['project_damage_cell_plane_count'] = 0
    p = source.project_damage
    if not p.cellf_enable or p.all_off or p.cellf_cut_plane is None or not len(out.get('cellf_points', ())):
        return out
    diagonal = float(np.linalg.norm(np.ptp(np.asarray(out['cellf_points']), axis=0)))
    try:
        data = mesh_arrays(context, source, factor, skin=max(diagonal * 2e-6, 1e-7))
        if data is None:
            source['project_damage_cell_plane_error'] = 'Cut Plane has no faces. Extrude edges into faces.'
            return out
        result, count, invalid = yield from bg.task('Cut Plane: splitting shards', cut_mesh,
            {k:v for k,v in out.items() if k.startswith('cellf_')}, *data, p.cellf_inner_uv_scale)
    except (ValueError, RuntimeError) as exc:
        source['project_damage_cell_plane_error'] = str(exc)
        print('Project Damage Cut Plane:', exc)
        return out
    source['project_damage_cell_plane_count'] = count
    if invalid:
        source['project_damage_cell_plane_error'] = f'{invalid} invalid shards kept unchanged. Check closed geometry.'
    return dict(out, **result)


def _source(context):
    from . import ownership
    source = ownership.context_source(context)
    return source if source is not None and source.type == 'MESH' and source.project_damage.cellf_enable else None


class PROJECT_DAMAGE_OT_cell_cut_plane(bpy.types.Operator):
    bl_idname = 'project_damage.cell_cut_plane'
    bl_label = 'Cell Fracture Cut Plane'
    bl_description = 'Create or edit a wireframe mesh that cuts existing Cell Fracture shards'
    bl_options = {'REGISTER', 'UNDO'}
    action: bpy.props.EnumProperty(items=[(k,k.title(),'') for k in ('ADD','EDIT','SELECT','REMOVE','SOLIDIFY')], default='ADD')

    @classmethod
    def poll(cls, context):
        return _source(context) is not None

    def execute(self, context):
        from . import live, output_collections
        source = _source(context)
        if source is None:
            return {'CANCELLED'}
        p = source.project_damage
        obj = p.cellf_cut_plane
        if context.object and context.object.mode != 'OBJECT':
            bpy.ops.object.mode_set(mode='OBJECT')
        previous = live._APPLYING
        live._APPLYING = True
        try:
            p.cellf_curve_type = 'PLANE'
            if self.action == 'REMOVE':
                p.cellf_cut_plane = None
                if obj is not None and obj.get(OWNER) == source:
                    mesh = obj.data
                    bpy.data.objects.remove(obj, do_unlink=True)
                    if mesh.users == 0:
                        bpy.data.meshes.remove(mesh)
                source.hide_set(False); source.select_set(True); context.view_layer.objects.active = source
            else:
                if obj is None:
                    if self.action != 'ADD':
                        return {'CANCELLED'}
                    corners = [source.matrix_world @ Vector(v) for v in source.bound_box]
                    center = context.scene.cursor.location.copy()
                    size = max((v-center).length for v in corners) * 1.15
                    size = max(size, .01)
                    mesh = bpy.data.meshes.new(source.name + '_CutPlane')
                    mesh.from_pydata([(-size,-size,0),(size,-size,0),(size,size,0),(-size,size,0)], [], [(0,1,2,3)])
                    mesh.update()
                    obj = bpy.data.objects.new(mesh.name, mesh)
                    output_collections.link_object(context, source, obj)
                    obj.parent = source
                    obj.matrix_world = context.scene.cursor.matrix.copy()
                    obj.display_type = 'WIRE'; obj.hide_render = True
                    obj[OWNER] = source
                    p.cellf_cut_plane = obj
                if self.action == 'SOLIDIFY' and not any(m.type == 'SOLIDIFY' for m in obj.modifiers):
                    modifier = obj.modifiers.new('Cut Width', 'SOLIDIFY')
                    modifier.thickness = .005; modifier.offset = 0.; modifier.use_even_offset = True
                for selected in tuple(context.selected_objects):
                    selected.select_set(False)
                obj.hide_set(False); obj.select_set(True); context.view_layer.objects.active = obj
                if self.action == 'EDIT':
                    bpy.ops.object.mode_set(mode='EDIT')
        finally:
            live._APPLYING = previous
        live.invalidate_index()
        if self.action in {'ADD','REMOVE','SOLIDIFY'}:
            live._mark_dirty(source)
        return {'FINISHED'}


def draw(layout, context, source):
    from . import ui_image_hints
    p = source.project_damage
    row = layout.row(align=True)
    row.prop(p, 'cellf_cut_plane', text='Cutter')
    ui_image_hints.button(row, 'cell_cut_plane')
    row = layout.row(align=True)
    if p.cellf_cut_plane is None:
        row.operator('project_damage.cell_cut_plane', text='Add Cut Plane', icon='MESH_PLANE').action = 'ADD'
    else:
        row.operator('project_damage.cell_cut_plane', text='Edit Plane', icon='EDITMODE_HLT').action = 'EDIT'
        row.operator('project_damage.cell_cut_plane', text='Select', icon='RESTRICT_SELECT_OFF').action = 'SELECT'
        row.operator('project_damage.cell_cut_plane', text='', icon='TRASH').action = 'REMOVE'
        modifier = next((m for m in p.cellf_cut_plane.modifiers if m.type == 'SOLIDIFY'), None)
        if modifier:
            layout.prop(modifier, 'thickness', text='Cut Thickness')
            layout.prop(modifier, 'offset', text='Offset')
        else:
            layout.operator('project_damage.cell_cut_plane', text='Add Solidify', icon='MOD_SOLIDIFY').action = 'SOLIDIFY'
    layout.label(text='Edit / extrude faces across the shards', icon='INFO')
    error = source.get('project_damage_cell_plane_error', '')
    if error:
        import textwrap
        for line in textwrap.wrap(error, width=48):
            layout.label(text=line, icon='ERROR')
    elif p.cellf_cut_plane is not None:
        layout.label(text=f"Last update: {source.get('project_damage_cell_plane_count',0)} shard cuts", icon='CHECKMARK')


def register():
    bpy.utils.register_class(PROJECT_DAMAGE_OT_cell_cut_plane)


def unregister():
    bpy.utils.unregister_class(PROJECT_DAMAGE_OT_cell_cut_plane)
