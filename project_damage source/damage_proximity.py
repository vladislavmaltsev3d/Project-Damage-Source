# объекты задают контур и глубину локального повреждения
import math
import bpy
import numpy as np
from mathutils import Vector
from mathutils.bvhtree import BVHTree
from . import live, ownership

OBJECT_MODULES = {'concrete','lath','brick'}

MODULES = ('concrete','wood','lath','bullet','ground','brick','rusty')

def enabled(props, module):
    if props.all_off:
        return False
    on = getattr(props,module+'_enable',False)
    if module == 'concrete':
        on = on or props.concrete_v2_enable
    if module == 'lath':
        on = on or props.lath_v2_enable or props.lath_riven_enable or props.lath_framing_enable
    if module == 'brick':
        on = on or props.brick_v2_enable
    return on and getattr(props,module+'_mode') == '2'

def references(props):
    for module in MODULES:
        if enabled(props,module):
            for item in getattr(props,module+'_proximity'):
                if item.enabled and item.obj is not None:
                    yield item.obj

def object_poll(self, obj):
    return obj != self.id_data and getattr(obj,'project_damage_og_object',None) != self.id_data and not obj.get('project_damage_source_uid') and not obj.get('project_damage_result')

class ProjectDamageProximityPoint(bpy.types.PropertyGroup):
    obj: bpy.props.PointerProperty(name='Object',type=bpy.types.Object,poll=object_poll,update=live.touch_prop)
    enabled: bpy.props.BoolProperty(name='Enabled',default=True,update=live.touch_prop)
    use_size: bpy.props.BoolProperty(name='Radius from Empty Size',default=True,update=live.touch_prop,description='On: an Empty uses its display size and scale as the damage radius. Off: use Radius. Scaling a point updates its damage area')
    radius: bpy.props.FloatProperty(name='Radius (cm)',default=35,min=.02,soft_max=200,update=live.touch_prop,description='Radius of damage around this point, in cm. Low: a small local opening; high: a wider opening. The object must touch the source within this radius')

class PROJECT_DAMAGE_UL_proximity(bpy.types.UIList):
    def draw_item(self,context,layout,data,item,icon,active_data,active_propname,index):
        row=layout.row(align=True)
        row.prop(item,'enabled',text='')
        row.prop(item,'obj',text='',icon='MESH_CUBE' if item.obj is None or item.obj.type!='EMPTY' else 'EMPTY_AXIS')

