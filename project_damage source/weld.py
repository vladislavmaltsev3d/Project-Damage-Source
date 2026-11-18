# строим сварной шов по рёбрам или нарисованным линиям
from . import quality_ui
from . import preview_quality
# ProjectDamage welded edges - GPL-3.0-or-later; see LICENSE.
#
# Weld Edges: weld beads (core: weld.cpp) as their own object "<object>_Welds".
#   Edges     corners sharper than Angle - inside (fillet welds), outside or
#             both - limited to the Mask vertex group (weight >= Threshold on
#             both ends); no group = every such edge. On Edges uses the exact
#             edge-domain mask on the source mesh.
#   Drawn     Grease Pencil strokes dropped on the surface: a bead anywhere
#   Bead      Width / Height, ripples (Pitch, Ripple Height, Curvature),
#             Wobble (also spatter); UV map "UVMap" (U along in Width units,
#             V across) and the point attribute "weld_profile" (0 toes ..
#             1 crown) for heat tint / masks in the material
#   PD_Weld   default material: heat tint from weld_profile (weld-v2)
from .blender_compat import enable_nodes, uses_nodes
import bpy
from . import material_controls
import numpy as np
from . import core_loader

EDGE_MASK = 'project_damage_weld_edges'


def _poll_gp(self, obj):
    return obj.type in {'GREASEPENCIL', 'GPENCIL'}


# здесь хранятся значения, которые сохраняются вместе с объектом
class ProjectDamageWeldSettings(bpy.types.PropertyGroup):
    mask_group: bpy.props.StringProperty(name='Mask', description='Vertex group: only edges whose both ends weigh at least the Threshold get a weld. Empty: every matching edge')
    mask_threshold: bpy.props.FloatProperty(name='Threshold', default=0.5, min=0.0, max=1.0)
    on_edges: bpy.props.BoolProperty(
        name='On Edges',
        description='Also weld the exact edges added with Assign to Weld Mask, including flat and open edges')
    edges_enable: bpy.props.BoolProperty(name='Edge Welds', default=False)
    edge_type: bpy.props.EnumProperty(name='Edges', items=(('0', 'Inside Corners', 'Fillet welds in concave corners'), ('1', 'Outside Corners', 'Beads on convex edges'), ('2', 'Both', 'Every corner')), default='1')
    angle: bpy.props.FloatProperty(name='Angle', default=30.0, min=1.0, max=179.0, description='Corners sharper than this (degrees) get a weld')
    strokes_enable: bpy.props.BoolProperty(name='Drawn Welds', default=True)
    width: bpy.props.FloatProperty(name='Width', default=0.012, min=0.0001, soft_max=0.2, subtype='DISTANCE')
    height: bpy.props.FloatProperty(name='Height', default=0.004, min=0.0001, soft_max=0.1, subtype='DISTANCE')
    pitch: bpy.props.FloatProperty(name='Ripple Pitch', default=0.003, min=0.0001, soft_max=0.05, subtype='DISTANCE')
    ripple_height: bpy.props.FloatProperty(name='Ripple Height', default=0.25, min=0.0, max=1.0)
    curvature: bpy.props.FloatProperty(name='Ripple Curvature', default=0.6, min=0.0, max=3.0)
    wobble: bpy.props.FloatProperty(name='Wobble', default=0.5, min=0.0, max=2.0)
    sides: bpy.props.IntProperty(name='Section', default=16, min=6, max=48)
    detail: bpy.props.FloatProperty(name='Detail', default=1.0, min=0.01, max=4.0)
    uv_scale: bpy.props.FloatProperty(name='UV Scale', default=1.0, min=0.01, max=100.0)
    seed: bpy.props.IntProperty(name='Seed', default=0, min=0)
    material: bpy.props.PointerProperty(name='Material', type=bpy.types.Material)


