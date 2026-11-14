# режем осколки по рисунку и учитываем соединения между штрихами
# SPDX-License-Identifier: GPL-3.0-or-later
"""Finite Grease Pencil knives through cells or only the directly touched skin.

Cuts happen AFTER cell generation. They never move Voronoi seeds. The native
Boolean retains original corner UVs/normals and assigns new knife faces to the
Cell Fracture inner material. Tiny numerical kerfs let connected solids split.
"""
from pathlib import Path
import importlib.util
import sys
import bpy
import numpy as np
from mathutils import Vector
from mathutils.bvhtree import BVHTree

_native=None

def load():
 global _native
 if _native is not None:return _native
 from . import core_loader
 abi='cp'+str(sys.version_info.major)+str(sys.version_info.minor)
 for root in [Path(core_loader.core_dir()),*(Path(__file__).parent/'core').glob('*'+abi)]:
  for path in root.glob('project_damage_cellcuts*'):
   if path.suffix not in {'.so','.pyd'}:continue
   spec=importlib.util.spec_from_file_location('project_damage_cellcuts',path)
   module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);_native=module;return module
 raise RuntimeError('Cell Fracture drawing: native cutting module is missing')

def hash_inputs(h,source):
 from . import paint,build
 p=source.project_damage
 if not p.cellf_enable:return
 from . import cell_cut_plane
 cell_cut_plane.hash_inputs(h,source)
 build._hash_value(h,'finite-layered-cell-knives-v5')
 if p.cellf_material=='1':build._hash_value(h,p.cellf_knife_width)
 for mode in ('THROUGH','SURFACE'):
  paths=paint.read_cell_paths(bpy.context,source,mode)
  build._hash_value(h,len(paths))
  for path in paths:h.update(path.tobytes())

def _tree(points,polys):
 return BVHTree.FromPolygons([Vector(v) for v in points],[tuple(row[:3]) if row[2]==row[3] else tuple(row) for row in polys])

def _knife(path,normal,depths,kerf,width=0.):
 """Closed variable-depth prisms; neighbouring segments overlap at their joints."""
 vertices=[];triangles=[]
 width=max(width,kerf)
 overlap=max(kerf,width*.5)
 quads=((0,2,3,1),(4,5,7,6),(0,1,5,4),(2,6,7,3),(0,4,6,2),(1,3,7,5))
 for k,(a,b) in enumerate(zip(path[:-1],path[1:])):
  tangent=b-a;length=np.linalg.norm(tangent)
  if length<kerf:continue
  tangent/=length;side=np.cross(normal,tangent);side/=max(np.linalg.norm(side),1e-20);side*=width*.5
  offset=len(vertices)
  for point,depth in ((a-tangent*overlap,depths[k]),(b+tangent*overlap,depths[k+1])):
   for s in (-side,side):
    for d in (-kerf,depth+kerf):vertices.append(point+s-normal*d)
                                                           
  for q in quads:
   triangles.extend(((offset+q[0],offset+q[1],offset+q[2]),(offset+q[0],offset+q[2],offset+q[3])))
 v=np.asarray(vertices,np.float64).reshape(-1,3);t=np.asarray(triangles,np.int32).reshape(-1,3)
 if len(t):
  volume=np.einsum('ij,ij->i',v[t[:,0]],np.cross(v[t[:,1]],v[t[:,2]])).sum()
  if volume<0:t=t[:,::-1].copy()
 return v,t

def _samples(path, step):
 for a,b in zip(path[:-1],path[1:]):
  length=float(np.linalg.norm(b-a))
  for t in np.linspace(0.,1.,min(1024,max(2,int(length/step)+1))):
   yield a+(b-a)*t

