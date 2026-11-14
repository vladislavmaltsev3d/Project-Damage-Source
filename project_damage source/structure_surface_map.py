# переносим конструкцию на изгибы поверхности с сохранением пропорций
import math
from collections import deque
import numpy as np
from mathutils import Vector
from mathutils.bvhtree import BVHTree
from mathutils.geometry import tessellate_polygon


class Chart:
    def __init__(self,mesh,indices,uv):
        self.tri=[];self.uv=[];self.position=[];self.normals=[]
        # соседняя сторона коробки не наклоняет профиль на этой стороне
        normals={}
        for index in indices:
            face=mesh.polygons[index]
            for vertex in face.vertices:normals[vertex]=normals.get(vertex,Vector())+face.normal*face.area
        normals={key:value.normalized() for key,value in normals.items()}
        for index in indices:
            face=mesh.polygons[index]
            ids=list(face.vertices)
            local=[Vector((*uv[v],0)) for v in ids]
            for tri in tessellate_polygon([local]):
                corners=list(tri) if isinstance(tri[0],int) else [local.index(p) for p in tri]
                uvtri=[local[j] for j in corners]
                if (uvtri[1]-uvtri[0]).cross(uvtri[2]-uvtri[0]).z<=1e-12:continue
                start=len(self.uv);self.tri.append((start,start+1,start+2));self.uv.extend(uvtri)
                self.position.extend(mesh.vertices[ids[j]].co.copy() for j in corners)
                self.normals.extend(normals[ids[j]].copy() for j in corners)
        if not self.tri:raise ValueError('Control surface has no usable faces')
        self.tree=BVHTree.FromPolygons(self.uv,self.tri,all_triangles=True)
        self.bounds=tuple((min(p[a] for p in self.uv),max(p[a] for p in self.uv)) for a in (0,1))
        self.curved=max((n-self.normals[0]).length for n in self.normals)>.02
        self.tangents=[]
        for ids in self.tri:
            a,b,c=(self.uv[i] for i in ids);p,q,r=(self.position[i] for i in ids)
            ux,uy=b.x-a.x,b.y-a.y;vx,vy=c.x-a.x,c.y-a.y;den=ux*vy-uy*vx
            self.tangents.append(((q-p)*vy-(r-p)*uy)/den)
        self.bitangents=[]
        for ids in self.tri:
            a,b,c=(self.uv[i] for i in ids);p,q,r=(self.position[i] for i in ids)
            ux,uy=b.x-a.x,b.y-a.y;vx,vy=c.x-a.x,c.y-a.y;den=ux*vy-uy*vx
            self.bitangents.append(((r-p)*ux-(q-p)*vx)/den)

    def sample(self,x,y,offset=0):
        point=Vector((x,y,0));_,_,index,_=self.tree.find_nearest(point)
        if index is None:return None,None,False
        ids=self.tri[index];a,b,c=[self.uv[i] for i in ids]
        vx,vy=b.x-a.x,b.y-a.y;wx,wy=c.x-a.x,c.y-a.y
        qx,qy=x-a.x,y-a.y;den=vx*wy-vy*wx
        s=(qx*wy-qy*wx)/den;t=(vx*qy-vy*qx)/den
        weights=(1-s-t,s,t)
        pos=sum((self.position[i]*f for i,f in zip(ids,weights)),Vector())
        n=sum((self.normals[i]*f for i,f in zip(ids,weights)),Vector()).normalized()
        return pos+n*offset,n,min(weights)>=-1e-6

    def map(self,points):
        return [tuple(self.sample(*co)[0]) for co in points]

    def map_members(self,points,frames,vertex_pieces):
        points=np.asarray(points,dtype=np.float64)
        frame=np.asarray(frames,dtype=np.float64)[np.asarray(vertex_pieces).reshape(-1)]
        axis=frame[:,0].astype(np.int32)
        centres=points[:,:2].copy()
        centres[axis==0,1]=frame[axis==0,2]
        centres[axis==1,0]=frame[axis==1,1]
        centres[axis<0]=frame[axis<0,1:3]
        unique,inverse=np.unique(np.column_stack((centres,axis)),axis=0,return_inverse=True)
        positions=np.empty((len(unique),3));along=np.empty_like(positions);across=np.empty_like(positions);normal=np.empty_like(positions)
        for i,(x,y,direction) in enumerate(unique):
            base,n,_=self.sample(x,y)
            _,_,tri,_=self.tree.find_nearest(Vector((x,y,0)))
            t=(self.bitangents[tri] if direction==1 else self.tangents[tri]).normalized()
            n=self.tangents[tri].cross(self.bitangents[tri]).normalized()
            if direction==1:u=t.cross(n).normalized();v=t
            else:u=t;v=n.cross(t).normalized()
            positions[i]=base;along[i]=u;across[i]=v;normal[i]=n
        delta=points[:,:2]-centres
        return positions[inverse]+along[inverse]*delta[:,0,None]+across[inverse]*delta[:,1,None]+normal[inverse]*points[:,2,None]

    def intervals(self,axis,value):
        other=1-axis;spans=[]
        for ids in self.tri:
            poly=[self.uv[i] for i in ids];hits=[]
            for a,b in zip(poly,poly[1:]+poly[:1]):
                if (a[axis]<=value<b[axis]) or (b[axis]<=value<a[axis]):hits.append(a[other]+(b[other]-a[other])*(value-a[axis])/(b[axis]-a[axis]))
            if len(hits)>=2:spans.append((min(hits),max(hits)))
        merged=[]
        for a,b in sorted(spans):
            if merged and a<=merged[-1][1]+1e-6:merged[-1]=(merged[-1][0],max(b,merged[-1][1]))
            elif b-a>1e-7:merged.append((a,b))
        return merged


