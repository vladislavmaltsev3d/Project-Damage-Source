# заново строим трубы и кабели между перемещёнными фрагментами
# SPDX-License-Identifier: GPL-3.0-or-later
"""Replay native pipe/cable builders against posed Boolean fragments.
Only centrelines/anchors are transported; no surface vertices are stretched.
"""
import json
import numpy as np
import bpy
from mathutils import Vector
from mathutils.bvhtree import BVHTree
from . import tube_restructure_math as geometry

RECIPES='project_damage_tube_recipes'
FRAME='project_damage_tube_recipe_matrix'


def record(buf,path,normal,start_free,end_free,slot,key,state,**extra):
    buf.restructure_recipes.append(dict(path=np.array(path,copy=True),normal=np.array(normal,copy=True),
        start_free=bool(start_free),end_free=bool(end_free),slot=int(slot),key=int(key),rng_state=state,**extra))


def write_recipes(mesh,buf,factor):
    items=[]
    for recipe in buf.restructure_recipes:
        item=dict(recipe)
        item['path']=(recipe['path']/factor).tolist()
        item['normal']=geometry.unit(recipe['normal']/factor).tolist()
        items.append(item)
    if items:mesh[RECIPES]=json.dumps(dict(version=1,space='LOCAL',items=items),separators=(',',':'))


def _rest_points(template):
    from .boolean_restructure import REST
    coords=np.empty(len(template.vertices)*3,dtype=np.float64)
    template.attributes[REST].data.foreach_get('vector',coords)
    return coords.reshape(-1,3)


def _held(point,trees):
    from . import pipes
    from .boolean_restructure import _nearest
    owner,_,_=_nearest(Vector(point),trees)
    tree=trees[owner][2]
    return pipes._inside(tree,point) or tree.find_nearest(Vector(point))[3]<1e-6


def _infer(template,kind,S,trees,factor):
    """Legacy scenes: recover native rings from the original REST attribute.
    Native hollow shells have two ring arrays; solid sheaths have cap poles.
    Secondary wires/cores are regenerated from their main pipe/cable.
    """
    from .boolean_break import _components
    points=_rest_points(template);comp,count=_components(template)
    grouped={}
    for face,key in zip(template.polygons,comp):grouped.setdefault(int(key),[]).append(face)
    recipes=[]
    for key,faces in sorted(grouped.items(),key=lambda item:min(item[1][0].vertices)):
        vi=np.unique([i for f in faces for i in f.vertices])
        first=next((f for f in faces if len(f.vertices)==4),None)
        if first is None:continue
        sides=int(first.vertices[3]-first.vertices[0])
        if not 4<=sides<=128:continue
        hollow=all(len(f.vertices)==4 for f in faces) and len(vi)%(sides*2)==0
        if kind=='PIPE' and not hollow:continue
        if kind=='CABLE' and (hollow or (len(vi)-2)%sides):continue
        rings=len(vi)//(sides*2) if hollow else (len(vi)-2)//sides
        if rings<2:continue
        outer=points[vi[:rings*sides]].reshape(rings,sides,3)
        centers=outer.mean(axis=1)
        normal=geometry.unit(outer[min(1,rings-1),0]-centers[min(1,rings-1)])
        radii=np.linalg.norm(outer-centers[:,None,:],axis=2)
        if kind=='CABLE':
                                                                              
                                                                             
            if np.median(np.max(radii,axis=1))*factor<S.hw*.65:continue
        start_free=not _held(centers[0],trees);end_free=not _held(centers[-1],trees)
        if kind=='CABLE':
                                                                              
                                                                               
            strip=min(S.strip/factor,geometry.cumulative(centers)[-1]*.4)
            if start_free:centers=np.vstack((centers[0]-geometry.unit(centers[1]-centers[0])*strip,centers))
            if end_free:centers=np.vstack((centers,centers[-1]+geometry.unit(centers[-1]-centers[-2])*strip))
        recipes.append(dict(path=centers.tolist(),normal=normal.tolist(),start_free=start_free,
                            end_free=end_free,slot=int(faces[0].material_index),key=key))
    if not recipes:raise RuntimeError('Regenerate Boolean %s once to record its tube paths'%kind.title())
    template[RECIPES]=json.dumps(dict(version=1,space='WORLD',items=recipes),separators=(',',':'))
    return recipes