def generate(context,source,target,s,reports):
    from . import background_compute as bg,generated_assets
    if bg.async_for(source):
        def work():
            messages=[]
            result=yield from _generate_steps(bpy.context,source,target,s,messages)
            for flags,message in messages:
                if 'ERROR' in flags:raise RuntimeError(message)
            if result is None:
                old=generated_assets.output(source,'weld')
                if old is not None:old.hide_set(True);old.hide_render=True
            return result
        bg.queue(source,'generate_weld',work,guard=lambda:bg.valid_id(target))
        return generated_assets.output(source,'weld')
    return bg.immediate(_generate_steps(context,source,target,s,reports))

def _mesh_arrays(me):
    points = np.empty((len(me.vertices), 3), dtype=np.float64)
    me.vertices.foreach_get('co', points.ravel())
    ls = np.empty(len(me.polygons), dtype=np.int64); me.polygons.foreach_get('loop_start', ls)
    lt = np.empty(len(me.polygons), dtype=np.int64); me.polygons.foreach_get('loop_total', lt)
    lv = np.empty(len(me.loops), dtype=np.int64); me.loops.foreach_get('vertex_index', lv)
    polys = []
    for a, t in zip(ls.tolist(), lt.tolist()):
        ring = lv[a:a + t].tolist()
        if t == 3:
            polys.append(ring + [ring[2]])
        elif t == 4:
            polys.append(ring)
        else:                                                              
            for k in range(1, t - 1):
                polys.append([ring[0], ring[k], ring[k + 1], ring[k + 1]])
    return points, np.asarray(polys, dtype=np.int32).reshape(-1, 4)


def _weights(source, target, mesh, points, group_name):
    group = target.vertex_groups.get(group_name)
    if group is not None:
        weights = np.zeros(len(points), dtype=np.float32)
        for vertex in mesh.vertices:
            weights[vertex.index] = next((g.weight for g in vertex.groups if g.group == group.index), 0.0)
        return weights
    original = source.vertex_groups.get(group_name) if target != source else None
    if original is None or not len(source.data.vertices):
        return np.empty(0, dtype=np.float32)
    from mathutils.kdtree import KDTree
    vertices = source.data.vertices
    tree = KDTree(len(vertices))
    transform = target.matrix_world.inverted_safe() @ source.matrix_world
    source_weights = []
    for vertex in vertices:
        tree.insert(transform @ vertex.co, vertex.index)
        source_weights.append(next((g.weight for g in vertex.groups if g.group == original.index), 0.0))
    tree.balance()
    return np.asarray([source_weights[tree.find(point)[1]] for point in points], dtype=np.float32)


def _masked_edges(mesh):
    mask = mesh.attributes.get(EDGE_MASK)
    if mask is None or mask.domain != 'EDGE' or mask.data_type != 'INT':
        return np.empty((0, 2), dtype=np.int32)
    selected = np.empty(len(mesh.edges), dtype=np.int32)
    mask.data.foreach_get('value', selected)
    edges = np.empty((len(mesh.edges), 2), dtype=np.int32)
    mesh.edges.foreach_get('vertices', edges.ravel())
    return edges[selected != 0]


def _matching_edges(mesh, points, edge_points, edge_selection, to_local):
    """Find edges shared by the original mesh and the evaluated damage mesh."""
    from mathutils.kdtree import KDTree

    if not len(points):
        return np.empty((0, 2), dtype=np.int32)
    tree = KDTree(len(points))
    for i, point in enumerate(points):
        tree.insert(point, i)
    tree.balance()
    originals = np.unique(edge_selection)
    mapped = edge_points[originals] @ to_local[:3, :3].T + to_local[:3, 3]
    tolerance = max(1e-6, float(np.max(np.abs(mapped))) * 2e-7)
    neighbors = {}
    for a, b in edge_selection:
        neighbors.setdefault(a, set()).add(b)
        neighbors.setdefault(b, set()).add(a)
    matches = {}
    for i, point in zip(originals, mapped):
        for _, vertex_index, _ in tree.find_range(point, tolerance):
            matches.setdefault(vertex_index, set()).add(i)
    edges = np.empty((len(mesh.edges), 2), dtype=np.int32)
    mesh.edges.foreach_get('vertices', edges.ravel())
    overlap = [(a, b) for a, b in edges if a in matches and b in matches
               and any(neighbors[i] & matches[b] for i in matches[a])]
    return np.asarray(overlap, dtype=np.int32).reshape(-1, 2)


