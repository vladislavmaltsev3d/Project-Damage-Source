# кирпичная кладка на плоскости: те же размеры и материалы, без поиска стены за рисунком
import math
import numpy as np
import bpy
from mathutils import Vector

FIELDS={
 'brick_bond':'bond','brick_brick_length':'brickLength','brick_brick_height':'brickHeight',
 'brick_brick_width':'brickWidth','brick_brick_scale':'brickScale','brick_mortar':'mortar',
 'brick_mortar_depth':'mortarDepth','brick_bond_offset':'bondOffset',
 'brick_face_jitter':'faceJitter','brick_face_tilt':'faceTilt','brick_brick_rough':'brickRough',
 'brick_chip':'chip','brick_chip_size':'chipSize','brick_joint_rough':'jointRough',
 'brick_break_size':'breakSize','brick_break_jagged':'breakJagged','brick_missing':'missing',
 'brick_broken':'broken','brick_broken_rough':'brokenRough','brick_loose':'loose',
 'brick_loose_depth':'looseDepth','brick_loose_tilt':'looseTilt','brick_loose_out':'looseOut',
 'brick_loose_out_dist':'looseOutDist'}
MATERIALS=('material','brick_seam_material','brick_break_material')


def add_properties(cls,changed,material_changed):
    from .props import ProjectDamageProps
    for name in (*FIELDS,'brick_seam_material','brick_break_material'):
        original=ProjectDamageProps.__annotations__[name]
        args=dict(original.keywords);args['update']=material_changed if name in MATERIALS else changed
        descriptions={
            'brick_broken':'Remove additional whole bricks near drawn damage. Zero keeps all unselected neighbours; one removes every eligible neighbour. Their mortar is removed too',
            'brick_broken_rough':'Roughen surviving bricks next to damage. Zero keeps their normal surface profile; higher values make exposed edges more irregular',
            'brick_break_size':'Expand the set of whole bricks selected by damage strokes. Larger values reach farther into adjacent rows; bricks are never sliced by a stroke',
        }
        if name in descriptions:args['description']=descriptions[name]
        if name=='brick_face_jitter':args['default']=1.62
        cls.__annotations__[name]=original.function(**args)
    cls.__annotations__.update(
        brick_joint_fill=bpy.props.BoolProperty(name='Mortar',default=True,update=changed,description='Fill joints between surviving bricks. Off leaves separate bricks; mortar is removed around missing, broken and loose bricks'),
        brick_boundary_offset=bpy.props.FloatProperty(name='Boundary Offset (cm)',default=0,min=-25,max=50,update=changed,description='Extend the outer contour beyond the control plane. Zero clips to its border; positive values extend the border; negative values inset it. Internal folds remain connected'),
        brick_loose_drawing=bpy.props.PointerProperty(name='Loose Bricks Drawing',type=bpy.types.Object,poll=lambda self,obj:obj.type in {'GREASEPENCIL','GPENCIL'},update=changed,description='Loosen whole bricks touched by these strokes without removing them. Recess, Tilt and Protrusion control their pose; Draw Damage removes bricks instead'),
        brick_loose_radius=bpy.props.FloatProperty(name='Loose Drawing Radius',default=.15,min=.001,soft_max=1,subtype='DISTANCE',update=changed,description='Distance around the loose-brick strokes. A smaller radius selects fewer bricks; a larger radius selects a wider strip'),
        brick_loose_enabled=bpy.props.BoolProperty(name='Use Loose Drawing',default=True,update=changed),
    )


def defaults(p):
    from . import material_defaults
    p.material=material_defaults.get('brick');p.brick_seam_material=material_defaults.get('mortar')
    p.brick_break_material=p.material
    # сначала целая ровная кладка, повреждения пользователь задаёт сам
    p.brick_missing=0.;p.brick_broken=.4;p.brick_loose=0.
    p.brick_face_tilt=0.;p.brick_face_jitter=1.62;p.brick_brick_rough=.35;p.brick_chip=.6
    p.bend=0.;p.resolution=50.;p.axes='HORIZONTAL'


