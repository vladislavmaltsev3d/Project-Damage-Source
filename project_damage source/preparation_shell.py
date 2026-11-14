# соединяем раздельные стороны стены в закрытый объём
import bpy
import bmesh
import numpy as np
from mathutils import Vector
from mathutils.geometry import delaunay_2d_cdt


def layered_wall(mesh):
    xyz=np.asarray([v.co[:] for v in mesh.vertices],dtype=np.float64)
    if len(xyz)<6 or len(mesh.polygons)>100000:return None
    mesh.calc_loop_triangles()
    raw=np.asarray([t.vertices[:] for t in mesh.loop_triangles],dtype=np.int32)
    c=np.cross(xyz[raw[:,1]]-xyz[raw[:,0]],xyz[raw[:,2]]-xyz[raw[:,0]])
    lengths=np.maximum(np.linalg.norm(c,axis=1),1e-30)
    _,vectors=np.linalg.eigh((c/lengths[:,None]).T@c)
    normal=vectors[:,-1]
    center=xyz.mean(axis=0)
    _,_,pca=np.linalg.svd(xyz-center,full_matrices=False)
    tangent=pca[0]-normal*np.dot(pca[0],normal);tangent/=np.linalg.norm(tangent)
    basis=np.asarray((tangent,np.cross(normal,tangent),normal))
    local=(xyz-center)@basis.T
    span=np.ptp(local,axis=0);scale=max(float(np.linalg.norm(span)),1e-6);eps=scale*1e-6
    if span[2]<eps*20 or span[2]>min(span[:2])*.25:return None
    mesh.calc_loop_triangles()
    triangles=np.asarray([t.vertices[:] for t in mesh.loop_triangles],dtype=np.int32)
    corners=np.asarray([t.loops[:] for t in mesh.loop_triangles],dtype=np.int32)
    polys=np.asarray([t.polygon_index for t in mesh.loop_triangles],dtype=np.int32)
    coords=local[triangles];cross=np.cross(coords[:,1]-coords[:,0],coords[:,2]-coords[:,0]);area=np.linalg.norm(cross,axis=1)
    good=np.abs(cross[:,2])>area*.5
    if area[good].sum()<area.sum()*.85:return None
    ids=np.flatnonzero(good & (np.abs(cross[:,2])>eps**2))
    if len(ids)<2:return None
    projected=[]
    for i in ids:
        tri=list(triangles[i])
        if cross[i,2]<0:tri.reverse()
        projected.append(tri)
    input_buckets={};input_xy=[];input_map=[]
    for point in local[:,:2]:
        a,b=(int(np.floor(c/eps)) for c in point)
        candidates=[v for i in (-1,0,1) for j in (-1,0,1) for v in input_buckets.get((a+i,b+j),())]
        match=next((v for v in candidates if np.linalg.norm(point-input_xy[v])<=eps),None)
        if match is None:
            match=len(input_xy);input_xy.append(point);input_buckets.setdefault((a,b),[]).append(match)
        input_map.append(match)
    projected=[[input_map[v] for v in face] for face in projected]
    keep=[i for i,face in enumerate(projected) if len(set(face))==3]
    projected=[projected[i] for i in keep];ids=ids[keep]
    xy,_,cells,_,_,orig=delaunay_2d_cdt([Vector(v) for v in input_xy],[],projected,0,eps,True)
    buckets={};unique=[];remap=[]
    for point in xy:
        a,b=(int(np.floor(c/eps)) for c in point)
        candidates=[v for i in (-1,0,1) for j in (-1,0,1) for v in buckets.get((a+i,b+j),())]
        match=next((v for v in candidates if np.linalg.norm(np.asarray(point)-unique[v])<=eps),None)
        if match is None:
            match=len(unique);unique.append(np.asarray(point,dtype=np.float64));buckets.setdefault((a,b),[]).append(match)
        remap.append(match)
    merged={}
    for cell,source in zip(cells,orig):
        cell=list(dict.fromkeys(remap[v] for v in cell))
        if len(cell)!=3:continue
        key=tuple(sorted(cell))
        if key not in merged:merged[key]=(cell,set(source))
        else:merged[key][1].update(source)
    cells=[v[0] for v in merged.values()];orig=[sorted(v[1]) for v in merged.values()]
    xy=np.asarray(unique,dtype=np.float64)
    planes=[]
    for i in ids:
        co=coords[i];planes.append(np.linalg.solve(np.column_stack((co[:,:2],np.ones(3))),co[:,2]))
    planes=np.asarray(planes)
    middle=(local[:,2].min()+local[:,2].max())*.5
    pieces=[];edges={};levels={};total=missing=0.
    for cell,sources in zip(cells,orig):
        if not sources:continue
        uv=xy[cell];a,b=uv[1]-uv[0],uv[2]-uv[0];signed=float(a[0]*b[1]-a[1]*b[0]);ar=abs(signed)*.5
        if ar<eps**2:continue
        total+=ar
        probe=np.r_[uv.mean(axis=0),1.];height=planes[sources]@probe
        low=sources[int(np.argmin(height))];high=sources[int(np.argmax(height))]
        if float(height.min())>=middle-eps or float(height.max())<=middle+eps:
            missing+=ar;continue
        if signed<0:cell=list(reversed(cell))
        index=len(pieces);pieces.append((cell,low,high))
        for v in cell:
            q=np.r_[xy[v],1.]
            levels.setdefault(v,[]).extend((float(planes[low]@q),float(planes[high]@q)))
        for a,b in zip(cell,cell[1:]+cell[:1]):edges.setdefault(tuple(sorted((a,b))),[]).append((index,a,b))
    if not pieces or missing>max(total*.00001,eps**2):return None
    vertices=[];vert_id={}
    for v,heights in levels.items():
        merged=[]
        for h in sorted(heights):
            if not merged or h-merged[-1]>eps:merged.append(h)
        levels[v]=merged
    def vertex(v,h):
        hs=levels[v];k=min(range(len(hs)),key=lambda j:abs(hs[j]-h));key=(v,k)
        if key not in vert_id:
            vert_id[key]=len(vertices);vertices.append(np.r_[xy[v],hs[k]]@basis+center)
        return vert_id[key]
    faces=[];source_ids=[];side_uv=[]
    for cell,low,high in pieces:
        for plane,order in ((low,list(reversed(cell))),(high,cell)):
            faces.append([vertex(v,float(planes[plane]@np.r_[xy[v],1.])) for v in order]);source_ids.append(int(ids[plane]));side_uv.append(None)
    def wall(a,b,al,bl,ah,bh,source):
        left=[al]+[h for h in levels[a] if min(al,ah)+eps<h<max(al,ah)-eps]+[ah]
        right=[bl]+[h for h in levels[b] if min(bl,bh)+eps<h<max(bl,bh)-eps]+[bh]
        left=sorted(left,reverse=al>ah);right=sorted(right,reverse=bl>bh)
        ring=[vertex(a,h) for h in left]+[vertex(b,h) for h in reversed(right)]
        uv=[(0.,h) for h in left]+[(float(np.linalg.norm(xy[a]-xy[b])),h) for h in reversed(right)]
        clean=[];tex=[]
        for v,t in zip(ring,uv):
            if not clean or clean[-1]!=v:clean.append(v);tex.append(t)
        if len(clean)>1 and clean[0]==clean[-1]:clean.pop();tex.pop()
        if len(set(clean))>=3:faces.append(clean);source_ids.append(source);side_uv.append(tex)
    for edge,adj in edges.items():
        if len(adj)>2:return None
        i,a,b=adj[0];_,lo,hi=pieces[i];qa=np.r_[xy[a],1.];qb=np.r_[xy[b],1.]
        if len(adj)==1:
            wall(a,b,float(planes[lo]@qa),float(planes[lo]@qb),float(planes[hi]@qa),float(planes[hi]@qb),int(ids[hi]))
        else:
            _,lo2,hi2=pieces[adj[1][0]]
            for p,q in ((lo,lo2),(hi,hi2)):
                ha,hb=float(planes[p]@qa),float(planes[p]@qb);ka,kb=float(planes[q]@qa),float(planes[q]@qb)
                if (ha-ka)*(hb-kb)<-eps**2:return None
                if max(abs(ha-ka),abs(hb-kb))>eps:wall(a,b,ha,hb,ka,kb,int(ids[p]))
    result=bpy.data.meshes.new(mesh.name+' · Closed Surface')
    try:
        result.from_pydata(vertices,[],faces);result.update()
        for mat in mesh.materials:result.materials.append(mat)
        for f,src,uv in zip(result.polygons,source_ids,side_uv):
            old=mesh.polygons[int(polys[src])];f.material_index=old.material_index;f.use_smooth=old.use_smooth if uv is None else False
        local_result=(np.asarray(vertices)-center)@basis.T
        weights=[]
        for f,src,uv in zip(result.polygons,source_ids,side_uv):
            if uv is not None:weights.append(None);continue
            co=coords[src,:,:2];query=local_result[list(f.vertices),:2]
            w=np.linalg.solve(np.vstack((co.T,np.ones(3))),np.vstack((query.T,np.ones(len(query))))).T
            weights.append(w)
        for layer in mesh.uv_layers:
            old=np.empty((len(mesh.loops),2),dtype=np.float32);layer.uv.foreach_get('vector',old.ravel());out=np.zeros((len(result.loops),2),dtype=np.float32)
            for f,src,uv,w in zip(result.polygons,source_ids,side_uv,weights):out[f.loop_start:f.loop_start+f.loop_total]=uv if uv is not None else w@old[corners[src]]
            new=result.uv_layers.new(name=layer.name);new.uv.foreach_set('vector',out.ravel());new.active_render=layer.active_render
        if mesh.uv_layers.active:result.uv_layers.active=result.uv_layers[mesh.uv_layers.active.name]
        bm=bmesh.new()
        try:
            bm.from_mesh(result);bmesh.ops.recalc_face_normals(bm,faces=list(bm.faces))
            if any(not e.is_manifold for e in bm.edges):raise ValueError('Layered wall borders could not be joined safely')
            if bm.calc_volume(signed=True)<0:bmesh.ops.reverse_faces(bm,faces=list(bm.faces))
            if abs(bm.calc_volume())<max(scale**3*1e-12,1e-18):raise ValueError('The wall has no enclosed volume')
            bm.to_mesh(result)
        finally:bm.free()
        result.update()
        return result,'Closed: segmented front and back surfaces joined; overlapping surface layers merged.'
    except Exception:
        bpy.data.meshes.remove(result)
        raise