def _generate_steps(context, source, target, s, reports):
    from . import background_compute as bg, paint
    core = core_loader.load()
    if core is None or not hasattr(core, 'build_welds'):
        reports.append(({'ERROR'}, 'Weld Edges: the native core has no weld generator (rebuild the core)'))
        return None
    if not s.is_property_set('edges_enable'):
        s['edges_enable'] = s.edges_enable
    on_edges = s.on_edges
    mask_group = s.mask_group
    has_group = bool(mask_group and (mask_group in target.vertex_groups or mask_group in source.vertex_groups))
    automatic_edges = s.edges_enable and (not on_edges or has_group)
    edge_selection = _masked_edges(source.data) if on_edges else None
    has_selected_edges = edge_selection is not None and len(edge_selection) > 0
    if has_selected_edges:
        edge_points, edge_polys = _mesh_arrays(source.data)
        source_world = np.asarray(source.matrix_world, dtype=np.float64)
        edge_points = np.ascontiguousarray(edge_points @ source_world[:3, :3].T + source_world[:3, 3])

    to_local = np.asarray(target.matrix_world.inverted_safe(), dtype=np.float64)
    world = np.asarray(target.matrix_world, dtype=np.float64)
    strokes = paint.read_paths(context, target, source.project_damage.weld_drawing) if s.strokes_enable else []
    strokes = [np.ascontiguousarray(path @ world[:3,:3].T + world[:3,3]) for path in strokes]
    scale = 1.0 / max(source.project_damage.global_scale / 100.0, 0.0001)
    p = core.WeldParams()
    p.seed = (s.seed + source.project_damage.seed) % 2147483647
    p.edges_enable = s.edges_enable
    p.edge_type = int(s.edge_type)
    p.angle = s.angle
    p.mask_threshold = s.mask_threshold
    p.strokes_enable = s.strokes_enable
    p.stroke_reach = s.width * 4.0 / scale
    p.width = s.width / scale
    p.height = s.height / scale
    p.pitch = s.pitch / scale
    p.ripple_height = s.ripple_height
    p.curvature = s.curvature
    p.wobble = s.wobble
    p.sides = s.sides
    p.detail = max(0.01, s.detail * (preview_quality.percent(source.project_damage) / 100.0))
    p.uv_scale = s.uv_scale
    results = []
    guide = surface_guidance.kwargs(core)
    no_weights = np.empty(0, dtype=np.float32)
    if automatic_edges or strokes:
        options = dict(guide)
        ev = target.evaluated_get(context.evaluated_depsgraph_get())
        me = ev.to_mesh()
        try:
            points, polys = _mesh_arrays(me)
            weights = _weights(source, target, me, points, mask_group) if automatic_edges and has_group else no_weights
            if automatic_edges and has_selected_edges:
                options['edge_exclusion'] = _matching_edges(me, points, edge_points, edge_selection, to_local)
        finally:
            ev.to_mesh_clear()
        points = np.ascontiguousarray(points @ world[:3, :3].T + world[:3, 3])
        p.edges_enable = automatic_edges
        results.append((yield from bg.native(core, 'build_welds', points, polys, weights, strokes, p, **options)))
    if has_selected_edges:
        p.edges_enable = True
        p.strokes_enable = False
        results.append((yield from bg.native(core, 'build_welds', edge_points, edge_polys, no_weights, [], p,
                                            edge_selection=edge_selection, **guide)))
    results = [r for r in results if len(r['polys'])]
    if not results:
        message = ('Weld Edges: select edges in Edit Mask, then use Assign to Weld Mask' if on_edges
                   else 'Weld Edges: no weld (check the Mask group, the Angle and the Edges type, or draw strokes)')
        reports.append(({'WARNING'}, message))
        return None
    r = results[0]
    wp, wf, wuv, prof = r['points'], r['polys'], r['uvs'], r['profile']
    beads = r['beads']
    if len(results) == 2:
        edges = results[1]
        wf = np.concatenate((wf, edges['polys'] + len(wp)))
        wp = np.concatenate((wp, edges['points']))
        wuv = np.concatenate((wuv, edges['uvs']))
        prof = np.concatenate((prof, edges['profile']))
        beads += edges['beads']
    wp = wp @ to_local[:3, :3].T + to_local[:3, 3]
    name = source.name + '_project_damage_weld'
    mesh = bpy.data.meshes.new(name)
    mesh.vertices.add(len(wp))
    mesh.vertices.foreach_set('co', wp.astype(np.float32).ravel())
    nf = len(wf)
    mesh.loops.add(nf * 4)
    mesh.polygons.add(nf)
    mesh.loops.foreach_set('vertex_index', wf.astype(np.int32).ravel())
    mesh.polygons.foreach_set('loop_start', np.arange(0, nf * 4, 4, dtype=np.int32))
    try:
        mesh.polygons.foreach_set('loop_total', np.full(nf, 4, dtype=np.int32))
    except (AttributeError, RuntimeError):
        pass                                                             
    mesh.update(calc_edges=True)
    uv = mesh.uv_layers.new(name='UVMap')
    uv.data.foreach_set('uv', wuv[wf.ravel()].astype(np.float32).ravel())
    attr = mesh.attributes.new('weld_profile', 'FLOAT', 'POINT')
    attr.data.foreach_set('value', prof.astype(np.float32))
    mesh.shade_smooth()
    mat = s.material
    if mat is None:
        mat = _default_material()
    mesh.materials.append(mat)
    from . import generated_assets
    obj = generated_assets.place(context, source, target, 'weld', mesh)
    reports.append(({'INFO'}, 'Weld Edges: %d welds' % beads))
    return obj


