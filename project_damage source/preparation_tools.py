# готовим отдельную копию меша: закрываем отверстия или добавляем толщину
from . import output_collections
import bpy
import bmesh
import numpy as np
import math
from mathutils import Matrix
from bpy.props import BoolProperty, FloatProperty, PointerProperty, StringProperty

SOLIDIFY = 'Project Damage · Preparation Thickness'
SMOOTH = 'Project Damage · Preparation Auto Smooth'
SMOOTH_GROUP = 'Preparation Smooth by Angle | Project Damage'
PENDING = 'project_damage_preparation_target'


def prepared(obj):
    return bool(obj and obj.type == 'MESH' and obj.project_damage_preparation.active and not obj.get('project_damage_result_of'))


def target(context):
    obj = context.active_object
    if obj is None or obj.type != 'MESH' or context.mode != 'OBJECT':
        return None
    from . import ownership, build, damage_gen_common
    if prepared(obj):
        return obj
    if ownership.source_of(obj) != obj or build.result_of(obj) is not None or damage_gen_common.kind_of(obj) is not None:
        return None
    return obj


def _thickness(self, context):
    obj = self.id_data
    if prepared(obj):
        mod = obj.modifiers.get(SOLIDIFY)
        if mod is not None:
            mod.thickness = self.thickness


def _smooth_modifier(obj):
    mod = obj.modifiers.get(SMOOTH)
    if mod is not None:
        return mod
    group = bpy.data.node_groups.get(SMOOTH_GROUP)
    if group is None:
        group = bpy.data.node_groups.new(SMOOTH_GROUP, 'GeometryNodeTree')
        group.is_modifier = True
        group.interface.new_socket(name='Geometry', in_out='INPUT', socket_type='NodeSocketGeometry')
        angle = group.interface.new_socket(name='Angle', in_out='INPUT', socket_type='NodeSocketFloat')
        angle.subtype = 'ANGLE'
        angle.default_value = math.radians(30)
        angle.min_value = 0.0
        angle.max_value = math.pi
        angle.description = 'Auto Smooth angle. At 0 degrees all bends stay sharp; at 180 degrees all connected faces are smoothed'
        group.interface.new_socket(name='Geometry', in_out='OUTPUT', socket_type='NodeSocketGeometry')
        nodes, links = group.nodes, group.links
        start = nodes.new('NodeGroupInput'); start.location = (-600, 100)
        faces = nodes.new('GeometryNodeSetShadeSmooth'); faces.domain = 'FACE'; faces.location = (-350, 100)
        faces.inputs['Shade Smooth'].default_value = True
        edge_angle = nodes.new('GeometryNodeInputMeshEdgeAngle'); edge_angle.location = (-600, -150)
        compare = nodes.new('FunctionNodeCompare'); compare.data_type = 'FLOAT'; compare.operation = 'LESS_EQUAL'; compare.location = (-350, -150)
        edges = nodes.new('GeometryNodeSetShadeSmooth'); edges.domain = 'EDGE'; edges.location = (-100, 100)
        end = nodes.new('NodeGroupOutput'); end.location = (150, 100)
        links.new(start.outputs['Geometry'], faces.inputs['Geometry'])
        links.new(faces.outputs['Geometry'], edges.inputs['Geometry'])
        links.new(edge_angle.outputs['Unsigned Angle'], compare.inputs[0])
        links.new(start.outputs['Angle'], compare.inputs[1])
        links.new(compare.outputs['Result'], edges.inputs['Shade Smooth'])
        links.new(edges.outputs['Geometry'], end.inputs['Geometry'])
    mod = obj.modifiers.new(SMOOTH, 'NODES')
    mod.node_group = group
    angle = next(s for s in group.interface.items_tree if s.item_type == 'SOCKET' and s.in_out == 'INPUT' and s.name == 'Angle')
    inputs = getattr(getattr(mod, 'properties', None), 'inputs', None)
    if inputs is not None:
        getattr(inputs, angle.identifier).value = angle.default_value
    else:
        mod[angle.identifier] = angle.default_value
        mod.id_properties_ui(angle.identifier).update(subtype='ANGLE', min=0.0, max=math.pi, default=angle.default_value, description=angle.description)
    return mod


