# выносим тяжёлые операции Python в отдельный процесс
# SPDX-License-Identifier: GPL-3.0-or-later
"""Isolated Blender transactions for expensive bpy/bmesh operations.

The private process owns its own scene. Only mesh snapshots and primitive
metadata cross the boundary. Live RNA is read/published on the UI thread.
"""
from pathlib import Path
import os, tempfile
import bpy
import numpy as np
from . import background_compute as bg,ownership,build

ACTIONS={'FILL','FILL_V2','FILL_EXACT','SEPARATE_EXACT','FILL_HOLE','FRACTURE','SEPARATE','TRIM','CLEAN'}

def plain(value):
 if isinstance(value,bpy.types.ID):return None
 if hasattr(value,'to_dict'):return {k:plain(v) for k,v in value.to_dict().items() if plain(v) is not None}
 if hasattr(value,'to_list'):return [plain(v) for v in value.to_list()]
 if isinstance(value,dict):return {k:plain(v) for k,v in value.items() if plain(v) is not None}
 if isinstance(value,(tuple,list)):return [plain(v) for v in value]
 return value if value is None or isinstance(value,(str,int,float,bool)) else None

def mesh_key(ob):
 from . import incremental
 common=(ob.session_uid, ob.type, tuple(tuple(row) for row in ob.matrix_world),
         ob.parent.session_uid if ob.parent else None, ob.parent_type, ob.parent_bone,
         tuple(tuple(row) for row in ob.matrix_parent_inverse), ob.hide_viewport, ob.hide_render,
         ob.hide_get(), ob.display_type, ob.mode,
         incremental._state(bpy.context, ob, {k:v for k,v in ob.items()}),
         incremental._state(bpy.context, ob, list(ob.modifiers)))
 if ob.type!='MESH':return incremental.digest(common)
 groups=([(g.name,g.lock_weight) for g in ob.vertex_groups],
         [(v.index,g.group,g.weight) for v in ob.data.vertices for g in v.groups])
 return incremental.digest((common,ob.data.session_uid,incremental.mesh_state(ob.data),groups))

def metadata(ob):
 return dict(name=ob.name,type=ob.type,mesh=ob.data.name if ob.type=='MESH' else None,
  parent=ob.parent.name if ob.parent else None,parent_type=ob.parent_type,parent_bone=ob.parent_bone,
  parent_inverse=[list(r) for r in ob.matrix_parent_inverse],basis=[list(r) for r in ob.matrix_basis],display_type=ob.display_type,hide_viewport=ob.hide_viewport,
  matrix=[list(r) for r in ob.matrix_world],props={k:plain(v) for k,v in ob.items() if k!='_RNA_UI' and plain(v) is not None},
  materials=[m.name if m else None for m in ob.data.materials] if ob.type=='MESH' else [],
  groups=[g.name for g in ob.vertex_groups] if ob.type=='MESH' else [],
  group_locks=[g.lock_weight for g in ob.vertex_groups] if ob.type=='MESH' else [],
  group_index=ob.vertex_groups.active_index if ob.type=='MESH' else 0,
  collections=[c.name for c in ob.users_collection],hidden=ob.hide_get(),hide_render=ob.hide_render)

