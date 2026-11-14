# создаём поверхность из досок и управляем её повреждениями
from . import quality_ui
# SPDX-License-Identifier: GPL-3.0-or-later
"""Two-layer wooden surface with movable destruction handles, in local XY."""
from . import ui_image_hints
import math, random
import bpy
from mathutils import Vector
from bpy.app.handlers import persistent
_busy=False
_pending=set()

def owner(context):
    ob=context.active_object
    if ob and ob.get('pd_plank_surface'):return ob
    if ob is None:return None
    direct=bpy.data.objects.get(ob.get('pd_plank_owner',''))
    if direct:return direct
    from . import runtime_lookup
    return runtime_lookup.plank_owner(context)

def update(self,context):
    from . import runtime_lookup
    runtime_lookup.clear_planks()
    if _busy:return
    ob=self.id_data
    if ob and ob.type=='MESH' and ob.pd_planks.live_update:
        _pending.add(ob.name)
        if not bpy.app.timers.is_registered(tick):bpy.app.timers.register(tick,first_interval=.15)

class PDPlankPoint(bpy.types.PropertyGroup):
    obj:bpy.props.PointerProperty(name='Proximity Object',type=bpy.types.Object,update=update)
    enabled:bpy.props.BoolProperty(name='Enabled',default=True,update=update)

# здесь хранятся значения, которые сохраняются вместе с объектом
class PDPlankProps(bpy.types.PropertyGroup):
    points:bpy.props.CollectionProperty(type=PDPlankPoint)
    mode:bpy.props.EnumProperty(name='Damage Mode',items=[('PROXIMITY','Proximity','Damage at the current object position; no dragging needed'),('PULL','Pull Pins','Move a pin from its original position to pull and tear')],default='PROXIMITY',update=update)
    auto_smooth:bpy.props.BoolProperty(name='Auto Smooth',default=True,update=update)
    smooth_angle:bpy.props.FloatProperty(name='Smooth Angle',default=math.radians(45),min=0,max=math.pi,subtype='ANGLE',update=update)
    bend_depth:bpy.props.FloatProperty(name='Bend Depth',default=.25,min=0,soft_max=2,update=update,description='Downward bend around proximity damage; zero creates an unbent opening')
    width:bpy.props.FloatProperty(name='Width',default=3,min=.1,soft_max=20,update=update)
    length:bpy.props.FloatProperty(name='Length',default=4,min=.1,soft_max=20,update=update)
    plank_width:bpy.props.FloatProperty(name='Plank Width',default=.18,min=.02,soft_max=.5,update=update)
    thickness:bpy.props.FloatProperty(name='Top Thickness',default=.025,min=.002,soft_max=.2,update=update)
    gap:bpy.props.FloatProperty(name='Gap',default=.006,min=0,soft_max=.03,update=update)
    horizontal:bpy.props.BoolProperty(name='Horizontal',default=False,update=update)
    under:bpy.props.BoolProperty(name='Under Layer',default=True,update=update)
    under_thickness:bpy.props.FloatProperty(name='Under Thickness',default=.04,min=.002,soft_max=.2,update=update)
    under_spacing:bpy.props.FloatProperty(name='Under Spacing',default=.65,min=.05,soft_max=2,update=update)
    seed:bpy.props.IntProperty(name='Seed',default=0,min=0,update=update)
    radius:bpy.props.FloatProperty(name='Damage Radius',default=.65,min=.05,soft_max=3,update=update,description='Radius of damage around a proximity object, or influence around a pull pin')
    break_distance:bpy.props.FloatProperty(name='Break Distance',default=.25,min=.01,soft_max=2,update=update,description='Handle travel needed to start tearing boards; smaller values break sooner')
    splinters:bpy.props.FloatProperty(name='Splinter Length',default=.18,min=0,soft_max=.5,update=update)
    resolution:bpy.props.IntProperty(name='Segments per Plank',default=48,min=12,max=160,update=update)
    live_update:bpy.props.BoolProperty(name='Live Update',default=True,update=update)
    material:bpy.props.PointerProperty(name='Top Wood',type=bpy.types.Material,update=update)
    under_material:bpy.props.PointerProperty(name='Under Wood',type=bpy.types.Material,update=update)