class ProjectDamagePreparation(bpy.types.PropertyGroup):
    active: BoolProperty(default=False)
    original: PointerProperty(type=bpy.types.Object)
    source: PointerProperty(type=bpy.types.Object)
    closed: BoolProperty(default=False)
    original_hidden: BoolProperty(default=False)
    original_render_hidden: BoolProperty(default=False)
    thickness: FloatProperty(name='Thickness', default=0.1, min=0.00001, soft_max=2.0,
        subtype='DISTANCE', unit='LENGTH', update=_thickness,
        description='Thickness added to the preparation copy. Small values make a thin shell; larger values create a deeper volume. The original mesh is kept unchanged')
    report: StringProperty(default='')


def _components(bm):
    groups = []
    unseen = set(bm.faces)
    while unseen:
        first = unseen.pop(); group = {first}; todo = [first]
        while todo:
            for e in todo.pop().edges:
                for f in e.link_faces:
                    if f in unseen:
                        unseen.remove(f);group.add(f);todo.append(f)
        groups.append(group)
    return groups


def _loops(bm):
    edges = {e for e in bm.edges if e.is_boundary}
    adjacency = {}
    for e in edges:
        for v in e.verts:adjacency.setdefault(v, []).append(e)
    if any(len(a) != 2 for a in adjacency.values()):
        raise ValueError('Open edges branch or meet at one vertex. Separate overlapping edges before closing this mesh')
    loops = []
    while edges:
        start = next(iter(edges)).verts[0]; v = start; ring = []; walked = []
        while True:
            ring.append(v)
            e = next((e for e in adjacency[v] if e in edges), None)
            if e is None:raise ValueError('An open border could not be traced')
            edges.remove(e);walked.append(e);v = e.other_vert(v)
            if v == start:break
        loops.append((ring, walked))
    return loops


