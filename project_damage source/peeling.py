# управление отслоением покрытия, внутренним слоем и рисованием
from . import quality_ui
from . import preview_quality
# SPDX-License-Identifier: GPL-3.0-or-later
"""Attached peeling: cut a scalar contour in the existing skin, bend its rim,
then connect that rim through a thin underside to a recessed substrate.

The whole build runs in the native module (core/source/source/peeling_full.cpp).
This file holds what belongs to Blender: properties, panel, materials and the
translation of settings. peeling_native.py moves the mesh in and out as arrays.

No flakes are scattered and no objects are spawned. Every original connected
component remains connected. All dimensions are fractions of Patch Size (which
follows Master Scale).
"""
from .blender_compat import enable_nodes
import random
import bpy
from mathutils import Vector
from . import live

STAMP = 33
P = bpy.props

def _enable_changed(self, context):
    if self.peeling_enable:
        if ('_peeling_resolution_version' not in self and 'peeling_detail' not in self
                and 'peeling_resolution' not in self and not self.is_property_set('peeling_wrap_edges')):
            self['peeling_wrap_edges'] = True
        self['_peeling_resolution_version'] = 1
    live.touch_prop(self, context)


def _resolution_get(self):
    value=self.get('peeling_resolution')
    if value is not None:return float(value)
    legacy=self.get('peeling_detail')
    return float(legacy)*100./24. if legacy is not None else 50.

def _resolution_set(self,value):
    self['peeling_resolution']=float(value)

def resolution(p):
    if hasattr(p,'peeling_resolution'):return p.peeling_resolution
    return float(getattr(p,'peeling_detail',12))*100./24.