# действие «Damage Placement Object»
class PROJECT_DAMAGE_OT_proximity_point(bpy.types.Operator):
    bl_idname='project_damage.proximity_point'
    bl_label='Damage Placement Object'
    bl_description='Choose a damage primitive at the 3D Cursor, pick an existing object or manage linked placement objects'
    bl_options={'REGISTER','UNDO'}
    module: bpy.props.EnumProperty(items=[(m,m.title(),'') for m in MODULES])
    action: bpy.props.EnumProperty(items=[('ADD','Add at 3D Cursor','Create a mesh cutter for Concrete, Lath or Brick damage, or a point for other effects, at the 3D Cursor'),('LINK','Add Object Slot','Pick an existing object'),('REMOVE','Remove','Remove the selected link'),('DELETE','Delete Object','Delete this generated placement object'),('SELECT','Select Object','Select the placement object in the viewport')])
    shape: bpy.props.StringProperty(default='CUBE',options={'HIDDEN','SKIP_SAVE'})
    size: bpy.props.FloatProperty(name='Size',default=.3,min=.01,max=4.,subtype='FACTOR',description='Initial cutter size as a share of the source object dimensions. Scale the created object to change its damage area')
    @classmethod
    def poll(cls,context):
        source=ownership.context_source(context)
        return source is not None and source.type=='MESH' and context.mode=='OBJECT'
    def invoke(self,context,event):
        if self.action!='ADD' or self.module not in OBJECT_MODULES:
            return self.execute(context)
        from .boolean_break import PRIMITIVES
        module=self.module;size=self.size
        def draw_menu(menu,context):
            menu.layout.operator_context='EXEC_DEFAULT'
            for shape,name,icon in PRIMITIVES:
                if shape=='PLANE':name='Plane (with thickness)'
                op=menu.layout.operator('project_damage.proximity_point',text=name,icon=icon)
                op.module=module;op.action='ADD';op.shape=shape;op.size=size
        context.window_manager.popup_menu(draw_menu,title='Add Damage Primitive',icon='MESH_CUBE')
        return {'FINISHED'}
    def execute(self,context):
        source=ownership.context_source(context)
        if source is None:return {'CANCELLED'}
        props=source.project_damage;items=getattr(props,self.module+'_proximity');key=self.module+'_proximity_index'
        if self.action in {'ADD','LINK'}:
            item=items.add()
            if self.action=='ADD':
                collection_name=source.name+' | '+self.module.title()+(' Damage Objects' if self.module in OBJECT_MODULES else ' Proximity')
                collection=bpy.data.collections.get(collection_name)
                if collection is None:
                    collection=bpy.data.collections.new(collection_name);context.scene.collection.children.link(collection)
                if self.module in OBJECT_MODULES:
                    from . import boolean_break
                    shapes={key:name for key,name,icon in boolean_break.PRIMITIVES}
                    if self.shape not in shapes:
                        items.remove(len(items)-1)
                        self.report({'ERROR'},'Unknown damage primitive')
                        return {'CANCELLED'}
                    corners=[Vector(c) for c in source.bound_box]
                    extent=max(max(c[i] for c in corners)-min(c[i] for c in corners) for i in range(3))
                    radius=max(1e-5,.5*self.size*extent)
                    mesh=boolean_break.primitive_mesh(self.shape,radius,seed=len(items))
                    label=shapes[self.shape].split(' (')[0]
                    obj=bpy.data.objects.new(source.name+'_'+self.module.title()+'_'+label,mesh)
                    mesh.name=obj.name;collection.objects.link(obj)
                    obj.matrix_world=source.matrix_world.copy()
                    obj[boolean_break.PRIMITIVE_KEY]=self.shape
                    if self.shape=='PLANE':
                        mod=obj.modifiers.new('Damage Cutter Thickness','SOLIDIFY')
                        mod.thickness=radius*.2;mod.offset=-1.;mod.use_even_offset=True
                    obj.display_type='WIRE'
                    boolean_break.primitive_material(obj)
                else:
                    obj=bpy.data.objects.new(source.name+'_'+self.module.title()+'_Point',None);collection.objects.link(obj)
                    obj.empty_display_type='SPHERE';obj.empty_display_size=.35/max(context.scene.unit_settings.scale_length,1e-9)
                obj.location=context.scene.cursor.location;obj.color=(1.,.34,.04,1.)
                obj['project_damage_proximity_owner']=source;obj['project_damage_proximity_module']=self.module
                item.obj=obj
                if self.module in OBJECT_MODULES:
                    for selected in tuple(context.selected_objects):selected.select_set(False)
                    obj.select_set(True);context.view_layer.objects.active=obj
            setattr(props,key,len(items)-1)
        elif items:
            index=min(max(0,getattr(props,key)),len(items)-1);obj=items[index].obj
            if self.action=='SELECT' and obj is not None:
                for selected in context.selected_objects:selected.select_set(False)
                obj.hide_set(False);obj.select_set(True);context.view_layer.objects.active=obj
            elif self.action in {'REMOVE','DELETE'}:
                items.remove(index);setattr(props,key,min(index,max(0,len(items)-1)))
                if self.action=='DELETE' and obj is not None and obj.get('project_damage_proximity_owner')==source:
                    bpy.data.objects.remove(obj,do_unlink=True)
        live.touch_prop(props,context)
        return {'FINISHED'}

