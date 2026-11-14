# общие неровные каттеры для совпадающих границ соседних осколков
# SPDX-License-Identifier: GPL-3.0-or-later
"""Noisy Boolean interfaces, independently implemented from the shared noisy-grid idea.
Conceptual reference: Destruction Tools by FxForge.
A single wavy cutter repartitions neighbouring pieces in their rest positions.
The original exterior and wall-contact surfaces bound the operation exactly.
"""
import math,time
import bpy
import numpy as np
from mathutils import Vector,noise
from mathutils.bvhtree import BVHTree
from .fracture_attributes import _read,_world,_copy_attributes,_groups


def _frame(n):
    axis=np.eye(3)[int(np.argmin(np.abs(n)))];u=np.cross(n,axis);u/=np.linalg.norm(u)
    return u,np.cross(n,u)


def _solid_grid(origin,u,v,n,lo,hi,heights,bottom):
    ny,nx=heights.shape
    x,y=np.meshgrid(np.linspace(lo[0],hi[0],nx),np.linspace(lo[1],hi[1],ny))
    top=origin+x[...,None]*u+y[...,None]*v+heights[...,None]*n
                                                                         
    ring=[*range(nx),*[j*nx+nx-1 for j in range(1,ny)],*range(ny*nx-2,(ny-1)*nx-1,-1),*[j*nx for j in range(ny-2,0,-1)]]
    points=list(top.reshape(-1,3));base=len(points)
    points.extend(np.asarray([points[i] for i in ring])-np.asarray([heights.ravel()[i]-bottom for i in ring])[:,None]*n)
    tris=[]
    for j in range(ny-1):
        for i in range(nx-1):
            a=j*nx+i;tris.extend(((a,a+1,a+nx+1),(a,a+nx+1,a+nx)))
    for k,a in enumerate(ring):
        q=(k+1)%len(ring);b=ring[q];tris.extend(((a,base+k,base+q),(a,base+q,b)))
    center=len(points);points.append(origin+(lo[0]+hi[0])*.5*u+(lo[1]+hi[1])*.5*v+bottom*n)
    for k in range(len(ring)):tris.append((center,base+(k+1)%len(ring),base+k))
    return np.ascontiguousarray(points,np.float64),np.ascontiguousarray(tris,np.int32)