def compute(input_file,output_file,payload):
 """Allowlisted worker entry. Its bpy calls never touch the user's Blender."""
 import importlib
 package=importlib.import_module(__package__)
 if not getattr(package,'_REGISTERED',False):package.register()
 from . import live,damage_physics as dp,fill_v2,fill_exact
 live._APPLYING=True
 for handler in (bpy.app.handlers.depsgraph_update_post,bpy.app.handlers.frame_change_post):handler.clear()
 for ob in list(bpy.data.objects):bpy.data.objects.remove(ob,do_unlink=True)
 with bpy.data.libraries.load(input_file,link=False) as (src,dst):dst.objects=list(src.objects)
 for ob in dst.objects:
  if ob is not None and ob.name not in bpy.context.scene.objects:bpy.context.scene.collection.objects.link(ob)
 scene=bpy.context.scene;scene.unit_settings.system=payload['units'][0];scene.unit_settings.scale_length=payload['units'][1];scene.frame_set(payload['frame'])
 source=bpy.data.objects[payload['source']]
 for ob in scene.objects:ob.select_set(ob.name in payload['selected'])
 active=bpy.data.objects.get(payload['active']) or source;bpy.context.view_layer.objects.active=active
 for name,value in payload.get('cleanup',{}).items():
  if name=='reference':value=bpy.data.objects.get(value) if value else None
  setattr(scene.project_damage_clean_floating,name,value)
 for name,state in payload.get('visibility',{}).items():
  ob=bpy.data.objects.get(name)
  if ob is not None:ob.hide_set(state[0]);ob.hide_render=state[1]
 bpy.context.view_layer.update()
 before={o.name:metadata(o) for o in bpy.data.objects};keys={o.name:mesh_key(o) for o in bpy.data.objects}
 action=payload['action'];module=payload['module'];reports=[];objects=[]
 with bg.synchronous():
  if action=='FILL':
   objects,reports=dp.fill_damage(bpy.context,source,module)
   objects=dp._live_physics_objects(objects)
   if objects and module in dp.ADDITIVE and dp.settings(source).detach_remove:
    kinds={k for o in objects for k in o.get('project_damage_fill_kinds',[module])};reports+=dp.remove_from_wall(bpy.context,source,sorted(kinds))
  elif action=='FILL_V2':objects,reports=fill_v2.fill(bpy.context,source,module)
  elif action=='FILL_EXACT':objects,reports=fill_exact.fill(bpy.context,source)
  elif action=='SEPARATE_EXACT':objects,reports=fill_exact.separate(bpy.context,source,module)
  elif action=='FILL_HOLE':objects,reports=dp.fill_hole(bpy.context,source)
  elif action=='FRACTURE':
   targets=payload.get('fracture_inputs')
   manual_targets=payload.get('manual_fracture_targets')
   if targets is not None:
    chosen=[bpy.data.objects[name] for name in targets]
   elif manual_targets is not None:
    chosen=[bpy.data.objects[name] for name in manual_targets] or None
   else:
    chosen=dp.fracture_targets(bpy.context,source,module,payload.get('fragments_only',False))
   objects,reports=dp.fracture(bpy.context,source,module,fills=chosen,live_preview=targets is not None)
  elif action=='SEPARATE':objects,reports=dp.separate_pieces(bpy.context,source)
  elif action in {'TRIM','CLEAN'}:

   live._APPLYING=False
   result=(bpy.ops.project_damage.trim_to_source() if action=='TRIM' else bpy.ops.project_damage.clean_floating_parts())
   if result!={'FINISHED'}:raise RuntimeError(action+' did not finish')
  else:raise ValueError('Unsupported private scene operation')
 objects=dp._live_physics_objects(objects)
 if action not in {'TRIM','CLEAN'} and not objects:
  return dict(ok=False,reports=[(list(k),m) for k,m in reports])
 after={o.name:metadata(o) for o in bpy.data.objects}
 changed=[];meshes=set()
 for name,meta in after.items():
  ob=bpy.data.objects[name]
  geometry=ob.type=='MESH' and (name not in before or mesh_key(ob)!=keys[name])
  if name not in before or geometry or meta!=before[name]:

   meta['geometry']=geometry;changed.append(meta)
   if geometry:
    meshes.add(ob.data)
    weights=[{} for _ in ob.vertex_groups]
    for v in ob.data.vertices:
     for group in v.groups:
      if group.group<len(weights):weights[group.group].setdefault(float(group.weight),[]).append(v.index)
    meta['weights']=[[(weight,np.asarray(indices,np.int32)) for weight,indices in rows.items()] for rows in weights]

 def delta(old,new):
  return {k:(delta(old.get(k,{}),v) if isinstance(v,dict) and isinstance(old.get(k),dict) else v) for k,v in new.items() if k not in old or v!=old[k]}
 for meta in changed:
  if meta['name'] in before:
   meta['props_removed']=list(set(before[meta['name']]['props'])-set(meta['props']))
   meta['props']=delta(before[meta['name']]['props'],meta['props'])
 if meshes:bpy.data.libraries.write(output_file,meshes,path_remap='ABSOLUTE',fake_user=True,compress=False)
 return dict(ok=True,changed=changed,removed=[n for n in before if n not in after],output_file=output_file,
             outputs=[o.name for o in objects],reports=[(list(k),m) for k,m in reports])

def _commit(source,answer,snapshot,key=mesh_key):
 if not answer['ok']:
  if any('ERROR' in k for k,m in answer['reports']):raise RuntimeError('; '.join(m for k,m in answer['reports']))
  print('Project Damage background:',answer['reports']);return {'CANCELLED'}
 from . import scene_publish
 return scene_publish.publish(source,answer,snapshot,key)

