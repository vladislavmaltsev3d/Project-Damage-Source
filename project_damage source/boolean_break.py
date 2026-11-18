# варианты Boolean Break, список каттеров и отдельные фрагменты
# SPDX-License-Identifier: GPL-3.0-or-later
"""Boolean Break variants, per-instance cutter controls, and loose fragments."""
from . import output_collections
import random

import bmesh
import bpy
import numpy as np
from bpy.app.handlers import persistent
from mathutils import Matrix, Vector

from .exact_cache import Results

from . import live, ownership

                                                                                  
VARIANTS = (
    ('SIMPLE', 'Boolean Break', 'Boolean', 'The cut alone: nothing inside the break', 'MOD_BOOLEAN', None),
    ('REBAR', 'Boolean Break Rebar', 'Boolean Rebar', 'Steel bars exposed in the break', 'MOD_WIREFRAME', '0'),
    ('BRICKS', 'Boolean Break Bricks', 'Boolean Bricks', 'A brick layer behind the surface, revealed by the break',
     'MOD_BUILD', '1'),
    ('PLANKS', 'Boolean Break Planks', 'Boolean Planks', 'Wooden boards behind the surface, splintered where broken',
     'MOD_ARRAY', '2'),
    ('LATHS', 'Boolean Break Laths', 'Boolean Laths', 'Thin wooden laths behind the surface, splintered where broken', 'MOD_ARRAY', '5'),
    ('FRAMING', 'Boolean Break Framing', 'Boolean Framing', 'A timber stud frame behind a drywall skin',
     'SNAP_GRID', '3'),
    ('PIPES', 'Boolean Break Pipes', 'Boolean Pipes', 'Hollow pipes in the wall, bent and broken at the edge of the break',
     'MESH_CYLINDER', 'P'),
    ('CABLES', 'Boolean Break Cables', 'Boolean Cables', 'Electrical cables in the wall, sagging, torn and hanging out of the break',
     'PLUGIN', 'C'),
)
VARIANTS += (('CENTRAL', 'Boolean Break Central Rebar', 'Central Rebar',
              'Central reinforcement follows individual columns, beams and curved structural members, exposed by the damage',
              'MESH_CYLINDER', '0'),)
_SAVED_VARIANTS = VARIANTS + (
    ('CHIPS', 'Boolean Break Wooden Chips', 'Boolean Chips', 'Plywood impact: plies torn along their grain, a stepped crater and bent splinters',
     'FORCE_FORCE', '4'),
)
VARIANT_ITEMS = tuple((v[0], v[2].replace('Boolean ', '') if v[0] != 'SIMPLE' else 'Simple', v[3], v[4], i)
                      for i, v in enumerate(_SAVED_VARIANTS))
# доступные варианты одни и те же в библиотеке и кнопках добавления
IMPORTABLE_VARIANTS = frozenset(v[0] for v in VARIANTS if v[0] != 'BRICKS')
FILL_TITLES = {'CENTRAL': 'Central Rebar', 'REBAR': 'Rebar', 'BRICKS': 'Brick Layer', 'PLANKS': 'Wood Planks', 'LATHS': 'Wood Laths', 'FRAMING': 'Wood Framing',
               'PIPES': 'Pipes', 'CABLES': 'Cables', 'CHIPS': 'Wooden Chips'}

FRAGMENT_STAMP = 'project_damage_bfrag_of'                                    
FRAGMENT_COLLECTION = 'project_damage_bfrag_collection'
STAGE_KEY = 'project_damage_bfrag_stage'                                                          
PRIMITIVE_KEY = 'project_damage_bb_primitive'                                       
DISPLAY_KEY = 'project_damage_bb_display'                                                     
CUTTER_TYPES = {'MESH', 'CURVE', 'SURFACE', 'FONT', 'META'}


                                                                             
          

def variant_index(p):
    if not getattr(p, 'rebar_enable', False):
        if getattr(p, 'cable_break_enable', False):
            return next(i for i, v in enumerate(VARIANTS) if v[0] == 'CABLES')
        if getattr(p, 'pipe_break_enable', False):
            return next(i for i, v in enumerate(VARIANTS) if v[0] == 'PIPES')
        return 0
    fill = str(getattr(p, 'rebar_fill', '0'))
    if fill == '0' and p.boolean_central_rebar:
        return next(i for i, v in enumerate(VARIANTS) if v[0] == 'CENTRAL')
    return next((i for i, v in enumerate(_SAVED_VARIANTS) if v[5] == fill), 1)


def variant(p):
    return _SAVED_VARIANTS[variant_index(p)][0]


def variant_get(p):
    return variant_index(p)


def variant_set(p, value):
    v = _SAVED_VARIANTS[max(0, min(int(value), len(_SAVED_VARIANTS) - 1))]
    central = v[0] == 'CENTRAL'
    if p.boolean_central_rebar != central:
        p.boolean_central_rebar = central
    cables = v[0] == 'CABLES'
    if getattr(p, 'cable_break_enable', cables) != cables:
        p.cable_break_enable = cables
    if cables:
        if getattr(p, 'pipe_break_enable', False):
            p.pipe_break_enable = False
        if p.rebar_enable:
            p.rebar_enable = False
        return
    pipes = v[0] == 'PIPES'
    if getattr(p, 'pipe_break_enable', pipes) != pipes:
        p.pipe_break_enable = pipes
    if pipes:
        if p.rebar_enable:
            p.rebar_enable = False
        return
    if v[5] is None:
        if p.rebar_enable:
            p.rebar_enable = False
        return
    if v[5] in {'2','5'} and str(p.rebar_fill) not in {'2','5'} and not p.is_property_set('boolean_wood_deepness'):
        p.boolean_wood_deepness=12.
    if str(p.rebar_fill) != v[5]:
        p.rebar_fill = v[5]
        if v[0]=='LATHS':
            p.rebar_plank_width=3.2
            p.rebar_plank_thickness=.8
            p.rebar_plank_gap=1.1
            p.rebar_plank_dir='1'
    if not p.rebar_enable:
        p.rebar_enable = True


def variant_label(src, row=None):
    from . import boolean_stack
    p = boolean_stack.layer_props(src, row) if row is not None and row.get(boolean_stack.INSTANCE) else src.project_damage
    return _SAVED_VARIANTS[variant_index(p)][1]


def variant_row(src, kind):
    from . import boolean_stack
    if boolean_stack.SERIAL not in src:
        return next((item for item in src.project_damage_effects if item.key == 'boolean'), None) if variant(src.project_damage) == kind else None
    matches = [item for item in boolean_stack.rows(src) if variant(boolean_stack.layer_props(src, item)) == kind]
    active = boolean_stack.edited(src)
    return active if active in matches else next(reversed(matches), None)


def _raw(layout):
    """The UILayout behind sidebar._Layout (it rewrites enums and labels)."""
    if type(layout).__name__ == '_Layout':
        return object.__getattribute__(layout, 'layout')
    return layout


