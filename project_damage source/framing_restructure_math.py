# вычисляем стыки, изломы и крепления досок без растяжения профиля
# SPDX-License-Identifier: GPL-3.0-or-later
"""Deterministic timber fracture planning and closed splintered lumber meshes."""
import math
import numpy as np


def unit(v, fallback=(1.,0.,0.)):
    n=np.linalg.norm(v)
    return np.asarray(v)/n if n>1e-10 else np.asarray(fallback,dtype=float)


def transformed(m,p):return m[:3,:3]@p+m[:3,3]


def rotation(m):
    u,_,v=np.linalg.svd(m[:3,:3]);r=u@v
    if np.linalg.det(r)<0:u[:,-1]*=-1;r=u@v
    return r


def angle(a,b):
    relative=rotation(a).T@rotation(b)
    return math.degrees(math.acos(float(np.clip((np.trace(relative)-1)/2,-1,1))))


def describe(points):
    points=np.asarray(points);center=points.mean(axis=0)
    _,basis=np.linalg.eigh((points-center).T@(points-center))
    axis=unit(basis[:,-1])
    if axis[np.argmax(np.abs(axis))]<0:axis=-axis
    width=unit(basis[:,-2]);depth=unit(np.cross(axis,width))
    local=np.column_stack(((points-center)@axis,(points-center)@width,(points-center)@depth))
    lo=local.min(axis=0);hi=local.max(axis=0)
    center=center+width*((lo[1]+hi[1])*.5)+depth*((lo[2]+hi[2])*.5)
    return center+axis*lo[0],center+axis*hi[0],width,max((hi[1]-lo[1])*.5,1e-5),max((hi[2]-lo[2])*.5,1e-5)


def select_survivors(members,minimum):
    """Spread protected links per pair of solids, rather than one local clump."""
    pairs={}
    for i,m in enumerate(members):
        if m['ia']!=m['ib']:pairs.setdefault(tuple(sorted((m['ia'],m['ib']))),[]).append(i)
    keep=set()
    for indices in pairs.values():
        chosen=[]
        for _ in range(min(minimum,len(indices))):
            pool=[i for i in indices if i not in chosen]
            if not chosen:
                pick=min(pool,key=lambda i:(members[i]['collision'],members[i]['angle'],members[i]['key']))
            else:
                pick=min(pool,key=lambda i:(members[i]['collision'],-min(np.linalg.norm(members[i]['mid']-members[j]['mid']) for j in chosen),members[i]['key']))
            chosen.append(pick)
        keep.update(chosen)
    return keep


def broken_paths(member):
    """Two independent straight splinters, each held by its original end."""
    a,b=member['a'],member['b'];rest=member['rest_b']-member['rest_a']
    ta=unit(member['ma'][:3,:3]@rest);tb=unit(member['mb'][:3,:3]@rest)
    span=np.linalg.norm(b-a);old=np.linalg.norm(rest)
    reach=min(span*.40,old*.60)
    return [np.array([a,a+ta*reach]),np.array([b-tb*reach,b])]


def lumber(path,width,depth,normal,key=0,broken_start=False,broken_end=False):
    """Continuous timber shell, constant rectangular section and jagged caps."""
    from .structure_rebuild import frames
    path=np.asarray(path,dtype=float)
    if len(path)<2 or np.linalg.norm(np.diff(path,axis=0),axis=1).sum()<1e-7:return None
                                                                             
    ring=np.array([(x,-depth) for x in np.linspace(-width,width,4,endpoint=False)]+
                  [(width,y) for y in np.linspace(-depth,depth,2,endpoint=False)]+
                  [(x,depth) for x in np.linspace(width,-width,4,endpoint=False)]+
                  [(-width,y) for y in np.linspace(depth,-depth,2,endpoint=False)])
    n,b=frames(path,normal);count=len(ring);vertices=[];grain=[]
    distances=np.r_[0,np.cumsum(np.linalg.norm(np.diff(path,axis=0),axis=1))]
    rng=np.random.default_rng(int(key)&0xffffffff)
    teeth=rng.uniform(.08,1.,count)*min(width*1.6,distances[-1]*.15)
    for j,center in enumerate(path):
        tangent=unit(path[min(j+1,len(path)-1)]-path[max(0,j-1)])
        for k,(x,y) in enumerate(ring):
            offset=0.
            if j==0 and broken_start:offset=teeth[k]
            if j==len(path)-1 and broken_end:offset=-teeth[k]
            vertices.append(center+n[j]*x+b[j]*y+tangent*offset)
            grain.append((distances[j]+offset,x,y))
    faces=[]
    for j in range(len(path)-1):
        for k in range(count):
            k1=(k+1)%count
                                                      
            faces.append((j*count+k,j*count+k1,(j+1)*count+k1,(j+1)*count+k))
    for end in (0,len(path)-1):
        center=np.mean(vertices[end*count:(end+1)*count],axis=0)
        ci=len(vertices);vertices.append(center);grain.append((distances[end],0.,0.))
        for k in range(count):
            k1=(k+1)%count
            faces.append((ci,end*count+k1,end*count+k) if end==0 else (ci,end*count+k,end*count+k1))
    return np.asarray(vertices),faces,np.asarray(grain)


def fit_nail(points,member,segments):
    """Rigidly transport a native nail from its timber's surviving attachment.
    Missing timber at the original shank position means the nail is discarded.
    """
    from .structure_rebuild import frames
    points=np.asarray(points,dtype=float);root=points[0]
    axis=unit(member['rest_b']-member['rest_a'])
    length=np.linalg.norm(member['rest_b']-member['rest_a'])
    if length<1e-9:return None
    fraction=float(np.dot(root-member['rest_a'],axis)/length)
    oldnormal=unit(member['rest_normal']-axis*np.dot(member['rest_normal'],axis))
    oldbin=unit(np.cross(axis,oldnormal))
    radial=root-(member['rest_a']+axis*(fraction*length))
    x,y=np.dot(radial,oldnormal),np.dot(radial,oldbin)
    if abs(x)>member['width']+1e-6 or abs(y)>member['depth']+1e-6:return None
    for segment in segments:
        lo,hi=segment['interval']
        if fraction<lo-1e-8 or fraction>hi+1e-8:continue
        path=np.asarray(segment['path']);dist=np.r_[0,np.cumsum(np.linalg.norm(np.diff(path,axis=0),axis=1))]
        if dist[-1]<1e-9:continue
        target=np.clip((fraction-lo)/max(hi-lo,1e-9),0.,1.)*dist[-1]
        i=min(len(path)-2,max(0,int(np.searchsorted(dist,target,side='right')-1)))
        tangent=unit(path[i+1]-path[i])
        normals,bins=frames(path,member['normal'])
        normal=unit(normals[i]-tangent*np.dot(normals[i],tangent));binormal=unit(np.cross(tangent,normal))
        center=path[i]+tangent*(target-dist[i]);anchor=center+x*normal+y*binormal
        oldbasis=np.column_stack((axis,oldnormal,oldbin));newbasis=np.column_stack((tangent,normal,binormal))
        orient=newbasis@oldbasis.T
        return (points-root)@orient.T+anchor,segment
    return None