def _close_borders(mesh):
    bm = bmesh.new()
    try:
        bm.from_mesh(mesh)
        if not bm.faces:raise ValueError('The mesh has no faces')
        coords = np.array([v.co[:] for v in bm.verts]);scale = max(float(np.linalg.norm(np.ptp(coords,axis=0))),1e-6)
        tolerance = scale * 1e-7
        bmesh.ops.remove_doubles(bm, verts=list(bm.verts), dist=tolerance)
        loose = [e for e in bm.edges if not e.link_faces]
        if loose:bmesh.ops.delete(bm,geom=loose,context='EDGES')
        loose = [v for v in bm.verts if not v.link_faces]
        if loose:bmesh.ops.delete(bm,geom=loose,context='VERTS')
        if any(len(e.link_faces)>2 for e in bm.edges):
            raise ValueError('Edges shared by more than two faces cannot be closed safely. Remove internal or overlapping faces first')
        original_faces = set(bm.faces)
        groups = _components(bm);component = {f:i for i,g in enumerate(groups) for f in g}
        rings = _loops(bm)
        data = []
        for verts, edges in rings:
            co = np.array([v.co[:] for v in verts]);center = co.mean(axis=0)
            _, _, basis = np.linalg.svd(co-center, full_matrices=False)
            normal = basis[-1];diag = max(float(np.linalg.norm(np.ptp(co,axis=0))),tolerance)
            data.append((co,center,normal,diag,component[edges[0].link_faces[0]]))
        candidates = []
        for i,(a,ca,na,da,ga) in enumerate(data):
            for j in range(i+1,len(data)):
                b,cb,nb,db,gb=data[j]
                if ga==gb or abs(float(na@nb))<0.98 or min(da,db)/max(da,db)<0.75:continue
                delta=cb-ca;gap=abs(float(delta@na));lateral=delta-na*(delta@na)
                if gap<tolerance*10 or gap>max(da,db)*0.6 or np.linalg.norm(lateral)>min(da,db)*0.12:continue
                aa=a-ca;bb=b-cb
                aa=aa-na*np.outer(aa@na,np.ones(3))
                bb=bb-na*np.outer(bb@na,np.ones(3))
                def distance(points, poly):
                    points=points[np.linspace(0,len(points)-1,min(len(points),64)).astype(int)]
                    start=poly;end=np.roll(poly,-1,axis=0);edge=end-start
                    t=np.clip(np.sum((points[:,None]-start[None])*edge[None],axis=2)/np.maximum(np.sum(edge*edge,axis=1),tolerance**2),0,1)
                    return float(np.sqrt(np.min(np.sum((points[:,None]-start[None]-t[:,:,None]*edge[None])**2,axis=2),axis=1)).max())
                mismatch=max(distance(aa,bb),distance(bb,aa))
                if mismatch>min(da,db)*0.05:continue
                candidates.append((gap+4*mismatch,i,j))
        used=set();bridges=0
        for _,i,j in sorted(candidates):
            if i in used or j in used:continue
            edges=rings[i][1]+rings[j][1]
            faces=bmesh.ops.bridge_loops(bm,edges=edges,use_pairs=True).get('faces',[])
            if faces:
                used.update((i,j));bridges+=1
        remaining=[e for e in bm.edges if e.is_boundary]
        caps=bmesh.ops.holes_fill(bm,edges=remaining,sides=0).get('faces',[]) if remaining else []
        new_faces=[f for f in bm.faces if f not in original_faces]
        for f in new_faces:
            neighbours=[g for e in f.edges for g in e.link_faces if g in original_faces]
            if neighbours:f.material_index=neighbours[0].material_index
            f.smooth=False
        ngons=[f for f in new_faces if len(f.verts)>4]
        if ngons:bmesh.ops.triangulate(bm,faces=ngons)
        bmesh.ops.recalc_face_normals(bm,faces=list(bm.faces))
        if any(not e.is_manifold for e in bm.edges):raise ValueError('Some borders could not be closed. The original and preparation copy were not changed')
        for group in _components(bm):
            volume=0.0
            origin=next(iter(group)).verts[0].co
            for f in group:
                vs=list(f.verts);a=vs[0].co-origin
                for k in range(1,len(vs)-1):volume+=a.dot((vs[k].co-origin).cross(vs[k+1].co-origin))/6
            if abs(volume)<max(scale**3*1e-12,1e-18):
                raise ValueError('A flat surface cannot enclose volume by closing its border. Use Add Thickness instead')
            if volume<0:bmesh.ops.reverse_faces(bm,faces=list(group))
        result=mesh.copy();bm.to_mesh(result);result.update()
        return result,f'Closed: {bridges} paired borders joined, {len(caps)} openings filled.'
    finally:bm.free()


def close_mesh(mesh):
    try:
        return _close_borders(mesh)
    except ValueError as exc:
        if not str(exc).startswith(('Some borders', 'A flat surface')):raise
        from . import preparation_shell
        result=preparation_shell.layered_wall(mesh)
        if result is not None:return result
        raise


def _select(context,obj):
    for other in context.selected_objects:other.select_set(False)
    obj.hide_set(False);obj.hide_select=False;obj.select_set(True);context.view_layer.objects.active=obj