def draw_tiles(grid, src, full, enabled, preview_icon, effect_name=lambda key, label: label, variant_filter=None):
    """The Add browser: one tile per variant instead of one Boolean Break tile."""
    from . import boolean_stack
    for vid, label, tile_label, _, icon, _ in VARIANTS:
        if vid not in IMPORTABLE_VARIANTS:continue
        if variant_filter is not None and not variant_filter(vid):
            continue
        if vid == 'SIMPLE':
            label = effect_name('boolean', label)
        entry = variant_row(src, vid)
        active = entry is not None and enabled(src, 'boolean', entry)
        tile = grid.column(align=True)
        if full:
            card = tile.box()
            preview = card.row(align=True)
            preview.alignment = 'CENTER'
            preview.template_icon(icon_value=preview_icon('boolean' if vid == 'SIMPLE' else 'boolean_' + vid.lower(),
                                                          icon), scale=5.0)
            row = tile.row(align=True)
            row.scale_y = 1.3
            row.operator_context = 'INVOKE_DEFAULT'
            op = row.operator('project_damage.boolean_variant_add', text=label, depress=active)
        else:
            tile.scale_y = 1.15
            row = tile.row(align=True)
            op = row.operator('project_damage.boolean_variant_add', text=tile_label, icon=icon,
                               depress=active)
        op.variant = vid
        if entry is not None:
            remove=row.operator('project_damage.effect_action',text='',icon='X')
            remove.effect='boolean'
            remove.action='REMOVE'
            remove.instance=entry.get(boolean_stack.INSTANCE, 0)


def draw_variant_selector(layout, src):
    raw = _raw(layout)
    row = raw.row(align=True)
    row.scale_y = 1.5
    for identifier, label, *_ in VARIANT_ITEMS[:len(VARIANTS)]:
        if identifier not in IMPORTABLE_VARIANTS:continue
        row.prop_enum(src.project_damage, 'break_variant', identifier, text=label)


def draw_fill_section(layout, context, src, wrap):
    """The variant's own controls (Rebar, Brick Layer, ...); nothing for Simple."""
    from types import SimpleNamespace
    from . import effect_layout, panels
    v = variant(src.project_damage)
    if v == 'SIMPLE':
        return
    from . import generated_geometry
    if not generated_geometry.draw(_raw(layout), src.project_damage, 'boolean'):return
    if v == 'CABLES':
        from . import cables
        cables.draw_cable_settings(_raw(layout), src.project_damage)
        return
    if v == 'PIPES':
        from . import pipes
        pipes.draw_pipe_settings(_raw(layout), src.project_damage)
        return
    body = effect_layout.section(_raw(layout), FILL_TITLES[v])
    panels.OBJECT_PT_project_damage_rebar.draw(SimpleNamespace(layout=wrap(body), variant_ui=True), context)


# действие «Add Boolean Break»
class PROJECT_DAMAGE_OT_boolean_variant_add(bpy.types.Operator):
    bl_idname = 'project_damage.boolean_variant_add'
    bl_label = 'Add Boolean Break'
    bl_options = {'REGISTER', 'UNDO'}
    variant: bpy.props.EnumProperty(items=tuple(item for item in VARIANT_ITEMS[:len(VARIANTS)] if item[0] in IMPORTABLE_VARIANTS), default='SIMPLE')
    defer_rebuild: bpy.props.BoolProperty(default=False, options={'HIDDEN', 'SKIP_SAVE'})

    @classmethod
    def description(cls, context, properties):
        v = next(v for v in VARIANTS if v[0] == properties.variant)
        return 'Add %s with default settings and its own cutter list' % v[1]

    @classmethod
    def poll(cls, context):
        from . import sidebar
        return sidebar.source(context) is not None and context.mode == 'OBJECT'

    def invoke(self, context, event):
        from . import sidebar, background_compute
        if background_compute.confirm_effect(context, sidebar.source(context), 'boolean', 'ADD', self.variant):
            return {'CANCELLED'}
        return self.execute(context)

    def execute(self, context):
        from . import sidebar, boolean_stack, live, build, background_compute
        src = sidebar.source(context)
        background_compute.cancel(src)
                                                                        
                                                                        
        boolean_stack.add(src, self.variant)
        live.invalidate_index()
        live._mark_dirty(src)
        if not self.defer_rebuild and build.result_of(src) is not None:
                                                                           
                                                                             
            result, reports = build.rebuild_object(context, src)
            for kind, message in reports:
                self.report(kind, message)
            if result is None:
                self.report({'WARNING'}, 'Boolean Break added; update again after resolving the reported mesh error')
        return {'FINISHED'}


                                                                             
                                               

def _is_output(obj):
    return (any(obj.get(k) for k in ownership.STAMPS + ownership.LOOSE_STAMPS)
            or obj.get(FRAGMENT_STAMP) is not None)


def can_cut(src, obj):
    if obj is None or obj == src or _is_output(obj):
        return False
    if obj.type == 'EMPTY':
        return True
    return obj.type in CUTTER_TYPES


def draw_breaker_row(layout, item):
    row = layout.row(align=True)
    obj = item.obj
    if getattr(item, 'from_collection', False):
        row.label(text='', icon='OUTLINER_COLLECTION')
        sub = row.row(align=True)
        sub.enabled = False
        sub.prop(item, 'obj', text='')
    else:
        icon = ('EMPTY_AXIS' if obj is None or obj.type == 'EMPTY'
                else 'MESH_CUBE' if obj.get(PRIMITIVE_KEY) else 'OBJECT_DATA')
        row.label(text='', icon=icon)
        row.prop(item, 'obj', text='')                                             
    if obj is not None:
        row.prop(obj.project_damage_breaker, 'enabled', text='')


def draw_breaker_list(layout, p, context):
    from . import boolean_slicers
    raw = _raw(layout)
    row = raw.row()
    row.template_list('PROJECT_DAMAGE_UL_breakers', '', p, 'breakers', p, 'breaker_index', rows=6)
    col = row.column(align=True)
    col.operator('project_damage.breaker_add', icon='MESH_PLANE', text='')
    col.menu('PROJECT_DAMAGE_MT_boolean_primitives', icon='MESH_CUBE', text='')
    col.operator('project_damage.boolean_pick', icon='EYEDROPPER', text='')
    col.separator()
    col.operator('project_damage.breaker_remove', icon='REMOVE', text='')
    col.operator('project_damage.boolean_delete', icon='TRASH', text='')
    boolean_slicers.draw_panel(raw, p.id_data, context)
    box = raw.box()
    head = box.row(align=True)
    head.label(text='Boolean Objects', icon='OUTLINER_COLLECTION')
    row = box.row(align=True)
    row.prop(p, 'break_objects', text='')
    row.operator('project_damage.boolean_objects_new', text='', icon='COLLECTION_NEW')
    if p.break_objects is not None:
        n = sum(1 for r in p.breakers if getattr(r, 'from_collection', False))
        box.label(text='%d objects of %s cut' % (n, p.break_objects.name), icon='INFO')


