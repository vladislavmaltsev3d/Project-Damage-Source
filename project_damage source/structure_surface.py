# самостоятельные конструкции по редактируемому мешу, их обновление и Apply
from . import quality_ui
"""Editable surface-based structural inserts, with local damage handles."""
import math
import hashlib
import time
from contextlib import contextmanager
from . import structure_drawing, structure_pipes, structure_rebar, structure_shading, pipes, rebar
import bpy
import bmesh
import numpy as np
from mathutils import Vector, Matrix
from bpy.app.handlers import persistent

KINDS = {'structure_rebars': ('Rebars Generator','REBAR'), 'structure_pipes': ('Pipes Generator','PIPE'),
         'structure_laths': ('Wooden Laths Generator','LATH'), 'structure_studs': ('Studs Generator','STUD'), 'structure_bricks': ('Bricks Generator','BRICK')}
_busy=False
_pending=set()
_states={}
_sources=set()
_due={}
_queued_ids={}
_history_restoring=False
_history_states={}
_frame_changing=False


def owner(context):
    obj=context.active_object
    if obj is None:return None
    drawing_source=structure_drawing.source_for_drawing(obj)
    if drawing_source is not None:return drawing_source
    if obj.get('pd_structure_source'):return obj
    source=obj.get('pd_structure_owner')
    return source if isinstance(source,bpy.types.Object) and source.get('pd_structure_source') else None


def output(source):
    return next((ob for ob in source.children if ob.get('pd_structure_output')),None)


def output_aligned(target):
    identity = Matrix.Identity(4)
    return target.matrix_parent_inverse == identity and target.matrix_basis == identity


def changed_property(self,context):
    source=self.id_data
    if not _busy and not _history_restoring and not _frame_changing and source.pd_structure.live_update:queue(source)


def control_material(source):
    name='Structure Control | Project Damage'
    mat=bpy.data.materials.get(name)
    if mat is None or not mat.get('pd_structure_control_material'):
        mat=next((m for m in bpy.data.materials if m.get('pd_structure_control_material')),None)
    if mat is None:
        mat=bpy.data.materials.new(name)
        mat['pd_structure_control_material']=True
        mat.use_nodes=True
        shader=next(n for n in mat.node_tree.nodes if n.type=='BSDF_PRINCIPLED')
        shader.inputs['Alpha'].default_value=0.0
        mat.diffuse_color=(.35,.35,.35,0.0)
        if hasattr(mat,'surface_render_method'):mat.surface_render_method='DITHERED'
        elif hasattr(mat,'blend_method'):mat.blend_method='BLEND'
    if len(source.data.materials)!=1 or source.data.materials[0]!=mat:
        source.data.materials.clear();source.data.materials.append(mat)
        for face in source.data.polygons:face.material_index=0
    return mat


def pipe_type_changed(self,context):
    structure_pipes.defaults(self)
    changed_property(self,context)


def material_changed(self,context):
    if _busy or _history_restoring:return
    target=output(self.id_data)
    if target:
        slots=(self.material,self.nail_material) if self.kind=='STUD' or (self.kind=='LATH' and self.wood_style=='1') else (self.material,)
        if self.kind=='BRICK':
            from . import structure_bricks
            slots=structure_bricks.slots(self)
        for index,mat in enumerate(slots):
            if mat or index<len(target.data.materials):
                while len(target.data.materials)<=index:target.data.materials.append(None)
                target.data.materials[index]=mat


class PDStructurePoint(bpy.types.PropertyGroup):
    obj:bpy.props.PointerProperty(name='Damage Object',type=bpy.types.Object,update=changed_property)
    enabled:bpy.props.BoolProperty(name='Enabled',default=True,update=changed_property)