PROPS = {
    'peeling_mode': P.EnumProperty(name='Placement',items=[('PROCEDURAL','Procedural','Generate peeling patches'),('PAINT','Draw','Use Grease Pencil contours or strokes')],default='PROCEDURAL',update=live.touch_prop),
    'peeling_drawing': P.PointerProperty(name='Drawing',type=bpy.types.Object,
        poll=lambda self,obj:obj.type in {'GREASEPENCIL','GPENCIL','CURVE'},update=live.touch_prop,
        description='Grease Pencil or Curve drawing in object space; current frame and visible layers are used'),
    'peeling_draw_shape': P.EnumProperty(name='Draw Shape',items=[('AREAS','Areas','Peel inside closed contours'),('STROKES','Strokes','Peel a strip along open strokes')],default='AREAS',update=live.touch_prop),
    'peeling_close_strokes': P.BoolProperty(name='Close Contours',default=True,update=live.touch_prop,
        description='Join the first and last point when drawing areas'),
    'peeling_stroke_width': P.FloatProperty(name='Stroke Width (cm)',default=8,min=.1,soft_max=100,update=live.touch_prop,
        description='Width of peeling along open strokes; follows Master Scale'),
    'peeling_paint_snap': P.FloatProperty(name='Projection Distance (cm)',default=5,min=.01,soft_max=50,update=live.touch_prop,
        description='Maximum drawing distance from the source surface; reverse-facing surfaces remain protected'),
    'peeling_adaptive': P.BoolProperty(name='Adaptive Topology',default=True,update=live.touch_prop,
        description='Refine only near peeled areas and their bending zone; restore untouched original polygons. Disable for uniform refinement'),
    'peeling_enable': P.BoolProperty(name='Peeling', default=False, update=_enable_changed,
        description='Lift the existing coating into attached curled edges, revealing a recessed inner layer'),
    'peeling_seed': P.IntProperty(name='Seed', default=0, min=0, max=100000, update=live.touch_prop,
        description='Choose a repeatable layout of worn patches'),
    'peeling_size': P.FloatProperty(name='Patch Size (cm)', default=35, min=.1, soft_max=200, update=live.touch_prop,
        description='Typical size of the peeling patches. Follows Master Scale; smaller patches need a finer mesh'),
    'peeling_amount': P.FloatProperty(name='Exposed Area', default=42, min=0, max=85, subtype='PERCENTAGE', update=live.touch_prop,
        description='Controls the noise threshold: higher values expose more substrate. Zero leaves the mesh unchanged'),
    'peeling_small_damage_amount': P.FloatProperty(name='Small Damage Amount', default=0, min=0, max=100, subtype='PERCENTAGE', update=live.touch_prop,
        description='Add small attached peeling holes near the main damage rims. More adds extra holes without moving existing ones. Zero disables secondary damage; works with Procedural and Draw'),
    'peeling_ragged': P.FloatProperty(name='Ragged Edges', default=55, min=0, max=100, subtype='PERCENTAGE', update=live.touch_prop,
        description='Adds small irregularities to the patch boundary. Increase Resolution to resolve the smallest chips'),
    'peeling_lift': P.FloatProperty(name='Edge Lift', default=5, min=0, max=20, subtype='PERCENTAGE', update=live.touch_prop,
        description='Height of the raised edge as a percentage of Patch Size. The bend fades smoothly into the original coating'),
    'peeling_bend': P.FloatProperty(name='Bend Width', default=18, min=5, max=40, subtype='PERCENTAGE', update=live.touch_prop,
        description='Width of the attached bending zone, relative to Patch Size. Larger values make broad, gentle curls'),
    'peeling_variation': P.FloatProperty(name='Curl Variation', default=70, min=0, max=100, subtype='PERCENTAGE', update=live.touch_prop,
        description='Varies lift around the rim, creating individual raised tongues that remain attached to the coating'),
    'peeling_depth': P.FloatProperty(name='Inner Layer Depth', default=1.2, min=.1, max=8, subtype='PERCENTAGE', update=live.touch_prop,
        description='Recess below the original surface, as a percentage of Patch Size. Automatically capped to 40% of detected local wall thickness'),
    'peeling_thickness': P.FloatProperty(name='Coating Thickness', default=.15, min=.02, max=1, subtype='PERCENTAGE', update=live.touch_prop,
        description='Thickness visible at the free lip, as a percentage of Patch Size; limited to the inner-layer depth'),
    'peeling_detail': P.IntProperty(name='Detail', default=24, min=8, max=64, update=live.touch_prop,
        description='Target mesh edges per Patch Size. Higher values give smoother curls and finer torn contours'),
    'peeling_resolution': P.FloatProperty(name='Resolution', default=50., min=1., max=400., soft_max=200., subtype='PERCENTAGE', get=_resolution_get, set=_resolution_set, update=live.touch_prop,
        description='Mesh density near peeling and curls. 50% is the fast starting value; 100% matches the former Detail 24. Low values simplify contours; high values resolve smaller tears and smoother curls, with more time and memory. Face Budget may limit refinement'),
    'peeling_face_budget': P.IntProperty(name='Face Budget', default=300000, min=10000, max=2000000, update=live.touch_prop,
        description='Safety limit for refinement. If reached, the effect uses the available mesh and reports reduced detail'),
    'peeling_group': P.StringProperty(name='Peeling Mask', default='', update=live.touch_prop,
        description='Optional vertex group on the original object. Weight zero protects the coating; weight one permits peeling'),
    'peeling_material': P.PointerProperty(name='Inner Layer', type=bpy.types.Material, update=live.touch_prop,
        description='Material for the exposed substrate. Empty creates Peeling Substrate | Project Damage'),
    'peeling_back_material': P.PointerProperty(name='Coating Underside', type=bpy.types.Material, update=live.touch_prop,
        description='Optional material on the back of the curled skin. Empty inherits the neighbouring original coating'),
    # ---- v2 (curl) additions; defaults chosen to match the reference look.
    'peeling_curl': P.FloatProperty(name='Curl', default=55, min=0, max=100, subtype='PERCENTAGE', update=live.touch_prop,
        description='How the lifted coating bends. Low: stiff plates lifted in a gentle arc. High: the lip rolls up near its free edge'),
    'peeling_plates': P.FloatProperty(name='Crack Plates', default=70, min=0, max=100, subtype='PERCENTAGE', update=live.touch_prop,
        description='Breaks the rim along a network of paint cracks: angular, plate-like flakes. Zero gives smooth torn outlines'),
    'peeling_tongues': P.FloatProperty(name='Tongues', default=40, min=0, max=100, subtype='PERCENTAGE', update=live.touch_prop,
        description='Occasional long, narrow lifted tongues along the rim. Zero keeps an even bend width'),
    'peeling_tears': P.FloatProperty(name='Tears', default=35, min=0, max=100, subtype='PERCENTAGE', update=live.touch_prop,
        description='Short torn slits from the rim into the coating that split it into separate attached flaps. More adds tears without moving existing ones'),
    'peeling_droop': P.FloatProperty(name='Gravity Droop', default=0, min=0, max=100, subtype='PERCENTAGE', update=live.touch_prop,
        description='Flaps hanging from the upper rim of a patch lift further, flaps on the lower rim lie flatter (object -Z is down)'),
    'peeling_wallpaper': P.BoolProperty(name='Wallpaper Peel', default=False, update=live.touch_prop,
        description='Paint or skim coat over wallpaper: the peeled coating reveals a paper layer, which is torn through to rough plaster in places'),
    'peeling_wallpaper_material': P.PointerProperty(name='Wallpaper', type=bpy.types.Material, update=live.touch_prop,
        description='Material of the exposed paper layer (uses the source UVs). Empty creates Peeling Wallpaper | Project Damage'),
    'peeling_paper_torn': P.FloatProperty(name='Torn Paper', default=45, min=0, max=85, subtype='PERCENTAGE', update=live.touch_prop,
        description='How much of the exposed paper is torn away down to the plaster'),
    'peeling_wrap_edges': P.BoolProperty(name='Wrap Across Edges',default=True,update=live.touch_prop,
        description='Let patches, cuts and flaps continue over sharp edges and corners, folding around them. Off: peeling stops short of sharp edges, which stay intact. Enabled by default; turn it off to preserve sharp edges'),
    'peeling_damage_factor': P.FloatProperty(name='Damage Factor',default=1.,min=0.,max=1.,subtype='FACTOR',update=live.touch_prop,
        description='How far the damage has gone. Low: only a network of thin cracks with slightly raised plate edges, no paint lost. Middle: some plates curl more and a few flake off. 1: full peeling'),
    'peeling_plaster_depth': P.FloatProperty(name='Plaster Depth', default=2.5, min=.2, max=10, subtype='PERCENTAGE', update=live.touch_prop,
        description='Depth of the broken plaster under torn paper, below the paper, as a percentage of Patch Size; capped by wall thickness'),
}