def _copy(context,original):
    from . import init_parenting
    graph=context.evaluated_depsgraph_get()
    mesh=bpy.data.meshes.new_from_object(original.evaluated_get(graph),preserve_all_data_layers=True,depsgraph=graph)
    if mesh is None or not mesh.polygons:raise ValueError('The selected object has no evaluated faces')
    copy=original.copy();copy.data=mesh;copy.name=original.name+' · Prepared'
    copy.animation_data_clear();copy.modifiers.clear();copy.constraints.clear();copy.parent=None
    world=original.evaluated_get(graph).matrix_world.copy()
    init_parenting.bake_mesh(mesh,world.to_3x3().to_4x4())
    copy.matrix_world=Matrix.Translation(world.translation)
    for key in list(copy.keys()):
        if key.startswith(('project_damage','edgy')):del copy[key]
    copy.project_damage_og_object=None
    excluded = {c for c in (getattr(context.scene.rigidbody_world, 'collection', None), getattr(context.scene.rigidbody_world, 'constraints', None)) if c is not None}
    destination = next((c for c in original.users_collection if c not in excluded and not c.get(output_collections.SOURCE_COLLECTION)), context.scene.collection)
    destination.objects.link(copy)
    if copy.rigid_body is not None:
        with context.temp_override(object=copy, active_object=copy, selected_objects=[copy], selected_editable_objects=[copy]):
            bpy.ops.rigidbody.object_remove()
    if copy.rigid_body_constraint is not None:
        with context.temp_override(object=copy, active_object=copy, selected_objects=[copy], selected_editable_objects=[copy]):
            bpy.ops.rigidbody.constraint_remove()
    base=mesh.copy();base.name=original.name+' · Preparation Base'
    source=bpy.data.objects.new(original.name+' · PD Source',base)
    collection=bpy.data.collections.get('Project Damage · Sources')
    if collection is None:
        collection=bpy.data.collections.new('Project Damage · Sources');context.scene.collection.children.link(collection)
    elif collection.name not in context.scene.collection.children:
        if collection not in list(context.scene.collection.children_recursive):context.scene.collection.children.link(collection)
    collection[output_collections.SOURCE_COLLECTION]=True
    destination = copy.users_collection[0]
    if not destination.is_embedded_data:
        source[output_collections.OUTPUT_COLLECTION]=destination
    collection.objects.link(source);source.matrix_world=copy.matrix_world
    source.hide_set(True);source.hide_render=True;source.hide_select=True
    p=copy.project_damage_preparation;p.active=True;p.original=original;p.source=source
    p.original_hidden=original.hide_get();p.original_render_hidden=original.hide_render
    original.hide_set(True);original.hide_render=True
    _select(context,copy)
    return copy


def reuse_target(source):
    obj=source.get(PENDING)
    return obj if isinstance(obj,bpy.types.Object) and prepared(obj) and obj.project_damage_preparation.source==source else None


def initialize(operator,context,obj):
    from . import live,build,init_parenting
    p=obj.project_damage_preparation;source=p.source
    if source is None:raise ValueError('Preparation source was removed; create the prepared copy again')
    previous=live._APPLYING;live._APPLYING=True
    old_source=source.data;old_world=source.matrix_world.copy();mesh=None
    old_result=obj.data;original_fake=old_result.use_fake_user;old_result.use_fake_user=True
    mods=[(m,m.show_viewport,m.show_render) for m in obj.modifiers]
    try:
        graph=context.evaluated_depsgraph_get()
        mesh=bpy.data.meshes.new_from_object(obj.evaluated_get(graph),preserve_all_data_layers=True,depsgraph=graph)
        if mesh is None or not mesh.polygons:raise ValueError('Preparation produced an empty mesh')
        world=obj.evaluated_get(graph).matrix_world.copy()
        init_parenting.bake_mesh(mesh,world.to_3x3().to_4x4())
        source.data=mesh;source.matrix_world=Matrix.Translation(world.translation)
        source[PENDING]=obj
        for m,_,_ in mods:m.show_viewport=False;m.show_render=False
        status=operator.initialize(context,source)
        if status!={'FINISHED'}:raise ValueError('Project Damage initialization could not finish')
        obj.modifiers.clear();p.active=False
        if PENDING in source:del source[PENDING]
        source.hide_set(True);source.hide_render=True
        _select(context,obj)
        if old_source.users==0:bpy.data.meshes.remove(old_source)
        return status
    except Exception:
        source.data=old_source;source.matrix_world=old_world
        if PENDING in source:del source[PENDING]
        if obj.data!=old_result:
            failed=obj.data;obj.data=old_result
            if not failed.users:bpy.data.meshes.remove(failed)
        for key in (build.RESULT_STAMP,):
            if key in obj:del obj[key]
        obj.project_damage_og_object=None
        for m,visible,render in mods:m.show_viewport=visible;m.show_render=render
        if mesh is not None and not mesh.users:bpy.data.meshes.remove(mesh)
        raise
    finally:
        old_result.use_fake_user=original_fake
        if not old_result.users:bpy.data.meshes.remove(old_result)
        live._APPLYING=previous