# здесь хранятся значения, которые сохраняются вместе с объектом
class PDStructureSettings(bpy.types.PropertyGroup):
    kind:bpy.props.EnumProperty(items=[(k,k.title(),'') for k in ('REBAR','PIPE','LATH','STUD','BRICK')],default='REBAR')
    material:bpy.props.PointerProperty(name='Material',type=bpy.types.Material,update=material_changed)
    nail_material:bpy.props.PointerProperty(name='Nails Material',type=bpy.types.Material,update=material_changed)
    rebar_type:bpy.props.EnumProperty(name='Bar Type',items=rebar.REBAR_TYPE_ITEMS,default='0',update=changed_property,description='Uses the original Boolean Break reinforcement profile and fracture ends')
    rebar_rib_height:bpy.props.FloatProperty(name='Rib Height',default=12,min=0,max=100,subtype='PERCENTAGE',update=changed_property,description='Height of ribs as a share of the bar radius. Zero gives a round surface; higher values make ribs stand out')
    rebar_rib_pitch:bpy.props.FloatProperty(name='Rib Spacing',default=.65,min=.01,soft_max=4,update=changed_property,description='Rib spacing relative to bar diameter. Smaller values pack ribs closer; larger values spread them apart')
    rebar_twist_pitch:bpy.props.FloatProperty(name='Twist Length',default=10,min=.01,soft_max=40,update=changed_property,description='Length of one twist relative to bar diameter. Small values make tight twists; large values make a gentler twist')
    rebar_neck:bpy.props.FloatProperty(name='Broken End Narrowing',default=50,min=0,max=100,subtype='PERCENTAGE',update=changed_property,description='Zero keeps the full diameter at damage boundaries; higher values narrow and roughen broken ends')
    rebar_wobble:bpy.props.FloatProperty(name='Surface Irregularity',default=15,min=0,max=100,subtype='PERCENTAGE',update=changed_property,description='Zero keeps an even centreline; higher values add subtle irregularity using the original reinforcement builder')
    pipe_type:bpy.props.EnumProperty(name='Pipe Type',items=pipes.TYPE_ITEMS,default='STEEL',update=pipe_type_changed,description='Uses the same pipe profiles as Pipe Damage. Choosing a type sets its usual radius, wall thickness, joint spacing and wires; a custom material is kept')
    pipe_wall:bpy.props.FloatProperty(name='Wall Thickness',default=.0033,min=.00005,soft_max=.03,subtype='DISTANCE',update=changed_property,description='Thickness of the hollow wall visible at open ends; limited to 85% of the radius to keep the bore open')
    pipe_joint_spacing:bpy.props.FloatProperty(name='Joint Spacing',default=1.2,min=0,soft_max=4,subtype='DISTANCE',update=changed_property,description='Distance between the type-specific couplings. Zero removes couplings; Conduit uses continuous corrugation')
    pipe_jagged:bpy.props.FloatProperty(name='Jagged Ends',default=60,min=0,max=100,subtype='PERCENTAGE',update=changed_property,description='0% makes clean cut rims; 100% makes strongly torn ends using the selected pipe type. Only damage boundaries are affected')
    pipe_dents:bpy.props.FloatProperty(name='Dents',default=40,min=0,max=100,subtype='PERCENTAGE',update=changed_property,description='0% keeps round ends; 100% allows stronger flattening at damaged ends of ductile pipes. PVC and Cast Iron remain brittle')
    pipe_wires:bpy.props.BoolProperty(name='Wires',default=False,update=changed_property,description='Generate internal wires and loose wire ends at damage. Enabled by default for Conduit')
    pipe_wire_count:bpy.props.IntProperty(name='Wire Count',default=3,min=1,max=5,update=changed_property,description='Number of wires inside each pipe, from one to five')
    wood_style:bpy.props.EnumProperty(name='Wood Style',items=[('0','Lath Damage','The same strip geometry as Lath Damage'),('1','Riven Lath','The same split profiles as Riven Lath, with support studs')],default='0',update=changed_property)
    board_length:bpy.props.FloatProperty(name='Board Length',default=1.2,min=.05,soft_max=4,subtype='DISTANCE',update=changed_property)
    splinters:bpy.props.FloatProperty(name='Splinters',default=1.2,min=0,max=6,update=changed_property,description='0 makes clean breaks; higher values create longer split wooden tips, using the Lath Damage profile')
    support_spacing:bpy.props.FloatProperty(name='Support Spacing',default=.4,min=.03,soft_max=2,subtype='DISTANCE',update=changed_property)
    support_width:bpy.props.FloatProperty(name='Support Width',default=.05,min=.005,soft_max=.3,subtype='DISTANCE',update=changed_property)
    support_depth:bpy.props.FloatProperty(name='Support Depth',default=.09,min=.005,soft_max=.4,subtype='DISTANCE',update=changed_property)
    noggin_spacing:bpy.props.FloatProperty(name='Crosspiece Spacing',default=.8,min=.05,soft_max=3,subtype='DISTANCE',update=changed_property)
    nails:bpy.props.FloatProperty(name='Nails',default=100,min=0,max=100,subtype='PERCENTAGE',update=changed_property,description='0% removes nails; 100% uses every eligible position on studs or surviving front-lath/backing-stud crossings')
    nail_length:bpy.props.FloatProperty(name='Nail Length',default=.010125,min=.001,soft_max=.1,subtype='DISTANCE',update=changed_property,description='Visible nail length above the wood, stored in metres for compatibility')
    nail_length_cm:bpy.props.FloatProperty(name='Nail Length (cm)',default=1.0125,min=.1,soft_max=10,precision=4,step=1,get=lambda self:self.nail_length*100,set=lambda self,value:setattr(self,'nail_length',value*.01),description='Visible nail length in centimetres. 0.1 cm is 1 mm; 1 cm is 10 mm. Small length and position variations keep the shank embedded in the wood')
    nail_size:bpy.props.FloatProperty(name='Nail Size',default=100,min=10,max=400,subtype='PERCENTAGE',update=changed_property,description='Thickness of the nail shaft and size of its head, independent of protrusion. 10% makes fine nails; 100% uses the standard profile; 400% makes larger nails, limited by the wood width. Nail Length controls how far they stick out')
    auto_smooth:bpy.props.BoolProperty(name='Shade Auto Smooth',default=True,update=changed_property,description='Smooth the broad wooden surfaces while retaining sharp edges above Smooth Angle. Off uses flat face shading')
    smooth_angle:bpy.props.FloatProperty(name='Smooth Angle',default=math.radians(40),min=0,max=math.pi,subtype='ANGLE',update=changed_property,description='Edges sharper than this angle stay crisp. Zero keeps all creases; 180 degrees smooths every shared edge')
    spacing:bpy.props.FloatProperty(name='Spacing',default=.25,min=.01,soft_max=2,subtype='DISTANCE',update=changed_property,description='Distance between neighboring members. Smaller values create more rows; larger values create fewer rows')
    width:bpy.props.FloatProperty(name='Width',default=.08,min=.002,soft_max=.5,subtype='DISTANCE',update=changed_property)
    thickness:bpy.props.FloatProperty(name='Thickness',default=.025,min=.001,soft_max=.5,subtype='DISTANCE',update=changed_property)
    radius:bpy.props.FloatProperty(name='Radius',default=.012,min=.001,soft_max=.15,subtype='DISTANCE',update=changed_property)
    brick_length:bpy.props.FloatProperty(name='Brick Length',default=.24,min=.02,soft_max=1,subtype='DISTANCE',update=changed_property)
    gap:bpy.props.FloatProperty(name='Joint Gap',default=.01,min=0,soft_max=.05,subtype='DISTANCE',update=changed_property)
    axes:bpy.props.EnumProperty(name='Direction',items=[('VERTICAL','Vertical','Along the local surface height'),('HORIZONTAL','Horizontal','Across the local surface'),('BOTH','Both','Two crossing sets')],default='BOTH',update=changed_property)
    resolution:bpy.props.FloatProperty(name='Resolution',default=100,min=0,max=200,subtype='PERCENTAGE',update=changed_property,description='0% uses four-sided tubes and minimum segments; 100% uses the full profile and smooth damage bends; 200% adds samples')
    master_scale:bpy.props.FloatProperty(name='Master Scale',default=100.,min=10.,max=400.,subtype='PERCENTAGE',update=changed_property,
        description='Scale member dimensions, spacing and damage distances together. 10% uses one tenth of the set dimensions, 100% uses the set dimensions, and 400% uses four times those dimensions. The control mesh keeps its size')
    single_edge_members:bpy.props.BoolProperty(name='Single-Edge Members',default=True,update=changed_property,
        description='Use loose mesh edges as guides. Rebars also follows connected chains and branches')
    seed:bpy.props.IntProperty(name='Seed',default=0,min=0,update=changed_property,description='Varies the break boundary near damage handles')
    points:bpy.props.CollectionProperty(type=PDStructurePoint)
    drawing:bpy.props.PointerProperty(name='Damage Drawing',type=bpy.types.Object,poll=lambda self,obj:obj.type in {'GREASEPENCIL','GPENCIL'},update=changed_property,description='Grease Pencil strokes defining damage bands on the control plane; visible layers at the current frame are used')
    draw_damage:bpy.props.BoolProperty(name='Use Drawing',default=True,update=changed_property,description='Combine drawn damage with enabled Damage Objects. Off ignores the drawing without deleting it')
    drawing_radius:bpy.props.FloatProperty(name='Drawing Radius',default=.15,min=.001,soft_max=2.,subtype='DISTANCE',update=changed_property,description='Distance affected on each side of a stroke. Small values make narrow cuts; large values open a wider band. Opening scales the removed portion and Bend bends surviving geometry nearby')
    damage_randomness:bpy.props.FloatProperty(name='Damage Randomness',default=0,min=0,max=100,subtype='PERCENTAGE',update=changed_property,description='0% keeps a uniform influence around drawings and damage objects. 100% varies the damage boundary strongly; Seed selects a repeatable pattern. Intact areas outside the zone stay intact')
    damage_radius:bpy.props.FloatProperty(name='Damage Radius',default=.35,min=.01,soft_max=2,subtype='DISTANCE',update=changed_property,description='World-space radius around each damage object; moving the object moves the damaged area')
    break_amount:bpy.props.FloatProperty(name='Opening',default=65,min=0,max=100,subtype='PERCENTAGE',update=changed_property,description='0% bends members without removing them; 100% opens the entire damage radius')
    bend:bpy.props.FloatProperty(name='Bend',default=.08,min=0,soft_max=.5,subtype='DISTANCE',update=changed_property,description='Outward displacement near a damage handle; zero leaves straight members')
    live_update:bpy.props.BoolProperty(name='Live Update',default=True,update=changed_property)