def _interfaces(items,strength,step,scale,detail,roughness,seed,budget):
                                                                             
                                                                      
    span=max(np.ptp(np.vstack([x['points'] for x in items]),axis=0).max(),1e-5)
    tol=max(1e-7,span*1e-5);planes={}
    for owner,item in enumerate(items):
        p=item['points'];t=item['tri'];eligible=item['eligible'][item['tf']]
        for ti in np.flatnonzero(eligible):
            q=p[t[ti]];cross=np.cross(q[1]-q[0],q[2]-q[0]);length=np.linalg.norm(cross)
            if length<tol*tol:continue
            n=cross/length;k=int(np.argmax(np.abs(n)));sign=1 if n[k]>0 else -1;n*=sign
            key=tuple(np.round(n*10000).astype(int));d=float(q[0]@n)
            planes.setdefault(key,{}).setdefault((owner,sign,round(d/tol)),[]).append(int(ti))
    jobs=[]
    for key,groups in planes.items():
        n=np.asarray(key,dtype=float);n/=np.linalg.norm(n);u,v=_frame(n);sides=[]
        for (owner,sign,_),ts in groups.items():
            q=items[owner]['points'][items[owner]['tri'][ts]].reshape(-1,3)
            uv=np.c_[q@u,q@v];sides.append((owner,sign,float(np.mean(q@n)),uv.min(0),uv.max(0),ts))
        for a in sides:
            if a[1]!=1:continue
            for b in sides:
                if b[1]!=-1 or a[0]==b[0] or abs(a[2]-b[2])>max(tol*4,span*.002):continue
                lo=np.maximum(a[3],b[3]);hi=np.minimum(a[4],b[4])
                if np.any(hi-lo<tol*4):continue
                                                                              
                                                                             
                jobs.append((a[0],b[0],n,u,v,(a[2]+b[2])*.5,lo,hi))
                                                                       
                                                                             
                                                                         
    grouped={}
    for a,b,n,u,v,d,lo,hi in jobs:
        points=np.array([n*d+u*x+v*y for x,y in ((lo[0],lo[1]),(hi[0],lo[1]),(hi[0],hi[1]),(lo[0],hi[1]))])
        if a>b:a,b=b,a;n=-n
        area=float(np.prod(hi-lo));groups=grouped.setdefault((a,b),[])
        group=next((q for q in groups if np.dot(q[0]/np.linalg.norm(q[0]),n)>.65),None)
        if group is None:groups.append([n*area,area,[points]])
        else:group[0]+=n*area;group[1]+=area;group[2].append(points)
    jobs=[]
    for (a,b),groups in grouped.items():
        for normal,area,patches in groups:
            n=normal/np.linalg.norm(normal);u,v=_frame(n);q=np.vstack(patches)
            uv=np.c_[q@u,q@v];d=float(np.mean(q@n));height=float(np.ptp(q@n))*.5
            jobs.append((a,b,n,u,v,d,height,uv.min(0),uv.max(0)))
    if not jobs:return [],[],[],[],np.empty((0,2),np.int32)
    area=sum(np.prod(j[-1]-j[-2]) for j in jobs)
    step=max(step,math.sqrt(area*4/max(1000,budget)))
    hp=[];ht=[];bp=[];bt=[];pairs=[]
    offset=Vector((seed*7.317+11.19,seed*3.713-42.7,seed*13.9+8.1))
    for a,b,n,u,v,d,old_height,lo,hi in jobs:
        extent=hi-lo;amplitude=min(strength,float(extent.min())*.22)
        margin=max(step,amplitude*1.5);lo=lo-margin;hi=hi+margin
        size=hi-lo;nx=max(3,min(192,int(math.ceil(size[0]/step))+1));ny=max(3,min(192,int(math.ceil(size[1]/step))+1))
        x,y=np.meshgrid(np.linspace(lo[0],hi[0],nx),np.linspace(lo[1],hi[1],ny));h=np.zeros((ny,nx))
        origin=n*d
                                                                         
                                                                                 
        for j in range(ny):
            for i in range(nx):
                pos=Vector(origin+x[j,i]*u+y[j,i]*v)/max(scale,1e-8)+offset
                value=0.;weight=1.;norm=0.;frequency=1.
                for octave in range(max(1,int(detail)+1)):
                    value+=noise.noise(pos*frequency,noise_basis='PERLIN_NEW')*weight
                    norm+=weight;weight*=roughness;frequency*=2.
                edge=min(x[j,i]-lo[0],hi[0]-x[j,i],y[j,i]-lo[1],hi[1]-y[j,i])
                fade=min(1.,max(0.,edge/margin));fade=fade*fade*(3-2*fade)
                h[j,i]=amplitude*value/max(norm,1e-9)*fade
        band=max(strength*1.5+old_height,step*2,span*.005)
        p,t=_solid_grid(origin,u,v,n,lo,hi,h,-band*2)
                                                            
        pad=min(step*.1,margin*.1)
        q,r=_solid_grid(origin,u,v,n,lo+pad,hi-pad,np.full((2,2),band),-band)
        hp.append(p);ht.append(t);bp.append(q);bt.append(r);pairs.append((a,b))
    return hp,ht,bp,bt,np.ascontiguousarray(pairs,np.int32)


def _bary(q,tri):
    a,b,c=tri;ab=b-a;ac=c-a;ap=q-a;den=ab@ab*(ac@ac)-(ab@ac)**2
    if abs(den)<1e-24:return np.array((1.,0.,0.))
    v=((ac@ac)*(ap@ab)-(ab@ac)*(ap@ac))/den;w=((ab@ab)*(ap@ac)-(ab@ac)*(ap@ab))/den
    out=np.maximum((1-v-w,v,w),0.);return out/max(out.sum(),1e-20)