def primitive_material(obj):
    if not obj.get(PRIMITIVE_KEY) or obj.type!='MESH':return
    from .blender_compat import enable_nodes
    mat=next((m for m in bpy.data.materials if m.get('project_damage_cutter_material')),None)
    if mat is None:
        mat=bpy.data.materials.new('Boolean Cutter | Project Damage')
        mat['project_damage_cutter_material']=True
        enable_nodes(mat)
        nodes=mat.node_tree.nodes;nodes.clear()
        shader=nodes.new('ShaderNodeBsdfPrincipled');shader.inputs['Alpha'].default_value=0.0
        shader.location=(-260,0)
        out=nodes.new('ShaderNodeOutputMaterial');out.location=(40,0)
        mat.node_tree.links.new(shader.outputs['BSDF'],out.inputs['Surface'])
        mat.diffuse_color=(1.0,.3,.04,0.0)
        if hasattr(mat,'surface_render_method'):mat.surface_render_method='DITHERED'
    if mat not in obj.data.materials[:]:obj.data.materials.append(mat)
    index=list(obj.data.materials).index(mat)
    for face in obj.data.polygons:face.material_index=index
    obj.hide_render=False


def link_cutter(src, obj):
    from . import boolean_stack
    """Make `obj` a cutter of `src` (or select its row when it already is one)."""
    p = src.project_damage
    for i, row in enumerate(p.breakers):
        if row.obj == obj:
            p.breaker_index = i
            return False
    bp = obj.project_damage_breaker
    bp.is_breaker = True
    bp.enabled = True
    if obj.type != 'EMPTY':
        obj.display_type = 'WIRE'
        obj.hide_render = True
        primitive_material(obj)
    row = next((r for r in p.breakers if r.obj is None and not getattr(r, 'from_collection', False)), None)
    if row is None:
        row = p.breakers.add()
    row.obj = obj
    p.breaker_index = next(i for i, r in enumerate(p.breakers) if r.obj == obj)
    active = boolean_stack.edited(src)
    if active is not None:
        active[boolean_stack.ON] = True
        boolean_stack.refresh_master(src)
    elif boolean_stack.SERIAL not in src:
        p.ui_boolean_enable = True
    live.invalidate_index()
    return True


def _view_under_mouse(context, event):
    for area in context.window.screen.areas:
        if area.type != 'VIEW_3D':
            continue
        for region in area.regions:
            if (region.type == 'WINDOW' and region.x <= event.mouse_x < region.x + region.width
                    and region.y <= event.mouse_y < region.y + region.height):
                rv3d = region.data or area.spaces.active.region_3d
                return area, region, rv3d, (event.mouse_x - region.x, event.mouse_y - region.y)
    return None


def pick_object(context, event, src):
    """The object under the mouse: an Empty / curve near its origin first,
    then the first mesh the view ray hits that is not the source, its result
    or another output (a cutter inside the wall is found through it)."""
    from bpy_extras import view3d_utils
    found = _view_under_mouse(context, event)
    if found is None:
        return None
    area, region, rv3d, coord = found
    best, best_d = None, 18.0
    for obj in context.visible_objects:
        if obj.type in {'EMPTY', 'CURVE', 'FONT'} and can_cut(src, obj):
            p2 = view3d_utils.location_3d_to_region_2d(region, rv3d, obj.matrix_world.translation)
            if p2 is not None and (p2 - Vector(coord)).length < best_d:
                best, best_d = obj, (p2 - Vector(coord)).length
    if best is not None:
        return best
    origin = view3d_utils.region_2d_to_origin_3d(region, rv3d, coord)
    direction = view3d_utils.region_2d_to_vector_3d(region, rv3d, coord)
    dg = context.evaluated_depsgraph_get()
    for _ in range(32):
        hit, loc, _n, _i, obj, _m = context.scene.ray_cast(dg, origin, direction)
        if not hit:
            return None
        obj = obj.original
        if can_cut(src, obj) and ownership.source_of(obj) in (obj, None):
            return obj
        origin = loc + direction * 1e-4
    return None


# действие «Pick Cutter»
class PROJECT_DAMAGE_OT_boolean_pick(bpy.types.Operator):
    """Eyedropper: click an object in the viewport to make it a cutter (Shift-click picks more, Esc or right-click ends)"""
    bl_idname = 'project_damage.boolean_pick'
    bl_label = 'Pick Cutter'
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        src = ownership.context_source(context)
        return (context.mode == 'OBJECT' and src is not None and src.type == 'MESH'
                and not src.project_damage_breaker.is_breaker and context.window is not None)

    def invoke(self, context, event):
        self.src_name = ownership.context_source(context).name
        self.picked = 0
        context.window.cursor_modal_set('EYEDROPPER')
        if context.workspace is not None:
            context.workspace.status_text_set('Pick Cutter: click an object  |  Shift-click: pick more  |  '
                                              'Esc / right-click: done')
        context.window_manager.modal_handler_add(self)
        return {'RUNNING_MODAL'}

    def _end(self, context):
        context.window.cursor_modal_restore()
        if context.workspace is not None:
            context.workspace.status_text_set(None)
        return {'FINISHED'} if self.picked else {'CANCELLED'}

    def modal(self, context, event):
        if event.type in {'ESC', 'RIGHTMOUSE'} and event.value == 'PRESS':
            return self._end(context)
        if event.type == 'LEFTMOUSE' and event.value == 'PRESS':
            src = bpy.data.objects.get(self.src_name)
            if src is None:
                return self._end(context)
            obj = pick_object(context, event, src)
            if obj is None:
                if _view_under_mouse(context, event) is None:
                    return self._end(context)                                     
                self.report({'WARNING'}, 'Pick Cutter: no object there')
                return {'RUNNING_MODAL'}
            if link_cutter(src, obj):
                self.picked += 1
                self.report({'INFO'}, 'Pick Cutter: %s cuts %s' % (obj.name, src.name))
            else:
                self.report({'INFO'}, 'Pick Cutter: %s is already a cutter' % obj.name)
            for area in context.window.screen.areas:
                area.tag_redraw()
            if not event.shift:
                return self._end(context)
            return {'RUNNING_MODAL'}
        return {'PASS_THROUGH'}                                      


def _source_frame(src):
    """(local bounds lo, hi) of the source."""
    corners = [Vector(c) for c in src.bound_box]
    lo = Vector((min(c[i] for c in corners) for i in range(3)))
    hi = Vector((max(c[i] for c in corners) for i in range(3)))
    return lo, hi


PRIMITIVES = (
    ('CUBE', 'Cube', 'MESH_CUBE'),
    ('ICOSPHERE', 'Icosphere', 'MESH_ICOSPHERE'),
    ('SPHERE', 'UV Sphere', 'MESH_UVSPHERE'),
    ('CYLINDER', 'Cylinder', 'MESH_CYLINDER'),
    ('CONE', 'Cone', 'MESH_CONE'),
    ('ROCK', 'Rock', 'OUTLINER_OB_POINTCLOUD'),
    ('WOOD', 'Wood', 'MOD_ARRAY'),
    ('PLANE', 'Plane (cut one side)', 'MESH_PLANE'),
)


def _wood_primitive(bm, r, seed):
    """Closed plank with splintered ends elongated along local X."""
    rng = random.Random(seed)
    rows = []
    for i in range(17):
        y = r * (i / 16 - 0.5) * 1.3
        left = -r * (1.06 + rng.uniform(-0.29, 0.31))
        right = r * (1.06 + rng.uniform(-0.26, 0.35))
        rows.append((
            bm.verts.new((left, y, r * (-0.32 + rng.uniform(-0.035, 0.035)))),
            bm.verts.new((right, y, r * (-0.32 + rng.uniform(-0.035, 0.035)))),
            bm.verts.new((left * .94, y, r * (0.32 + rng.uniform(-0.06, 0.06)))),
            bm.verts.new((right * .94, y, r * (0.32 + rng.uniform(-0.06, 0.06)))),
        ))
    for (a, b, c, d), (e, f, g, h) in zip(rows, rows[1:]):
        for face in ((a,e,f,b), (c,d,h,g), (a,c,g,e), (b,f,h,d)):
            bm.faces.new(face)
    a, b, c, d = rows[0]
    bm.faces.new((a,b,d,c))
    a, b, c, d = rows[-1]
    bm.faces.new((a,c,d,b))