def wanted(p):
    return p.peeling_enable and not p.all_off and (p.peeling_mode=='PAINT' or p.peeling_amount > 0)


def default_material():
    name = 'Peeling Substrate | Project Damage'
    mat = bpy.data.materials.get(name)
    if mat is not None:
        return mat
    mat = bpy.data.materials.new(name)
    mat.diffuse_color = (.32,.23,.13,1)
    enable_nodes(mat)
    nodes = mat.node_tree.nodes
    shader = nodes.get('Principled BSDF')
    shader.inputs['Base Color'].default_value = mat.diffuse_color
    shader.inputs['Roughness'].default_value = .86
    tex = nodes.new('ShaderNodeTexNoise');tex.inputs['Scale'].default_value = 180
    tex.inputs['Detail'].default_value = 2
    bump = nodes.new('ShaderNodeBump');bump.inputs['Strength'].default_value = .22
    bump.inputs['Distance'].default_value = .002
    tex.location=(-480,40);bump.location=(-240,40)
    mat.node_tree.links.new(tex.outputs['Fac'],bump.inputs['Height'])
    mat.node_tree.links.new(bump.outputs['Normal'],shader.inputs['Normal'])
    return mat


def _slot(mesh, mat):
    for i, existing in enumerate(mesh.materials):
        if existing == mat:return i
    mesh.materials.append(mat)
    return len(mesh.materials)-1


def _offsets(seed, salt, count):
    rng = random.Random(seed*7919+salt)
    return [(rng.uniform(-90, 90), rng.uniform(-90, 90), rng.uniform(-90, 90)) for _ in range(count)]


# Calibrated so Exposed Area keeps roughly the v1 meaning (see NOTES).
_THRESHOLD_SCALE = 1.0


def default_wallpaper_material():
    name = 'Peeling Wallpaper | Project Damage'
    mat = bpy.data.materials.get(name)
    if mat is not None:
        return mat
    mat = bpy.data.materials.new(name)
    mat.diffuse_color = (.78, .66, .52, 1)
    enable_nodes(mat)
    nodes = mat.node_tree.nodes; links = mat.node_tree.links
    shader = nodes.get('Principled BSDF')
    shader.inputs['Roughness'].default_value = .8
    coord = nodes.new('ShaderNodeTexCoord'); coord.location = (-900, 0)
    pattern = nodes.new('ShaderNodeTexVoronoi'); pattern.location = (-680, 0)
    pattern.inputs['Scale'].default_value = 60
    ramp = nodes.new('ShaderNodeValToRGB'); ramp.location = (-460, 0)
    ramp.color_ramp.elements[0].position = .18
    ramp.color_ramp.elements[0].color = (.42, .26, .18, 1)
    ramp.color_ramp.elements[1].position = .34
    ramp.color_ramp.elements[1].color = mat.diffuse_color
    links.new(coord.outputs['Object'], pattern.inputs['Vector'])
    links.new(pattern.outputs['Distance'], ramp.inputs['Fac'])
    links.new(ramp.outputs['Color'], shader.inputs['Base Color'])
    return mat