def references(source):
    p=source.pd_structure
    extra=(p.brick_loose_enabled,p.brick_loose_drawing.session_uid if p.brick_loose_drawing else None) if p.kind=='BRICK' else None
    return (extra,p.draw_damage,p.drawing.session_uid if p.drawing else None,tuple((item.obj.session_uid if item.obj else None,item.enabled) for item in p.points))


@contextmanager
def control_mesh(source):
    if source.mode!='EDIT':
        yield source.evaluated_get(bpy.context.evaluated_depsgraph_get()).data if any(m.show_viewport for m in source.modifiers) else source.data
        return
    if any(m.show_viewport and m.show_in_editmode for m in source.modifiers):
        evaluated=source.evaluated_get(bpy.context.evaluated_depsgraph_get())
        mesh=evaluated.to_mesh()
        try:yield mesh
        finally:evaluated.to_mesh_clear()
        return
    mesh=bpy.data.meshes.new('_PD_Structure_Edit_Snapshot')
    bm=bmesh.from_edit_mesh(source.data).copy()
    try:
        bm.normal_update();bm.to_mesh(mesh);mesh.update()
        yield mesh
    finally:
        bm.free();bpy.data.meshes.remove(mesh)


def input_matrix(source,obj):
    # дочерний рисунок едет вместе с конструкцией, это не новое повреждение
    chain=[]
    current=obj
    while current is not None and current!=source:
        chain.append(current)
        current=current.parent
    if current is None:return obj.matrix_world
    matrix=Matrix.Identity(4)
    for child in reversed(chain):
        local=child.matrix_local if child.constraints or child.parent_type!='OBJECT' else child.matrix_parent_inverse @ child.matrix_basis
        matrix=matrix @ local
    return matrix


def matrix_values(matrix):
    return tuple(value for row in matrix for value in row)


def point_states(source):
    return tuple((p.obj.session_uid,matrix_values(input_matrix(source,p.obj)),p.enabled) for p in source.pd_structure.points if p.obj)


def state(source):
    digest=hashlib.blake2b(digest_size=16)
    settings=source.pd_structure
    with control_mesh(source) as mesh:
        fields=((mesh.vertices,'co',3,np.float32),(mesh.loops,'vertex_index',1,np.int32),(mesh.polygons,'loop_total',1,np.int32))
        if settings.single_edge_members:
            fields+=((mesh.edges,'vertices',2,np.int32),)
        for data,field,width,dtype in fields:
            values=np.empty(len(data)*width,dtype=dtype);data.foreach_get(field,values);digest.update(values.tobytes())
    points=point_states(source)
    drawing=source.pd_structure.drawing
    signature=None
    drawing_matrix=None
    if drawing and source.pd_structure.draw_damage:
        from . import live_input_state
        matrix=input_matrix(source,drawing)
        drawing_matrix=matrix_values(matrix)
        signature=live_input_state.signature(drawing,matrix=matrix)
    parameters=tuple((prop.identifier,getattr(settings,prop.identifier)) for prop in settings.bl_rna.properties if prop.type in {'FLOAT','INT','BOOLEAN','ENUM','STRING'} and prop.identifier not in {'rna_type','name','live_update','nail_length_cm'})
    if settings.kind=='BRICK':
        from . import structure_bricks
        signature=(signature,structure_bricks.input_state(source))
    return digest.digest(),points,drawing_matrix,signature,parameters,references(source)


def queue(source):
    if _history_restoring or not source.get('pd_structure_source'):return
    _sources.add(source.name)
    _pending.add(source.name)
    _queued_ids[source.name]=source.session_uid
    _due[source.name]=time.monotonic()+.3
    if not bpy.app.timers.is_registered(tick):bpy.app.timers.register(tick,first_interval=.3)


def animation_playing():
    return any(screen.is_animation_playing for screen in bpy.data.screens)


