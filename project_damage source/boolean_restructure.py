# перестраиваем внутренние детали после перемещения фрагментов
# SPDX-License-Identifier: GPL-3.0-or-later
"""Re-fit native Boolean fill geometry to manually moved loose fragments.
Rest coordinates are saved on the mesh, so repeated Restructure is idempotent.
No armature, dependency handler or full damage rebuild is involved.
"""
import bpy
import numpy as np
from mathutils import Matrix, Vector
from mathutils.bvhtree import BVHTree
from . import ownership

REST = 'project_damage_structure_rest'
BODY = 'project_damage_structure_body'
BASE = 'project_damage_structure_matrix'
STAMPS = ('project_damage_rebar_of', 'project_damage_pipes_of', 'project_damage_cables_of')


def outputs(source):
    uid = source.get('project_damage_uid')
    return [o for o in bpy.data.objects if uid and o.type == 'MESH'
            and any(o.get(key) == uid for key in STAMPS)]


def flat(matrix):
    return [v for row in matrix for v in row]


def matrix(values):
    return Matrix([values[i:i+4] for i in range(0,16,4)])


def bodies(source, result):
    from .boolean_break import fragments_of
    return [result] + fragments_of(source)


def rest_matrix(source, body, result):
    values = body.get(BASE)
    if values is not None and len(values) == 16:
        return matrix(values)
                                                                             
    com = body.get('project_damage_bfrag_com')
    return source.matrix_world @ Matrix.Translation(Vector(com)) if com is not None else result.matrix_world.copy()


def capture(source, result):
    """Cheap build hook, after ALL structure generators have finished."""
    from .boolean_break import wanted, fragments_of
    if not wanted(source) or not fragments_of(source):
        return
    for body in bodies(source, result):
        com = body.get('project_damage_bfrag_com')
        rest = source.matrix_world @ Matrix.Translation(Vector(com)) if com is not None else body.matrix_world
        body[BASE] = flat(rest)
    for obj in outputs(source):
        mesh = obj.data
        old_template = obj.get('pd_structure_template')
        if old_template is not None:
            del obj['pd_structure_template']
            if old_template.users == 0: bpy.data.meshes.remove(old_template)
        attr = mesh.attributes.get(REST)
        if attr is None:
            attr = mesh.attributes.new(REST, 'FLOAT_VECTOR', 'POINT')
        coords = np.array([obj.matrix_world @ v.co for v in mesh.vertices], dtype=np.float32)
        attr.data.foreach_set('vector', coords.ravel())
        if 'project_damage_tube_recipes' in mesh:
            mesh['project_damage_tube_recipe_matrix'] = flat(obj.matrix_world)
        obj[BODY] = source.get('project_damage_uid')
                                                       
        for name in ('pd_structure_a','pd_structure_b','pd_structure_weight'):
            old = mesh.attributes.get(name)
            if old is not None: mesh.attributes.remove(old)


def _trees(source, result):
    trees = []
    for obj in bodies(source, result):
        rest = rest_matrix(source, obj, result)
        vertices = [rest @ v.co for v in obj.data.vertices]
        polygons = [tuple(f.vertices) for f in obj.data.polygons]
        if vertices and polygons:
            trees.append((obj, rest, BVHTree.FromPolygons(vertices, polygons)))
    return trees


def _nearest(point, trees):
    hits = []
    for index, (_, _, tree) in enumerate(trees):
        hit = tree.find_nearest(point)
        if hit[0] is not None:
            hits.append((hit[3], index))
    hits.sort()
    if not hits:
        raise RuntimeError('No fragment surfaces to attach the structure to')
    da, a = hits[0]
    if len(hits) == 1: return a, a, 1.0
    db, b = hits[1]
                                                                            
                                                                             
    weight = db / max(da + db, 1e-12) if da + db > 1e-12 else 1.0
    weight = weight*weight*(3-2*weight)
    return a, b, weight


def _attribute(mesh, name, kind, values):
    attr = mesh.attributes.get(name)
    if attr is None: attr = mesh.attributes.new(name, kind, 'POINT')
    attr.data.foreach_set('value', values)