def default_paper_back_material():
    name = 'Peeling Paper Back | Project Damage'
    mat = bpy.data.materials.get(name)
    if mat is None:
        mat = bpy.data.materials.new(name)
        mat.diffuse_color = (.86, .82, .74, 1)
        enable_nodes(mat)
        shader = mat.node_tree.nodes.get('Principled BSDF')
        shader.inputs['Base Color'].default_value = mat.diffuse_color
        shader.inputs['Roughness'].default_value = .9
    return mat


def default_crack_material():
    name='Peeling Crack Shadow | Project Damage'
    mat=bpy.data.materials.get(name)
    if mat is None:
        mat=bpy.data.materials.new(name)
        mat.diffuse_color=(.02,.018,.015,1)
        enable_nodes(mat)
        shader=mat.node_tree.nodes.get('Principled BSDF')
        shader.inputs['Base Color'].default_value=mat.diffuse_color
        shader.inputs['Roughness'].default_value=.95
    return mat


def _coating_material():
    name='Peeling Coating | Project Damage'
    mat=bpy.data.materials.get(name)
    if mat is None:
        mat=bpy.data.materials.new(name);enable_nodes(mat)
        mat.diffuse_color=(.8,.8,.8,1)
        shader=mat.node_tree.nodes.get('Principled BSDF')
        shader.inputs['Base Color'].default_value=mat.diffuse_color
        shader.inputs['Roughness'].default_value=.75
    return mat


def generate(mesh, p, factor=100., group_index=None, paths=None, gravity=None, *, vertex_groups=None):
    """Edit a private result mesh transactionally. No bpy operators/selection changes.

    vertex_groups: whether the mesh may carry deform weights (None = look at it).
    """
    if not wanted(p) or not mesh.polygons:return {'faces':0}
    from . import peeling_native
    size = p.peeling_size * max(.001,p.global_scale/100.) / max(1e-8,factor)
    detail = max(2.,24.*resolution(p)/100.*max(.05,preview_quality.percent(p)/100.))
    step=size/detail
    bend=size*p.peeling_bend/100.
    depth=size*p.peeling_depth/100.
    thickness=min(depth*.5,size*p.peeling_thickness/100.)
    lift=size*p.peeling_lift/100.
    offset=Vector(tuple(random.Random(p.peeling_seed+7919+i*31).uniform(-80,80) for i in range(3)))
    ragged=p.peeling_ragged/100.
    # PERLIN_ORIGINAL is centred on zero. Amount is a visual threshold, not
    # an exact area percentage (surface orientation changes its distribution).
    threshold=(p.peeling_amount/100.-.5)*.95*_THRESHOLD_SCALE
    wallpaper=bool(getattr(p,'peeling_wallpaper',False))
    curl=getattr(p,'peeling_curl',55.)/100.
    tongues=getattr(p,'peeling_tongues',40.)/100.
    tears=getattr(p,'peeling_tears',35.)
    droop=getattr(p,'peeling_droop',0.)/100.
    plates=getattr(p,'peeling_plates',70.)/100.
    variation=p.peeling_variation/100.
    wrap_edges=bool(getattr(p,'peeling_wrap_edges',True))
    drawing=None
    if p.peeling_mode=='PAINT':
        drawing=peeling_native.drawing_input(paths or [],p,factor)
        if drawing is None:return {'faces':0,'drawing_empty':True}
    if not mesh.materials:
        # Do not let the first substrate slot also colour an unassigned
        # original skin (whose polygon material index is necessarily zero).
        mesh.materials.append(_coating_material())
    inner=_slot(mesh,p.peeling_material or default_material())
    back=_slot(mesh,p.peeling_back_material) if p.peeling_back_material else -1
    paper_mat=_slot(mesh,getattr(p,'peeling_wallpaper_material',None) or default_wallpaper_material()) if wallpaper else inner
    damage=float(getattr(p,'peeling_damage_factor',1.))
    crack=_slot(mesh,default_crack_material()) if damage<.999 else -1
    main=dict(size=size,detail=detail,step=step,bend=bend,depth=depth,thickness=thickness,lift=lift,
              variation=variation,curl=curl*(.6 if wallpaper else 1.),tongues=tongues,tears=tears,
              droop=max(droop,.45) if wallpaper else droop,small_amount=p.peeling_small_damage_amount,
              ragged=ragged,threshold=threshold,plates=plates,rough=0.,offset=tuple(offset),
              O=_offsets(p.peeling_seed,0,12),rim_offset=_offsets(p.peeling_seed,11,3),
              floor_mat=paper_mat,back_mat=back,bridge_mat=paper_mat,mark_paper=wallpaper,edge_all=False,paper_only=False,
              use_drawing=drawing is not None,use_group=group_index is not None,wrap=wrap_edges,damage=damage,crack_mat=crack)
    paper=None
    if wallpaper:
        # Paper layer: smaller, independent torn holes inside the exposed
        # paper, down to rough plaster; thin, curlier paper flaps.
        psize=size*.6
        torn=getattr(p,'peeling_paper_torn',45.)/100.
        pdepth=size*getattr(p,'peeling_plaster_depth',2.5)/100.
        paper=dict(size=psize,detail=detail/.6,step=step,bend=bend*.55,depth=pdepth,thickness=min(pdepth*.5,thickness*.35),
                   lift=lift*.7,variation=variation,curl=min(1.,curl+.3),tongues=tongues,tears=tears*.6,droop=max(droop,.6),small_amount=0.,
                   ragged=ragged,threshold=(torn-.5)*.95*_THRESHOLD_SCALE,plates=plates*.5,rough=.45,
                   offset=tuple(offset+Vector((31.7,-12.3,7.9))),O=_offsets(p.peeling_seed,101,12),rim_offset=_offsets(p.peeling_seed,13,3),
                   floor_mat=inner,back_mat=_slot(mesh,default_paper_back_material()),bridge_mat=inner,mark_paper=False,edge_all=True,paper_only=True,
                   use_drawing=False,use_group=False,wrap=wrap_edges,damage=1.,crack_mat=-1)
    return peeling_native.build(mesh,main,paper,drawing,group_index,tuple(gravity) if gravity is not None else (0.,0.,-1.),
                                p.peeling_adaptive,p.peeling_seed,p.peeling_face_budget,vertex_groups)


