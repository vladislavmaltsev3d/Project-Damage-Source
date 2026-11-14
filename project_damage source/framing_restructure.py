# строим каркас заново между перемещёнными частями
# SPDX-License-Identifier: GPL-3.0-or-later
"""Rebuild stud members after fragment transforms, retaining protected bridges."""
import bpy
import numpy as np
from mathutils import Vector
from mathutils.bvhtree import BVHTree
from . import framing_restructure_math as geometry


def _obstacles(trees):
    out=[]
    for body,_,_ in trees:
        points=[body.matrix_world@v.co for v in body.data.vertices]
        out.append(BVHTree.FromPolygons(points,[tuple(f.vertices) for f in body.data.polygons]))
    return out


def _collision(path,member,obstacles):
    from .structure_rebuild import frames
    lengths=np.linalg.norm(np.diff(path,axis=0),axis=1);total=lengths.sum();along=0.
    normals,bins=frames(path,member['normal'])
    for j,length in enumerate(lengths):
        if length<1e-8:continue
        corners=[(0,0),(1,1),(1,-1),(-1,1),(-1,-1)]
        best=None
        for x,y in corners:
            start=path[j]+normals[j]*member['width']*x+bins[j]*member['depth']*y
            end=path[j+1]+normals[j+1]*member['width']*x+bins[j+1]*member['depth']*y
            ray_length=float(np.linalg.norm(end-start))
            if ray_length<1e-8:continue
            direction=Vector((end-start)/ray_length)
            origin=Vector(start)
            for owner,tree in enumerate(obstacles):
                travelled=0.
                                                                              
                                                                   
                for _ in range(32):
                    hit=tree.ray_cast(origin+direction*travelled,direction,float(ray_length-travelled))
                    if hit[0] is None:break
                    distance=travelled+hit[3];mapped=distance/ray_length*length;s=along+mapped
                    allowed=(owner==member['ia'] and s<=member['embed_a']) or (owner==member['ib'] and total-s<=member['embed_b'])
                    if not allowed:
                        if best is None or mapped<best:best=mapped
                        break
                    travelled=distance+max(1e-7,total*1e-7)
                    if travelled>=ray_length:break
        if best is not None:return along+best
        along+=length
    return None


def _clear_bridge(member,obstacles):
    a,b=member['a'],member['b'];direct=np.array([a,b])
    if _collision(direct,member,obstacles) is None:return direct
    direction=geometry.unit(b-a);width=geometry.unit(member['normal']-direction*np.dot(member['normal'],direction))
    if abs(np.dot(width,direction))>.9:
        width=geometry.unit(np.cross(direction,np.eye(3)[np.argmin(np.abs(direction))]))
    other=geometry.unit(np.cross(direction,width));span=np.linalg.norm(b-a)
                                                                          
                                                                          
    for offset in (max(member['width'],member['depth'])*4,span*.15,span*.35,span*.7):
        for axis in (width,-width,other,-other):
            for side in (.25,.45):
                path=np.array([a,a+(b-a)*side+axis*offset,b-(b-a)*side+axis*offset,b])
                if _collision(path,member,obstacles) is None:return path
    return None