# делим управляющую поверхность на участки для размещения конструкции
def charts(mesh, clearance=0., fold=False):
    neighbors={p.index:[] for p in mesh.polygons};edges={}
    for face in mesh.polygons:
        ids=list(face.vertices)
        for a,b in zip(ids,ids[1:]+ids[:1]):edges.setdefault(tuple(sorted((a,b))),[]).append(face.index)
    for (a,b),faces in edges.items():
        if len(faces)==2:
            one,two=faces
            first,second=mesh.polygons[one],mesh.polygons[two]
            angle=first.normal.angle(second.normal,0.)
            edge=mesh.vertices[b].co-mesh.vertices[a].co
            if edge.length<1e-9:continue
            direction=edge.normalized()
            widths=[max((mesh.vertices[v].co-mesh.vertices[a].co).cross(direction).length for v in face.vertices) for face in (first,second)]
            # если доска толще изгиба, делаем стык, а не сминаем сечение
            radius=min(widths)/max(2*math.tan(angle*.5),1e-9)
            if (angle>math.radians(175) if fold else angle>math.radians(60) or (angle>math.radians(2) and radius<2*clearance)):continue
            neighbors[one].append((two,a,b));neighbors[two].append((one,a,b))
    remaining={f.index for f in mesh.polygons if f.area>1e-12}
    result=[]
    while remaining:
        root=max(remaining,key=lambda i:(mesh.polygons[i].area,-i));face=mesh.polygons[root];n=face.normal
        v=Vector((0,0,1))-n*n.z
        if v.length<.01:v=Vector((0,1,0))-n*n.y
        v.normalize();u=v.cross(n).normalized()
        uv={i:Vector((mesh.vertices[i].co.dot(u),mesh.vertices[i].co.dot(v))) for i in face.vertices}
        indices=[root];remaining.remove(root);todo=deque([root])
        while todo:
            current=todo.popleft()
            for other,a,b in neighbors[current]:
                if other not in remaining:continue
                start=mesh.vertices[a].co;along=mesh.vertices[b].co-start
                if along.length<1e-9:continue
                along.normalize();across=mesh.polygons[other].normal.cross(along).normalized()
                axis=(uv[b]-uv[a]).normalized();perp=Vector((-axis.y,axis.x))
                candidate={i:uv[a]+axis*(mesh.vertices[i].co-start).dot(along)+perp*(mesh.vertices[i].co-start).dot(across) for i in mesh.polygons[other].vertices}
                tolerance=max(1e-6,(mesh.vertices[b].co-start).length*1e-5)
                if any((uv[i]-point).length>tolerance for i,point in candidate.items() if i in uv):continue
                for i,point in candidate.items():
                    if i not in uv:uv[i]=point
                indices.append(other);remaining.remove(other);todo.append(other)
        result.append(Chart(mesh,indices,uv))
    return result