def _snap_branches(paths, tolerance):
 """Snap only nearby stroke endpoints to finite segments of other strokes.
 An endpoint may attach to a cut boundary, but never extend across a shard.
 """
 original=[np.asarray(p,dtype=np.float64) for p in paths]
 result=[p.copy() for p in original]
 for i,path in enumerate(original):
  for end in (0,-1):
   point=path[end];best=None;distance=tolerance
   for j,other in enumerate(original):
    if i==j or len(other)<2:continue
    a=other[:-1];d=other[1:]-a;l=np.einsum('ij,ij->i',d,d)
    f=np.clip(np.einsum('ij,ij->i',point-a,d)/np.maximum(l,1e-20),0,1)
    near=a+d*f[:,None];dist=np.linalg.norm(near-point,axis=1);k=int(np.argmin(dist))
    if dist[k]<distance:distance=float(dist[k]);best=near[k]
   if best is not None:result[i][end]=best
 return result


def prepare(context,source,out,factor,only=None,paths_by_role=None):
 from . import paint,damage_physics
 from .params import _clamp_scale
 p=source.project_damage
 from .params import unit_factor
 raw={role:paths_by_role.get(role,[]) if paths_by_role is not None else paint.read_cell_paths(context,source,role) for role in ('THROUGH','SURFACE')}
 reach=p.surf_paint_snap*_clamp_scale(p.global_scale/100.)*factor/unit_factor(context)
 return prepare_arrays(out,factor,only,raw,reach)

def _project_stroke(path, exterior, step, snap, kerf):
 samples=[Vector(co) for co in _samples(path,step)]
 hits=[exterior.find_nearest(point,snap) for point in samples]
 start=min((h for h in hits if h[0] is not None),key=lambda h:h[3],default=None)
 if start is None:return
 points=[];normals=[];reference=start[1]
 for point,(hit,normal,_,distance) in zip(samples,hits):
  if hit is not None and normal.dot(reference)<.25:
   candidates=[item for item in exterior.find_nearest_range(point,snap) if item[1].dot(reference)>=.25]
   hit,normal,_,distance=min(candidates,key=lambda item:item[3]) if candidates else (None,None,None,None)
  if hit is None:
   if len(points)>1:yield np.asarray(points),np.asarray(normals)
   points=[];normals=[];reference=start[1]
   continue
  if not points or (hit-points[-1]).length>kerf:
   points.append(hit)
   normals.append(normal)
   reference=normal
 if len(points)>1:yield np.asarray(points),np.asarray(normals)