def tick():
    if _history_restoring or not _pending:return None
    if _busy or _frame_changing or animation_playing():return .3
    names=tuple(_pending)
    for name in names:
        source=bpy.data.objects.get(name)
        if source is None or source.session_uid!=_queued_ids.get(name) or not source.get('pd_structure_source') or not source.pd_structure.live_update:
            _pending.discard(name);_due.pop(name,None);_queued_ids.pop(name,None)
            continue
        if time.monotonic()<_due.get(name,0):continue
        target=output(source)
        if target is None:
            _pending.discard(name);_due.pop(name,None);_queued_ids.pop(name,None)
            continue
        if target.mode!='OBJECT':continue
        _pending.discard(name);_due.pop(name,None);_queued_ids.pop(name,None)
        try:
            if not output_aligned(target) or _states.get(source.session_uid)!=state(source):rebuild(source)
        except Exception:
            import traceback;traceback.print_exc()
    return .1 if _pending else None


@persistent
def deps_changed(scene,depsgraph):
    # проигрывание двигает готовую конструкцию, а не пересоздаёт её каждый кадр
    if _busy or _history_restoring or _frame_changing or animation_playing():return
    geometry={u.id.original for u in depsgraph.updates if u.is_updated_geometry}
    transforms={u.id.original for u in depsgraph.updates if u.is_updated_transform}
    updates=geometry | transforms
    _sources.update(obj.name for obj in updates if isinstance(obj,bpy.types.Object) and obj.get('pd_structure_source'))
    pruned=False
    for name in tuple(_sources):
        source=bpy.data.objects.get(name)
        if source is None or not source.get('pd_structure_source'):
            _sources.discard(name)
            _pending.discard(name);_due.pop(name,None);_queued_ids.pop(name,None)
            pruned=True
            continue
        if not source.pd_structure.live_update:continue
        previous=_states.get(source.session_uid)
        links_changed=previous is None or previous[-1]!=references(source)
        mesh_changed=source in geometry or source.data in geometry
        # Changing Origin can update both the object and the mesh in one event.
        target=output(source)
        alignment_changed=source in transforms and target is not None and not output_aligned(target)
        drawing=source.pd_structure.drawing if source.pd_structure.draw_damage else None
        drawing_updated=False
        if drawing is not None:
            drawing_updated=drawing.data in geometry or (drawing in geometry and drawing not in transforms)
            if drawing in transforms:
                drawing_updated|=previous is None or previous[2]!=matrix_values(input_matrix(source,drawing))
        if source.pd_structure.kind=='BRICK':
            extra=source.pd_structure.brick_loose_drawing
            if extra is not None and (extra in updates or extra.data in geometry):
                from . import structure_bricks
                old=previous[3][1] if previous and isinstance(previous[3],tuple) else None
                drawing_updated|=old!=structure_bricks.input_state(source)
        points_updated=any(p.obj in updates for p in source.pd_structure.points) and (previous is None or previous[1]!=point_states(source))
        if links_changed or drawing_updated or mesh_changed or points_updated or alignment_changed:
            if name not in _pending:queue(source)
    if pruned:
        valid={obj.session_uid for name in _sources if (obj:=bpy.data.objects.get(name)) is not None}
        for uid in tuple(_states):
            if uid not in valid:_states.pop(uid,None)


def commit_mesh(target,mesh):
    existing=target.data
    if existing.users>1:
        target.data=existing.copy()
        existing=target.data
    slots=list(mesh.materials)
    if list(existing.materials)!=slots:
        existing.materials.clear()
        for material in slots:existing.materials.append(material)
    bm=bmesh.new()
    try:
        bm.from_mesh(mesh)
        bm.to_mesh(existing)
    finally:bm.free()
    existing.update();existing.update_tag();target.update_tag(refresh={'DATA'})
    bpy.data.meshes.remove(mesh)


def isolated_edges(mesh):
    if not len(mesh.edges):
        return []
    indices=np.empty((len(mesh.edges),2),np.int32)
    mesh.edges.foreach_get('vertices',indices.ravel())
    # Two degree-one endpoints make a complete single-edge island.
    degree=np.bincount(indices.ravel(),minlength=len(mesh.vertices))
    return [(mesh.vertices[int(a)].co.copy(),mesh.vertices[int(b)].co.copy())
            for a,b in indices[np.all(degree[indices]==1,axis=1)]]


def surviving_runs(a,b,step,keep,*,knots=()):
    steps=max(2,min(2000,math.ceil((b-a)/step)))
    samples=[a+(b-a)*i/steps for i in range(steps+1)]
    if knots:
        samples=sorted(set(samples).union(knots))
    flags=[keep(t) for t in samples]
    run=[]
    for i,t in enumerate(samples):
        if i and flags[i]!=flags[i-1]:
            lo,hi=samples[i-1],t
            for _ in range(16):
                middle=(lo+hi)/2
                if keep(middle)==flags[i-1]:lo=middle
                else:hi=middle
            boundary=(lo+hi)/2
            if flags[i]:
                run=[boundary]
            else:
                run.append(boundary)
                if len(run)>1:yield run
                run=[]
        if flags[i]:run.append(t)
    if len(run)>1:yield run


def edge_normal(tangent):
    normal=Vector((0,-1,0) if abs(tangent.y)<.99 else (0,0,1))
    return (normal-tangent*normal.dot(tangent)).normalized()


def edge_runs(edges, metric, damage, amount, bend, step):
    for a,b in edges:
        if metric is not None:
            a,b=metric@a,metric@b
        delta=b-a
        length=delta.length
        if length<=1e-8:continue
        tangent=delta/length
        normal=edge_normal(tangent)
        if not damage.base.parts:
            yield [a,b],normal,False,False
            continue
        def keep(t):return damage.ratio(a+tangent*t)>=amount
        for samples in surviving_runs(0.,length,step,keep):
            if samples[-1]-samples[0]<=1e-8:continue
            centers=[]
            for t in samples:
                center=a+tangent*t
                ratio=damage.ratio(center)
                if ratio<1:center+=normal*((1-ratio*ratio)**2*bend)
                centers.append(center)
            yield centers,normal,samples[0]>1e-8,samples[-1]<length-1e-8