def primitive_mesh(shape, r, seed=0):
    """A closed cutter mesh of radius / half size r around the origin
    (PLANE: an open sheet 2r x 2r facing +Z)."""
    bm = bmesh.new()
    if shape == 'CUBE':
        bmesh.ops.create_cube(bm, size=2.0 * r)
    elif shape == 'ICOSPHERE':
        bmesh.ops.create_icosphere(bm, subdivisions=2, radius=r)
    elif shape == 'SPHERE':
        bmesh.ops.create_uvsphere(bm, u_segments=24, v_segments=12, radius=r)
    elif shape in {'CYLINDER', 'CONE'}:
        bmesh.ops.create_cone(bm, cap_ends=True, cap_tris=False, segments=24, radius1=r,
                              radius2=r if shape == 'CYLINDER' else 0.0, depth=2.0 * r)
    elif shape == 'ROCK':
        from mathutils import noise
        bmesh.ops.create_icosphere(bm, subdivisions=3, radius=r)
        rng = random.Random(seed)
        off = Vector((rng.uniform(0, 100), rng.uniform(0, 100), rng.uniform(0, 100)))
        squash = Vector((rng.uniform(0.8, 1.2), rng.uniform(0.8, 1.2), rng.uniform(0.6, 0.9)))
        for v in bm.verts:
            d = v.co.normalized()
            n = noise.noise(d * 1.3 + off) * 0.28 + noise.noise(d * 3.1 + off) * 0.1
            v.co = Vector(c * s for c, s in zip(v.co * (1.0 + n), squash))
    elif shape == 'WOOD':
        _wood_primitive(bm, r, seed)
    elif shape == 'PLANE':
        bmesh.ops.create_grid(bm, x_segments=1, y_segments=1, size=r)
    bm.normal_update()
    if shape != 'PLANE' and bm.calc_volume(signed=True) < 0.0:
        bmesh.ops.reverse_faces(bm, faces=bm.faces[:])
    mesh = bpy.data.meshes.new('ProjectDamage Cutter')
    bm.to_mesh(mesh)
    bm.free()
    return mesh


# действие «Add Cutter Primitive»
class PROJECT_DAMAGE_OT_boolean_primitive(bpy.types.Operator):
    """Add a primitive cutter at the 3D cursor and link it (move, scale and rotate it to shape the break)"""
    bl_idname = 'project_damage.boolean_primitive'
    bl_label = 'Add Cutter Primitive'
    bl_options = {'REGISTER', 'UNDO'}
    shape: bpy.props.EnumProperty(items=tuple((k, n, '') for k, n, _ in PRIMITIVES), default='CUBE')
    size: bpy.props.FloatProperty(name='Size', default=0.3, min=0.01, max=4.0, subtype='FACTOR',
                                  description='Size as a share of the object\'s largest dimension')

    @classmethod
    def poll(cls, context):
        src = ownership.context_source(context)
        return (context.mode == 'OBJECT' and src is not None and src.type == 'MESH'
                and not src.project_damage_breaker.is_breaker and not src.name.endswith('_project_damage'))

    def execute(self, context):
        src = ownership.context_source(context)
        lo, hi = _source_frame(src)
        dims = hi - lo
        big = max(max(dims), 1e-6)
        if self.shape == 'PLANE':
                                                                              
                                                     
            r = 0.6 * max(dims.x, dims.y, 1e-6)
            centre = Vector(((lo.x + hi.x) * 0.5, (lo.y + hi.y) * 0.5, lo.z + 0.75 * dims.z))
        else:
            r = 0.5 * self.size * big
            centre = Vector(((lo.x + hi.x) * 0.5, (lo.y + hi.y) * 0.5, hi.z))                  
        n = sum(1 for o in bpy.data.objects if o.get(PRIMITIVE_KEY)) + 1
        mesh = primitive_mesh(self.shape, r, seed=n)
        label = next(name for key, name, _ in PRIMITIVES if key == self.shape).split(' (')[0]
        obj = bpy.data.objects.new('Cutter %s' % label, mesh)
        mesh.name = obj.name
        output_collections.link_object(context,src,obj)
        obj.matrix_world = src.matrix_world.copy()
        obj.matrix_world.translation = context.scene.cursor.location.copy()
        obj[PRIMITIVE_KEY] = self.shape
        if self.shape == 'PLANE':
            obj.project_damage_breaker.custom_sheet = '0'
        link_cutter(src, obj)
        live._mark_dirty(src)
        src.select_set(False)
        for o in tuple(context.selected_objects):
            o.select_set(False)
        obj.select_set(True)
        context.view_layer.objects.active = obj
        return {'FINISHED'}


class PROJECT_DAMAGE_MT_boolean_primitives(bpy.types.Menu):
    bl_idname = 'PROJECT_DAMAGE_MT_boolean_primitives'
    bl_label = 'Add Cutter Primitive'

    def draw(self, context):
        for key, name, icon in PRIMITIVES:
            self.layout.operator('project_damage.boolean_primitive', text=name, icon=icon).shape = key


def _users(obj):
    from . import boolean_stack
    return [source for source in bpy.data.objects if source.type == 'MESH'
            and obj in boolean_stack.cutter_objects(source, include_disabled=True)]


# действие «Delete Breaker»
class PROJECT_DAMAGE_OT_boolean_delete(bpy.types.Operator):
    """Delete the active breaker Empty or cutter primitive from the scene (your own objects are only unlinked)"""
    bl_idname = 'project_damage.boolean_delete'
    bl_label = 'Delete Breaker'
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        src = ownership.context_source(context)
        return (context.mode == 'OBJECT' and src is not None and src.type == 'MESH'
                and len(src.project_damage.breakers) > 0)

    def execute(self, context):
        src = ownership.context_source(context)
        p = src.project_damage
        i = max(0, min(p.breaker_index, len(p.breakers) - 1))
        row = p.breakers[i]
        obj = row.obj
        if getattr(row, 'from_collection', False):
            self.report({'WARNING'}, 'Delete Breaker: %s comes from Boolean Objects; take it out of %s instead'
                        % (obj.name if obj else 'this row', p.break_objects.name if p.break_objects else 'the collection'))
            return {'CANCELLED'}
        p.breakers.remove(i)
        p.breaker_index = max(0, min(i, len(p.breakers) - 1))
        if obj is not None:
            ours = obj.type == 'EMPTY' and obj.project_damage_breaker.is_breaker or obj.get(PRIMITIVE_KEY)
            others = _users(obj)
            if ours and not others:
                mesh = obj.data if obj.type == 'MESH' else None
                name = obj.name
                bpy.data.objects.remove(obj, do_unlink=True)
                if mesh is not None and mesh.users == 0:
                    bpy.data.meshes.remove(mesh)
                self.report({'INFO'}, 'Deleted %s' % name)
            elif ours:
                self.report({'INFO'}, 'Unlinked %s (still used by %s)' % (obj.name, others[0].name))
            else:
                self.report({'INFO'}, 'Unlinked %s (your own object is kept)' % obj.name)
        live._mark_dirty(src)
        return {'FINISHED'}


                                                                             
                                           

