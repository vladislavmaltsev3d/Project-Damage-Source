# используем те же профили труб, что и в Pipe Damage
from types import SimpleNamespace
import numpy as np
from mathutils import Vector
from . import pipes, material_controls


def defaults(props):
    values=pipes.TYPES[props.pipe_type]
    props['radius']=values['d']/200.0
    props['pipe_wall']=values['wall']/100.0
    props['pipe_joint_spacing']=values['joints']/100.0
    props['pipe_wires']=values['wires']
    material(props)


def material(props):
    current=props.material
    if current and not props.get('_pd_auto_material'):
        from .pipe_material_library import PIPE_NAMES
        if current.get('pd_pipe_library_name') in PIPE_NAMES.values():
            props['_pd_auto_material']=current.name
            props['_pd_auto_material_type']=''
    return material_controls.automatic_default(props,'material',props.pipe_type,pipes._type_material)


def add_run(props,buffer,centers,normals,unit,start_free,end_free,key,phase_offset,route_key):
    factor=100.0/unit
    path=np.asarray(centers,dtype=np.float64)*factor
    nout=np.asarray(normals,dtype=np.float64)
    keep=np.r_[True,np.linalg.norm(np.diff(path,axis=0),axis=1)>1e-6]
    path,nout=path[keep],nout[keep]
    if len(path)<2:return
    radius=props.radius*100.0
    settings=SimpleNamespace(t=pipes.TYPES[props.pipe_type],D=2*radius,R=radius,
        wall=min(max(props.pipe_wall*100.0,radius*.03),radius*.85),
        joint=props.pipe_joint_spacing*100.0,sides=max(4,round(4+12*props.resolution/100)),
        jag=props.pipe_jagged/100,dent=props.pipe_dents/100,
        wire_count=props.pipe_wire_count,surface_resolution=max(.2,props.resolution/100))
    rng=pipes._rng(props.seed,5183,key)
    settings._joint_phase=pipes._rng(props.seed,8041,route_key).uniform(0,max(settings.joint,1.0))-phase_offset*factor
    cum=pipes._cum(path)
    piece=pipes.Piece(0.,float(cum[-1]),start_free,end_free)
    piece.visible=[(0.,float(cum[-1]))]
    down=props.id_data.matrix_world.inverted_safe().to_3x3()@Vector((0,0,-1))
    down=np.asarray(down.normalized())
    part=key*2
    if pipes.build_piece(settings,buffer,part,path,cum,nout,piece,rng,down) and props.pipe_wires:
        pipes.build_wires(settings,buffer,part+1,[piece],rng,down)


def slots(props,used):
    result=[material(props)]
    if used>1:result.append(pipes._plain_material('ProjectDamage_Pipe_Inside',(.05,.045,.04),.9))
    for index,color in enumerate(pipes.WIRE_COLORS):
        if len(result)>=used:break
        result.append(pipes._plain_material('ProjectDamage_Wire_%d'%(index+1),color,.45))
    return result