# действие «Prepare Mesh»
class PROJECT_DAMAGE_OT_prepare_mesh(bpy.types.Operator):
    bl_idname='project_damage.prepare_mesh'
    bl_label='Prepare Mesh'
    bl_options={'REGISTER','UNDO'}
    action:StringProperty(default='CLOSE')

    @classmethod
    def poll(cls,context):return target(context) is not None

    @classmethod
    def description(cls,context,properties):
        return {'CLOSE':'Make a separate preparation copy and close open borders. Matching front/back borders are bridged; other openings are capped. A single plane needs Thickness instead',
            'REMOVE_CLOSE':'Remove closing faces and restore the preparation base. Thickness remains editable',
            'SOLIDIFY':'Add Solidify and Auto Smooth to a preparation copy. Adjust Thickness, Offset and smoothing angle before Add Project Damage; the original mesh stays unchanged',
            'REMOVE_SOLIDIFY':'Remove thickness from the preparation copy',
            'CANCEL':'Delete the preparation copy and its internal source, and restore visibility of the original'}.get(properties.action,'Prepare the selected mesh')

    def execute(self,context):
        obj=target(context);created=False;new_mesh=None
        try:
            if self.action=='CLOSE':
                if prepared(obj):base=obj.project_damage_preparation.source.data
                else:
                    graph=context.evaluated_depsgraph_get()
                    base=bpy.data.meshes.new_from_object(obj.evaluated_get(graph),preserve_all_data_layers=True,depsgraph=graph)
                try:new_mesh,message=close_mesh(base)
                finally:
                    if not prepared(obj) and base.users==0:bpy.data.meshes.remove(base)
            if not prepared(obj):obj=_copy(context,obj);created=True
            p=obj.project_damage_preparation
            if self.action=='CLOSE':
                if created:
                    from .init_parenting import bake_mesh
                    bake_mesh(new_mesh,p.original.matrix_world.to_3x3().to_4x4())
                old=obj.data;obj.data=new_mesh;new_mesh=None
                if old.users==0:bpy.data.meshes.remove(old)
                p.closed=True;p.report=message
            elif self.action=='REMOVE_CLOSE':
                old=obj.data;obj.data=p.source.data.copy()
                if old.users==0:bpy.data.meshes.remove(old)
                p.closed=False;p.report='Closed removed. Thickness can still be adjusted.'
            elif self.action=='SOLIDIFY':
                mod=obj.modifiers.get(SOLIDIFY)
                if mod is None:
                    mod=obj.modifiers.new(SOLIDIFY,'SOLIDIFY')
                    mod.solidify_mode='EXTRUDE';mod.use_even_offset=True;mod.use_rim=True;mod.offset=-1.0;mod.thickness=p.thickness
                _smooth_modifier(obj)
                p.report='Solidify and Auto Smooth stay editable until Add Project Damage.'
            elif self.action=='REMOVE_SOLIDIFY':
                mod=obj.modifiers.get(SOLIDIFY)
                if mod:obj.modifiers.remove(mod)
                smooth=obj.modifiers.get(SMOOTH)
                if smooth:obj.modifiers.remove(smooth)
            elif self.action=='CANCEL':
                original=p.original;source=p.source;hidden=p.original_hidden;render=p.original_render_hidden
                for other in (obj,source):
                    if other is not None:
                        data=other.data;bpy.data.objects.remove(other,do_unlink=True)
                        if data.users==0:bpy.data.meshes.remove(data)
                if original is not None:
                    original.hide_render=render;_select(context,original);original.hide_set(hidden)
                return {'FINISHED'}
            _select(context,obj)
            return {'FINISHED'}
        except Exception as exc:
            if new_mesh is not None and not new_mesh.users:bpy.data.meshes.remove(new_mesh)
            self.report({'ERROR'},str(exc));return {'CANCELLED'}