def collection_cutters(src, coll):
    from . import build
    out = []
    for obj in sorted(coll.all_objects, key=lambda o: o.name):
        if not can_cut(src, obj):
            continue
        if obj.type == 'EMPTY' and not obj.project_damage_breaker.is_breaker:
            continue                                                                          
        if obj.type == 'MESH' and build.result_of(obj) is not None:
            continue                                                          
        out.append(obj)
    return out


def _prepare_member(obj):
    if obj.type == 'EMPTY' or DISPLAY_KEY in obj:
        return
    obj[DISPLAY_KEY] = {'display_type': obj.display_type, 'hide_render': int(obj.hide_render)}
    obj.display_type = 'WIRE'
    obj.hide_render = True
    primitive_material(obj)


def _release_member(obj):
    old = obj.get(DISPLAY_KEY)
    if old is None:
        return
    try:
        obj.display_type = str(old['display_type'])
        obj.hide_render = bool(old['hide_render'])
    except (TypeError, ValueError, KeyError):
        pass
    del obj[DISPLAY_KEY]


def _has_flag(p):
    """False until Blender restarts after the update (the row type predates the flag)."""
    try:
        return 'from_collection' in p.bl_rna.properties['breakers'].fixed_type.properties
    except (KeyError, AttributeError):
        return False


def sync_collection(src):
    """Mirror the Boolean Objects collection into flagged breaker rows.
    Returns True when the rows changed."""
    from . import boolean_stack
    p = src.project_damage
    if not _has_flag(p):
        return False
    coll = getattr(p, 'break_objects', None)
    rows = p.breakers
    flagged = [r.obj for r in rows if getattr(r, 'from_collection', False)]
    manual = {r.obj for r in rows if not getattr(r, 'from_collection', False) and r.obj is not None}
    want = [o for o in collection_cutters(src, coll) if o not in manual] if coll is not None else []
    if flagged == want:
        return False
    previous = live._APPLYING
    live._APPLYING = True                                                          
    try:
        active = rows[p.breaker_index].obj if 0 <= p.breaker_index < len(rows) else None
        for i in reversed(range(len(rows))):
            if getattr(rows[i], 'from_collection', False):
                obj = rows[i].obj
                rows.remove(i)
                if obj is not None and obj not in want and not _users(obj):
                    _release_member(obj)
        for obj in want:
            _prepare_member(obj)
            row = rows.add()
            row.from_collection = True
            row.obj = obj
        if want and boolean_stack.SERIAL not in src:
            p.ui_boolean_enable = True
        p.breaker_index = next((i for i, r in enumerate(rows) if r.obj == active and active is not None),
                               max(0, min(p.breaker_index, len(rows) - 1)))
    finally:
        live._APPLYING = previous
    live._mark_dirty(src)
    return True


_SYNCING = False


@persistent
def _sync_all(*_unused):
    global _SYNCING
    if _SYNCING or live._APPLYING or live._HISTORY_RESTORING or not hasattr(bpy.data, 'objects'):
        return
    if not hasattr(bpy.types.Object, 'project_damage'):
        return
    depsgraph=_unused[1] if len(_unused)==2 and isinstance(_unused[1],bpy.types.Depsgraph) else None
    if depsgraph is not None and not live.event_relevant(depsgraph):return
    if depsgraph is None:live.invalidate_index()
    _SYNCING = True
    try:
        for obj in live._scan().linkers:
            if obj.type != 'MESH' or obj.library is not None:
                continue
            p = obj.project_damage
            if getattr(p, 'break_objects', None) is not None or any(getattr(r, 'from_collection', False)
                                                                     for r in p.breakers):
                sync_collection(obj)
    finally:
        _SYNCING = False


def break_objects_changed(self, context):
    src = self.id_data
    if src is not None and src.type == 'MESH' and not live._HISTORY_RESTORING and not live._APPLYING:
        sync_collection(src)
    live.touch_prop(self, context)


# действие «New Boolean Objects from Selected»
class PROJECT_DAMAGE_OT_boolean_objects_new(bpy.types.Operator):
    """New Boolean Objects collection holding the selected objects (they stay in their own collections too)"""
    bl_idname = 'project_damage.boolean_objects_new'
    bl_label = 'New Boolean Objects from Selected'
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        src = ownership.context_source(context)
        return (context.mode == 'OBJECT' and src is not None and src.type == 'MESH'
                and not src.project_damage_breaker.is_breaker)

    def execute(self, context):
        src = ownership.context_source(context)
        picked = [o for o in context.selected_objects if can_cut(src, o) and o.type != 'EMPTY']
        coll = bpy.data.collections.new(src.name + '_BooleanObjects')
        output_collections.link_collection(context,src,coll)
        for obj in picked:
            coll.objects.link(obj)
        src.project_damage.break_objects = coll
        self.report({'INFO'}, 'Boolean Objects: %s with %d objects. Add more by linking them to it (Ctrl+L)'
                    % (coll.name, len(picked)))
        return {'FINISHED'}


                                                                             
                    

def fragments_of(source):
    uid = source.get('project_damage_uid')
    if not uid:
        return []
    out = [o for o in bpy.data.objects if o.get(FRAGMENT_STAMP) == uid
           and getattr(o, 'project_damage_og_object', None) == source]
    out.sort(key=lambda o: o.get('project_damage_bfrag_index', 0))
    return out


def _remove(obj):
    mesh = obj.data if obj.type == 'MESH' else None
    bpy.data.objects.remove(obj, do_unlink=True)
    if mesh is not None and mesh.users == 0:
        bpy.data.meshes.remove(mesh)


def drop_fragments(source):
    for obj in fragments_of(source):
        _remove(obj)
    uid = source.get('project_damage_uid')
    for coll in [c for c in bpy.data.collections if uid and c.get(FRAGMENT_COLLECTION) == uid]:
        if not coll.objects and not coll.children:
            bpy.data.collections.remove(coll)


def _face_components(nv, first_vertex, edges):
    parent = list(range(nv))

    def find(i):
        root = i
        while parent[root] != root:
            root = parent[root]
        while parent[i] != root:
            parent[i], i = root, parent[i]
        return root

    for a, b in edges:
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[max(ra, rb)] = min(ra, rb)
    roots = np.fromiter((find(i) for i in first_vertex), dtype=np.int64, count=len(first_vertex))
    labels, comp = np.unique(roots, return_inverse=True)
    return comp.ravel(), len(labels)


def _weld_links(mesh):
    from .effect_groups import _weld_ids
    weld = _weld_ids(mesh)
    if weld is None:
        return
    coords = np.empty(len(weld) * 3, dtype=np.float32)
    mesh.vertices.foreach_get('co', coords)
    points = coords.reshape(-1, 3)
    first = {}
    displaced = {}
    for i, key in enumerate(weld.tolist()):
        if key < 0:
            continue
        other = first.setdefault(key, i)
        if other == i:
            continue
        point = tuple(points[i])
        if point != tuple(points[other]):
            other = displaced.setdefault(key, {}).setdefault(point, i)
        if other != i:
            yield i, other