def _default_material():
                                                                     
                                                                          
                                                                            
                                                                       
    mat = bpy.data.materials.get('PD_Weld')
    if mat is not None and (mat.get('project_damage_weld_shader') or not _placeholder(mat)):
        return mat
    if mat is None:
        mat = bpy.data.materials.new('PD_Weld')
    mat['project_damage_weld_shader'] = 1
    mat.diffuse_color = (0.42, 0.40, 0.37, 1.0)
    mat.metallic = 1.0
    mat.roughness = 0.35
    enable_nodes(mat)
    nt = mat.node_tree
    nt.nodes.clear()
    N, L = nt.nodes.new, nt.links.new
    out = N('ShaderNodeOutputMaterial'); out.location = (900, 0)
    bsdf = N('ShaderNodeBsdfPrincipled'); bsdf.location = (600, 0)
    bsdf.inputs['Metallic'].default_value = 1.0
    attr = N('ShaderNodeAttribute'); attr.location = (-900, 200)
    attr.attribute_type = 'GEOMETRY'
    attr.attribute_name = 'weld_profile'
    coord = N('ShaderNodeTexCoord'); coord.location = (-1100, -200)
                                                                    
    noise = N('ShaderNodeTexNoise'); noise.location = (-900, -100)
    noise.inputs['Scale'].default_value = 3.0
    noise.inputs['Detail'].default_value = 4.0
    L(coord.outputs['UV'], noise.inputs['Vector'])
    wiggle = N('ShaderNodeMath'); wiggle.location = (-700, -100)
    wiggle.operation = 'MULTIPLY_ADD'
    wiggle.inputs[1].default_value = 0.22
    wiggle.inputs[2].default_value = -0.11
    L(noise.outputs['Fac'], wiggle.inputs[0])
    fac = N('ShaderNodeMath'); fac.location = (-500, 150)
    fac.operation = 'ADD'
    fac.use_clamp = True
    L(attr.outputs['Fac'], fac.inputs[0])
    L(wiggle.outputs['Value'], fac.inputs[1])
    tint = N('ShaderNodeValToRGB'); tint.location = (-300, 250)
    els = tint.color_ramp.elements
    stops = ((0.00, (0.040, 0.032, 0.028)), (0.10, (0.09, 0.06, 0.09)), (0.22, (0.14, 0.12, 0.30)),
             (0.36, (0.18, 0.25, 0.42)), (0.52, (0.50, 0.37, 0.17)), (0.68, (0.60, 0.50, 0.32)),
             (0.85, (0.58, 0.56, 0.52)), (1.00, (0.64, 0.63, 0.61)))
    els[0].position, els[0].color = stops[0][0], stops[0][1] + (1.0,)
    els[1].position, els[1].color = stops[-1][0], stops[-1][1] + (1.0,)
    for pos, col in stops[1:-1]:
        e = els.new(pos)
        e.color = col + (1.0,)
    L(fac.outputs['Value'], tint.inputs['Fac'])
    rough = N('ShaderNodeMapRange'); rough.location = (-300, -50)
    rough.inputs['To Min'].default_value = 0.55
    rough.inputs['To Max'].default_value = 0.24
    L(fac.outputs['Value'], rough.inputs['Value'])
                                                                                 
    blob = N('ShaderNodeTexNoise'); blob.location = (-700, -400)
    blob.inputs['Scale'].default_value = 2.2
    blob.inputs['Detail'].default_value = 2.0
    L(coord.outputs['UV'], blob.inputs['Vector'])
    isl = N('ShaderNodeMapRange'); isl.location = (-500, -400)
    isl.inputs['From Min'].default_value = 0.66
    isl.inputs['From Max'].default_value = 0.70
    L(blob.outputs['Fac'], isl.inputs['Value'])
    crown = N('ShaderNodeMath'); crown.location = (-300, -400)
    crown.operation = 'MULTIPLY'
    crown.use_clamp = True
    L(isl.outputs['Result'], crown.inputs[0])
    L(attr.outputs['Fac'], crown.inputs[1])
    mixc = N('ShaderNodeMix'); mixc.location = (100, 250)
    mixc.data_type = 'RGBA'
    mixc.inputs[7].default_value = (0.09, 0.06, 0.035, 1.0)
    L(crown.outputs['Value'], mixc.inputs[0])
    L(tint.outputs['Color'], mixc.inputs[6])
    mixr = N('ShaderNodeMix'); mixr.location = (100, -50)
    mixr.data_type = 'FLOAT'
    mixr.inputs[3].default_value = 0.12
    L(crown.outputs['Value'], mixr.inputs[0])
    L(rough.outputs['Result'], mixr.inputs[2])
                                          
    grain = N('ShaderNodeTexNoise'); grain.location = (-100, -600)
    grain.inputs['Scale'].default_value = 60.0
    grain.inputs['Detail'].default_value = 6.0
    L(coord.outputs['UV'], grain.inputs['Vector'])
    bump = N('ShaderNodeBump'); bump.location = (300, -400)
    bump.inputs['Strength'].default_value = 0.15
    bump.inputs['Distance'].default_value = 0.0002
    L(grain.outputs['Fac'], bump.inputs['Height'])
    L(mixc.outputs[2], bsdf.inputs['Base Color'])
    L(mixr.outputs[0], bsdf.inputs['Roughness'])
    L(bump.outputs['Normal'], bsdf.inputs['Normal'])
    L(bsdf.outputs['BSDF'], out.inputs['Surface'])
    return mat