# обновляем конструкцию по текущей форме управляющего меша
def rebuild(source):
    global _busy
    if _busy or _history_restoring:return
    target=output(source)
    if target is not None and target.mode!='OBJECT':return
    _busy=True
    mesh=None
    try:
        bpy.context.view_layer.update()
        _sources.add(source.name)
        p=source.pd_structure
        if p.kind=='BRICK':
            from . import structure_bricks
        if (p.kind=='STUD' or (p.kind=='LATH' and p.wood_style=='1')) and p.nail_material is None:
            from . import stud_framing
            p.nail_material=stud_framing.nail_material()
        control_material(source)
        inv=source.matrix_world.inverted_safe()
        unit=1/max(bpy.context.scene.unit_settings.scale_length,1e-8)
        metric=None
        if p.kind in {'LATH','STUD','BRICK'}:
            metric=source.matrix_world.to_quaternion().to_matrix().transposed() @ source.matrix_world.to_3x3()
        else:
            scale=max(1e-8,sum(source.matrix_world.to_scale())/3)
            unit/=scale
        uv_unit=unit
        unit*=p.master_scale/100.
        spacing=p.spacing*unit;width=p.width*unit;depth=p.thickness*unit;radius=p.radius*unit;gap=p.gap*unit
        damage_r=p.damage_radius*unit
        quality=max(.08,p.resolution/100)
        step=max(.003*unit,min(spacing/3,.03*unit,p.drawing_radius*unit/4,damage_r/4))/quality
        centers=[inv@item.obj.matrix_world.translation for item in p.points if item.enabled and item.obj]
        strokes=structure_drawing.paths(source)
        if metric is not None:
            centers=[metric@q for q in centers]
            strokes=[[metric@Vector(q) for q in path] for path in strokes]
        damage=structure_drawing.DamageField(centers,damage_r,strokes,p.drawing_radius*unit)
        from .structure_damage import VariedDamage
        damage=VariedDamage(damage,p.damage_randomness,p.seed)
        from . import structure_surface_map,structure_wood
        clearance=max(depth,p.support_depth*unit,p.width*unit*.5) if p.kind in {'LATH','STUD'} else radius*2
        with control_mesh(source) as control:
            paths=[]
            edges=[]
            if p.single_edge_members:
                if p.kind=='REBAR':
                    paths=list(structure_rebar.custom_paths(control))
                else:
                    edges=isolated_edges(control)
            if metric is None:charts=structure_surface_map.charts(control,clearance)
            else:
                metric_mesh=control.copy()
                try:
                    metric_mesh.transform(metric.to_4x4());metric_mesh.update()
                    charts=structure_surface_map.charts(metric_mesh,clearance,fold=p.kind=='BRICK')
                finally:bpy.data.meshes.remove(metric_mesh)
        verts=[];faces=[];materials=[];pieces=[];uv_points=[]
        pipe_buffer=pipes.MeshBuffer() if p.kind=='PIPE' else None
        pipe_run=0;bar_run=0;bar_ids=[]
        if p.kind=='BRICK':
            loose_strokes=structure_bricks.loose_paths(source)
            if metric is not None:loose_strokes=[[metric@Vector(q) for q in path] for path in loose_strokes]
            loose_field=structure_drawing.DamageField([],1.,loose_strokes,p.brick_loose_radius*unit)
            charts.extend(structure_bricks.edge_chart(metric@a, metric@b, p, unit)
                          for a,b in edges if (metric@(b-a)).length > 1e-6)
            edges=[]
            for chart in charts:
                points,polygons,mats,ids,flat=structure_bricks.generate(p,chart,damage,unit,loose_field)
                off=len(verts);base=max(pieces,default=-1)+1
                verts.extend(points);uv_points.extend(flat);faces.extend(tuple(off+int(i) for i in f) for f in polygons)
                materials.extend(int(v) for v in mats);pieces.extend(int(v)+base if v>=0 else -1 for v in ids)
        elif p.kind in {'LATH','STUD'}:
            for chart in charts:
                points,polygons,mats,ids,flat=structure_wood.generate(p,chart,damage,unit)
                off=len(verts);piece_base=max(pieces,default=-1)+1
                verts.extend(points);uv_points.extend(flat);faces.extend(tuple(off+int(i) for i in face) for face in polygons)
                materials.extend(int(v) for v in mats);pieces.extend(int(v)+piece_base for v in ids)
        else:
            for chart_index,chart in enumerate(charts):
                dirs=[0,1] if p.axes=='BOTH' else ([0] if p.axes=='VERTICAL' else [1])
                for axis in dirs:
                    low,high=chart.bounds[axis]
                    first=math.ceil((low+radius*.5)/spacing);last=math.floor((high-radius*.5)/spacing)
                    positions=[i*spacing for i in range(first,last+1)] or [(low+high)*.5]
                    for row,x in enumerate(positions):
                        def surface(t,offset=0):return chart.sample(x if axis==0 else t,t if axis==0 else x,offset)
                        amount=p.break_amount/100
                        for a,b in chart.intervals(axis,x):
                            def keep(t):return damage.ratio(surface(t)[0])>=amount
                            for samples in surviving_runs(a,b,step,keep):
                                if samples[-1]-samples[0]<1e-5:continue
                                hollow=p.kind=='PIPE'
                                centers=[];normals=[]
                                for t in samples:
                                    center,n,_=surface(t,radius*(2 if axis==1 else 0));ratio=damage.ratio(surface(t)[0])
                                    bend_type=pipes.TYPES[p.pipe_type]['bend'] if hollow else 1.0
                                    if ratio<1:center+=n*((1-ratio*ratio)**2*p.bend*unit*bend_type)
                                    centers.append(center);normals.append(n)
                                if hollow:
                                    structure_pipes.add_run(p,pipe_buffer,centers,normals,unit,
                                        samples[0]>a+1e-5,samples[-1]<b-1e-5,pipe_run,samples[0]-a,chart_index*100000+axis*10000+row)
                                    pipe_run+=1
                                    if pipe_buffer.nv>1500000:raise ValueError('Too many pipe polygons; increase Spacing or lower Resolution')
                                    continue
                                points,polys=structure_rebar.generate(p,centers,normals,unit,
                                    samples[0]>a+1e-5,samples[-1]<b-1e-5,bar_run)
                                off=len(verts);verts.extend(points);faces.extend(tuple(off+i for i in f) for f in polys)
                                bar_ids.extend([bar_run]*len(polys));bar_run+=1
                                if len(verts)>1500000:raise ValueError('Too many members; increase Spacing or lower Resolution')
        piece_id=max(pieces,default=-1)+1
        for centers,normals,start_free,end_free,custom in structure_rebar.path_runs(
                paths,damage,p.break_amount/100,p.bend*unit,step):
            points,polys=structure_rebar.generate(p,centers,normals,unit,start_free,end_free,bar_run,custom_path=custom)
            off=len(verts);verts.extend(points);faces.extend(tuple(off+i for i in f) for f in polys)
            bar_ids.extend([bar_run]*len(polys));bar_run+=1
            if len(verts)>1500000:raise ValueError('Too many custom rebars; lower Resolution or simplify the guide paths')
        for centers,normal,start_free,end_free in edge_runs(edges,metric,damage,p.break_amount/100,p.bend*unit,step):
            if p.kind=='PIPE':
                structure_pipes.add_run(p,pipe_buffer,centers,[normal]*len(centers),unit,
                    start_free,end_free,pipe_run,0.,pipe_run)
                pipe_run+=1
                if pipe_buffer.nv>1500000:raise ValueError('Too many pipe polygons; increase Spacing or lower Resolution')
                continue
            else:
                points,polys,flat=structure_wood.generate_edge(p,centers,normal,unit)
                uv_points.extend(flat)
                materials.extend([0]*len(polys))
                pieces.extend([piece_id]*len(polys));piece_id+=1
            off=len(verts);verts.extend(points);faces.extend(tuple(off+i for i in f) for f in polys)
            if len(verts)>1500000:raise ValueError('Too many single-edge members; lower Resolution or use fewer edges')
        if pipe_buffer is not None and pipe_buffer.verts:
            verts=(np.vstack(pipe_buffer.verts)/(100.0/unit)).tolist()
            faces=pipe_buffer.faces;materials=pipe_buffer.mat
        if metric is not None and verts:verts=(np.asarray(verts)@np.asarray(metric.inverted_safe()).T).tolist()
        mesh=bpy.data.meshes.new(source.name+' Geometry');mesh.from_pydata(verts,[],faces);mesh.update()
        if p.kind=='PIPE':
            for mat in structure_pipes.slots(p,max(materials,default=0)+1):mesh.materials.append(mat)
        elif p.kind=='BRICK':
            for mat in structure_bricks.slots(p):mesh.materials.append(mat)
        elif p.material:mesh.materials.append(p.material)
        if (p.kind=='STUD' or (p.kind=='LATH' and p.wood_style=='1')) and p.nail_material:
            if not mesh.materials:mesh.materials.append(None)
            mesh.materials.append(p.nail_material)
        if materials:mesh.polygons.foreach_set('material_index',materials)
        if pipe_buffer is not None and pipe_buffer.part:
            attr=mesh.attributes.new('pipe_id','INT','FACE');attr.data.foreach_set('value',pipe_buffer.part)
        if bar_ids:
            attr=mesh.attributes.new('bar_id','INT','FACE');attr.data.foreach_set('value',bar_ids)
        if pieces:
            attr=mesh.attributes.new('project_damage_brick_id' if p.kind=='BRICK' else 'project_damage_lath_piece','INT','FACE');attr.data.foreach_set('value',pieces)
        bm=bmesh.new();bm.from_mesh(mesh);bmesh.ops.recalc_face_normals(bm,faces=list(bm.faces));bm.to_mesh(mesh);bm.free()
        if p.kind in {'PIPE','REBAR','LATH','STUD','BRICK'}:
            for face in mesh.polygons:face.use_smooth=True
            if p.kind in {'PIPE','REBAR'}:mesh.set_sharp_from_angle(angle=p.smooth_angle)
        uv=mesh.uv_layers.new(name='UVMap')
        for face in mesh.polygons:
            normal=face.normal
            if uv_points and len(face.vertices)>=3:
                a,b,c=(Vector(uv_points[i]) for i in face.vertices[:3]);normal=(b-a).cross(c-a)
            drop=max(range(3),key=lambda i:abs(normal[i]));axes=[i for i in range(3) if i!=drop]
            for li in face.loop_indices:
                vi=mesh.loops[li].vertex_index
                q=uv_points[vi] if uv_points else mesh.vertices[vi].co;uv.data[li].uv=(q[axes[0]]/uv_unit,q[axes[1]]/uv_unit)
        target=output(source)
        if target is None:
            target=bpy.data.objects.new(source.name+' Result',mesh)
            (source.users_collection[0] if source.users_collection else bpy.context.collection).objects.link(target)
            target.parent=source;target['pd_structure_owner']=source;target['pd_structure_output']=True
        else:
            commit_mesh(target,mesh)
        if not output_aligned(target):
            target.matrix_parent_inverse=Matrix.Identity(4)
            target.matrix_basis=Matrix.Identity(4)
        mesh=None
        target.hide_select = True
        structure_shading.ensure(source,target)
        _states[source.session_uid]=state(source)
        _pending.discard(source.name);_due.pop(source.name,None);_queued_ids.pop(source.name,None)
    finally:
        if mesh is not None and mesh.users==0:bpy.data.meshes.remove(mesh)
        _busy=False


