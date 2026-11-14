# подбираем маршруты и радиусы изгибов труб и кабелей
# SPDX-License-Identifier: GPL-3.0-or-later
"""Physical centreline routing for regenerated pipe and cable surfaces."""
import math
import numpy as np


def unit(v, fallback=(1.,0.,0.)):
    v=np.asarray(v,dtype=float);length=np.linalg.norm(v)
    return v/length if length>1e-10 else np.asarray(fallback,dtype=float)


def cumulative(path):
    return np.r_[0.,np.cumsum(np.linalg.norm(np.diff(path,axis=0),axis=1))]


def at(path,s):
    path=np.asarray(path);arc=cumulative(path)
    return np.column_stack([np.interp(np.atleast_1d(s),arc,path[:,i]) for i in range(3)])


def pose(matrix,points):
    return np.asarray(points)@matrix[:3,:3].T+matrix[:3,3]


def rigid_rotation(matrix):
    u,_,v=np.linalg.svd(matrix[:3,:3]);r=u@v
    if np.linalg.det(r)<0:u[:,-1]*=-1;r=u@v
    return r


def simplify(path,tolerance):
    path=np.asarray(path)
    keep={0,len(path)-1};stack=[(0,len(path)-1)]
    while stack:
        a,b=stack.pop()
        if b-a<2:continue
        direction=unit(path[b]-path[a]);delta=path[a+1:b]-path[a]
        distances=np.linalg.norm(delta-direction[None,:]*(delta@direction)[:,None],axis=1)
        k=int(np.argmax(distances))
        if distances[k]>tolerance:
            k+=a+1;keep.add(k);stack.extend(((a,k),(k,b)))
    return path[sorted(keep)]


def fillet(polyline,radius):
    """Circular elbows with a specified physical radius. None = cannot fit."""
    pts=np.asarray(polyline,dtype=float)
    pts=pts[np.r_[True,np.linalg.norm(np.diff(pts,axis=0),axis=1)>1e-8]]
    if len(pts)<2:return None
    result=[pts[0]]
    for i in range(1,len(pts)-1):
        incoming=unit(pts[i]-pts[i-1]);outgoing=unit(pts[i+1]-pts[i])
        turn=math.acos(float(np.clip(incoming@outgoing,-1,1)))
        if turn<1e-3:continue
        if turn>math.pi-.05:return None
        cut=radius*math.tan(turn*.5)
        available=.42*min(np.linalg.norm(pts[i]-pts[i-1]),np.linalg.norm(pts[i+1]-pts[i]))
        if cut>available+1e-8:return None
        entry=pts[i]-incoming*cut;exit=pts[i]+outgoing*cut
        radial=unit(outgoing-incoming*(incoming@outgoing))
        center=entry+radial*radius
        x=entry-center;y=incoming*radius
        count=max(3,int(math.ceil(turn/(math.pi/24)))+1)
        result.extend(center+math.cos(a)*x+math.sin(a)*y for a in np.linspace(0,turn,count))
        result[-1]=exit
    result.append(pts[-1])
    result=np.asarray(result)
    return result[np.r_[True,np.linalg.norm(np.diff(result,axis=0),axis=1)>1e-8]]


def pipe_route(rest,ma,mb,diameter,embed,kind):
    """Straight rigid runs and physical elbows, with no gravity curve.
    If the required elbow cannot fit, the caller emits two broken pipe ends.
    """
    rest=np.asarray(rest,dtype=float)
    if np.allclose(ma,mb,atol=1e-9):return pose(ma,rest)
    radius=max(diameter*(4. if kind=='CONDUIT' else 2. if kind=='COPPER' else 1.5),1e-5)
    a,b=pose(ma,rest[:1])[0],pose(mb,rest[-1:])[0]
    span=np.linalg.norm(b-a)
    if span<diameter*.5:return None
    ta=unit(rigid_rotation(ma)@(rest[1]-rest[0]));tb=unit(rigid_rotation(mb)@(rest[-1]-rest[-2]))
    lead=max(radius*2.5,embed)
    if span<lead*2.2:
                                                                                
        chord=unit(b-a)
        if min(ta@chord,tb@chord)>.995:return np.array([a,b])
        return None
    original=simplify(rest,max(diameter*.8,cumulative(rest)[-1]*.015))
    arc=cumulative(original);inside=[]
    for point,s in zip(original[1:-1],arc[1:-1]):
        fraction=s/max(arc[-1],1e-9)
        if .15<fraction<.85:
            inside.append((1-fraction)*pose(ma,point[None,:])[0]+fraction*pose(mb,point[None,:])[0])
    path=np.array([a,a+ta*lead,*inside,b-tb*lead,b])
    return fillet(path,radius)


def split_pipe(rest,ma,mb):
    """Held ends are rigid pieces of original length, not stretched stumps."""
    rest=np.asarray(rest);arc=cumulative(rest);reach=arc[-1]*.4
    left=np.vstack((rest[0],rest[(arc>0)&(arc<reach)],at(rest,reach)[0]))
    right=np.vstack((at(rest,arc[-1]-reach)[0],rest[(arc>arc[-1]-reach)&(arc<arc[-1])],rest[-1]))
    return pose(ma,left),pose(mb,right)


def fixed_lead(path,diameter,embed):
    """Keep a short end fixed in its support while the free span relaxes."""
    arc=cumulative(path)
    length=min(max(diameter*1.5,embed),arc[-1]*.15)
    return max(length,min(arc[-1]*.05,diameter*.5))


def loop_length(old_length,chord,slack,hang_length=0.):
                                                                            
                                                                     
    slack=max(0.,float(slack))
    desired=max(chord*(1+slack),2*math.hypot(chord*.5,max(0.,hang_length)))
    return max(chord*1.001,min(max(old_length,desired),max(desired,chord*1.001)*1.25))