def _placeholder(mat):
    # the flat PD_Weld earlier versions made: viewport colour only, no node
    # setup beyond the default Principled BSDF and the output
    if tuple(round(c, 3) for c in mat.diffuse_color[:3]) != (0.32, 0.3, 0.28):
        return False
    if not uses_nodes(mat) or mat.node_tree is None:
        return True
    kinds = sorted(n.bl_idname for n in mat.node_tree.nodes)
    return kinds in ([], ['ShaderNodeBsdfPrincipled', 'ShaderNodeOutputMaterial'])


# раздел «Welded Edges»
class PROJECT_DAMAGE_PT_weld(bpy.types.Panel):
    bl_label = 'Welded Edges'
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "⚒ Project Damage"
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        obj = context.active_object
        return obj is not None and obj.type == 'MESH'

    def draw(self, context):
        from . import build,ownership
        layout=self.layout;obj=context.active_object;s=obj.project_damage_weld
        box=layout.box();box.label(text='Enable to spawn on all edges');box.prop(s,'edges_enable')
        column=box.column(align=True)
        column.prop_search(s,'mask_group',obj,'vertex_groups',text='Vertex Group')
        column.prop(s,'mask_threshold')
        row=column.row(align=True)
        row.operator('project_damage.weld_mask',text='Paint Mask',icon='BRUSH_DATA').action='PAINT'
        row.operator('project_damage.weld_mask',text='Edit Mask',icon='EDITMODE_HLT').action='EDIT'
        row.prop(s,'on_edges',toggle=True)
        if bpy.context.mode=='EDIT_MESH':
            column.operator('project_damage.weld_mask',text='Assign to Weld Mask',icon='CHECKMARK').action='ASSIGN'
            if s.on_edges:
                column.operator('project_damage.weld_mask',text='Clear Edge Mask',icon='TRASH').action='CLEAR_EDGES'
        if bpy.context.mode in {'EDIT_MESH','PAINT_WEIGHT'}:column.operator('project_damage.weld_mask',text='Finish Mask',icon='CHECKMARK').action='FINISH'
        column.prop(s,'edge_type',expand=True);column.prop(s,'angle')
        box=layout.box();box.prop(s,'strokes_enable');column=box.column(align=True);column.enabled=s.strokes_enable
        from . import paint
        paint.drawing_row(column,obj.project_damage,'WELD',1)

        box=layout.box();box.label(text='Bead');column=box.column(align=True)
        material_controls.inline(column,s,'material',text='Weld Bead')
        quality_ui.draw(column, s,'detail',text='Resolution');column.prop(s,'seed',text='Seed')
        column.separator(factor=.5)
        column.prop(s,'width');column.prop(s,'height');column.prop(s,'pitch')
        column.prop(s,'ripple_height');column.prop(s,'curvature');column.prop(s,'wobble')
        column.prop(s,'sides');column.prop(s,'uv_scale')