def after_build(context, source, result, reports):
    p=source.project_damage
    if not wanted(p):return
    from .params import unit_factor
    group=result.vertex_groups.get(p.peeling_group) if p.peeling_group else None
    if p.peeling_group and group is None:
        reports.append(({'WARNING'},'Peeling: mask group not found; effect skipped'))
        return
    # Work on a copy: an invalid contour/budget failure cannot corrupt the last
    # valid output. Do not modify the original mesh or the user's active object.
    old=result.data;mesh=old.copy()
    try:
        from . import paint
        paths=paint.read_paths(context,source,p.peeling_drawing) if p.peeling_mode=='PAINT' else None
        down=result.matrix_world.inverted_safe().to_3x3()@Vector((0.,0.,-1.))
        stats=generate(mesh,p,unit_factor(context),group.index if group else None,paths,
                       tuple(down.normalized()) if down.length>1e-12 else None,
                       vertex_groups=bool(result.vertex_groups))
        if not stats['faces']:
            bpy.data.meshes.remove(mesh)
            reports.append(({'INFO'},'Peeling: draw on the surface or increase Projection Distance' if p.peeling_mode=='PAINT' else 'Peeling: no patches at these settings; increase Exposed Area or reduce Patch Size'))
            return
        result.data=mesh
        if old.users==0:bpy.data.meshes.remove(old)
        reports.append(({'INFO'},'Peeling: %d attached faces, %d rim edges%s' % (stats['faces'],stats['contour_edges'],
            '; quality budget reached, detail reduced' if stats['limited'] else '')))
    except Exception as exc:
        import traceback;traceback.print_exc()
        if mesh.users==0:bpy.data.meshes.remove(mesh)
        reports.append(({'ERROR'},'Peeling: '+str(exc)))