def prepare_arrays(out,factor,only,raw,reach,project_surface=False,width=0.):
 from . import damage_physics
 points=np.asarray(out['cellf_points'],np.float64);polys=np.asarray(out['cellf_polys'],np.int32)
 ids=np.asarray(out['cellf_piece'],np.int32)
 if not len(polys):return [],[],[]
 diagonal=float(np.linalg.norm(np.ptp(points,axis=0)));kerf=max(diagonal*2e-6,1e-7)
                                                                             
                                                                               
 surface=_tree(points,polys);records=[]
 outer=np.flatnonzero(np.asarray(out['cellf_base_poly'])>=0) if project_surface else np.arange(len(polys))
 if not len(outer):outer=np.arange(len(polys))
 exterior=_tree(points,polys[outer]) if project_surface else surface
 snap=max(kerf*10,reach)
                                                                                 
 unique,indices=np.unique(ids,return_inverse=True)
 lo=np.full((len(unique),3),np.inf);hi=np.full((len(unique),3),-np.inf)
 corners=points[polys.reshape(-1)];owners=np.repeat(indices,4)
 np.minimum.at(lo,owners,corners);np.maximum.at(hi,owners,corners)
 spans=np.linalg.norm(hi-lo,axis=1)
 step=max(kerf*10,min(diagonal/256,float(spans.min())/8))
 edge_reach=max(kerf*8,min(float(np.median(spans))*.035,snap*.35))
 allpaths=[np.asarray(path)*factor for role in raw for path in raw[role]]
 snapped=_snap_branches(allpaths,max(kerf*8,min(float(spans.min())*.025,snap*.25)))
 roles=['THROUGH']*len(raw['THROUGH'])+['SURFACE']*len(raw['SURFACE'])
 projected=[]
 for role,path in zip(roles,snapped):
  if only is not None and (role=='THROUGH')!=only:continue
  if project_surface:
   projected.extend((role,co,normals) for co,normals in _project_stroke(path,exterior,step,snap,kerf))
  else:projected.append((role,path,None))
 for role,path,projected_normals in projected:
   through=role=='THROUGH'
   if only is not None and through!=only:continue
   path=damage_physics._simplify(path,max(diagonal*1e-5,kerf*2))
   if len(path)<2:continue
   samples=list(_samples(path,step))
   hits=[exterior.find_nearest(Vector(co)) for co in samples]
   valid=[h for h in hits if h[0] is not None and h[3]<=snap]
   if not valid:continue
   if projected_normals is None:
    normal=np.asarray(min(valid,key=lambda h:h[3])[1],np.float64)
   else:
    normal=projected_normals.sum(axis=0)
    norm=np.linalg.norm(normal)
    normal=normal/norm if norm>1e-8 else projected_normals[len(projected_normals)//2]
   touched=set()
   for co,h in zip(samples,hits):
    if not through:
     if h[0] is not None and h[3]<=snap and np.dot(h[1],normal)>.2:
      touched.add(int(ids[outer[h[2]]]))
    else:
                                                                           
                                                                               
     origin=co+normal*snap;remain=diagonal*2+snap
     for _ in range(min(4096,len(polys))):
      hit=surface.ray_cast(Vector(origin),Vector(-normal),remain)
      if hit[0] is None:break
      touched.add(int(ids[hit[2]]))
      advance=float(hit[3])+kerf*4;remain-=advance
      if remain<=0:break
      origin-=normal*advance
   if not touched:continue
                                                                          
                                                                           
   tangent=path[-1]-path[0]
   if projected_normals is not None:
    tangent=tangent-normal*np.dot(tangent,normal)
    if np.linalg.norm(tangent)<kerf*8:continue
    for end,adjacent in ((0,1),(-1,-2)):
     outward=path[end]-path[adjacent]
     outward-=normal*np.dot(outward,normal)
     length=np.linalg.norm(outward)
     if length>kerf:path[end]+=outward/length*edge_reach
   support=float((points@normal).max())+kerf*10
   path=path+(support-path@normal)[:,None]*normal
   depth=np.maximum(kerf,path@normal-float((points@normal).min())+kerf*4)
   records.append([path,normal,depth,sorted(touched)])
 sheets=[];cp=[];ct=[];targets=[]
 for path,normal,depth,target in records:
  for i,point in enumerate(path):
   for sheet in sheets:
    hit=sheet.ray_cast(Vector(point-normal*kerf*4),Vector(-normal),float(depth[i]))
    if hit[0] is not None and hit[3]>kerf*8:depth[i]=min(depth[i],hit[3]+kerf*4)
  for i in range(len(path)-1):
   v,t=_knife(path[i:i+2],normal,depth[i:i+2],kerf,width)
   if len(t):cp.append(v);ct.append(t);targets.append(np.asarray(target,np.int32))
  sv=[];sf=[]
  for i in range(len(path)-1):
   j=len(sv);sv.extend((path[i],path[i+1],path[i+1]-normal*depth[i+1],path[i]-normal*depth[i]));sf.append((j,j+1,j+2,j+3))
  if sf:sheets.append(BVHTree.FromPolygons([Vector(v) for v in sv],sf))
 return cp,ct,targets

def apply(context,source,out,factor,paths_by_role=None):
 from . import background_compute as bg
 return bg.immediate(apply_steps(context,source,out,factor,paths_by_role))

def apply_steps(context,source,out,factor,paths_by_role=None):
 from . import cell_cut_plane
 out=yield from _apply_drawing_steps(context,source,out,factor,paths_by_role)
 if paths_by_role is None:
  out=yield from cell_cut_plane.apply_steps(context,source,out,factor)
 return out

def _apply_drawing_steps(context,source,out,factor,paths_by_role=None):
 from . import background_compute as bg,paint
 from .params import _clamp_scale,unit_factor
 p=source.project_damage
 source['project_damage_cell_cut_count']=0
 source['project_damage_cell_cut_invalid']=0
 if paths_by_role is None and (not p.cellf_enable or p.all_off or (p.cellf_drawing is None and p.cellf_surface_drawing is None)):return out
 raw=paths_by_role if paths_by_role is not None else {k:paint.read_cell_paths(context,source,k) for k in ('THROUGH','SURFACE')}
 reach=p.surf_paint_snap*_clamp_scale(p.global_scale/100.)*factor/unit_factor(context)
 if not any(raw.values()):return out
 glass=paths_by_role is None and p.cellf_material=='1'
 width=p.cellf_knife_width*.1 if glass else 0.
 data,count,invalid=yield from bg.task('Cutting drawn fractures',cut_paths,{k:v for k,v in out.items() if k.startswith('cellf_')},factor,raw,reach,max(p.cellf_inner_uv_scale,1e-6),paths_by_role is None,width)
 source['project_damage_cell_cut_count']=count;source['project_damage_cell_cut_invalid']=invalid
 if invalid:print('Project Damage Cell Fracture: %d invalid cells kept unchanged; other cuts completed'%invalid)
 return dict(out,**data)

def cut_paths(out,factor,raw,reach,uv_scale,project_surface=False,width=0.):
 result=out;count=0;invalid=0
                                                                         
                                                                         
 for through in (True,False):
  cp,ct,targets=prepare_arrays(result,factor,through,raw,reach,project_surface,width)
  if not cp:continue
  D=lambda name:np.ascontiguousarray(result['cellf_'+name],np.float64)
  I=lambda name:np.ascontiguousarray(result['cellf_'+name],np.int32)
  original=()
  if project_surface and 'cellf_piece_center' in result and 'cellf_piece_volume' in result:
   original=(D('piece_center').reshape(-1,3),D('piece_volume'))
  cut=load().slice_cells(D('points'),I('polys'),I('piece'),I('base_poly'),D('uvs'),D('normals'),cp,ct,targets,uv_scale,*original)
  invalid+=int(cut['invalid_cells'])
  count+=int(cut['cut_cells']);result=dict(result);result.update(cut)
 return result,count,invalid

# действие «Draw Cell Fracture Cut»
class PROJECT_DAMAGE_OT_cell_cut_draw(bpy.types.Operator):
 bl_idname='project_damage.cell_cut_draw';bl_label='Draw Cell Fracture Cut';bl_options={'UNDO','INTERNAL'}
 mode:bpy.props.EnumProperty(items=(('THROUGH','Knife Through','Cut through the object, stopping at earlier crossing cuts'),('SURFACE','Knife Surface','Split only cells directly touched by this stroke'),('GUIDANCE','Guidance','Concentrate smaller cells around a separate guide drawing')),default='THROUGH')
 def execute(self,context):
  from . import ownership,paint,live
  source=ownership.context_source(context)
  if source is None or source.type!='MESH':return {'CANCELLED'}
  p=source.project_damage;previous=live._APPLYING;live._APPLYING=True
  try:
   p.cellf_curve_type=self.mode
   p.cellf_draw_tool='GUIDANCE' if self.mode=='GUIDANCE' else 'CUT'
   if self.mode!='GUIDANCE':p.cellf_draw_mode=self.mode
  finally:live._APPLYING=previous
  drawing=paint.ensure_cell_layer(context,source,self.mode)
  if drawing.type=='GREASEPENCIL':paint._start_drawing(context,drawing)
  else:
   paint._activate(context,drawing)
   if p.cellf_enable and drawing.type=='CURVE':bpy.ops.object.mode_set(mode='EDIT')
  return {'FINISHED'}

# подключаем свойства и действия этого модуля к Blender
def register():bpy.utils.register_class(PROJECT_DAMAGE_OT_cell_cut_draw)
# убираем подключения модуля при выключении аддона
def unregister():bpy.utils.unregister_class(PROJECT_DAMAGE_OT_cell_cut_draw)


# рисунок хранится отдельно для сквозного ножа, поверхностного ножа и Guidance
def draw_layers(layout,context,source):
 from . import paint,material_controls
 p=source.project_damage
 box=layout.box();material_controls.centered_heading(box,'Fracture Drawing',icon='GREASEPENCIL')
 row=box.row(align=True);row.scale_y=1.5
 for kind,label,icon in (('GUIDANCE','Guidance','CURVE_PATH'),
                         ('THROUGH','Knife Through','SCULPTMODE_HLT'),
                         ('SURFACE','Knife Surface','SCULPTMODE_HLT')):
  if p.cellf_enable:
   row.operator('project_damage.cell_cut_draw',text=label,icon=icon,depress=p.cellf_curve_type==kind).mode=kind
  else:
   row.prop_enum(p,'cellf_curve_type',kind,text=label,icon=icon)
 if p.cellf_enable:
  row=box.row(align=True);row.scale_y=1.3
  row.prop_enum(p,'cellf_curve_type','PLANE',text='Cut Plane',icon='MESH_PLANE')
  from . import ui_image_hints
  ui_image_hints.button(row,'cell_cut_plane')
 mode=p.cellf_curve_type
 if mode=='PLANE' and p.cellf_enable:
  from . import cell_cut_plane
  cell_cut_plane.draw(box,context,source)
  return
 if mode not in paint.CELL_PROPS:mode='THROUGH'
 prop=paint.CELL_PROPS[mode]
 target={'GUIDANCE':'CELLF_GUIDANCE','THROUGH':'CELLF','SURFACE':'CELLF_SURFACE'}[mode]
 row=box.row(align=True);row.prop(p,prop,text='Drawing')
 row.operator('project_damage.paint_delete',text='',icon='TRASH').target=target
 row=box.row(align=True);row.scale_y=1.5
 drawing=getattr(p,prop)
 label='New Drawing' if drawing is None else 'Edit Curve' if drawing.type=='CURVE' else 'Draw / Edit'
 row.operator('project_damage.cell_cut_draw',text=label,icon='CURVE_DATA' if drawing and drawing.type=='CURVE' else 'GREASEPENCIL').mode=mode
 col=box.column(align=True);col.use_property_split=False
 if mode=='GUIDANCE':
  col.prop(p,'cellf_guidance_amount',text='Additional Pieces')
  col.prop(p,'cellf_guidance_radius',text='Radius (cm)')
  if not p.cellf_enable or p.cellf_material!='1':
   col.prop(p,'cellf_guidance_depth',text='Inward Reach (cm)')
  col.prop(p,'cellf_guidance_falloff',text='Concentration')
  col.prop(p,'cellf_guidance_distance',text='Projection Distance (cm)')
  box.label(text='Primary: generate cells first, then apply both knives',icon='INFO')
 else:
  if p.cellf_enable and p.cellf_material=='1':col.prop(p,'cellf_knife_width')
  cuts=source.get('project_damage_cell_cut_count',0)
  if p.cellf_enable and cuts:box.label(text=f'Last update: {cuts} shard cuts',icon='CHECKMARK')
  box.label(text='Through depth; stops at earlier cuts' if mode=='THROUGH' else 'Only pieces directly touched by the stroke',icon='INFO')
  box.label(text='Connect two edges, or an edge to another cut',icon='INFO')