def _recipes(obj,template,kind,S,trees,factor):
    stored=template.get(RECIPES)
    if not stored:return _infer(template,kind,S,trees,factor)
    data=json.loads(stored);items=data['items']
    if data.get('space')=='WORLD':return items
    frame=np.asarray(obj.matrix_world,dtype=float)
    values=template.get(FRAME)
    if values is not None:frame=np.asarray(values,dtype=float).reshape(4,4)
    out=[]
    for original in items:
        recipe=dict(original)
        recipe['path']=geometry.pose(frame,np.asarray(original['path'])).tolist()
        recipe['normal']=geometry.unit(frame[:3,:3]@original['normal']).tolist()
        out.append(recipe)
    return out


def _current_bvh(trees,factor):
    vertices=[];faces=[]
    for body,_,_ in trees:
        base=len(vertices);mw=body.matrix_world
        vertices.extend([mw@v.co*factor for v in body.data.vertices])
        mirrored=mw.to_3x3().determinant()<0
        for f in body.data.polygons:
            indices=tuple(base+i for i in f.vertices)
            faces.append(tuple(reversed(indices)) if mirrored else indices)
    return BVHTree.FromPolygons(vertices,faces) if faces else None


def _owners(recipe,trees,factor):
    from .boolean_restructure import _nearest
    path=np.asarray(recipe['path'],dtype=float)*factor
    ia,_,_=_nearest(Vector(path[0]/factor),trees)
    ib,_,_=_nearest(Vector(path[-1]/factor),trees)
    if recipe['start_free'] and not recipe['end_free']:ia=ib
    if recipe['end_free'] and not recipe['start_free']:ib=ia
    matrices=[]
    for index in (ia,ib):
        body,rest,_=trees[index]
        delta=np.array(body.matrix_world@rest.inverted_safe(),dtype=float);delta[:3,3]*=factor
        matrices.append(delta)
    return path,matrices[0],matrices[1]