def slots(p):return [getattr(p,name) or p.material for name in MATERIALS]


def loose_paths(source):
    from . import paint
    p=source.pd_structure
    return paint.read_paths(bpy.context,source,p.brick_loose_drawing,min_points=1) if p.brick_loose_enabled and p.brick_loose_drawing else []


def input_state(source):
    from . import live_input_state,structure_surface
    p=source.pd_structure;d=p.brick_loose_drawing
    return (d.session_uid,live_input_state.signature(d,matrix=structure_surface.input_matrix(source,d))) if d and p.brick_loose_enabled else None


def edge_chart(a, b, p, unit):
    from .structure_surface import edge_normal
    from .structure_surface_map import Chart
    delta = b - a
    tangent = delta.normalized()
    width = max(1e-5, (p.brick_brick_height + p.brick_mortar) * p.brick_brick_scale * unit / 100)
    half = width / 2
    side = edge_normal(tangent).cross(tangent) * half
    mesh = bpy.data.meshes.new('_PD_Brick_Edge')
    try:
        mesh.from_pydata([a-side, b-side, b+side, a+side], [], [(0, 1, 2, 3)])
        mesh.update()
        return Chart(mesh, [0], dict(enumerate(((0, 0), (delta.length, 0), (delta.length, width), (0, width)))))
    finally:
        bpy.data.meshes.remove(mesh)


def generate(p,chart,damage,unit,loose):
    from . import core_loader
    core=core_loader.load()
    if not hasattr(core,'build_brick_surface'):raise RuntimeError('Restart Blender to load Bricks Generator')
    params=core.BrickDamageParams()
    for name,field in FIELDS.items():
        import re
        native_name=re.sub(r'(?<!^)(?=[A-Z])','_',field).lower()
        value=getattr(p,name);setattr(params,native_name,int(value) if name=='brick_bond' else value)
    params.seed=p.seed;params.detail=max(.1,p.resolution/100);params.joint_fill=p.brick_joint_fill
    factor=100/unit
    # соседние грани сходятся на одной линии даже при толщине и повороте на 90 градусов
    normals={};indices={}
    for ids in chart.tri:
        a,b,c=[chart.position[i] for i in ids];n=(b-a).cross(c-a).normalized()
        for i in ids:
            key=tuple(round(x,7) for x in chart.position[i]);indices[i]=key
            group=normals.setdefault(key,[])
            if not any(n.dot(old)>.99999 for old in group):group.append(n)
    miters={}
    for key,group in normals.items():
        value=np.linalg.lstsq(np.array(group),np.ones(len(group)),rcond=None)[0]
        length=np.linalg.norm(value)
        if length>4:value*=4/length
        miters[key]=value
    offsets=np.array([miters[indices[i]] for i in range(len(chart.position))])
    margin=abs(p.brick_boundary_offset)/factor
    (a,b),(c,d)=chart.bounds;u0,u1,v0,v1=a-margin,b+margin,c-margin,d+margin
    span=max(u1-u0,v1-v0)
    step=max(.005*unit,min(p.brick_brick_height*p.brick_brick_scale/factor/2,p.drawing_radius*unit/3,span/48))
    nu=max(2,min(256,math.ceil((u1-u0)/step)));nv=max(2,min(256,math.ceil((v1-v0)/step)))
    damage_values=[];loose_values=[]
    for i in range(nu+1):
        x=u0+(u1-u0)*i/nu
        for j in range(nv+1):
            y=v0+(v1-v0)*j/nv;point,_,_=chart.sample(x,y)
            damage_values.append(min(3.,damage.ratio(point)));loose_values.append(min(3.,loose.ratio(point)))
    data=core.build_brick_surface(params,(np.array(chart.uv)[:,:2]*factor).ravel().tolist(),
        (np.array(chart.position)*factor).ravel().tolist(),offsets.ravel().tolist(),
        u0*factor,u1*factor,v0*factor,v1*factor,nu,nv,damage_values,loose_values,p.brick_boundary_offset,p.break_amount/100)
    return np.asarray(data['points'])/factor,np.asarray(data['faces']),np.asarray(data['materials']).ravel(),np.asarray(data['pieces']).ravel(),np.asarray(data['flat'])/factor