def piece_maps(result,owner,item):
    rows=np.flatnonzero(result['owners']==owner);fids=result['faces'][rows]-item['offset']
    out_tri=result['triangles'][rows];used,remap=np.unique(out_tri,return_inverse=True);remap=remap.ravel();co=result['points'][used];world=result['points'][out_tri]
    valid=(fids>=0)&(fids<len(item['tri']));bary=result['weights'].reshape(-1,3,3)[rows].copy()
    inner_tri=np.flatnonzero(item['eligible'][item['tf']]);tree=BVHTree.FromPolygons(item['points'],item['tri'][inner_tri],all_triangles=True)
    for f in np.flatnonzero(~valid):
        hit=tree.find_nearest(Vector(world[f].mean(0)));fid=int(inner_tri[hit[2]]) if hit[2] is not None else int(inner_tri[0]);fids[f]=fid
        for j in range(3):bary[f,j]=_bary(world[f,j],item['points'][item['tri'][fid]])
    face_sources=item['tf'][fids];cs=np.repeat(item['tl'][fids],3,axis=0);cw=bary.reshape(-1,3)
    ps=np.zeros((len(used),3),np.int32);pw=np.zeros((len(used),3))
    ps[remap]=np.repeat(item['tri'][fids],3,axis=0);pw[remap]=cw
    return rows,remap,co,valid,cs,cw,face_sources,ps,pw

def compute_interfaces(items,args,native_path):
    hp,ht,bp,bt,pairs=_interfaces(items,*args)
    if not len(pairs):return None
    points=[];tri=[];owners=[];start=0
    for owner,item in enumerate(items):
        points.append(item['points']);tri.append(item['tri']+start);owners.extend([owner]*len(item['tri']));start+=len(item['points'])
    import importlib,sys
    from pathlib import Path
    from . import cell_cuts
    core=cell_cuts.load()
    result=core.noisy_boolean_interfaces(np.ascontiguousarray(np.vstack(points),np.float64),np.ascontiguousarray(np.vstack(tri),np.int32),np.asarray(owners,np.int32),hp,ht,bp,bt,pairs,args[-1]*2)
    result['maps']=[piece_maps(result,owner,item) for owner,item in enumerate(items)]
    return {k:result[k] for k in ('maps','faces','weights','interfaces')}

def process(context,source,objects,s,materials,contact=(),proxies=None):
    from . import background_compute as bg
    objects=tuple(objects);contact=tuple(contact)
    if bg.async_for(source):
        group=source.project_damage_inner_rough
        kind=next(k for k in ('pieces','fragments','cellf','rbf','debris') if getattr(group,k)==s)
                                                                                
        refs=[(o,o.data,o.matrix_world.copy()) for o in objects+contact if o is not None]
        def valid():
            return bool(s.enable) and all(bg.valid_id(o) and o.data==me and o.matrix_world==mw and o.mode=='OBJECT' for o,me,mw in refs)
        bg.queue(source,'rough_'+kind,lambda:_steps(bpy.context,source,objects,s,materials,contact,proxies),guard=valid)
        return 0
    return bg.immediate(_steps(context,source,objects,s,materials,contact,proxies))

