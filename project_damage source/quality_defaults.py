# понижаем качество только у новых эффектов в режиме Low Details
# SPDX-License-Identifier: GPL-3.0-or-later
"""20% starting quality for newly added effects, never a live override."""
import bpy

FIELDS={
 'edge':('detail',),'surface':('surf_detail',),'corner':('corner_detail',),
 'fracture':('fract_detail',),'buildup':('build_detail',),'piles':('pile_detail',),
 'deposits':('deposit_detail',),'boolean':('boolean_quality','rebar_detail'),
 'concrete':('concrete_detail',),'lath':('lath_detail',),'brick':('brick_detail','brick_back_resolution'),
 'fabric':('fabric_detail',),'glass':('glass_detail',),'hit':('hit_detail',),
 'hang':('hang_sides',),'bullet':('bullet_detail',),'fissure':('fissure_detail',),
 'ground':('ground_detail',),'plaster':('plaster_detail',),'scratch':('scratch_detail',),
 'wallpaper':('wallpaper_detail',),'rusty':('rusty_detail',),'dbevel':('dbevel_detail',),
 'cellf':('cellf_detail',),'pits':('pits_detail',),'metaltear':('metaltear_detail',),
 'clay':('clay_resolution',),'peeling':('peeling_resolution',),'erosion':('erosion_voxel_size',),
}
ALIASES={'concrete_v2':'concrete','pipes':'concrete','cables':'concrete',
 'lath_v2':'lath','lath_riven':'lath','lath_framing':'lath','brick_v2':'brick',
 'fabric_solid':'fabric','glass_hit':'glass','glass_drawn':'glass','hit_through':'hit'}

def enabled(context=None):
 context=context or bpy.context
 entry=getattr(getattr(context,'preferences',None),'addons',{}).get(__package__)
 return bool(entry and getattr(entry.preferences,'low_details_mode',False))

def _set(owner,name,value):
 prop=owner.bl_rna.properties.get(name)
 if prop is None:return
 if name=='peeling_resolution':
  if owner.get(name) is not None or owner.get('peeling_detail') is not None:return
 elif owner.is_property_set(name):return
 value=min(prop.hard_max,max(prop.hard_min,value))
 owner[name]=int(round(value)) if prop.type=='INT' else float(value)

def apply(source,key):
 """Called once on Add, before user defaults/variant looks. Explicit edits win."""
 if not enabled():return
 from .fracture_assets import FRACTURE_KEYS
 family='fracture' if key in FRACTURE_KEYS else ALIASES.get(key,key)
 p=source.project_damage
 for name in FIELDS.get(family,()):
  prop=p.bl_rna.properties.get(name)
  if prop is None:continue
  if name in {'metaltear_detail','erosion_voxel_size'}:value=prop.default/.2
  elif name=='hang_sides':value=max(3,round(prop.default*.2))
  elif name=='clay_resolution':value=20.
  else:value=20. if prop.subtype=='PERCENTAGE' else .2
  _set(p,name,value)
 if family == 'paper_generator':
  group=source.project_damage_paper
  _set(group,'resolution',group.bl_rna.properties['resolution'].default/.2)
 if family in {'ivy','roots','weld','foam'}:
  group=getattr(source,'project_damage_'+family)
  field='detail' if family=='weld' else 'resolution'
  prop=group.bl_rna.properties[field]
  value=prop.default/.2 if family=='foam' else .2 if family=='weld' else 20.
  _set(group,field,value)

def standalone(source,key):
 if not enabled():return
 if key.startswith('cloth_'):
  p=source.project_damage_cloth
  _set(p,'detail',p.bl_rna.properties['detail'].default/.2)
                                                                                 
 elif key=='plank_surface':
  p=source.pd_planks;_set(p,'resolution',round(p.bl_rna.properties['resolution'].default*.2))
 elif key.startswith('cable_'):
  p=source.project_damage_cable_gen
  prop=p.bl_rna.properties.get('step')
  if prop is not None:_set(p,'step',prop.default/.2)
  _set(p,'sides',round(p.bl_rna.properties['sides'].default*.2))

def breaker(obj):
 if enabled():_set(obj.project_damage_breaker,'resolution',20.)