# действие «Structural Surface»
class PROJECT_DAMAGE_OT_structure_surface(bpy.types.Operator):
    bl_idname='project_damage.structure_surface';bl_label='Structural Surface';bl_options={'REGISTER','UNDO'}
    action:bpy.props.EnumProperty(items=[(v,v,'') for v in ('ADD','UPDATE','UPDATE_DAMAGE','DAMAGE','REMOVE','APPLY')])
    asset:bpy.props.StringProperty(default='structure_rebars')
    index:bpy.props.IntProperty(default=0)
    @classmethod
    def description(cls,context,properties):
        if properties.action=='UPDATE_DAMAGE':
            return 'Rebuild using the current damage objects and drawings, even when Live Update is off'
        return 'Add, update or apply the construction on its control mesh'
    def execute(self,context):
        global _busy
        source=owner(context)
        if _history_restoring:return {'CANCELLED'}
        if self.action=='ADD':
            if self.asset not in KINDS:return {'CANCELLED'}
            if context.mode!='OBJECT':bpy.ops.object.mode_set(mode='OBJECT')
            title,kind=KINDS[self.asset];unit=1/max(context.scene.unit_settings.scale_length,1e-8)
            mesh=bpy.data.meshes.new(title+' Plane');mesh.from_pydata([(-unit,0,-unit),(unit,0,-unit),(unit,0,unit),(-unit,0,unit)],[],[(0,1,2,3)])
            source=bpy.data.objects.new(title,mesh);context.collection.objects.link(source);source.location=context.scene.cursor.location
            source['pd_structure_source']=True;source['pd_structure_asset']=self.asset;source.display_type='WIRE';source.hide_render=True
            _busy=True
            try:
                p=source.pd_structure;p.kind=kind
                from . import material_defaults
                if kind=='PIPE':structure_pipes.defaults(p)
                else:p.material=material_defaults.get({'REBAR':'rebar','LATH':'wood_planks','STUD':'framing','BRICK':'brick'}[kind])
                if kind=='LATH':p.spacing=.044;p.width=.035;p.thickness=.008;p.axes='HORIZONTAL';p.nails=41;p.wood_style='1'
                if kind=='STUD':p.spacing=.406;p.width=.038;p.thickness=.089
                if kind in {'LATH','STUD'}:
                    from . import stud_framing
                    p.nail_material=stud_framing.nail_material()
                if kind=='BRICK':
                    from . import structure_bricks
                    structure_bricks.defaults(p)
                if kind=='PIPE':p.spacing=.45;p.axes='VERTICAL'
            finally:_busy=False
            for obj in context.selected_objects:obj.select_set(False)
            source.select_set(True);context.view_layer.objects.active=source
            context.view_layer.update()
        if source is None:return {'CANCELLED'}
        if self.action=='DAMAGE':
            obj=bpy.data.objects.new('Structure Damage',None);context.collection.objects.link(obj);obj.location=context.scene.cursor.location;obj.empty_display_type='SPHERE';obj.empty_display_size=source.pd_structure.damage_radius;obj['pd_structure_owner']=source
            source.pd_structure.points.add().obj=obj
        elif self.action=='REMOVE':
            if self.index<len(source.pd_structure.points):source.pd_structure.points.remove(self.index)
        elif self.action=='APPLY':
            if context.mode!='OBJECT':structure_drawing.finish(context,source)
            if output(source) is None or not output_aligned(output(source)) or _states.get(source.session_uid)!=state(source):rebuild(source)
            target=output(source)
            if target is None:
                self.report({'WARNING'}, 'No generated structure to apply')
                return {'CANCELLED'}
            world=target.matrix_world.copy();target.parent=None;target.matrix_world=world
            del target['pd_structure_owner'];del target['pd_structure_output']
            target.hide_select = False
            from . import drawing_cleanup, material_uv
            target[material_uv.PRESERVE_OUTPUT_UV]=True
            drawing_cleanup.after_apply(source,[target])
            del source['pd_structure_source'];_states.pop(source.session_uid,None);_pending.discard(source.name);source.hide_set(True)
            for obj in context.selected_objects:obj.select_set(False)
            target.select_set(True);context.view_layer.objects.active=target
            return {'FINISHED'}
        rebuild(source);return {'FINISHED'}