# показываем настройки выбранного эффекта
def draw_settings(layout, context):
    from . import ownership, effect_layout, material_controls
    source=ownership.context_source(context)
    if source is None:return
    p=source.project_damage
    layout.use_property_split=False;layout.use_property_decorate=False
    row=layout.row(align=True);row.scale_y=1.5
    row.prop(p,'peeling_mode',expand=True)
    if p.peeling_mode=='PAINT':
        box=effect_layout.section(layout,'Drawing')
        row=box.row(align=True);row.scale_y=1.5
        row.prop(p,'peeling_draw_shape',expand=True)
        row=box.row(align=True);row.prop(p,'peeling_drawing',text='Drawing')
        button=row.operator('project_damage.paint_delete',text='',icon='TRASH');button.target='PEELING'
        row=box.row(align=True);row.scale_y=1.5
        button=row.operator('project_damage.paint_draw' if p.peeling_drawing else 'project_damage.paint_new',
                            text='Draw / Edit' if p.peeling_drawing else 'New Drawing',icon='GREASEPENCIL')
        button.target='PEELING'
        row.operator('project_damage.peeling_update',text='Update',icon='FILE_REFRESH')
        if p.peeling_draw_shape=='AREAS':box.prop(p,'peeling_close_strokes',text='Close Contours')
        else:box.prop(p,'peeling_stroke_width',text='Stroke Width (cm)')
        box.prop(p,'peeling_paint_snap',text='Projection Distance (cm)')
    box=effect_layout.section(layout,'Peeling Pattern')
    col=box.column(align=True)
    col.prop(p,'peeling_size',text='Patch Size (cm)')
    if p.peeling_mode=='PROCEDURAL':col.prop(p,'peeling_amount',text='Exposed Area')
    col.prop(p,'peeling_damage_factor',text='Damage Factor',slider=True)
    col.prop(p,'peeling_small_damage_amount',text='Small Damage Amount')
    col.prop(p,'peeling_ragged',text='Ragged Edges')
    col.prop(p,'peeling_plates',text='Crack Plates')
    col.prop(p,'peeling_seed',text='Seed')
    box=effect_layout.section(layout,'Attached Coating')
    material_controls.inline(box,p,'peeling_back_material',text='Underside')
    col=box.column(align=True)
    col.prop(p,'peeling_lift',text='Edge Lift')
    col.prop(p,'peeling_bend',text='Bend Width')
    col.prop(p,'peeling_curl',text='Curl')
    col.prop(p,'peeling_variation',text='Curl Variation')
    col.prop(p,'peeling_tongues',text='Tongues')
    col.prop(p,'peeling_tears',text='Tears')
    col.prop(p,'peeling_droop',text='Gravity Droop')
    col.prop(p,'peeling_wrap_edges',text='Wrap Across Edges',toggle=True)
    col.prop(p,'peeling_thickness',text='Coating Thickness')
    box=effect_layout.section(layout,'Inner Layer')
    material_controls.inline(box,p,'peeling_material',text='Exposed Substrate')
    box.prop(p,'peeling_depth',text='Depth')
    box=effect_layout.section(layout,'Wallpaper Peel')
    box.prop(p,'peeling_wallpaper',text='Wallpaper Peel',toggle=True,icon='TEXTURE')
    if p.peeling_wallpaper:
        material_controls.inline(box,p,'peeling_wallpaper_material',text='Wallpaper')
        col=box.column(align=True)
        col.prop(p,'peeling_paper_torn',text='Torn Paper')
        col.prop(p,'peeling_plaster_depth',text='Plaster Depth')
    box=effect_layout.section(layout,'Quality & Mask')
    col=box.column(align=True)
    col.prop(p,'peeling_adaptive',text='Adaptive Topology',toggle=True,icon='MOD_SUBSURF')
    quality_ui.draw(col, p,'peeling_resolution',text='Resolution')
    col.prop(p,'peeling_face_budget',text='Face Budget')
    row=col.row(align=True)
    row.prop_search(p,'peeling_group',source,'vertex_groups',text='Mask')
    button=row.operator('project_damage.erosion_mask_group',text='',icon='ADD')
    button.effect='PEELING';button.action='NEW'
    if context.mode=='EDIT_MESH' and context.active_object==source:
        row=col.row(align=True)
        for action,text,icon in (('ASSIGN','Assign Selection','CHECKMARK'),('REMOVE','Remove Selection','REMOVE')):
            button=row.operator('project_damage.erosion_mask_group',text=text,icon=icon)
            button.effect='PEELING';button.action=action
        button=col.operator('project_damage.erosion_mask_group',text='Finish Mask',icon='CHECKMARK')
        button.effect='PEELING';button.action='FINISH'
    elif source.vertex_groups.get(p.peeling_group) is not None:
        button=col.operator('project_damage.erosion_mask_group',text='Edit Mask Vertices',icon='EDITMODE_HLT')
        button.effect='PEELING';button.action='EDIT'