def rebuild(ob):
    global _busy
    if _busy:return
    _busy=True
    try:
        p=ob.pd_planks;verts=[];faces=[];mats=[]
        pins=[]
        linked={item.obj for item in p.points if item.obj is not None}
        targets=[item.obj for item in p.points if item.obj is not None and item.enabled]
        targets += [pin for pin in bpy.data.objects if pin.get('pd_plank_owner')==ob.name and pin not in linked]
        for pin in dict.fromkeys(targets):
            now=ob.matrix_world.inverted()@pin.matrix_world.translation
            rest=Vector(pin.get('pd_plank_rest',now))
            pins.append((now,Vector((0,0,-p.bend_depth))) if p.mode=='PROXIMITY' else (rest,now-rest))
        def warp(co):
            delta=Vector((0,0,0));total=0.
            for rest,move in pins:
                r=(co-rest).length/p.radius if p.mode=='PROXIMITY' else (Vector((co.x,co.y,0))-Vector((rest.x,rest.y,0))).length/p.radius
                w=max(0,1-r*r)**2
                delta+=move*w;total+=w
            return co+delta/max(1,total)
        def layer(horizontal,spacing,width,thick,z,mat):
            span=p.length if horizontal else p.width;long=p.width if horizontal else p.length
            count=min(400,max(1,math.ceil(span/spacing)))
            for board in range(count):
                center=-span*.5+(board+.5)*span/count
                half=min(width,span/count)*.5-p.gap*.5
                if half<=0:continue
                n=p.resolution;cross=6;grid={};cells=[]
                rng=random.Random(p.seed+board*971+mat*829)
                tears=[rng.uniform(-p.splinters,p.splinters) for _ in range(cross+1)]
                def coord(i,j):
                    a=center-half+2*half*j/cross;b=-long*.5+long*i/n
                    return Vector((b,a,z)) if horizontal else Vector((a,b,z))
                for i in range(n):
                    for j in range(cross):
                        mid=(coord(i,j)+coord(i+1,j+1))*.5;remove=False
                        for rest,move in pins:
                            if p.mode=='PULL' and move.length<=p.break_distance:continue
                            dx=mid.x-rest.x;dy=mid.y-rest.y
                            if horizontal:dx+=tears[j]
                            else:dy+=tears[j]
                            radius=p.radius*.7 if p.mode=='PROXIMITY' else p.radius*min(.8,(move.length-p.break_distance)/p.radius)
                            dz=mid.z-rest.z if p.mode=='PROXIMITY' else 0.
                            if dx*dx+dy*dy+dz*dz<radius*radius:remove=True;break
                        if not remove:cells.append(((i,j),(i+1,j),(i+1,j+1),(i,j+1)))
                used={k for c in cells for k in c}
                for key in sorted(used):
                    co=coord(*key);grid[key]=(len(verts),len(verts)+1)
                    verts.extend((tuple(warp(co)),tuple(warp(co-Vector((0,0,thick))))))
                edges={}
                for cell in cells:
                    top=[grid[k][0] for k in cell]
                                                                         
                    if not horizontal:top.reverse()
                    faces.append(top);mats.append(mat)
                    faces.append([v+1 for v in reversed(top)]);mats.append(mat)
                    for a,b in zip(top,top[1:]+top[:1]):
                        key=tuple(sorted((a,b)))
                        if key in edges:del edges[key]
                        else:edges[key]=(a,b)
                for a,b in edges.values():faces.append((b,a,a+1,b+1));mats.append(mat)
        layer(p.horizontal,p.plank_width,p.plank_width,p.thickness,0,0)
        if p.under:layer(not p.horizontal,p.under_spacing,p.plank_width,p.under_thickness,-p.thickness,1)
        mesh=bpy.data.meshes.new('Plank Surface');mesh.from_pydata(verts,[],faces);mesh.update()
        for mat in (p.material,p.under_material or p.material):
            if mat:mesh.materials.append(mat)
        for f,m in zip(mesh.polygons,mats):f.material_index=min(m,max(0,len(mesh.materials)-1))
        for face in mesh.polygons:face.use_smooth=p.auto_smooth
        if p.auto_smooth:mesh.set_sharp_from_angle(angle=p.smooth_angle)
        uv=mesh.uv_layers.new(name='ProjectDamage_Zones')
        for f in mesh.polygons:
            for li in f.loop_indices:
                co=mesh.vertices[mesh.loops[li].vertex_index].co;uv.data[li].uv=(co.x,co.y)
        old=ob.data;ob.data=mesh
        if old.users==0:bpy.data.meshes.remove(old)
    finally:_busy=False

