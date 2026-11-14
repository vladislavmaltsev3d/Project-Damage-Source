# восстанавливаем внутреннюю конструкцию с её обычными размерами
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate fresh reinforcement surfaces at physical, not stretched, spacing.
Profile equations follow break_rebar.cpp Builder::Profile. Pure NumPy math.
"""
import math
import numpy as np


def unit(v, fallback=(1.,0.,0.)):
    n=np.linalg.norm(v)
    return np.asarray(v)/n if n>1e-12 else np.asarray(fallback,dtype=float)


def sample(path, step, limit=50000):
    path=np.asarray(path,dtype=float)
    lengths=np.linalg.norm(np.diff(path,axis=0),axis=1)
    path=path[np.r_[True,lengths>1e-10]]
    if len(path)<2:return path,np.zeros(len(path))
    cumulative=np.r_[0.,np.cumsum(np.linalg.norm(np.diff(path,axis=0),axis=1))]
    count=max(2,int(math.ceil(cumulative[-1]/max(step,1e-8)))+1)
    if count>limit:raise ValueError('Restructure exceeds 50,000 sections per bar; reduce detail or the gap')
    s=np.linspace(0,cumulative[-1],count)
    return np.column_stack([np.interp(s,cumulative,path[:,i]) for i in range(3)]),s


def frames(path, normal):
    tangents=np.empty_like(path)
    tangents[0]=path[1]-path[0];tangents[-1]=path[-1]-path[-2]
    tangents[1:-1]=path[2:]-path[:-2]
    normals=[];binormals=[]
    for tangent in tangents:
        t=unit(tangent)
        n=np.asarray(normal)-t*np.dot(normal,t)
        if np.linalg.norm(n)<1e-8:
            axis=np.eye(3)[np.argmin(np.abs(t))];n=np.cross(t,axis)
        n=unit(n);normals.append(n);binormals.append(np.cross(t,n));normal=n
    return np.asarray(normals),np.asarray(binormals)


def bump(x):
    return np.maximum(0.,1.-np.asarray(x)**2)**2


def profile(kind,r,theta,s,exposure,height,pitch,twist):
    theta=np.asarray(theta)[None,:];s=np.asarray(s)[:,None]
    e=np.asarray(exposure)[:,None];d=2*r
    if kind==0:
        w=np.mod(theta,2*np.pi);half=(w>=np.pi).astype(int);alpha=(w-half*np.pi)/np.pi
        dl=np.minimum(np.minimum(w,np.abs(w-np.pi)),2*np.pi-w)
        longitudinal=.75*bump(dl/.16)
        phase=np.mod(s/max(1e-8,pitch*d)+half*.5+np.where(half==0,.35,-.35)*(alpha-.5),1.)
        transverse=np.maximum(0.,np.sin(np.pi*alpha))**.6*bump((phase-.5)/.2)
        return r+height*r*e*np.maximum(longitudinal,transverse)
    if kind==1:
        phase=np.mod(s/max(1e-8,2*pitch*d)+theta/np.pi,1.)
        return r+height*r*e*bump((phase-.5)/.16)
    if kind==3:
        a=theta-2*np.pi*s/max(1e-8,twist*d)
        q=(np.abs(np.cos(a))**5+np.abs(np.sin(a))**5)**.2
        return .886*r/np.maximum(q,1e-6)
    if kind==4:
        rw=r/3;dc=2*rw;phase=2*np.pi*s/max(1e-8,twist*d)
        result=np.full((len(s),theta.shape[1]),rw)
        for k in range(6):
            angle=theta-phase-k*np.pi/3
            disc=rw*rw-(dc*np.sin(angle))**2;cs=dc*np.cos(angle)
            result=np.maximum(result,np.where((disc>=0)&(cs>0),cs+np.sqrt(np.maximum(0,disc)),rw))
        return result
    return np.full((len(s),theta.shape[1]),r)


def route(rest,ma,mb,radius):
    """Endpoints stay anchored. Short bends follow the local steel directions.
    A free-ended bar is passed the same transform for both endpoints.
    """
    rest=np.asarray(rest)
    transform=lambda m,p: np.asarray(m)[:3,:3]@p+np.asarray(m)[:3,3]
    if np.allclose(ma,mb):return np.array([transform(ma,p) for p in rest])
    a,b=transform(ma,rest[0]),transform(mb,rest[-1]);chord=b-a;length=np.linalg.norm(chord)
    if length<1e-8:return np.array([a,b])
    arc=np.r_[0,np.cumsum(np.linalg.norm(np.diff(rest,axis=0),axis=1))]
                                                                                  
    if np.linalg.norm(rest[-1]-rest[0])<.65*arc[-1]:
        weight=arc/max(arc[-1],1e-9)
        return np.array([(1-t)*transform(ma,p)+t*transform(mb,p) for p,t in zip(rest,weight)])
    direction=unit(chord)
    def tangent(m,v):
        t=unit(np.asarray(m)[:3,:3]@v,direction)
        return unit(t+direction*max(0.,.25-np.dot(t,direction)),direction)
    ta=tangent(ma,rest[1]-rest[0]);tb=tangent(mb,rest[-1]-rest[-2])
    lead=min(length*.2,max(radius*6,length*.08))
    controls=np.array([a,a+ta*lead,b-tb*lead,b])
    t=np.linspace(0,1,97)[:,None]
    return (1-t)**3*controls[0]+3*(1-t)**2*t*controls[1]+3*(1-t)*t*t*controls[2]+t**3*controls[3]


def rebar_lod_indices(points, radius, quality):
    if quality >= 1 or len(points) < 3:
        return np.arange(len(points))
    arc=np.r_[0,np.cumsum(np.linalg.norm(np.diff(points,axis=0),axis=1))]
    tolerance=max(1e-8,radius*.25)*(1-quality)**2
    max_arc=arc[-1]/(len(points)-1)/quality if quality>0 else arc[-1]+1
    keep={0,len(points)-1};pending=[(0,len(points)-1)]
    while pending:
        a,b=pending.pop()
        if b<=a+1:continue
        chord=points[b]-points[a];length2=np.dot(chord,chord)
        delta=points[a+1:b]-points[a]
        t=np.clip(delta@chord/max(length2,1e-20),0,1)
        error=np.sum((delta-t[:,None]*chord)**2,axis=1)
        split=a+1+int(np.argmax(error))
        if error[split-a-1]<=tolerance*tolerance:
            if arc[b]-arc[a]<=max_arc:continue
            split=(a+b)//2
        keep.add(split);pending.extend(((a,split),(split,b)))
    return np.array(sorted(keep),dtype=int)


def sweep(path,radius,sides=12,kind=0,height=.12,pitch=.65,twist=10.,normal=(0,1,0),exposure=None,detail=1.,closed=False,resolution=1.):
    """Recalculate ring count, ribs and UVs from actual new arc length."""
    step=radius*.6/max(.1,detail)
    if kind in (0,1):step=min(step,max(.05,pitch)*radius*2/8)
    if closed: path=np.vstack((path,np.asarray(path)[0]))
    points,s=sample(path,step)
    if len(points)<2:return None
    resolution=max(0.,min(1.,resolution))
    ids=rebar_lod_indices(points,radius,resolution)
    points=points[ids];s=s[ids]
    sides=4+int((max(4,sides)-4)*resolution+.5)
    n,b=frames(points,normal)
    theta=np.arange(sides)*2*np.pi/sides
    e=np.ones(len(points)) if exposure is None else np.interp(s/max(s[-1],1e-9),np.linspace(0,1,len(exposure)),exposure)
    rr=profile(kind,radius,theta,s,e,height,pitch,twist)
    if resolution<1:rr=radius+resolution*(rr-radius)
    verts=(points[:,None,:]+rr[:,:,None]*(np.cos(theta)[None,:,None]*n[:,None,:]+np.sin(theta)[None,:,None]*b[:,None,:])).reshape(-1,3)
    faces=[];uv=[]
    for j in range(len(points)-1):
        for k in range(sides):
            k1=(k+1)%sides
            faces.append((j*sides+k,j*sides+k1,(j+1)*sides+k1,(j+1)*sides+k))
                                                                                  
            uv.append(((s[j],k*2*np.pi*radius/sides),(s[j],(k+1)*2*np.pi*radius/sides),(s[j+1],(k+1)*2*np.pi*radius/sides),(s[j+1],k*2*np.pi*radius/sides)))
    if closed:
        last=(len(points)-1)*sides
        faces=[tuple(v if v<last else v-last for v in face) for face in faces]
        verts=verts[:-sides];e=e[:-1];s=s[:-1];points=points[:-1]
    else:
        faces.extend((tuple(reversed(range(sides))),tuple((len(points)-1)*sides+k for k in range(sides))))
        uv.append(tuple((float(np.cos(t)*radius),float(np.sin(t)*radius)) for t in theta[::-1]))
        uv.append(tuple((float(np.cos(t)*radius),float(np.sin(t)*radius)) for t in theta))
    return verts,faces,uv,np.repeat(e,sides),s,points