def _fit_beams(obj, coords, trees):
    """Keep boards and studs straight, fitting their two ends to the solids.
    Length changes along the principal axis; the cross section stays intact.
    """
    from .boolean_break import _components
    comp, count = _components(obj.data)
    updated = coords.copy()
    deltas = [body.matrix_world @ rest.inverted_safe() for body,rest,_ in trees]
    for k in range(count):
        ids = np.unique([v for face in obj.data.polygons if comp[face.index] == k for v in face.vertices])
        if not len(ids): continue
        points = coords[ids];center = points.mean(axis=0);local = points-center
        _, axes = np.linalg.eigh(local.T @ local)
        direction = Vector(axes[:,-1]);along = local @ np.asarray(direction)
        lo,hi = float(along.min()),float(along.max())
        a = Vector(center)+direction*lo;b = Vector(center)+direction*hi
        ia,_,_ = _nearest(a,trees);ib,_,_ = _nearest(b,trees)
        na,nb = deltas[ia] @ a,deltas[ib] @ b
        chord = nb-na
        if chord.length < 1e-9 or hi-lo < 1e-9:
            updated[ids] = np.array([deltas[ia] @ Vector(point) for point in points])
            continue
        rotation = direction.rotation_difference(chord.normalized())
        for index,point,t in zip(ids,points,along):
            offset = Vector(point)-(Vector(center)+direction*float(t))
            updated[index] = na+chord*((float(t)-lo)/(hi-lo))+rotation @ offset
    return updated


def _bindings(obj, coords, trees, rigid):
    from .boolean_break import _components
    mesh = obj.data
    a = np.zeros(len(coords), dtype=np.int32)
    b = np.zeros(len(coords), dtype=np.int32)
    w = np.zeros(len(coords), dtype=np.float32)
    if rigid:
        comp, count = _components(mesh)
        for k in range(count):
            ids = np.unique([v for face in mesh.polygons if comp[face.index] == k for v in face.vertices])
            if not len(ids): continue
            owner, _, _ = _nearest(Vector(coords[ids].mean(axis=0)), trees)
            a[ids] = owner; b[ids] = owner; w[ids] = 1
    else:
        for i, point in enumerate(coords):
            a[i], b[i], w[i] = _nearest(Vector(point), trees)
    _attribute(mesh,'pd_structure_a','INT',a)
    _attribute(mesh,'pd_structure_b','INT',b)
    _attribute(mesh,'pd_structure_weight','FLOAT',w)
                                                                            
    obj['pd_structure_count'] = len(trees)
    for i,(body, rest, _) in enumerate(trees):
        obj['pd_structure_owner_%d'%i] = body
        obj['pd_structure_base_%d'%i] = flat(rest)
    obj['pd_structure_rigid'] = rigid
    return a,b,w


def _template(obj):
    mesh = obj.get('pd_structure_template')
    if mesh is not None:
        return mesh
                                                                           
                                                                        
    mesh = obj.data.copy()
    attr = mesh.attributes.get(REST)
    if attr is None:
        attr = mesh.attributes.new(REST, 'FLOAT_VECTOR', 'POINT')
        attr.data.foreach_set('vector', np.array([obj.matrix_world @ v.co for v in mesh.vertices],dtype=np.float32).ravel())
    mesh.name = obj.name + '_StructureTemplate'
    obj['pd_structure_template'] = mesh
    return mesh


def _bar_descriptions(template):
    """Recover native centreline rings, never the stretched surface."""
    coords=np.empty(len(template.vertices)*3,dtype=np.float64)
    template.attributes[REST].data.foreach_get('vector',coords);coords=coords.reshape(-1,3)
    attr=template.attributes.get('rebar_id')
    if attr is None:raise RuntimeError('Regenerate Boolean Break once to record its native bar IDs')
    ids=np.empty(len(template.polygons),dtype=np.int32);attr.data.foreach_get('value',ids)
    exposure=template.attributes.get('rebar_exposed')
    ex=np.ones(len(coords),dtype=np.float32)
    if exposure is not None:exposure.data.foreach_get('value',ex)
    groups={}
    for face,key in zip(template.polygons,ids):groups.setdefault(int(key),[]).append(face)
    descriptions=[]
    for key,faces in groups.items():
        verts=np.unique([v for f in faces for v in f.vertices])
        sides=next((int(f.vertices[3]-f.vertices[0]) for f in faces
                    if len(f.vertices)==4 and f.vertices[3]-f.vertices[0]>=4),0)
        if not sides or sides>64:raise RuntimeError('Unsupported native bar layout; regenerate Boolean Break first')
        closed=len(verts)%sides==0
        count=len(verts)//sides if closed else (len(verts)-2)//sides
        if count<2 or count*sides+(0 if closed else 2)!=len(verts):
            raise RuntimeError('Bar topology was edited; regenerate Boolean Break before Restructure')
        rings=coords[verts[:count*sides]].reshape(count,sides,3)
        centers=rings.mean(axis=1)
        radii=np.linalg.norm(rings-centers[:,None,:],axis=2)
                                                                  
        radius=float(np.median(np.min(radii,axis=1)))
        descriptions.append((key,centers,rings[0,0]-centers[0],radius,sides,
                             ex[verts[:count*sides]].reshape(count,sides).mean(axis=1),
                             faces[0].material_index,closed,radii))
    return descriptions