# собираем видимые настройки этого раздела
def draw(layout,props,module):
    from . import material_controls
    box=layout.box();material_controls.centered_heading(box,'Damage Objects' if module in OBJECT_MODULES else 'Proximity Objects',icon='MOD_BOOLEAN')
    key=module+'_proximity';items=getattr(props,key);idx=module+'_proximity_index'
    row=box.row();row.template_list('PROJECT_DAMAGE_UL_proximity',module,props,key,props,idx,rows=min(4,max(2,len(items))))
    buttons=row.column(align=True);buttons.operator_context='INVOKE_DEFAULT'
    for action,icon in [('ADD','MESH_CUBE' if module in OBJECT_MODULES else 'ADD'),('LINK','EYEDROPPER'),('REMOVE','REMOVE')]:
        op=buttons.operator('project_damage.proximity_point',text='',icon=icon);op.module=module;op.action=action
    if items:
        item=items[min(max(0,getattr(props,idx)),len(items)-1)]
        if module in OBJECT_MODULES and (item.obj is None or item.obj.type!='EMPTY'):
            box.label(text='Object shape sets the outline and cutting depth',icon='INFO')
        else:
            if module in OBJECT_MODULES:box.label(text='Legacy point: pick a mesh to use its volume',icon='INFO')
            box.prop(item,'use_size')
            if not item.use_size or item.obj is None or item.obj.type!='EMPTY':box.prop(item,'radius')
        row=box.row(align=True)
        op=row.operator('project_damage.proximity_point',text='Select Object',icon='RESTRICT_SELECT_OFF');op.module=module;op.action='SELECT'
        op=row.operator('project_damage.proximity_point',text='',icon='TRASH');op.module=module;op.action='DELETE'
    else:
        box.label(text='Add a primitive at the 3D Cursor or pick an object' if module in OBJECT_MODULES else 'Add a point at the 3D Cursor or pick an object',icon='INFO')

def paths(context,source,module,points,polys,factor):
    props=source.project_damage
    vertices=np.asarray(points,dtype=np.float64)
    if len(vertices)==0 or len(polys)==0:return []
    faces=[tuple(map(int,row[:3] if row[2]==row[3] else row)) for row in polys]
    tree=BVHTree.FromPolygons([Vector(p) for p in vertices],faces,all_triangles=False)
    inverse=source.matrix_world.inverted_safe();result=[]
    for item in getattr(props,module+'_proximity'):
        obj=item.obj
        if not item.enabled or obj is None:continue
        relative=inverse@obj.matrix_world;center=relative.translation*factor
        local_scale=max(abs(v) for v in relative.to_scale())
        radius=item.radius
        if item.use_size and obj.type=='EMPTY':radius=obj.empty_display_size*factor*max(1e-8,abs(local_scale))
        hit,normal,face,distance=tree.find_nearest(center)
        if hit is None or distance>radius:continue
        radius=math.sqrt(max(0.,radius*radius-distance*distance))
        if radius<1e-5:continue
        tangent=normal.orthogonal().normalized();bitangent=normal.cross(tangent).normalized()
        circle=[hit+radius*(math.cos(2*math.pi*i/48)*tangent+math.sin(2*math.pi*i/48)*bitangent) for i in range(49)]
        if module=='bullet':
            circle=[hit]
        result.append(np.ascontiguousarray(circle,dtype=np.float64))
    return result

def add_properties(cls):
    for module in MODULES:
        prop=cls.__annotations__[module+'_mode'].keywords
        prop['items']=tuple(prop['items'])+(('2','Objects' if module in OBJECT_MODULES else 'Proximity','Use mesh objects to define the damage contour and depth' if module in OBJECT_MODULES else 'Place damage around linked proximity objects'),)
        cls.__annotations__[module+'_proximity']=bpy.props.CollectionProperty(type=ProjectDamageProximityPoint)
        cls.__annotations__[module+'_proximity_index']=bpy.props.IntProperty(default=0,min=0,options={'SKIP_SAVE'})

CLASSES=(ProjectDamageProximityPoint,PROJECT_DAMAGE_UL_proximity,PROJECT_DAMAGE_OT_proximity_point)
def _actual(cls):
    base=bpy.types.PropertyGroup if issubclass(cls,bpy.types.PropertyGroup) else bpy.types.UIList if issubclass(cls,bpy.types.UIList) else bpy.types.Operator
    return base.bl_rna_get_subclass_py(cls.__name__,None)
# подключаем свойства и действия этого модуля к Blender
def register():
    made=[]
    try:
        for cls in CLASSES:
            if _actual(cls) is None:
                bpy.utils.register_class(cls);made.append(cls)
    except Exception:
        for cls in reversed(made):bpy.utils.unregister_class(cls)
        raise
# убираем подключения модуля при выключении аддона
def unregister():
    for cls in reversed(CLASSES):
        actual=_actual(cls)
        if actual is not None:bpy.utils.unregister_class(actual)