def _steps(context,source,objects,s,materials,contact=(),proxies=None):
    from . import background_compute as bg
    from . import inner_rough as ir,cell_cuts,cellf_output,fracture_uv
    begin=time.perf_counter();items=[];total=0
    contact_tree,epsilon=ir._contact_tree(contact) if contact else (None,0.)
    for obj in objects:
        if obj is None or obj.type!='MESH' or obj.get(ir.DETAIL_STAMP) or not len(obj.data.polygons):continue
        mesh=obj.get('project_damage_noisify_original') or obj.data
        mat=_read(mesh.polygons,'material_index',dtype=np.int32);idx=ir._inner_indices(obj,materials,mesh)
        eligible=ir._inner_face_mask(mesh,mat,idx)
        if not eligible.any():continue
        mw=_world(source,obj);matrix=np.asarray(mw);points=_read(mesh.vertices,'co',3,np.float64)@matrix[:3,:3].T+matrix[:3,3]
        mesh.calc_loop_triangles();tl=_read(mesh.loop_triangles,'loops',3,np.int32);tf=_read(mesh.loop_triangles,'polygon_index',dtype=np.int32)
        loops=_read(mesh.loops,'vertex_index',dtype=np.int32);tri=loops[tl]
        if contact_tree:
            for f in np.flatnonzero(eligible):
                face=mesh.polygons[int(f)];hit=contact_tree.find_nearest(Vector(points[list(face.vertices)].mean(0)))
                if hit[0] is not None and hit[3]<epsilon:eligible[f]=False
        if not eligible.any():continue
        items.append(dict(obj=obj,mesh=mesh,mw=mw,points=points,tri=tri,tl=tl,tf=tf,eligible=eligible,mat=mat,offset=total));total+=len(tri)
    if not items:return 0
    step=ir._bu(context,source,s.detail)/max(.05,s.resolution/100.)
    args=(ir._bu(context,source,s.strength),step,ir._bu(context,source,s.scale),s.noise_detail,s.noise_roughness,s.seed,ir.MAX_NEW_VERTS)
    data=[{key:it[key] for key in ('points','tri','eligible','tf','tl','offset')} for it in items]
    result=yield from bg.task('Building shared noisy cutters',compute_interfaces,data,args,cell_cuts.load().__file__)
    timed_shards = [item['obj'] for item in items if item['obj'].get('project_damage_cellf_of')] if source.get(cellf_output.SWAP_FRAME) is not None else []
    has_timing = bool(timed_shards or source.get(cellf_output.SWAP_ALL))
    if result is None:
                                                                         
                                                                         
        for item in items:ir._plain([item['obj']])
        if has_timing:cellf_output.sync_switch(context,source,timed_shards)
        return 0
    prepared=[]
    try:
        for owner,item in enumerate(items):
            yield None                                                                      
            old=item['mesh']
            rows,remap,co,valid,cs,cw,face_sources,ps,pw=result['maps'][owner]
            inv=np.asarray(item['mw'].inverted());local=co@inv[:3,:3].T+inv[:3,3]
            me=bg.stage_mesh(bpy.data.meshes.new(old.name+'_NoisyCutters'));me.from_pydata(local,[],remap.reshape(-1,3));me.update()
            for mat in old.materials:me.materials.append(mat)
            _copy_attributes(old,me,ps,pw,cs,cw,face_sources)
            me.polygons.foreach_set('material_index',item['mat'][face_sources])
            inside=item['eligible'][face_sources]|~valid
            for name in ('project_damage_physics_inner',):
                attr=me.attributes.get(name)
                if attr is not None:attr.data.foreach_set('value',inside)
            for name in ('project_damage_base_poly','project_damage_debris_base_poly'):
                attr=me.attributes.get(name)
                if attr is not None:
                    values=_read(attr.data,'value',dtype=np.int32);values[~valid]=-1;attr.data.foreach_set('value',values)
                                                                         
                                                               
            normals=ir._corner_normals(old).reshape(-1,3);transferred=np.einsum('ijk,ij->ik',normals[cs],cw)
            attr=me.attributes.get(ir.NORMAL_ATTR) or me.attributes.new(ir.NORMAL_ATTR,'FLOAT_VECTOR','CORNER');attr.data.foreach_set('vector',transferred.ravel())
                                                                             
                                                                                 
                                                                                  
            global_faces=np.asarray(result['faces'])[rows]
            offsets=np.array([it['offset'] for it in items]+[total])
            source_owners=np.searchsorted(offsets,global_faces,side='right')-1
            fields={'FLOAT':('value',1,np.float32),'INT':('value',1,np.int32),'BOOLEAN':('value',1,bool),'FLOAT_VECTOR':('vector',3,np.float32),'FLOAT2':('vector',2,np.float32),'FLOAT_COLOR':('color',4,np.float32),'BYTE_COLOR':('color',4,np.float32)}
            for other in np.unique(source_owners[(global_faces>=0)&~valid]):
                if other<0 or other>=len(items):continue
                source_item=items[int(other)];source_mesh=source_item['mesh']
                selection=np.flatnonzero((source_owners==other)&(global_faces>=0)&~valid)
                face_id=global_faces[selection]-source_item['offset'];source_faces=source_item['tf'][face_id]
                weights=np.asarray(result['weights']).reshape(-1,3,3)[rows[selection]]
                source_loops=source_item['tl'][face_id]
                dest_loops=(selection[:,None]*3+np.arange(3)).ravel()
                inside[selection]=source_item['eligible'][source_faces]
                slots={m:i for i,m in enumerate(me.materials)}
                for dest,src_face in zip(selection,source_faces):
                    matid=int(source_mesh.polygons[int(src_face)].material_index)
                    material=source_mesh.materials[matid] if matid<len(source_mesh.materials) else None
                    if material not in slots:me.materials.append(material);slots[material]=len(me.materials)-1
                    me.polygons[int(dest)].material_index=slots[material]
                for source_attr in source_mesh.attributes:
                    if source_attr.name.startswith('.') or source_attr.name in {'position','material_index',ir.NORMAL_ATTR} or source_attr.domain not in {'FACE','CORNER'}:continue
                    spec=fields.get(source_attr.data_type)
                    if spec is None:continue
                    field,width,dtype=spec;src_values=_read(source_attr.data,field,width,dtype)
                    dest_attr=me.attributes.get(source_attr.name)
                    if dest_attr is None:dest_attr=me.attributes.new(source_attr.name,source_attr.data_type,source_attr.domain)
                    values=_read(dest_attr.data,field,width,dtype)
                    if source_attr.domain=='FACE':values[selection]=src_values[source_faces]
                    else:
                        gathered=src_values[source_loops]
                        if dtype in (np.int32,bool):new=gathered[np.arange(len(selection))[:,None],weights.argmax(axis=2)]
                        elif width==1:new=np.einsum('ijk,ik->ij',weights,gathered)
                        else:new=np.einsum('ijk,ikl->ijl',weights,gathered)
                        values[dest_loops]=new.reshape((-1,width) if width>1 else (-1,))
                    dest_attr.data.foreach_set(field,values.ravel())
                ns=ir._corner_normals(source_mesh).reshape(-1,3)[source_loops]
                ns=np.einsum('ijk,ikl->ijl',weights,ns).reshape(-1,3)
                ns=ns@np.linalg.inv(np.asarray(source_item['mw'])[:3,:3])@np.asarray(item['mw'])[:3,:3]
                ns/=np.maximum(np.linalg.norm(ns,axis=1),1e-20)[:,None]
                transferred[dest_loops]=ns
            attr=me.attributes.get(ir.NORMAL_ATTR);attr.data.foreach_set('vector',transferred.ravel())
            marker=me.attributes.get('project_damage_physics_inner')
            if marker is not None:marker.data.foreach_set('value',inside)
            ir._shade(me,set(),s.smooth,True,face_mask=inside,angle=s.smooth_angle)
            fracture_uv.copy_flags(me, *fracture_uv.flags(old))
            uv_name = fracture_uv.layer_name(old)
            if uv_name and me.uv_layers.get(uv_name) is not None:
                me[fracture_uv.OWNER] = uv_name
            me['project_damage_noisify_form']='CUTTERS';me.update();prepared.append((item,me,ps,pw))
    except Exception:
        for _,me,_,_ in prepared:
            if not me.users:bpy.data.meshes.remove(me)
        raise
    proxy=bool(s.proxy) if proxies is None else bool(proxies)
    for item,me,ps,pw in prepared:
        obj=item['obj'];saved=obj.get('project_damage_noisify_original')
        if proxy:
            if saved is not None:
                previous=obj.data;obj.data=saved;del obj['project_damage_noisify_original']
                if previous.users==0:bpy.data.meshes.remove(previous)
            target=ir.detail_of(obj)
            if target is None:target=bpy.data.objects.new(obj.name+'_detail',me);ir._detail_collection(context,obj).objects.link(target)
            else:
                previous=target.data;target.data=me
                if not previous.users:bpy.data.meshes.remove(previous)
            target[ir.DETAIL_STAMP]=source.get('project_damage_uid') or source.name;target.project_damage_og_object=source
            target.parent=obj;target.matrix_parent_inverse.identity();target.matrix_basis.identity();target.hide_set(False);target.hide_viewport=False;target.hide_render=False
            obj.hide_set(False);ir._set_proxy(obj,True)
            for col in target.users_collection:col.hide_viewport=False;col.hide_render=False
            if context.space_data and context.space_data.type=='VIEW_3D' and context.space_data.local_view:target.local_view_set(context.space_data,True)
        else:
            previous=obj.data
            if saved is None:obj['project_damage_noisify_original']=previous
            obj.data=me;target=obj;ir._set_proxy(obj,False)
            det=ir.detail_of(obj)
            if det:ir._remove_object(det)
            if not previous.users:bpy.data.meshes.remove(previous)
        _groups(obj,target,ps,pw,item['mesh'])
        obj['project_damage_noisify_seconds']=time.perf_counter()-begin
        obj['project_damage_noisy_interfaces']=int(result['interfaces'])
    if has_timing:cellf_output.sync_switch(context,source,timed_shards)
    return len(prepared)