def _regenerate_rebar(source,obj,trees):
    from . import structure_rebuild as generator, preview_quality
    template=_template(obj);p=source.project_damage
    if 'pd_rebar_template_resolution' not in template:
        template['pd_rebar_template_resolution']=preview_quality.rebar_resolution(p,'rebar_resolution')
    template_resolution=float(template['pd_rebar_template_resolution'])
    descriptions=_bar_descriptions(template)
    vertices=[];faces=[];uvs=[];exposed=[];bar_ids=[];materials=[]
    kind=int(p.rebar_type)
    deltas=[np.asarray(body.matrix_world @ rest.inverted_safe(),dtype=float) for body,rest,_ in trees]
    for key,centers,normal,radius,sides,ex,mat,closed,radii in descriptions:
        if kind==3:radius/=1+template_resolution*(.886-1)
        elif kind==4:radius=float(np.median(np.max(radii,axis=1)))
        ia,_,_=_nearest(Vector(centers[0]),trees)
        ib,_,_=_nearest(Vector(centers[-1]),trees)
                                                                            
        if ex[0]>.5 and ex[-1]<=.5:ia=ib
        elif ex[-1]>.5 and ex[0]<=.5:ib=ia
        elif ex[0]>.5 and ex[-1]>.5:
            ia,_,_=_nearest(Vector(centers.mean(axis=0)),trees);ib=ia
        if closed:ib=ia                                                 
        path=generator.route(centers,deltas[ia],deltas[ib],radius)
        normal=deltas[ia][:3,:3]@normal
        sides=max(4,min(64,p.rebar_sides))
        if kind==3:sides=max(16,sides)
        elif kind==4:sides=max(36,sides)
        built=generator.sweep(path,radius,sides,kind,p.rebar_rib_height,p.rebar_rib_pitch,
                              p.rebar_twist_pitch,normal,ex,max(.1,p.rebar_detail),closed,
                              preview_quality.rebar_resolution(p,'rebar_resolution'))
        if built is None:continue
        v,f,uv,e,_,_=built
        offset=len(vertices);vertices.extend(v);faces.extend(tuple(offset+i for i in face) for face in f)
        uvs.extend(uv);exposed.extend(e);bar_ids.extend([key]*len(f));materials.extend([mat]*len(f))
    inverse=obj.matrix_world.inverted_safe()
    mesh=bpy.data.meshes.new(obj.name+'_Restructured')
    try:
        mesh.from_pydata([inverse @ Vector(v) for v in vertices],[],faces)
        for material in obj.data.materials:mesh.materials.append(material)
        mesh.polygons.foreach_set('material_index',np.asarray(materials,dtype=np.int32))
        mesh.polygons.foreach_set('use_smooth',np.ones(len(faces),dtype=bool))
        attr=mesh.attributes.new('rebar_id','INT','FACE');attr.data.foreach_set('value',bar_ids)
        attr=mesh.attributes.new('rebar_exposed','FLOAT','POINT');attr.data.foreach_set('value',exposed)
                                                                            
        for saved in template.attributes:
            if saved.domain!='FACE' or saved.name in {'rebar_id','material_index'} or saved.data_type not in {'INT','BOOLEAN','FLOAT'}:
                continue
            dtype=bool if saved.data_type=='BOOLEAN' else np.int32 if saved.data_type=='INT' else np.float32
            values=np.empty(len(saved.data),dtype=dtype);saved.data.foreach_get('value',values)
            if len(values) and np.all(values==values[0]) and mesh.attributes.get(saved.name) is None:
                copied=mesh.attributes.new(saved.name,saved.data_type,'FACE')
                copied.data.foreach_set('value',np.full(len(faces),values[0],dtype=dtype))
                                                                              
        layer_names=[layer.name for layer in template.uv_layers] or ['ProjectDamage_Zones']
        for name in layer_names:
            layer=mesh.uv_layers.new(name=name)
            layer.data.foreach_set('uv',np.asarray([uv for face in uvs for uv in face],dtype=np.float32).ravel())
        mesh.update()
    except Exception:
        bpy.data.meshes.remove(mesh);raise
    old=obj.data;obj.data=mesh
    if old.users==0:bpy.data.meshes.remove(old)
                                                                 
    for group in obj.vertex_groups:group.add(list(range(len(vertices))),1.,'REPLACE')
    obj.update_tag()