def fill(params,props,core,source,geometry=None):
    if source is None or not hasattr(core,'SurfaceDamagePoint'):
        return
    from .mesh_io import read_mesh
    from .params import unit_factor
    if not any(enabled(props,m) for m in ('concrete','lath','brick')):
        return
    if geometry is None:
        points,polys=read_mesh(source.data);factor=unit_factor(bpy.context);points=points*factor
    else:
        points,polys,factor=geometry
    faces=[tuple(map(int,row[:3] if row[2]==row[3] else row)) for row in polys]
    tree=BVHTree.FromPolygons([Vector(p) for p in points],faces,all_triangles=False)
    inverse=source.matrix_world.inverted_safe()
    for module in ('concrete','lath','brick'):
        if not enabled(props,module):continue
        regions=[]
        for item in getattr(props,module+'_proximity'):
            obj=item.obj
            if obj is None or not item.enabled:continue
            relative=inverse@obj.matrix_world;center=relative.translation*factor
            if obj.type!='EMPTY':
                if not hasattr(core.SurfaceDamagePoint(),'mesh_points'):
                    raise ValueError('Damage Objects needs the updated core. Save the scene and restart Blender')
                point=mesh_region(bpy.context,source,obj,core,factor)
                regions.append(point)
                continue
            radius=item.radius
            if item.use_size and obj.type=='EMPTY':radius=obj.empty_display_size*factor*max(abs(v) for v in relative.to_scale())
            hit,normal,face,distance=tree.find_nearest(center)
            if hit is None or distance>=radius:continue
            point=core.SurfaceDamagePoint();point.x,point.y,point.z=hit;point.nx,point.ny,point.nz=normal
            point.radius=math.sqrt(max(0.,radius*radius-distance*distance));regions.append(point)
        getattr(params,module).proximity=regions

def hash_params(h,params,hash_value):
    for module in ('concrete','lath','brick'):
        block=getattr(params,module)
        if not block.enable or block.mode!=2:continue
        hash_value(h,module+'_proximity');regions=getattr(block,'proximity',[])
        hash_value(h,len(regions))
        for region in regions:
            for name in ('x','y','z','nx','ny','nz','radius'):hash_value(h,getattr(region,name))
            for name,dtype in [('mesh_points',np.float64),('mesh_triangles',np.int32)]:
                values=np.asarray(getattr(region,name,[]),dtype=dtype);hash_value(h,len(values));h.update(values.tobytes())


def mesh_region(context,source,obj,core,factor):
    from . import mesh_prep
    if obj.type not in {'MESH','CURVE','SURFACE','META','FONT'}:
        raise ValueError('Choose a closed mesh object for the damage contour')
    deps=context.evaluated_depsgraph_get();evaluated=obj.evaluated_get(deps)
    mesh=evaluated.to_mesh(preserve_all_data_layers=False,depsgraph=deps)
    try:
        if mesh is None or not mesh.vertices or not mesh.polygons:
            raise ValueError('Damage object '+obj.name+' has no mesh geometry')
        mesh.calc_loop_triangles()
        points=np.empty(len(mesh.vertices)*3,dtype=np.float64);mesh.vertices.foreach_get('co',points);points=points.reshape(-1,3)
        triangles=np.empty(len(mesh.loop_triangles)*3,dtype=np.int32);mesh.loop_triangles.foreach_get('vertices',triangles);triangles=triangles.reshape(-1,3)
        matrix=source.matrix_world.inverted_safe()@evaluated.matrix_world
        basis=np.asarray(matrix,dtype=np.float64);points=(points@basis[:3,:3].T+basis[:3,3])*factor
        if matrix.to_3x3().determinant()<0:triangles=triangles[:,[0,2,1]]
        rows=np.column_stack((triangles,triangles[:,2])).astype(np.int32)
        if not mesh_prep.is_closed_manifold(rows):
            raise ValueError('Damage object '+obj.name+' must be a closed mesh; close its boundary or add Solidify')
        if not np.isfinite(points).all():raise ValueError('Damage object coordinates must be finite')
        point=core.SurfaceDamagePoint();point.mesh_points=points.ravel().tolist();point.mesh_triangles=triangles.ravel().tolist()
        point.x,point.y,point.z=np.mean(points,axis=0);point.radius=float(np.linalg.norm(points.max(axis=0)-points.min(axis=0)))
        return point
    finally:
        evaluated.to_mesh_clear()