def tick():
    names=list(_pending);_pending.clear()
    for name in names:
        ob=bpy.data.objects.get(name)
        if ob and ob.get('pd_plank_surface') and ob.pd_planks.live_update:
            try:rebuild(ob)
            except Exception:
                import traceback;traceback.print_exc()
    return None

@persistent
def changed(scene,depsgraph):
    from . import live_input_state
    if _busy:return
    moved={event.id.original for event in depsgraph.updates if isinstance(event.id.original,bpy.types.Object) and event.is_updated_transform}
    moved={obj for obj in moved if live_input_state.changed(obj, 'planks')}
    if not moved:return
    for target in scene.objects:
        if not target.get('pd_plank_surface') or not target.pd_planks.live_update:continue
        refs={item.obj for item in target.pd_planks.points if item.enabled and item.obj}
        if refs & moved or any(o.get('pd_plank_owner')==target.name for o in moved if o!=target):
            _pending.add(target.name)
    if _pending and not bpy.app.timers.is_registered(tick):bpy.app.timers.register(tick,first_interval=.15)

@persistent
def clear(*args):
    _pending.clear()
    if bpy.app.timers.is_registered(tick):bpy.app.timers.unregister(tick)

# действие «Plank Surface»
class PD_OT_planks(bpy.types.Operator):
    bl_idname='project_damage.plank_surface';bl_label='Plank Surface';bl_options={'REGISTER','UNDO'}
    action:bpy.props.EnumProperty(items=[('ADD','Add',''),('UPDATE','Update',''),('PIN','Add Pin',''),('AUTO','Automatic Pins',''),('LINK','Link Selected',''),('SLOT','Add Object Slot',''),('REMOVE','Unlink','')])
    target:bpy.props.StringProperty()
    index:bpy.props.IntProperty()
    def execute(self,context):
        ob=bpy.data.objects.get(self.target) if self.target else owner(context)
        if self.action=='ADD':
            ob=bpy.data.objects.new('Plank Surface',bpy.data.meshes.new('Plank Surface'));context.collection.objects.link(ob)
            ob.location=context.scene.cursor.location;ob['pd_plank_surface']=True
            from . import quality_defaults
            quality_defaults.standalone(ob,'plank_surface')
            name='Wood Planks Shader | Project Damage'
            mat=bpy.data.materials.get(name)
            if mat is None:
                from pathlib import Path
                path=Path(__file__).parent/'blends/wood_mat.blend'
                if path.exists():
                    with bpy.data.libraries.load(str(path),link=False) as (src,dst):
                        if name in src.materials:dst.materials=[name]
                    mat=bpy.data.materials.get(name)
            if mat:ob.pd_planks.material=mat
            for selected in context.selected_objects:selected.select_set(False)
            ob.select_set(True);context.view_layer.objects.active=ob
        if ob is None:return {'CANCELLED'}
        if self.action=='SLOT':ob.pd_planks.points.add()
        if self.action=='REMOVE' and 0<=self.index<len(ob.pd_planks.points):
            point=ob.pd_planks.points[self.index].obj
            if point and point.get('pd_plank_owner')==ob.name:
                del point['pd_plank_owner']
            ob.pd_planks.points.remove(self.index)
        if self.action=='LINK':
            for point in context.selected_objects:
                if point!=ob and not any(item.obj==point for item in ob.pd_planks.points):
                    ob.pd_planks.points.add().obj=point
        if self.action in {'PIN','AUTO'}:
            coords=[ob.matrix_world.inverted()@context.scene.cursor.location] if self.action=='PIN' else [Vector((0,-ob.pd_planks.length*.22,0)),Vector((0,ob.pd_planks.length*.22,0))]
            for co in coords:
                pin=bpy.data.objects.new('Plank Damage Pin',None);context.collection.objects.link(pin)
                pin.parent=ob;pin.location=co;pin.empty_display_type='SPHERE';pin.empty_display_size=.12
                pin['pd_plank_owner']=ob.name;pin['pd_plank_rest']=list(co)
                ob.pd_planks.points.add().obj=pin
            for selected in context.selected_objects:selected.select_set(False)
            pin.select_set(True);context.view_layer.objects.active=pin
        rebuild(ob);return {'FINISHED'}