def _middle_held(recipes,trees):
    """A cable can be held in the middle, with two torn ends. Keep its support."""
    expanded=[]
    for recipe in recipes:
        if not (recipe['start_free'] and recipe['end_free']):expanded.append(recipe);continue
        path=np.asarray(recipe['path']);candidates=np.linspace(1,len(path)-2,min(32,max(0,len(path)-2)),dtype=int)
        held=[int(i) for i in candidates if _held(path[i],trees)]
        if not held:continue                                              
        i=held[len(held)//2]
        for sub in (path[:i+1][::-1],path[i:]):
            if len(sub)<2:continue
            item=dict(recipe,path=sub.tolist(),start_free=False,end_free=True)
            expanded.append(item)
    return expanded


def _pipe_buffer(source,recipes,trees,factor,S):
    from . import pipes
    from .structure_rebuild import sample
    buf=pipes.MeshBuffer();part=0;down=np.array([0.,0.,-1.])
    for index,recipe in enumerate(recipes):
        path,ma,mb=_owners(recipe,trees,factor)
        sf,ef=recipe['start_free'],recipe['end_free']
        if sf and ef:
                                                                   
            from .boolean_restructure import _nearest
            middle=path[len(path)//2]/factor;owner,_,_=_nearest(Vector(middle),trees)
            body,rest,_=trees[owner];ma=np.array(body.matrix_world@rest.inverted_safe());ma[:3,3]*=factor;mb=ma
        routed=geometry.pipe_route(path,ma,mb,S.D,S.embed,S.type)
        paths=[(routed,sf,ef)] if routed is not None else [(p,False,True) if j==0 else (p,True,False) for j,p in enumerate(geometry.split_pipe(path,ma,mb))]
        rng=np.random.default_rng()
        if 'rng_state' in recipe:rng.bit_generator.state=recipe['rng_state']
        else:rng=pipes._rng(S.seed,849,index)
        S._joint_phase=float(recipe['joint_phase']) if 'joint_phase' in recipe else float(rng.uniform(0.,max(S.joint,1.)))
        built=[]
        for line,start_free,end_free in paths:
            line,arc=sample(line,max(S.D*.15,.005))
            if len(line)<2 or arc[-1]<S.D*.2:continue
            piece=pipes.Piece(0.,float(arc[-1]),start_free,end_free)
            piece.visible=[(0.,float(arc[-1]))]
            normal=geometry.rigid_rotation(ma)@np.asarray(recipe['normal'])
            nout=np.broadcast_to(normal,line.shape).copy()
            if pipes.build_piece(S,buf,part,line,arc,nout,piece,rng,down):built.append(piece);part+=1
        if S.wires and built:pipes.build_wires(S,buf,part,built,rng,down);part+=1
    return buf


def _cable_buffer(source,recipes,trees,factor,S,current):
    from . import cables,pipes
    down=np.array([0.,0.,-1.]);ropes=[];plans=[]
    recipes=_middle_held(recipes,trees)
    for index,original in enumerate(recipes):
        recipe=dict(original)
        if recipe['start_free']:
            recipe['path']=recipe['path'][::-1];recipe['start_free']=False;recipe['end_free']=True
        rest,ma,mb=_owners(recipe,trees,factor)
        arc=geometry.cumulative(rest);total=arc[-1]
        if total<S.Dw*.5:continue
        lead=geometry.fixed_lead(rest,S.Dw,S.embed)
        old_a=geometry.at(rest,[0.,lead]);fixed_a=geometry.pose(ma,old_a)
        Aback,A=fixed_a
        normal=geometry.unit(geometry.rigid_rotation(ma)@np.asarray(recipe['normal']))
        side=geometry.unit(np.cross(normal,geometry.unit(A-Aback)))
        rng=pipes._rng(S.seed,850,index)
                                                                              
                                                   
        mesh_rng=pipes._rng(S.seed,851,index)
        if 'rng_state' in recipe:mesh_rng.bit_generator.state=recipe['rng_state']
        hang=S.hang=='ALWAYS' or (S.hang=='HORIZONTAL' and abs(normal@down)>.6)
        pc=cables.Piece(start_free=False);pc.slot=recipe.get('slot',0)
        pc.chunks.append(('fixed',fixed_a,np.broadcast_to(normal,fixed_a.shape).copy(),np.full(2,-1.)))
        if not recipe['end_free']:
            fixed_b=geometry.pose(mb,geometry.at(rest,[total-lead,total]))
            B,Bfwd=fixed_b
            length=geometry.loop_length(max(0.,total-2*lead),np.linalg.norm(B-A),S.slack,S.hang_len*.7 if hang else 0.)
            rope=cables._loop_rope(S,A,Aback,B,Bfwd,normal,down,length,hang,rng,side)
            pc.chunks.append(('rope',rope,False))
            pc.chunks.append(('fixed',fixed_b,np.broadcast_to(normal,fixed_b.shape).copy(),np.full(2,-1.)))
        else:
            length=max(S.Dw*3,total-lead)
            if hang:length=max(length,S.hang_len)
            rope=cables._free_rope(S,A,Aback,geometry.unit(A-Aback),normal,down,length,hang,rng,side)
            pc.chunks.append(('rope',rope,False));pc.end_free=True
        ropes.append(rope);pc.visible=[(0.,float('inf'))];plans.append((pc,mesh_rng))
    cables.simulate(S,ropes,down,current)
    buf=pipes.MeshBuffer();part=0
                                                                             
                                                                           
    from .structure_rebuild import sample
    for rope in ropes:
        both=bool(rope.pinned[-1] and rope.pinned[-2])
        core=rope.X[1:-1] if both else rope.X[1:]
        step=max(S.Dw*.25,.005)
        if S.t['helix']>0:step=min(step,.45*S.Dw/6.)
        dense,_=sample(core,step)
        rope.X=np.vstack((rope.X[0],dense,rope.X[-1])) if both else np.vstack((rope.X[0],dense))
        rope.pinned=np.zeros(len(rope.X),dtype=bool);rope.pinned[:2]=True
        if both:rope.pinned[-2:]=True
        rope.rest=np.linalg.norm(np.diff(rope.X,axis=0),axis=1)
    for pc,rng in plans:part=cables.build_piece(S,buf,part,pc,rng,down)
    return buf


def _commit(obj,template,buf,kind,factor):
    world=np.vstack(buf.verts)/factor if buf.verts else np.empty((0,3),dtype=float)
    inverse=np.asarray(obj.matrix_world.inverted_safe(),dtype=float)
    points=geometry.pose(inverse,world)
    mesh=bpy.data.meshes.new(obj.name+'_Restructured')
    try:
        mesh.from_pydata(points.tolist(),[],buf.faces)
        for material in obj.data.materials:mesh.materials.append(material)
        slots=np.asarray(buf.mat,dtype=np.int32)
        slots[(slots<0)|(slots>=len(mesh.materials))]=0
        mesh.polygons.foreach_set('material_index',slots)
        mesh.polygons.foreach_set('use_smooth',np.asarray(buf.smooth,dtype=bool))
        prefix='pipe' if kind=='PIPE' else 'cable'
        attr=mesh.attributes.new(prefix+'_id','INT','FACE');attr.data.foreach_set('value',buf.part)
        attr=mesh.attributes.new(prefix+'_exposed','FLOAT','POINT');attr.data.foreach_set('value',np.concatenate(buf.expo))
        for saved in template.attributes:
            if saved.domain!='FACE' or saved.data_type not in {'INT','BOOLEAN','FLOAT'} or mesh.attributes.get(saved.name) is not None:continue
            dtype=bool if saved.data_type=='BOOLEAN' else np.int32 if saved.data_type=='INT' else np.float32
            values=np.empty(len(saved.data),dtype=dtype);saved.data.foreach_get('value',values)
            if len(values) and np.all(values==values[0]):
                attr=mesh.attributes.new(saved.name,saved.data_type,'FACE');attr.data.foreach_set('value',np.full(len(buf.faces),values[0],dtype=dtype))
                                                                               
                                                                   
        uv=[]
        for face in buf.faces:
            p=world[np.asarray(face)];normal=np.cross(p[1]-p[0],p[2]-p[0]);axis=int(np.argmax(np.abs(normal)))
            axes=((1,2),(0,2),(0,1))[axis];uv.extend(p[:,axes])
        for name in [layer.name for layer in template.uv_layers] or ['ProjectDamage_Zones']:
            layer=mesh.uv_layers.new(name=name);layer.data.foreach_set('uv',np.asarray(uv,dtype=np.float32).ravel())
        mesh.update()
    except Exception:
        bpy.data.meshes.remove(mesh);raise
    old=obj.data;obj.data=mesh
    if old.users==0:bpy.data.meshes.remove(old)
    if len(points):
        for group in obj.vertex_groups:group.add(list(range(len(points))),1.,'REPLACE')
    obj.update_tag()


def rebuild(context,source,obj,trees):
    from .boolean_restructure import _template
    from . import pipes,cables,boolean_ui_controls
    from .params import unit_factor
    kind='PIPE' if obj.get(pipes.PIPES_STAMP) else 'CABLE'
    p=source.project_damage;factor=unit_factor(context)
    damage=pipes.damage_wanted(p) if kind=='PIPE' else cables.damage_wanted(p)
    scale=pipes._gscale(p)
    if not damage:scale*=(p.boolean_pipe_scale if kind=='PIPE' else p.boolean_cable_scale)/100.
    S=pipes.Settings(p,scale) if kind=='PIPE' else cables.Settings(p,scale)
    if not damage:boolean_ui_controls.tube_settings(S,p,'pipe' if kind=='PIPE' else 'cable')
    template=_template(obj);recipes=_recipes(obj,template,kind,S,trees,factor)
    current=_current_bvh(trees,factor)
    buf=_pipe_buffer(source,recipes,trees,factor,S) if kind=='PIPE' else _cable_buffer(source,recipes,trees,factor,S,current)
    _commit(obj,template,buf,kind,factor)
    obj['pd_tubes_regenerated']=len(recipes)