# собираем видимые настройки этого раздела
def draw(layout,context):
    from .material_controls import centered_heading
    box=layout.box();centered_heading(box,'Preparation Tools',icon='MODIFIER')
    warning = box.box()
    warning.alert = True
    warning.label(text='Mesh must have thickness', icon='ERROR')
    warning.label(text='and be closed.')
    obj=target(context)
    col=box.column(align=True);col.enabled=obj is not None
    p=obj.project_damage_preparation if prepared(obj) else None
    row=col.row(align=True)
    if p and p.closed:
        row.label(text='Closed',icon='CHECKMARK')
        row.operator('project_damage.prepare_mesh',text='',icon='TRASH').action='REMOVE_CLOSE'
    else:row.operator('project_damage.prepare_mesh',text='Make Closed',icon='MESH_CUBE').action='CLOSE'
    mod=obj.modifiers.get(SOLIDIFY) if p else None
    row=col.row(align=True)
    if mod:
        row.prop(mod,'thickness',text='Thickness')
        row.operator('project_damage.prepare_mesh',text='',icon='TRASH').action='REMOVE_SOLIDIFY'
        col.prop(mod,'offset',text='Offset',slider=True)
        smooth=obj.modifiers.get(SMOOTH)
        if smooth and smooth.node_group:
            col.label(text='Auto Smooth',icon='MOD_SMOOTH')
            controls=col.column(align=True);controls.enabled=smooth.show_viewport
            angle=next((s for s in smooth.node_group.interface.items_tree if s.item_type=='SOCKET' and s.in_out=='INPUT' and s.name=='Angle'),None)
            if angle:
                inputs=getattr(getattr(smooth,'properties',None),'inputs',None)
                if inputs is not None:controls.prop(getattr(inputs,angle.identifier),'value',text='Smooth Angle')
                else:controls.prop(smooth,'["'+angle.identifier+'"]',text='Smooth Angle')
        else:col.operator('project_damage.prepare_mesh',text='Add Auto Smooth',icon='MOD_SMOOTH').action='SOLIDIFY'
    else:row.operator('project_damage.prepare_mesh',text='Add Thickness',icon='MOD_SOLIDIFY').action='SOLIDIFY'
    if p:
        col.label(text='Adjust the copy, then Add Project Damage.',icon='INFO')
        col.operator('project_damage.prepare_mesh',text='Discard Preparation',icon='LOOP_BACK').action='CANCEL'
    else:col.label(text='Creates a copy; keeps the original mesh.',icon='DUPLICATE')


# подключаем свойства и действия этого модуля к Blender
def register():
    bpy.utils.register_class(ProjectDamagePreparation)
    bpy.types.Object.project_damage_preparation=PointerProperty(type=ProjectDamagePreparation)
    bpy.utils.register_class(PROJECT_DAMAGE_OT_prepare_mesh)


# убираем подключения модуля при выключении аддона
def unregister():
    if PROJECT_DAMAGE_OT_prepare_mesh.is_registered:bpy.utils.unregister_class(PROJECT_DAMAGE_OT_prepare_mesh)
    if hasattr(bpy.types.Object,'project_damage_preparation'):del bpy.types.Object.project_damage_preparation
    if ProjectDamagePreparation.is_registered:bpy.utils.unregister_class(ProjectDamagePreparation)