def _components(mesh):
    """Group polygons into loose parts, including the original shading-seam links."""
    from itertools import chain
    nv, nf = len(mesh.vertices), len(mesh.polygons)
    if nf == 0:
        return np.zeros(0, dtype=np.int64), 0
    edges = np.empty(len(mesh.edges) * 2, dtype=np.int64)
    mesh.edges.foreach_get('vertices', edges)
    flat = edges.tolist()
    start = np.empty(nf, dtype=np.int64)
    mesh.polygons.foreach_get('loop_start', start)
    corner = np.empty(len(mesh.loops), dtype=np.int64)
    mesh.loops.foreach_get('vertex_index', corner)
    return _face_components(nv, corner[start], chain(_weld_links(mesh), zip(flat[0::2], flat[1::2])))


def _part_measures(mesh, comp, n, scale):
    """Return local volume, scaled bounding-box extent and local centre of mass."""
    mesh.calc_loop_triangles()
    nt = len(mesh.loop_triangles)
    co = np.empty(len(mesh.vertices) * 3)
    mesh.vertices.foreach_get('co', co)
    co = co.reshape(-1, 3)
    tri = np.empty(nt * 3, dtype=np.int64)
    mesh.loop_triangles.foreach_get('vertices', tri)
    tri = tri.reshape(-1, 3)
    tpoly = np.empty(nt, dtype=np.int64)
    mesh.loop_triangles.foreach_get('polygon_index', tpoly)
    tc = comp[tpoly]
    a, b, c = co[tri[:, 0]], co[tri[:, 1]], co[tri[:, 2]]
    vol = np.einsum('ij,ij->i', a, np.cross(b, c)) / 6.0
    cen = (a + b + c) * vol[:, None] / 4.0
    v_part = np.bincount(tc, weights=vol, minlength=n)
    c_part = np.stack([np.bincount(tc, weights=cen[:, k], minlength=n) for k in range(3)], axis=1)
    out = []
    for k in range(n):
        pts = co[tri[tc == k].ravel()]
        if not len(pts):
            out.append((0.0, 0.0, Vector()))
            continue
        ext = float(((pts.max(axis=0) - pts.min(axis=0)) * scale).max())
        com = c_part[k] / v_part[k] if abs(v_part[k]) > 1e-15 else pts.mean(axis=0)
        out.append((abs(float(v_part[k])), ext, Vector(com)))
    return out


def _main_parts(source, mesh, comp, n, measures, source_geometry=None, factor=1.0):
    """The parts that stay in the result: the biggest part of each loose part
    of the source (a source made of several pieces keeps one body per piece)."""
    src_mesh = source.data
    if source_geometry is None:
        s_comp, s_n = _components(src_mesh)
    else:
        points, polys = source_geometry
        source_polys = polys.tolist()
        edges = ((poly[0], corner) for poly in source_polys for corner in poly[1:])
        s_comp, s_n = _face_components(len(points), polys[:, 0], edges)
    if s_n <= 1:
        return {int(np.argmax([m[0] for m in measures]))}
                                                                          
                                                                           
                                                                           
                                                                           
                                                                             
    from mathutils.bvhtree import BVHTree
    if source_geometry is None:
        vertices = [v.co for v in src_mesh.vertices]
        faces = [tuple(f.vertices) for f in src_mesh.polygons]
    else:
        vertices = points / factor
        faces = [tuple(face[:3] if face[2] == face[3] else face) for face in source_polys]
    tree = BVHTree.FromPolygons(vertices, faces)
    areas = np.empty(len(mesh.polygons), dtype=np.float64)
    centres = np.empty((len(mesh.polygons), 3), dtype=np.float64)
    mesh.polygons.foreach_get('area', areas)
    mesh.polygons.foreach_get('center', centres.ravel())
    from .effect_groups import _face_values
    base_faces = _face_values(mesh, 'surface_P', bool)
    order = np.argsort(comp, kind='stable')
    offsets = np.concatenate(([0], np.cumsum(np.bincount(comp, minlength=n))))
    best = {}
    for k in range(n):
        faces = order[offsets[k]:offsets[k + 1]]
        if base_faces is not None:
            traced = faces[base_faces[faces]]
            if len(traced):
                faces = traced
        weights = areas[faces]
        total = float(weights.sum())
        if not len(faces):
            continue
        if total <= 1e-20:
            weights = np.ones(len(faces), dtype=np.float64)
            total = float(len(faces))
        if len(faces) > 512:
            quantiles = (np.arange(512) + 0.5) * total / 512
            samples = np.searchsorted(np.cumsum(weights), quantiles)
            faces = faces[np.minimum(samples, len(faces) - 1)]
            weights = np.full(512, total / 512)
        votes = np.zeros(s_n, dtype=np.float64)
        for face, weight in zip(faces, weights):
            hit = tree.find_nearest(centres[face])
            if hit[2] is not None:
                votes[s_comp[hit[2]]] += weight
        group = int(np.argmax(votes))
        if group not in best or measures[k][0] > measures[best[group]][0]:
            best[group] = k
    return set(best.values())


def _fragment_metadata(source_mesh, mesh, split_valid):
    """Face/vertex data survive BMesh extraction; its mesh-level tags do not.
    Keep the exact rim provenance so inner cut faces stay with their body.
    """
    from . import effect_groups
    if split_valid and mesh.attributes.get(effect_groups.WELD) is not None:
        mesh[effect_groups.SPLIT_MARK] = len(mesh.vertices)
    elif effect_groups.SPLIT_MARK in mesh:
        del mesh[effect_groups.SPLIT_MARK]
    masks = dict(source_mesh.get(effect_groups.OWNED_FACES, {}))
    if masks:
        mesh[effect_groups.OWNED_FACES] = {
            key: name for key, name in masks.items() if mesh.attributes.get(name) is not None}
    active = source_mesh.uv_layers.active
    if active is not None and active.name in mesh.uv_layers:
        mesh.uv_layers.active = mesh.uv_layers[active.name]
    for layer in source_mesh.uv_layers:
        if layer.active_render and layer.name in mesh.uv_layers:
            mesh.uv_layers[layer.name].active_render = True


def _sub_mesh(bm_full, faces_keep_mask, name, materials):
    bm = bm_full.copy()
    bm.faces.ensure_lookup_table()
    drop = [f for f, keep in zip(bm.faces, faces_keep_mask) if not keep]
    if drop:
        bmesh.ops.delete(bm, geom=drop, context='FACES')
    loose = [v for v in bm.verts if not v.link_faces]
    if loose:
        bmesh.ops.delete(bm, geom=loose, context='VERTS')
    mesh = bpy.data.meshes.new(name)
    bm.to_mesh(mesh)
    bm.free()
    for m in materials:
        mesh.materials.append(m)
    return mesh


def _fragment_collection(context, source, res):
    uid = source.get('project_damage_uid')
    coll = next((c for c in bpy.data.collections if c.get(FRAGMENT_COLLECTION) == uid), None)
    if coll is None:
        coll = bpy.data.collections.new(source.name + '_Fragments')
        coll[FRAGMENT_COLLECTION] = uid
        parent = res.users_collection[0] if res.users_collection else context.scene.collection
        parent.children.link(coll)
    return coll


