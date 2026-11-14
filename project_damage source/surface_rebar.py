# ведём центральную арматуру внутри отдельных частей объекта
from . import preview_quality
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded ray-clipped reinforcement for the Surface Damage crater faces.

Use original-face tangent planes and final effect IDs. Each sample must hit a
Surface crater below the chosen cover depth. Other effects and intact faces
cannot reveal bars. No Blender operators run while building the output.
"""
import math
import bpy
from mathutils import Vector
from mathutils.bvhtree import BVHTree


STAMP='project_damage_surface_rebar_of'


def _remove(source):
    from .generated_assets import output
    obj=output(source,'surface_rebar')
    if obj is not None:
        mesh=obj.data;bpy.data.objects.remove(obj,do_unlink=True)
        if mesh.users==0:bpy.data.meshes.remove(mesh)


def after_build(context,source,target,reports=None,geometry=None,input_geometry=None):
    p=source.project_damage
    if (target is None or not p.surf_enable or p.all_off or p.surf_type!='0'
            or not p.surface_rebar_enable or not p.surface_rebar_generated_enable or p.surface_rebar_amount<=0):
        _remove(source);return None
    from . import params,effect_groups,generated_assets,material_defaults
    from . import incremental
    existing=generated_assets.output(source,'surface_rebar')
    if incremental.reuse_output(context,source,target,'surface_rebar',existing):return existing
    factor=params.unit_factor(context)
                                                                             
                                                                             
    if geometry is not None and len(geometry.get('effect_ids',()))==len(geometry.get('polys',())):
        verts=[Vector(row)/factor for row in geometry['points']]
        faces=[tuple(int(x) for x in (row[:3] if row[2]==row[3] else row)) for row in geometry['polys']]
        damaged={i for i,value in enumerate(geometry['effect_ids']) if value==3}
    else:
        attr=target.data.attributes.get(effect_groups.ATTRIBUTE)
        if attr is None:
            _remove(source);return None
        damaged={i for i,item in enumerate(attr.data) if item.value==3}
        verts=[v.co.copy() for v in target.data.vertices]
        faces=[tuple(f.vertices) for f in target.data.polygons]
    if not damaged:
        _remove(source);return None
    if input_geometry is not None:
        original=[Vector(row)/factor for row in input_geometry[0]]
        original_faces=[tuple(int(x) for x in (row[:3] if row[2]==row[3] else row)) for row in input_geometry[1]]
    else:
        original=[v.co.copy() for v in source.data.vertices]
        original_faces=[tuple(f.vertices) for f in source.data.polygons]
    original_bvh=BVHTree.FromPolygons(original,original_faces)
    from . import surface_guidance
    original_bvh=surface_guidance.wrap_bvh(original_bvh,original,original_faces)
    result_bvh=BVHTree.FromPolygons(verts,faces)
    scale=max(.01,p.surf_scale/100.)*max(.01,p.global_scale/100.)/factor
    radius=p.surface_rebar_radius*scale
    cover=max(p.surface_rebar_cover*scale,radius)
    pitch=max(radius*5.,max(p.surf_size_min,1.)*scale*.3/math.sqrt(p.surface_rebar_amount))
    groups={}
    for face_id in damaged:
        face=faces[face_id]
        center=sum((verts[v] for v in face),Vector())/len(face)
        hit,normal,index,distance=original_bvh.find_nearest(center)
        if hit is None:continue
        d=normal.dot(hit)
        key=tuple(round(x,4) for x in normal)+(round(d,5),)
        group=groups.setdefault(key,[normal,hit,[]])
        group[2].extend(verts[v] for v in face)
    points=[];faces=[];budget=60000;steps_used=0
    sides=4+round(4*preview_quality.rebar_resolution(p,'surface_rebar_resolution'))
    def tube(a,b,n):
        direction=(b-a).normalized()
        if (b-a).length<radius:return
        u=direction.cross(n).normalized();v=direction.cross(u).normalized()
        offset=len(points)
        for center in (a,b):
            points.extend(tuple(center+radius*(u*math.cos(2*math.pi*k/sides)+v*math.sin(2*math.pi*k/sides))) for k in range(sides))
        for k in range(sides):
            j=(k+1)%sides;faces.append((offset+k,offset+j,offset+sides+j,offset+sides+k))
        faces.append(tuple(offset+k for k in reversed(range(sides))))
        faces.append(tuple(offset+sides+k for k in range(sides)))
    for normal,origin,patch in groups.values():
        helper=Vector((0,0,1)) if abs(normal.z)<.9 else Vector((0,1,0))
        u=helper.cross(normal).normalized();v=normal.cross(u).normalized()
        xy=[((pt-origin).dot(u),(pt-origin).dot(v)) for pt in patch]
        lo=[min(pt[k] for pt in xy) for k in (0,1)];hi=[max(pt[k] for pt in xy) for k in (0,1)]
        local_pitch=max(pitch,max(hi[k]-lo[k] for k in (0,1))/96.)
        eps=max(radius*.1,1e-6)
        for family,(along,across) in enumerate(((u,v),(v,u))):
            k=family;j=1-family
            count=min(96,max(1,math.ceil((hi[j]-lo[j])/local_pitch)))
            samples=min(384,max(2,math.ceil((hi[k]-lo[k])/(local_pitch*.2))))
            step=(hi[k]-lo[k])/samples
            depth=cover+family*2.05*radius
            for line in range(count+1):
                cross=lo[j]+(hi[j]-lo[j])*(line+.5)/(count+1)
                start=None
                for sample in range(samples+2):
                    visible=False
                    x=lo[k]+sample*step
                    if sample<=samples and steps_used<budget:
                        steps_used+=1
                        outer=origin+along*x+across*cross
                        hit,n,face_id,distance=result_bvh.ray_cast(outer+normal*eps,-normal)
                        visible=hit is not None and face_id in damaged and distance>depth+radius+eps
                    if visible and start is None:start=x
                    if not visible and start is not None:
                                                                               
                        a=origin+along*(start-step)+across*cross-normal*depth
                        b=origin+along*x+across*cross-normal*depth
                        tube(a,b,normal);start=None
                if steps_used>=budget:break
            if steps_used>=budget:break
        if steps_used>=budget:break
    if not faces:
        _remove(source);return None
    mesh=bpy.data.meshes.new(source.name+'_SurfaceReinforcement')
    mesh.from_pydata(points,[],faces);mesh.update()
    mesh.materials.append(p.surface_rebar_material or material_defaults.get('rebar'))
    zones=mesh.attributes.new(effect_groups.ATTRIBUTE,'INT','FACE')
    zones.data.foreach_set('value',[3]*len(mesh.polygons))
    for face in mesh.polygons:face.use_smooth=len(face.vertices)==4
    obj=generated_assets.place(context,source,target,'surface_rebar',mesh)
    if reports is not None and steps_used>=budget:
        reports.append(({'WARNING'},'Surface reinforcement reached its sampling budget; lower Amount for very large patches'))
    return obj