def restructure(context, source):
    from . import build, boolean_break, boolean_stack, wall_stack
    result = build.result_of(source)
    if result is None or not boolean_break.fragments_of(source):
        raise RuntimeError('Enable Separate Fragments and generate Boolean damage first')
    structures = outputs(source)
    if boolean_stack.SERIAL in source:
        row = boolean_stack.edited(source)
        serial = boolean_stack.OUTPUT_OFFSET + row[boolean_stack.INSTANCE] if row else -1
        structures = [obj for obj in structures if obj.get(wall_stack.INSTANCE) == serial]
    if not structures: raise RuntimeError('This Boolean Break has no internal structure')
    trees = None
    done = 0
    for obj in structures:
        if obj.get('project_damage_pipes_of') or obj.get('project_damage_cables_of'):
            if obj.mode!='OBJECT':raise RuntimeError('Exit Edit Mode before restructuring')
            if trees is None:trees=_trees(source,result)
            from . import tube_restructure
            tube_restructure.rebuild(context,source,obj,trees);done+=1
            continue
        if obj.get('project_damage_rebar_of') and source.project_damage.rebar_fill=='3':
            if obj.mode!='OBJECT':raise RuntimeError('Exit Edit Mode before restructuring')
            if trees is None:trees=_trees(source,result)
            from . import framing_restructure
            framing_restructure.rebuild(source,obj,trees);done+=1
            continue
        if obj.get('project_damage_rebar_of') and source.project_damage.rebar_fill=='0':
            if obj.mode!='OBJECT':raise RuntimeError('Exit Edit Mode before restructuring')
            if trees is None:trees=_trees(source,result)
            _regenerate_rebar(source,obj,trees);done+=1
            continue
        mesh = obj.data
        if obj.mode != 'OBJECT': raise RuntimeError('Exit Edit Mode before restructuring')
        attr = mesh.attributes.get(REST)
        if attr is None or attr.domain != 'POINT':
                                                                                  
            attr = mesh.attributes.new(REST, 'FLOAT_VECTOR', 'POINT')
            attr.data.foreach_set('vector', np.array([obj.matrix_world @ v.co for v in mesh.vertices],dtype=np.float32).ravel())
        coords = np.empty(len(mesh.vertices)*3,dtype=np.float32)
        attr.data.foreach_get('vector',coords);coords=coords.reshape(-1,3)
        fill = source.project_damage.rebar_fill if obj.get('project_damage_rebar_of') else None
        if fill in {'2','5'}:
            if trees is None: trees = _trees(source,result)
            world = _fit_beams(obj,coords,trees)
            inverse = obj.matrix_world.inverted_safe()
            mesh.vertices.foreach_set('co',np.array([inverse @ Vector(point) for point in world],dtype=np.float32).ravel())
            mesh.update();obj.update_tag();done+=1
            continue
        rigid = fill == '1'
        names = ('pd_structure_a','pd_structure_b','pd_structure_weight')
        count = obj.get('pd_structure_count',0)
        valid = count and all(mesh.attributes.get(n) is not None for n in names)
        valid = valid and obj.get('pd_structure_rigid') == rigid
        valid = valid and all(obj.get('pd_structure_owner_%d'%i) is not None for i in range(count))
        if valid:
            a=np.empty(len(coords),dtype=np.int32);b=a.copy();w=np.empty(len(coords),dtype=np.float32)
            for name,values in zip(names,(a,b,w)):mesh.attributes[name].data.foreach_get('value',values)
        else:
            if trees is None: trees = _trees(source,result)
            a,b,w = _bindings(obj,coords,trees,rigid)
            count = len(trees)
        delta=[]
        for i in range(count):
            owner=obj['pd_structure_owner_%d'%i]
            delta.append(owner.matrix_world @ matrix(obj['pd_structure_base_%d'%i]).inverted_safe())
        inverse=obj.matrix_world.inverted_safe()
        updated=[]
        for point,ia,ib,weight in zip(coords,a,b,w):
            point=Vector(point)
            updated.append(inverse @ ((delta[int(ia)] @ point)*float(weight)+(delta[int(ib)] @ point)*(1-float(weight))))
        mesh.vertices.foreach_set('co',np.asarray(updated,dtype=np.float32).ravel())
        mesh.update();obj.update_tag();done+=1
    return done


class PROJECT_DAMAGE_OT_boolean_restructure(bpy.types.Operator):
    bl_idname='project_damage.boolean_restructure'
    bl_label='Restructure'
    bl_description='Rebuild internal structures at moved fragment anchors while preserving cross sections, physical detail spacing and material assignments'
    bl_options={'REGISTER','UNDO'}

    def execute(self,context):
        source=ownership.context_source(context)
        if source is None:return {'CANCELLED'}
        try: count=restructure(context,source)
        except Exception as exc:
            self.report({'ERROR'},str(exc));return {'CANCELLED'}
        self.report({'INFO'},'Restructured %d internal structure objects'%count)
        return {'FINISHED'}


# подключаем свойства и действия этого модуля к Blender
def register():
    bpy.utils.register_class(PROJECT_DAMAGE_OT_boolean_restructure)


# убираем подключения модуля при выключении аддона
def unregister():
    cls=getattr(bpy.types,PROJECT_DAMAGE_OT_boolean_restructure.__name__,None)
    if cls is not None:bpy.utils.unregister_class(cls)