def _copy_shading(res, obj):
    for mod in [m for m in obj.modifiers if m.type == 'WEIGHTED_NORMAL']:
        obj.modifiers.remove(mod)
    wn = res.modifiers.get('Weighted Normal')
    if wn is not None and wn.type == 'WEIGHTED_NORMAL':
        new = obj.modifiers.new('Weighted Normal', 'WEIGHTED_NORMAL')
        for attr in ('mode', 'weight', 'thresh', 'keep_sharp', 'use_face_influence', 'vertex_group',
                     'invert_vertex_group', 'show_viewport', 'show_render'):
            if hasattr(wn, attr):
                try:
                    setattr(new, attr, getattr(wn, attr))
                except (AttributeError, TypeError):
                    pass


def wanted(source):
    return bool(_fragment_settings(source))


def _fragment_settings(source):
    from . import boolean_stack
    p = source.project_damage
    if p.all_off or not p.ui_boolean_enable:
        return []
    candidates = [props for _, props in boolean_stack.layers(source)] if boolean_stack.SERIAL in source else [p]
    if not any(item.obj is not None for props in candidates for item in props.breakers):
        return []
    return [props.break_separate_min for props in candidates if props.break_separate]


_split_plans = Results(limit=32 * 1024 * 1024)
_SPLIT_PLAN = 'project_damage_bfrag_plan'
_SPLIT_APPEARANCE = 'project_damage_bfrag_appearance'


def clear_split_plans():
    _split_plans.clear()


def _separation_plan(context, source, res, threshold, source_geometry):
    from . import effect_groups, incremental
    from .params import unit_factor, _clamp_scale

    mesh = res.data
    scale = np.linalg.norm(np.asarray(res.matrix_world.to_3x3()), axis=0)
    factor = unit_factor(context)
    limit = max(0.0, threshold) * _clamp_scale(source.project_damage.global_scale / 100.0) / factor
    if source_geometry is None:
        # Apply Selected stages a finished result, so read its prepared source here.
        from . import input_prep
        prepared = input_prep.plan(context, source, factor)
        if prepared is not None:
            prep = prepared[1]
            source_geometry = (prep.points, prep.polys)
    base_faces = effect_groups._face_values(mesh, 'surface_P', bool)
    inputs = (source_geometry if source_geometry is not None else
              (incremental.geometry_arrays(source.data), effect_groups._weld_ids(source.data)))
    key = _split_plans.key(__name__, '_separation_plan',
                           (incremental.geometry_arrays(mesh), effect_groups._weld_ids(mesh),
                            base_faces, inputs, scale, limit, factor), {})
    found, plan = _split_plans.get(key)
    if found:
        return key, plan

    comp, count = _components(mesh)
    plan = None
    if count > 1:
        measures = _part_measures(mesh, comp, count, scale)
        keep = _main_parts(source, mesh, comp, count, measures, source_geometry, factor)
        parts = [k for k in range(count) if k not in keep and measures[k][1] >= limit]
        if parts:
            parts.sort(key=lambda k: -measures[k][0])
            fragment_index = np.zeros(count, dtype=np.int32)
            fragment_index[parts] = np.arange(1, len(parts) + 1, dtype=np.int32)
            plan = fragment_index[comp], [tuple(measures[k][2]) for k in parts]
    _split_plans.put(key, plan)
    return key, plan


def after_build(context, source, res, reports=None, *, source_geometry=None):
    """build.rebuild_object calls this once the result is placed: the loose
    parts of the result that are not the main body become fragment objects."""
    if source is None or source.get(STAGE_KEY):
        return []
    thresholds = _fragment_settings(source)
    if res is None or res.type != 'MESH' or not thresholds:
        drop_fragments(source)
        return []
    uid = source.get('project_damage_uid')
    mesh = res.data
    from . import effect_groups
    cuts = effect_groups._face_values(mesh, 'break_P', bool)
    if cuts is None or not cuts.any():
        drop_fragments(source)
        return []
    split_valid = effect_groups._weld_ids(mesh) is not None
    key, plan = _separation_plan(context, source, res, min(thresholds), source_geometry)
    if plan is None:
        drop_fragments(source)
        return []
    from . import incremental
    labels, centres = plan
    plan_id = key.hex() if key is not None else ''
    groups = tuple(g.name for g in res.vertex_groups)
    weights = [(v.index, g.group, g.weight) for v in mesh.vertices for g in v.groups] if groups else ()
    appearance = format(incremental.digest((incremental.mesh_state(mesh), groups, weights)), 'x')
    bm = bmesh.new()
    bm.from_mesh(mesh)
    old = {obj.get('project_damage_bfrag_index', 0): obj for obj in fragments_of(source)}
    coll = _fragment_collection(context, source, res)
    materials = list(mesh.materials)
    out = []
    for j, center in enumerate(centres, 1):
        name = '%s_frag_%03d' % (source.name, j)
        com = Vector(center)
        obj = old.pop(j, None)
        same_shape = bool(plan_id and obj is not None and obj.get(_SPLIT_PLAN) == plan_id)
        same_mesh = same_shape and obj.get(_SPLIT_APPEARANCE) == appearance
        if not same_mesh:
            sub = _sub_mesh(bm, labels == j, name, materials)
            _fragment_metadata(mesh, sub, split_valid)
            sub.transform(Matrix.Translation(-com))
            sub.update()
            if obj is None:
                obj = bpy.data.objects.new(name, sub)
            else:
                previous = obj.data
                obj.data = sub
                if previous is not None and previous.users == 0:
                    bpy.data.meshes.remove(previous)
                obj.vertex_groups.clear()
            for g in groups:
                obj.vertex_groups.new(name=g)
        if obj.name not in coll.objects:
            for c in list(obj.users_collection):
                c.objects.unlink(obj)
            coll.objects.link(obj)
        obj.name = name
        obj.data.name = obj.name
        for key in (effect_groups.OWNED, 'project_damage_damage_materials'):
            if key in res:
                obj[key] = dict(res[key])
        if not same_shape:
            obj.parent = None
            obj.matrix_world = res.matrix_world @ Matrix.Translation(com)
        obj[FRAGMENT_STAMP] = uid
        obj[_SPLIT_PLAN] = plan_id
        obj[_SPLIT_APPEARANCE] = appearance
        obj['project_damage_fragment_of'] = res.name                                                      
        obj['project_damage_bfrag_index'] = j
        obj['project_damage_bfrag_com'] = list(com)
        if not same_shape:
            obj['project_damage_bfrag_rest']=[float(v) for row in (source.matrix_world.inverted_safe()@obj.matrix_world) for v in row]
        ownership.bind(source, obj)
        obj.hide_render = res.hide_render
        if not same_mesh:
            _copy_shading(res, obj)
        out.append(obj)
    for obj in old.values():
        _remove(obj)
                                                                   
    stay = labels == 0
    bm.faces.ensure_lookup_table()
    drop = [f for f, s in zip(bm.faces, stay) if not s]
    bmesh.ops.delete(bm, geom=drop, context='FACES')
    loose = [v for v in bm.verts if not v.link_faces]
    if loose:
        bmesh.ops.delete(bm, geom=loose, context='VERTS')
    bm.to_mesh(mesh)
    bm.free()
    mesh.update()
    _fragment_metadata(mesh, mesh, split_valid)
    if reports is not None:
        reports.append(({'INFO'}, 'Boolean Break: %d separate fragments in %s' % (len(out), coll.name)))
    return out