# собираем видимые настройки этого раздела
def draw(layout,context):
    from . import effect_layout
    from .material_library import draw_property
    ob=owner(context);p=ob.pd_planks
    layout.label(text=ob.name,icon='MOD_BUILD')
    box=effect_layout.section(layout,'General');col=box.column(align=True)
    col.prop(p,'live_update',toggle=True,icon='FILE_REFRESH')
    col.prop(p,'width');col.prop(p,'length');col.prop(p,'horizontal',toggle=True)
    col.prop(p,'seed');quality_ui.draw(col, p,'resolution')
    col.prop(p,'auto_smooth',toggle=True,icon='SHADING_RENDERED')
    if p.auto_smooth:col.prop(p,'smooth_angle')
    box=effect_layout.section(layout,'Top Planks');col=box.column(align=True)
    draw_property(col,p,'material',text='Top Wood');col.prop(p,'plank_width');col.prop(p,'thickness');col.prop(p,'gap')
    box=effect_layout.section(layout,'Under Layer');box.prop(p,'under',toggle=True)
    if p.under:
        col=box.column(align=True);draw_property(col,p,'under_material',text='Under Wood');col.prop(p,'under_thickness');col.prop(p,'under_spacing')
    box=effect_layout.section(layout,'Damage Objects');col=box.column(align=True)
    row=col.row(align=True);row.scale_y=1.5;ui_image_hints.prop(row,p,'mode',expand=True)
    col.prop(p,'radius')
    if p.mode=='PULL':col.prop(p,'break_distance')
    else:col.prop(p,'bend_depth')
    col.prop(p,'splinters')
    for i,item in enumerate(p.points):
        row=box.row(align=True);row.prop(item,'enabled',text='',icon='HIDE_OFF' if item.enabled else 'HIDE_ON')
        row.prop(item,'obj',text='')
        op=row.operator('project_damage.plank_surface',text='',icon='REMOVE');op.action='REMOVE';op.index=i;op.target=ob.name
    row=box.row(align=True)
    for action,label,icon in (('SLOT','Link Object','ADD'),('LINK','From Selected','RESTRICT_SELECT_OFF')):
        op=row.operator('project_damage.plank_surface',text=label,icon=icon);op.action=action;op.target=ob.name

    row=box.row(align=True);row.operator('project_damage.plank_surface',text='Add Empty at Cursor',icon='ADD').action='PIN'
    row.operator('project_damage.plank_surface',text='Automatic Pins',icon='EMPTY_DATA').action='AUTO'
    box.operator('project_damage.plank_surface',text='Update',icon='FILE_REFRESH').action='UPDATE'
    box.label(text='Damage follows the object position' if p.mode=='PROXIMITY' else 'Move a pin to bend and tear the boards',icon='INFO')

CLASSES=(PDPlankPoint,PDPlankProps,PD_OT_planks)
# подключаем свойства и действия этого модуля к Blender
def register():
    for cls in CLASSES:bpy.utils.register_class(cls)
    bpy.types.Object.pd_planks=bpy.props.PointerProperty(type=PDPlankProps)
    bpy.app.handlers.depsgraph_update_post.append(changed)
    bpy.app.handlers.undo_pre.append(clear);bpy.app.handlers.load_pre.append(clear)
# убираем подключения модуля при выключении аддона
def unregister():
    clear()
    for handlers,fn in ((bpy.app.handlers.depsgraph_update_post,changed),(bpy.app.handlers.undo_pre,clear),(bpy.app.handlers.load_pre,clear)):
        if fn in handlers:handlers.remove(fn)
    if hasattr(bpy.types.Object,'pd_planks'):del bpy.types.Object.pd_planks
    for cls in reversed(CLASSES):bpy.utils.unregister_class(cls)
