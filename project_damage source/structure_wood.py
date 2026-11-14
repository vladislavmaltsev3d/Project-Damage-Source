# те же профили дранки и каркаса, но на управляемой поверхности
import math
import numpy as np
from . import core_loader


def generate(p,chart,damage,unit):
    core=core_loader.load()
    if core is None or not hasattr(core,'build_lath_surface'):raise RuntimeError('Restart Blender to load the structural wood generator')
    params=core.LathDamageParams()
    params.style=2 if p.kind=='STUD' else int(p.wood_style)
    params.seed=p.seed
    quality=max(.08,p.resolution/100)
    params.detail=quality
    factor=100/unit
    params.lath_width=p.width*100;params.lath_thickness=p.thickness*100
    params.lath_gap=max(.001,p.spacing-p.width)*100
    params.lath_length=p.board_length*100
    params.lath_missing=0;params.lath_broken=0;params.hole_laths=0
    params.lath_optimize=0 if chart.curved else min(1.,.35/quality)
    params.lath_bend=0
    params.lath_splinter=p.splinters
    params.stud_enable=True;params.stud_spacing=p.support_spacing*100
    params.stud_width=p.support_width*100;params.stud_depth=p.support_depth*100
    if p.kind=='STUD':
        params.stud_spacing=p.spacing*100;params.stud_width=p.width*100;params.stud_depth=p.thickness*100
    params.frame_plates=True;params.double_plate=True;params.noggin_spacing=p.noggin_spacing*100
    params.nail_amount=p.nails/100;params.nail_length=p.nail_length*100
    (u0,u1),(v0,v1)=chart.bounds
    # Native grid uses centimetres, while projection stays in the source's local coordinates.
    step=max(.003*unit,min(p.width*.5*unit,max(u1-u0,v1-v0)/150))/quality
    nu=max(4,math.ceil((u1-u0)/step));nv=max(4,math.ceil((v1-v0)/step))
    if nu*nv>180000:
        scale=math.sqrt(nu*nv/180000);nu=max(4,int(nu/scale));nv=max(4,int(nv/scale))
    heights=[];valid=[];holes=[]
    amount=p.break_amount/100
    for i in range(nu+1):
        x=u0+(u1-u0)*i/nu
        for j in range(nv+1):
            y=v0+(v1-v0)*j/nv;pos,normal,inside=chart.sample(x,y)
            ratio=damage.ratio(pos) if inside else 2
            valid.append(int(inside));holes.append(int(ratio<amount))
            heights.append(((1-ratio*ratio)**2*p.bend*100 if ratio<1 else 0))
    data=core.build_lath_surface(params,u0*factor,u1*factor,v0*factor,v1*factor,nu,nv,heights,valid,holes,p.nail_size/100)
    points=np.asarray(data['points'])/factor
    frames=np.asarray(data['frames'],dtype=np.float64).copy();frames[:,1:3]/=factor
    mapped=chart.map_members(points,frames,data['vertex_pieces'])
    return mapped,np.asarray(data['faces']),np.asarray(data['materials']).ravel(),np.asarray(data['pieces']).ravel(),points