def follow_source(source, *, timeline=False):
    """A transform-only move of the source carries its fragments along."""
    from . import pipes
    pipes.follow_source(source)
    from . import cables
    cables.follow_source(source)
    for obj in fragments_of(source):
        if timeline and (obj.rigid_body is not None or obj.animation_data is not None or obj.constraints):
            continue
        com = obj.get('project_damage_bfrag_com')
        if com is not None and len(com) == 3:
            matrix = source.matrix_world @ Matrix.Translation(Vector(com))
            if obj.matrix_world != matrix:
                obj.matrix_world = matrix


def after_apply_selected(context, source):
    """Apply Selected bakes the effects one at a time on a staging copy that
    never splits; split the final result once it is committed."""
    from . import build
    res = build.result_of(source)
    if res is not None:
        after_build(context, source, res)


def draw_fragments(layout, src, physics=False):
    from . import effect_layout
    p = src.project_damage
    raw = _raw(layout)
    box = raw if physics else effect_layout.section(raw, 'Separate Fragments')
    row = box.row(align=True)
    row.scale_y = 1.5 if not physics else 1.3
    row.prop(p, 'break_separate', text='Separate Fragments', toggle=True, icon='MOD_EXPLODE')
    if p.break_separate:
        label={'PIPES':'Pipes','CABLES':'Cables','REBAR':'Bar','CENTRAL':'Central Bar','FRAMING':'Start Frames','BRICKS':'Bricks','PLANKS':'Planks','CHIPS':'Chips'}.get(variant(p),'Structure')
        box.operator('project_damage.boolean_restructure',text='Restructure '+label,icon='FILE_REFRESH')
        from . import damage_physics
        if not physics:damage_physics.draw_fragment_fracture(box,src)
        if variant(p)=='FRAMING':
            settings=box.box();row=settings.row();row.alignment='CENTER'
            row.label(text='Framing Breakage',icon='MOD_BUILD')
            col=settings.column(align=True)
            col.prop(p,'frame_restructure_angle',text='Break Angle (degrees)')
            col.prop(p,'frame_restructure_minimum')
            col.prop(p,'frame_restructure_collisions',toggle=True,icon='MOD_BOOLEAN')
            from .rebar import rebar_of
            from . import boolean_stack
            entry=boolean_stack.edited(src)
            output=rebar_of(src, boolean_stack.OUTPUT_OFFSET + entry[boolean_stack.INSTANCE] if entry else 0)
            if output is not None and 'pd_framing_broken_members' in output:
                settings.label(text='%d broken members / %d protected connections' %
                    (output['pd_framing_broken_members'], output['pd_framing_kept_connections']),icon='INFO')


        box.prop(p, 'break_separate_min')
        n = len(fragments_of(src))
        box.label(text=('%d fragment objects' % n) if n else 'Loose parts become objects on the next update',
                  icon='INFO')


def draw_physics(box, p):
    src = p.id_data
    draw_fragments(box, src, physics=True)
    if variant(p) != 'SIMPLE':
        box.label(text='Fills are cut around the %s' % FILL_TITLES[variant(p)].lower(), icon='INFO')


class PROJECT_DAMAGE_MT_bricks_surface_presets(bpy.types.Menu):
    bl_label = 'Break Surface Presets'

    def draw(self, context):
        from . import break_surface
        for key, (label, hint, values) in break_surface.STYLES.items():
            self.layout.operator('project_damage.break_surface_style', text=label).style = key


_CLASSES = (PROJECT_DAMAGE_MT_bricks_surface_presets, PROJECT_DAMAGE_OT_boolean_variant_add, PROJECT_DAMAGE_OT_boolean_pick, PROJECT_DAMAGE_OT_boolean_primitive,
            PROJECT_DAMAGE_MT_boolean_primitives, PROJECT_DAMAGE_OT_boolean_delete,
            PROJECT_DAMAGE_OT_boolean_objects_new)


@bpy.app.handlers.persistent
def _upgrade_primitive_materials(*args):
    if not hasattr(bpy.data,'objects'):return .2
    previous=live._APPLYING;live._APPLYING=True
    try:
        for obj in bpy.data.objects:
            if obj.library is None and obj.get(PRIMITIVE_KEY):primitive_material(obj)
    finally:live._APPLYING=previous
    return None


# подключаем свойства и действия этого модуля к Blender
def register():
    if _upgrade_primitive_materials not in bpy.app.handlers.load_post:bpy.app.handlers.load_post.append(_upgrade_primitive_materials)
    if not bpy.app.timers.is_registered(_upgrade_primitive_materials):bpy.app.timers.register(_upgrade_primitive_materials,first_interval=.2)
    from . import boolean_slicers
    boolean_slicers.register()
    from . import boolean_auto
    boolean_auto.register()
    from . import boolean_restructure
    boolean_restructure.register()
    from . import asset_updates
    asset_updates.register()
    from . import boolean_ui_controls
    boolean_ui_controls.register()
                                                                            
    bpy.types.WindowManager.project_damage_bricks_noise_open = bpy.props.BoolProperty(default=False)
    bpy.types.WindowManager.project_damage_bricks_edge_open = bpy.props.BoolProperty(default=False)
    for cls in _CLASSES:
        bpy.utils.register_class(cls)
    from . import pipes
    pipes.register()
    from . import cables
    cables.register()
    if FRAGMENT_STAMP not in ownership.LOOSE_STAMPS:
        ownership.LOOSE_STAMPS = ownership.LOOSE_STAMPS + (FRAGMENT_STAMP,)
    for handlers in (bpy.app.handlers.depsgraph_update_post, bpy.app.handlers.load_post):
        if _sync_all not in handlers:
            handlers.append(_sync_all)


# убираем подключения модуля при выключении аддона
def unregister():
    if _upgrade_primitive_materials in bpy.app.handlers.load_post:bpy.app.handlers.load_post.remove(_upgrade_primitive_materials)
    if bpy.app.timers.is_registered(_upgrade_primitive_materials):bpy.app.timers.unregister(_upgrade_primitive_materials)
    from . import boolean_slicers
    boolean_slicers.unregister()
    from . import boolean_auto
    boolean_auto.unregister()
    from . import boolean_restructure
    boolean_restructure.unregister()
    from . import asset_updates
    asset_updates.unregister()
    from . import boolean_ui_controls
    boolean_ui_controls.unregister()
    for name in ('project_damage_bricks_noise_open', 'project_damage_bricks_edge_open'):
        if hasattr(bpy.types.WindowManager,name):delattr(bpy.types.WindowManager,name)
    from . import cables
    cables.unregister()
    from . import pipes
    pipes.unregister()
    for handlers in (bpy.app.handlers.depsgraph_update_post, bpy.app.handlers.load_post):
        if _sync_all in handlers:
            handlers.remove(_sync_all)
    for cls in reversed(_CLASSES):
        try:
            bpy.utils.unregister_class(cls)
        except RuntimeError:
            pass