def draw(layout,context,source):
    from . import material_library,material_controls,structure_drawing,structure_shading,structure_surface,quality_ui
    p=source.pd_structure
    box=layout.box();material_controls.centered_heading(box,'Bricks Settings',icon='MOD_BUILD')
    material_library.draw_property(box,p,'material',text='Bricks')
    quality_ui.draw(box,p,'resolution')
    box.prop(p,'master_scale')
    box.prop(p,'single_edge_members')
    box.prop(p,'brick_bond',text='Bond');box.prop(p,'brick_brick_scale',text='Brick Scale')
    for name,title in [('brick_brick_length','Brick Length (cm)'),('brick_brick_height','Brick Height (cm)'),('brick_brick_width','Brick Width (cm)'),('brick_bond_offset','Bond Offset'),('brick_boundary_offset','Boundary Offset (cm)')]:box.prop(p,name,text=title)
    box.prop(p,'seed')
    box=layout.box();material_controls.centered_heading(box,'Mortar',icon='MATERIAL')
    material_library.draw_property(box,p,'brick_seam_material',text='Mortar')
    box.prop(p,'brick_joint_fill');box.prop(p,'brick_mortar',text='Joint Width (cm)');box.prop(p,'brick_mortar_depth',text='Recess (cm)');box.prop(p,'brick_joint_rough',text='Joint Roughness')
    box=layout.box();material_controls.centered_heading(box,'Brick Surface')
    for name,title in [('brick_face_jitter','Depth Variation (cm)'),('brick_face_tilt','Face Tilt'),('brick_brick_rough','Surface Roughness'),('brick_chip','Chipped Edges'),('brick_chip_size','Chip Size (cm)')]:box.prop(p,name,text=title)
    structure_drawing.draw(layout,context,source)
    box=layout.box();material_controls.centered_heading(box,'Knocked Out Bricks')
    material_library.draw_property(box,p,'brick_break_material',text='Exposed Brick Sides')
    for name,title in [('brick_break_size','Break Size'),('brick_break_jagged','Break Irregularity'),('brick_missing','Missing Bricks'),('brick_broken','Additional Knocked Out'),('brick_broken_rough','Damaged Edge Roughness')]:box.prop(p,name,text=title)
    box=layout.box();material_controls.centered_heading(box,'Loose Bricks')
    for name,title in [('brick_loose','Random Loose Bricks'),('brick_loose_depth','Recess (cm)'),('brick_loose_tilt','Tilt'),('brick_loose_out','Protruding Bricks'),('brick_loose_out_dist','Protrusion (cm)')]:box.prop(p,name,text=title)
    box.label(text='Keeps bricks; changes their depth and tilt.',icon='INFO')
    box.prop(p,'brick_loose_enabled');row=box.row(align=True);row.prop(p,'brick_loose_drawing',text='Drawing')
    op=row.operator('project_damage.structure_drawing',text='',icon='TRASH');op.action='REMOVE';op.role='LOOSE';op.source_name=source.name
    row=box.row(align=True)
    for action,label in [('NEW','New Drawing'),('DRAW','Draw / Edit')]:
        op=row.operator('project_damage.structure_drawing',text=label,icon='GREASEPENCIL');op.action=action;op.role='LOOSE';op.source_name=source.name
    box.prop(p,'brick_loose_radius')
    if p.brick_loose_drawing and context.active_object==p.brick_loose_drawing:
        op=box.operator('project_damage.structure_drawing',text='Finish Drawing',icon='CHECKMARK');op.action='FINISH';op.role='LOOSE';op.source_name=source.name
    box=layout.box();material_controls.centered_heading(box,'Shading');structure_shading.draw(box,structure_surface.output(source))