# собираем видимые настройки этого раздела
def draw(layout,context):
    source=owner(context)
    if source is None:return
    from . import effect_layout, material_controls, material_library
    p=source.pd_structure
    material_controls.centered_heading(layout,KINDS.get(source['pd_structure_asset'],('Structural Surface',''))[0],icon='OUTLINER_OB_MESH')
    effect_layout.standalone_update(layout, p, 'project_damage.structure_surface', action='UPDATE')
    layout.label(text='Edit the wire plane to extend or bend the structure.',icon='EDITMODE_HLT')
    if p.kind=='BRICK':
        from . import structure_bricks
        structure_bricks.draw(layout,context,source)
        draw_damage_footer(layout,context,source)
        return
    box=layout.box();material_controls.centered_heading(box,'Geometry')
    material_library.draw_property(box,p,'material',text='Material')
    quality_ui.draw(box, p,'resolution')
    box.prop(p,'master_scale')
    box.prop(p,'single_edge_members',text='Custom Rebars' if p.kind=='REBAR' else 'Single-Edge Members')
    if p.kind=='PIPE':box.prop(p,'pipe_type')
    if p.kind=='REBAR':box.prop(p,'rebar_type')
    if p.kind=='BRICK':
        box.prop(p,'brick_length');box.prop(p,'width',text='Brick Height');box.prop(p,'thickness');box.prop(p,'gap')
    else:
        if p.kind in {'REBAR','PIPE'}:box.prop(p,'axes',expand=True)
        if p.kind=='LATH':box.prop(p,'wood_style',expand=True)
        box.prop(p,'spacing')
        if p.kind in {'REBAR','PIPE'}:box.prop(p,'radius')
        else:box.prop(p,'width');box.prop(p,'thickness')
    if p.kind=='REBAR':
        if p.rebar_type in {'0','1'}:
            box.prop(p,'rebar_rib_height');box.prop(p,'rebar_rib_pitch')
        if p.rebar_type in {'3','4'}:box.prop(p,'rebar_twist_pitch')
        box.prop(p,'rebar_neck');box.prop(p,'rebar_wobble')
    if p.kind=='PIPE':
        box.prop(p,'pipe_wall');box.prop(p,'pipe_joint_spacing')
        box.prop(p,'pipe_jagged');box.prop(p,'pipe_dents')
        box.prop(p,'pipe_wires')
        if p.pipe_wires:box.prop(p,'pipe_wire_count')
    if p.kind=='LATH':
        box.prop(p,'board_length');box.prop(p,'splinters')
        if p.wood_style=='1':
            box.prop(p,'support_spacing');box.prop(p,'support_width');box.prop(p,'support_depth')
    if p.kind=='STUD':
        box.prop(p,'noggin_spacing');box.prop(p,'splinters')
    if p.kind=='STUD' or (p.kind=='LATH' and p.wood_style=='1'):
        nails=layout.box();material_controls.centered_heading(nails,'Nails')
        material_library.draw_property(nails,p,'nail_material',text='Nails')
        nails.prop(p,'nails');nails.prop(p,'nail_length_cm');nails.prop(p,'nail_size')
    if p.kind in {'LATH','STUD'}:
        shading=layout.box();material_controls.centered_heading(shading,'Shading')
        structure_shading.draw(shading,output(source))
    structure_drawing.draw(layout,context,source)
    draw_damage_footer(layout,context,source)