_SETTINGS = ProjectDamageWeldSettings
_PANEL = PROJECT_DAMAGE_PT_weld


@bpy.app.handlers.persistent
def _restore_existing_default(_unused):
    from . import generated_assets
    for obj in bpy.data.objects:
        if obj.type != 'MESH' or not obj.is_editable or not obj.project_damage.weld_enable:
            continue
        settings = obj.project_damage_weld
        if not settings.is_property_set('edges_enable') and generated_assets.output(obj, 'weld') is not None:
            settings['edges_enable'] = True


# подключаем свойства и действия этого модуля к Blender
def register():
    from . import live, generated_assets
    for name, prop in _SETTINGS.__annotations__.items():
        prop.keywords['update'] = generated_assets.material_changed if name == 'material' else live.touch_prop
    bpy.utils.register_class(_SETTINGS)
    bpy.types.Object.project_damage_weld = bpy.props.PointerProperty(type=_SETTINGS)
    bpy.app.handlers.load_post.append(_restore_existing_default)

# убираем подключения модуля при выключении аддона
def unregister():
    if _restore_existing_default in bpy.app.handlers.load_post:
        bpy.app.handlers.load_post.remove(_restore_existing_default)
    if hasattr(bpy.types.Object, 'project_damage_weld'):
        del bpy.types.Object.project_damage_weld
    if _SETTINGS.is_registered:
        bpy.utils.unregister_class(_SETTINGS)

                                                
from . import surface_guidance