def _run(source,action,module,selected,active,fragments_only,*,fracture_inputs=None,ready=None,fracture_scope=None):

 folder=Path(tempfile.mkdtemp(prefix='project_damage_scene_'))
 try:
  from . import damage_physics as dp
  objects={source}
  cleanup={}
  helpers=set()
  preview=fracture_inputs is not None
  if preview:
   from . import physics_live
   helpers=set(physics_live.inputs(source))
   for helper in helpers:
    if helper.mode=='EDIT':helper.update_from_editmode()
   objects.update(bpy.data.objects[name] for name in fracture_inputs)
   objects.update(o for o in dp.pieces_of(source) if o.get('project_damage_piece_fill') in fracture_inputs)
  else:
   result=build.result_of(source)
   if result:objects.add(result)
   uid=source.get('project_damage_uid')
   if action in {'FRACTURE','FILL_HOLE'}:
    from . import physics_live
    objects.update(physics_live.inputs(source))
   objects.update(o for o in bpy.data.objects if ownership.source_of(o)==source)
   objects.update(o for o in bpy.data.objects if uid and any(o.get(k)==uid for k in (dp.FILL_STAMP,dp.PIECE_STAMP,dp.SPLIT_STAMP,'project_damage_bfrag_of')))
   objects.update(o for name in selected if (o:=bpy.data.objects.get(name)) is not None)
   settings=bpy.context.scene.project_damage_clean_floating
   cleanup={name:(getattr(settings,name).name if getattr(settings,name) else None) if name=='reference' else getattr(settings,name) for name in ('reference','allowance','use_reference_modifiers','flip_open_side','min_part_percent','keep_largest_only','report_only')}
   if settings.reference:objects.add(settings.reference)
  pending=list(objects - {source} if preview else objects)
  while pending:
   ob=pending.pop()
   for relative in ob.children:
    if relative not in objects:objects.add(relative);pending.append(relative)
  def snapshot_key(obj):
   if preview and obj==source:
    return obj.session_uid,obj.data.session_uid
   return mesh_key(obj)
  snapshot={o.name:snapshot_key(o) for o in objects}
  objects.update(helpers)
  for ob in list(objects):
   parent=ob.parent
   while parent is not None and parent not in objects:
    objects.add(parent)
    if not preview:snapshot[parent.name]=snapshot_key(parent)
    parent=parent.parent
  input_file=folder/'input.blend';output_file=folder/'output.blend'
  yield None
  bpy.data.libraries.write(str(input_file),objects,path_remap='ABSOLUTE',fake_user=True,compress=False)
  payload=dict(visibility={o.name:(o.hide_get(),o.hide_render) for o in objects},source=source.name,selected=selected,active=active,action=action,module=module,fragments_only=fragments_only,
               units=(bpy.context.scene.unit_settings.system,bpy.context.scene.unit_settings.scale_length),frame=bpy.context.scene.frame_current,cleanup=cleanup,fracture_inputs=fracture_inputs,
               manual_fracture_targets=fracture_scope[0] if fracture_scope is not None else None)
  answer=yield from bg.task({'FILL':'Filling damage','FILL_V2':'Calculating Fill V2','FILL_EXACT':'Matching damage volumes','SEPARATE_EXACT':'Separating fill fragments','FILL_HOLE':'Filling holes','FRACTURE':'Fracturing pieces','SEPARATE':'Separating pieces','TRIM':'Trimming result','CLEAN':'Cleaning floating parts'}[action],compute,str(input_file),str(output_file),payload)
  yield None
  if preview and not ready():
   return None
  layer = bpy.context.view_layer
  current = layer.objects.active
  replace_active = (action == 'SEPARATE_EXACT' and current is not None
                    and current.name == active and active in answer.get('removed', ()))
  status = _commit(source,answer,snapshot,snapshot_key)
  if status == {'FINISHED'}:
   if preview:
    from . import paint
    paint.refresh_physics_projection(bpy.context, source,
        [layer.objects[name] for name in answer['outputs'] if name in layer.objects])
   else:
    from . import cellf_output
    if source.get(cellf_output.SWAP_ALL):
     cellf_output.sync_switch(bpy.context, source)
    if fracture_scope is not None:
     from . import physics_live
     names, fragments, notify = fracture_scope
     physics_live.fracture_finished(bpy.context, source, module, names, fragments, notify=notify)
  if replace_active and status == {'FINISHED'}:
   layer.update()
   for name in answer['outputs']:
    fragment = layer.objects.get(name)
    if fragment is not None and fragment.visible_get(view_layer=layer):
     fragment.select_set(True, view_layer=layer)
     if layer.objects.active is None:
      layer.objects.active = fragment
  return status
 finally:
  import shutil
  shutil.rmtree(folder,ignore_errors=True)

def queue(context,source,action,module='concrete',fragments_only=False,*,fracture_scope=None):
 if action not in ACTIONS:raise ValueError(action)
 if not bg.async_for(source) or os.environ.get('PD_PRIVATE_WORKER')=='1':return False
 selected=[o.name for o in context.selected_objects];active=context.active_object.name if context.active_object else source.name
 bg.queue(source,'python_operation',lambda:_run(source,action,module,selected,active,fragments_only,
                                                           fracture_scope=fracture_scope))
 return True


def queue_preview(context, source, module, targets, ready):
 if not bg.async_for(source) or os.environ.get('PD_PRIVATE_WORKER')=='1':
  return False
 from . import physics_live
 names=tuple(obj.name for obj in targets)
 bg.queue(source, physics_live.JOB,
          lambda: _run(source, 'FRACTURE', module, names, source.name, False,
                       fracture_inputs=names, ready=ready),
          guard=lambda: source.project_damage.live_update and not source.project_damage.all_off)
 return True