def draw_damage_footer(layout,context,source):
    from . import material_controls
    p=source.pd_structure
    box=layout.box();material_controls.centered_heading(box,'Damage Zones',icon='EMPTY_DATA')
    for i,item in enumerate(p.points):
        row=box.row(align=True);row.prop(item,'enabled',text='');row.prop(item,'obj',text='');op=row.operator('project_damage.structure_surface',text='',icon='X');op.action='REMOVE';op.index=i
    row=box.row(align=True)
    row.operator('project_damage.structure_surface',text='Add Damage at Cursor',icon='ADD').action='DAMAGE'
    row.operator('project_damage.structure_surface',text='Update Damage Zone',icon='FILE_REFRESH').action='UPDATE_DAMAGE'
    box.prop(p,'damage_radius');box.prop(p,'break_amount');box.prop(p,'damage_randomness')
    if p.kind!='BRICK':box.prop(p,'bend')
    if len(p.points) or p.drawing:box.prop(p,'seed',text='Damage Seed')
    layout.operator('project_damage.structure_surface',text='Apply Geometry',icon='CHECKMARK').action='APPLY'


_CLASSES=(PDStructurePoint,PDStructureSettings,PROJECT_DAMAGE_OT_structure_surface,structure_drawing.PROJECT_DAMAGE_OT_structure_drawing)

# после смены кадра запоминаем положения помощников без пересчёта меша
def remember_animated_inputs():
    if _busy or _history_restoring:return
    from . import live_input_state,structure_bricks
    for name in tuple(_sources):
        source=bpy.data.objects.get(name)
        if source is None or name in _pending:continue
        old=_states.get(source.session_uid)
        if old is None:continue
        p=source.pd_structure;drawing=p.drawing if p.draw_damage else None
        matrix=input_matrix(source,drawing) if drawing else None
        signature=live_input_state.signature(drawing,matrix=matrix) if drawing else None
        if p.kind=='BRICK':signature=(signature,structure_bricks.input_state(source))
        _states[source.session_uid]=(old[0],point_states(source),matrix_values(matrix) if matrix is not None else None,signature,old[4],references(source))


@persistent
def frame_pre(*_):
    global _frame_changing
    _frame_changing=True


@persistent
def frame_post(*_):
    global _frame_changing
    _frame_changing=False
    if not animation_playing():remember_animated_inputs()


@persistent
def playback_post(*_):
    remember_animated_inputs()


# перед Undo останавливаем обновления и забываем временные ссылки
@persistent
def history_pre(*_):
    global _history_restoring,_frame_changing
    _history_restoring=True;_frame_changing=False
    _history_states.clear();_history_states.update(_states)
    for fn in (tick,refresh_sources):
        if bpy.app.timers.is_registered(fn):bpy.app.timers.unregister(fn)
    structure_drawing.forget_projection()
    _sources.clear();_states.clear();_pending.clear();_due.clear();_queued_ids.clear()


# после Undo откладываем чтение восстановленных объектов
@persistent
def history_post(*_):
    global _history_restoring
    _history_restoring=True
    if not bpy.app.timers.is_registered(refresh_sources):bpy.app.timers.register(refresh_sources,first_interval=.1)


# заново находим источники после загрузки или отмены действия
def refresh_sources():
    global _history_restoring,_busy
    _busy=True
    try:
        previous_states=dict(_states)
        _sources.clear();_states.clear();_pending.clear();_due.clear();_queued_ids.clear()
        changed=[]
        for obj in bpy.data.objects:
            if obj.get('pd_structure_source'):
                structure_drawing.restore_saved_projection(obj)
                target = output(obj)
                if target is not None and target.is_editable:
                    target.hide_select = True
                    layer = bpy.context.view_layer
                    if layer is not None and layer.objects.active == target and obj.name in layer.objects and obj.visible_get(view_layer=layer):
                        target.select_set(False, view_layer=layer)
                        obj.select_set(True, view_layer=layer)
                        layer.objects.active = obj
                structure_shading.ensure(obj,target)
                _sources.add(obj.name)
                signature=state(obj)
                previous=_history_states.get(obj.session_uid,previous_states.get(obj.session_uid))
                if target is not None and obj.pd_structure.live_update and (not output_aligned(target) or
                        (previous is not None and previous!=signature)):
                    changed.append(obj.name)
                else:_states[obj.session_uid]=signature
    finally:
        _busy=False;_history_restoring=False;_history_states.clear()
    for name in changed:
        obj=bpy.data.objects.get(name)
        if obj is not None:queue(obj)
    return None


_HISTORY_HOOKS=(('load_pre',history_pre),('undo_pre',history_pre),('redo_pre',history_pre),
                ('load_post',history_post),('undo_post',history_post),('redo_post',history_post),
                ('frame_change_pre',frame_pre),('frame_change_post',frame_post),
                ('animation_playback_post',playback_post))


# подключаем свойства и действия этого модуля к Blender
def register():
    global _history_restoring
    from . import structure_bricks
    structure_bricks.add_properties(PDStructureSettings,changed_property,material_changed)
    for cls in _CLASSES:bpy.utils.register_class(cls)
    bpy.types.Object.pd_structure=bpy.props.PointerProperty(type=PDStructureSettings)
    _history_restoring=False
    for name,callback in _HISTORY_HOOKS:
        handlers=getattr(bpy.app.handlers,name)
        if callback not in handlers:handlers.append(callback)
    bpy.app.timers.register(refresh_sources,first_interval=.1)
    if deps_changed not in bpy.app.handlers.depsgraph_update_post:bpy.app.handlers.depsgraph_update_post.append(deps_changed)


# убираем подключения модуля при выключении аддона
def unregister():
    for name,callback in _HISTORY_HOOKS:
        handlers=getattr(bpy.app.handlers,name)
        if callback in handlers:handlers.remove(callback)
    if deps_changed in bpy.app.handlers.depsgraph_update_post:bpy.app.handlers.depsgraph_update_post.remove(deps_changed)
    structure_drawing.restore_projection()
    history_pre()
    _history_states.clear()
    if hasattr(bpy.types.Object,'pd_structure'):del bpy.types.Object.pd_structure
    for cls in reversed(_CLASSES):bpy.utils.unregister_class(cls)