def rebuild(source,obj,trees):
    from .boolean_restructure import _template,REST,_nearest
    from . import structure_rebuild
    template=_template(obj);p=source.project_damage
    coords=np.empty(len(template.vertices)*3,dtype=np.float64)
    template.attributes[REST].data.foreach_get('vector',coords);coords=coords.reshape(-1,3)
    tag=template.attributes.get('rebar_id')
    if tag is None:raise RuntimeError('Regenerate Boolean Framing once before Restructure')
    ids=np.empty(len(template.polygons),dtype=np.int32);tag.data.foreach_get('value',ids)
    parts=np.zeros(len(ids),dtype=np.int32)
    part=template.attributes.get('rebar_part')
    if part is not None:part.data.foreach_get('value',parts)
    grouped={}
    for face,key,part in zip(template.polygons,ids,parts):grouped.setdefault(int(key),[]).append((face,int(part)))
    deltas=[np.asarray(body.matrix_world@rest.inverted_safe(),dtype=float) for body,rest,_ in trees]
    members=[];nails=[];last_member=None
    for key,items in sorted(grouped.items()):
        faces=[f for f,_ in items];vi=np.unique([v for f in faces for v in f.vertices]);points=coords[vi]
        if items[0][1]==1:
                                                                      
                                                                        
            nails.append((vi,faces,last_member,key));continue
        a,b,normal,width,depth=geometry.describe(points)
        ia,_,_=_nearest(Vector(a),trees);ib,_,_=_nearest(Vector(b),trees)
        na=geometry.transformed(deltas[ia],a);nb=geometry.transformed(deltas[ib],b)
        padding=max(width,depth)*2
        ea=trees[ia][2].find_nearest(Vector(a))[3]+padding
        eb=trees[ib][2].find_nearest(Vector(b))[3]+padding
        member=dict(key=key,rest_a=a,rest_b=b,a=na,b=nb,ma=deltas[ia],mb=deltas[ib],ia=ia,ib=ib,
                    width=width,depth=depth,rest_normal=normal,normal=geometry.rotation(deltas[ia])@normal,
                    angle=geometry.angle(deltas[ia],deltas[ib]),mid=(na+nb)*.5,
                    embed_a=ea,embed_b=eb,material=faces[0].material_index)
        members.append(member);last_member=key
    obstacles=_obstacles(trees) if p.frame_restructure_collisions else None
    for m in members:
        # Members attached to one solid retain their authored shape and pose.
        m['collision']=bool(obstacles and _collision(np.array([m['a'],m['b']]),m,obstacles) is not None)
    keep=geometry.select_survivors(members,p.frame_restructure_minimum)
    vertices=[];faces=[];grain=[];material=[];piece_ids=[];out_parts=[];broken=0;preserved=0
    attachments={}
    def emit(path,m,start=False,end=False,interval=(0.,1.)):
        built=geometry.lumber(path,m['width'],m['depth'],m['normal'],m['key'],start,end)
        if built is None:return
        v,f,g=built;base=len(vertices)
                                                                             
                                                                               
        tree=BVHTree.FromPolygons([Vector(point) for point in v],f)
        attachments.setdefault(m['key'],[]).append(dict(path=np.asarray(path),interval=interval,tree=tree))
        vertices.extend(v);faces.extend(tuple(base+i for i in face) for face in f);grain.extend(g)
        material.extend([m['material']]*len(f));piece_ids.extend([m['key']]*len(f));out_parts.extend([0]*len(f))
    for i,m in enumerate(members):
        fail=m['ia']!=m['ib'] and m['angle']>=p.frame_restructure_angle
        fail=fail or m['collision']
        if i in keep:
            path=_clear_bridge(m,obstacles) if obstacles else np.array([m['a'],m['b']])
            if path is None:
                raise RuntimeError('Cannot preserve a collision-free framing connection. Move fragments apart or lower Minimum Connections')
            emit(path,m);preserved+=1
        elif fail:
            for j,path in enumerate(geometry.broken_paths(m)):
                if obstacles:
                                                                                  
                    probe=path if j==0 else path[::-1]
                    collision=_collision(probe,dict(m,ia=m['ia'] if j==0 else m['ib'],ib=-1,embed_b=0),obstacles)
                    if collision is not None:
                        reach=max(0.,collision-max(m['width'],m['depth'])*1.2)
                        if reach<max(m['width'],m['depth'])*.5:continue
                        probe=np.array([probe[0],probe[0]+geometry.unit(probe[1]-probe[0])*reach])
                        path=probe if j==0 else probe[::-1]
                fraction=min(1.,np.linalg.norm(path[-1]-path[0])/max(np.linalg.norm(m['rest_b']-m['rest_a']),1e-9))
                interval=(0.,fraction) if j==0 else (1.-fraction,1.)
                emit(path,m,start=j==1,end=j==0,interval=interval)
            broken+=1
        else:
            emit(np.array([m['a'],m['b']]),m)
    member_by_key={m['key']:m for m in members}
    retained_nails=0;removed_nails=0
    for vi,original,owner,key in nails:
        member=member_by_key.get(owner)
        fitted=geometry.fit_nail(coords[vi],member,attachments.get(owner,[])) if member else None
        if fitted is None:
            removed_nails+=1;continue
        v,segment=fitted
                                                                          
                                                                           
        location,normal,_,distance=segment['tree'].find_nearest(Vector(v[0]))
        if location is None or (Vector(v[0])-location).dot(normal)>1e-7:
            removed_nails+=1;continue
        lookup={int(vertex):len(vertices)+i for i,vertex in enumerate(vi)}
        vertices.extend(v);grain.extend(np.zeros_like(v))
        faces.extend(tuple(lookup[int(i)] for i in f.vertices) for f in original)
        material.extend([f.material_index for f in original]);piece_ids.extend([key]*len(original));out_parts.extend([1]*len(original))
        retained_nails+=1
    mesh=bpy.data.meshes.new(obj.name+'_BrokenFraming')
    try:
        inverse=obj.matrix_world.inverted_safe()
        mesh.from_pydata([inverse@Vector(v) for v in vertices],[],faces)
        for mat in obj.data.materials:mesh.materials.append(mat)
        mesh.polygons.foreach_set('material_index',np.asarray(material,dtype=np.int32))
        for name,values in (('rebar_id',piece_ids),('rebar_part',out_parts)):
            attr=mesh.attributes.new(name,'INT','FACE');attr.data.foreach_set('value',values)
        attr=mesh.attributes.new('framing_grain','FLOAT_VECTOR','POINT');attr.data.foreach_set('vector',np.asarray(grain,dtype=np.float32).ravel())
        attr=mesh.attributes.new('rebar_exposed','FLOAT','POINT');attr.data.foreach_set('value',np.ones(len(vertices),dtype=np.float32))
        for saved in template.attributes:
            if saved.domain!='FACE' or saved.data_type not in {'INT','BOOLEAN','FLOAT'} or mesh.attributes.get(saved.name) is not None:
                continue
            dtype=bool if saved.data_type=='BOOLEAN' else np.int32 if saved.data_type=='INT' else np.float32
            values=np.empty(len(saved.data),dtype=dtype);saved.data.foreach_get('value',values)
            if len(values) and np.all(values==values[0]):
                attr=mesh.attributes.new(saved.name,saved.data_type,'FACE')
                attr.data.foreach_set('value',np.full(len(faces),values[0],dtype=dtype))
                                                                            
        for layer_name in [uv.name for uv in template.uv_layers] or ['ProjectDamage_Zones']:
            uv=mesh.uv_layers.new(name=layer_name)
            uv.data.foreach_set('uv',np.asarray([(grain[i][0],grain[i][1]) for f in faces for i in f],dtype=np.float32).ravel())
        mesh.update();mesh.set_sharp_from_angle(angle=np.radians(45))
    except Exception:
        bpy.data.meshes.remove(mesh);raise
    old=obj.data;obj.data=mesh
    if old.users==0:bpy.data.meshes.remove(old)
    for group in obj.vertex_groups:group.add(list(range(len(vertices))),1.,'REPLACE')
    obj['pd_framing_broken_members']=broken;obj['pd_framing_kept_connections']=preserved
    obj['pd_framing_attached_nails']=retained_nails;obj['pd_framing_removed_nails']=removed_nails
    obj.update_tag()
    return broken,preserved
