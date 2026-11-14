# используем профили арматуры на управляемой поверхности
import numpy as np
from . import core_loader


def generate(props,centers,normals,unit,start_free,end_free,key):
    core=core_loader.load()
    if core is None or not hasattr(core,'build_rebar_surface_path'):
        raise RuntimeError('Restart Blender to load the updated rebar generator')
    params=core.BreakRebarParams()
    params.type=int(props.rebar_type)
    params.radius=props.radius*100.0
    params.rib_height=props.rebar_rib_height/100
    params.rib_pitch=props.rebar_rib_pitch
    params.twist_pitch=props.rebar_twist_pitch
    params.neck=props.rebar_neck/100
    params.wobble=props.rebar_wobble/100
    params.rebar_resolution=min(1.,props.resolution/100)
    params.detail=max(.05,props.resolution/100)
    params.seed=(props.seed+key*977)&0x7fffffff
    factor=100.0/unit
    result=core.build_rebar_surface_path(params,(np.asarray(centers)*factor).ravel().tolist(),
        np.asarray(normals).ravel().tolist(),start_free,end_free)
    faces=[tuple(int(i) for i in (f[:3] if f[2]==f[3] else f)) for f in result['polys']]
    return np.asarray(result['points'])/factor,faces
