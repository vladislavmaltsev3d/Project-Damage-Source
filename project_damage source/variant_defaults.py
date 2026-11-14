# храним независимые значения вариантов стекла, дерева и кирпичей
# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent object-local snapshots for Lath variants sharing one core block."""
KEYS=('lath','lath_v2','lath_riven','lath_framing')
STAMP='project_damage_lath_variant_values'

def switch(source,key):
    p=source.project_damage
    previous=source.get('project_damage_lath_variant')
    active=next((k for k in KEYS if getattr(p,k+'_enable')),None)
    previous=previous or active
    if previous==key:return
    fields=[r for r in p.bl_rna.properties if r.identifier.startswith('lath_') and r.identifier not in {k+'_enable' for k in KEYS}
            and r.type in {'BOOLEAN','INT','FLOAT','ENUM','STRING','POINTER'}]
    saved=dict(source.get(STAMP,{}))
    if previous:
        saved[previous]={r.identifier:getattr(p,r.identifier) for r in fields if getattr(p,r.identifier) is not None}
    snapshot=saved.get(key)
    for r in fields:
        setattr(p,r.identifier,None if r.type=='POINTER' else r.default)
    if snapshot is not None:
        for name,value in snapshot.items():setattr(p,name,value)
    else:
        if key=='lath_v2':
            p.lath_keys=0.
            p.lath_key_size=5.
        elif key=='lath_riven':
            from .riven_lath import LOOK
            for name,value in LOOK.items():setattr(p,name,value)
        elif key=='lath_framing':
            from .stud_framing import LOOK
            for name,value in LOOK.items():setattr(p,name,value)
    source[STAMP]=saved
    source['project_damage_lath_variant']=key


def switch_hit(source,key):
    """Hit and Hit Through keep independent object-local values, including input pointers."""
    p=source.project_damage
    previous=source.get('project_damage_hit_variant') or ('hit_through' if p.hit_through_enable else 'hit' if p.hit_enable else None)
    if previous==key:return
    fields=[r for r in p.bl_rna.properties if (r.identifier.startswith('hit_') or r.identifier in {'surface_damage_material','fracture_l1_material','fracture_l2_material'})
            and r.identifier not in {'hit_enable','hit_through_enable'}
            and r.type in {'BOOLEAN','INT','FLOAT','ENUM','STRING','POINTER'} and not r.is_readonly]
    saved=dict(source.get('project_damage_hit_variant_values',{}))
    if previous:
        saved[previous]={r.identifier:getattr(p,r.identifier) for r in fields if getattr(p,r.identifier) is not None}
    snapshot=saved.get(key,{})
    for r in fields:setattr(p,r.identifier,None if r.type=='POINTER' else r.default)
    for name,value in snapshot.items():
        if name in p.bl_rna.properties:setattr(p,name,value)
    p.hit_through=(key=='hit_through')
    source['project_damage_hit_variant_values']=saved
    source['project_damage_hit_variant']=key


def switch_glass(source,key):
    """Each Glass asset keeps its input, materials and shape controls independently."""
    keys=('glass','glass_through','glass_hit','glass_drawn');p=source.project_damage
    previous=source.get('project_damage_glass_variant') or next((k for k in keys if getattr(p,k+'_enable')),None)
    if previous==key:return
    fields=[r for r in p.bl_rna.properties if (r.identifier.startswith('glass_') or r.identifier in {'surface_damage_material','fracture_l1_material','edge_damage_material'})
            and r.identifier not in {k+'_enable' for k in keys}
            and r.type in {'BOOLEAN','INT','FLOAT','ENUM','STRING','POINTER'} and not r.is_readonly]
    saved=dict(source.get('project_damage_glass_variant_values',{}))
    if previous:saved[previous]={r.identifier:getattr(p,r.identifier) for r in fields if getattr(p,r.identifier) is not None}
    snapshot=saved.get(key)
    for r in fields:setattr(p,r.identifier,None if r.type=='POINTER' else r.default)
    if snapshot is not None:
        for name,value in snapshot.items():
            if name in p.bl_rna.properties:setattr(p,name,value)
    elif key in {'glass_hit','glass_drawn'}:
        p.glass_paint_holes_enable=key=='glass_drawn'
        p.glass_paint_impacts_enable=True
        p.glass_paint_only=True
        p.glass_paint_hole_cracks=True
        p.glass_paint_point_hole=True
    source['project_damage_glass_variant_values']=saved
    source['project_damage_glass_variant']=key


def switch_brick(source,key):
    """Classic and V2 keep separate settings and first-use defaults."""
    p=source.project_damage;keys=('brick','brick_v2')
    previous=source.get('project_damage_brick_variant') or next((k for k in keys if getattr(p,k+'_enable')),None)
    if previous==key and source.get('project_damage_brick_variant'):return
    fields=[r for r in p.bl_rna.properties if r.identifier.startswith('brick_')
            and r.identifier not in {k+'_enable' for k in keys}
            and r.type in {'BOOLEAN','INT','FLOAT','ENUM','STRING','POINTER'} and not r.is_readonly]
    saved=dict(source.get('project_damage_brick_variant_values',{}))
    if previous:saved[previous]={r.identifier:getattr(p,r.identifier) for r in fields if getattr(p,r.identifier) is not None}
    snapshot=saved.get(key)
    for r in fields:setattr(p,r.identifier,None if r.type=='POINTER' else r.default)
    if snapshot is not None:
        for name,value in snapshot.items():
            if name in p.bl_rna.properties:setattr(p,name,value)
    if key=='brick':
        for field,value in {'brick_style':'0','brick_brick_mode':'1','brick_surf_mode':'0','brick_behind':'1',
                            'brick_damage_depth':0.,'brick_damage_noise':0.,'brick_damage_scale':0.}.items():setattr(p,field,value)
        for field,value in {'brick_detail':.5,'brick_noise_strength':2.,'brick_noise_scale':10.,'brick_noise_detail':4.}.items():
            if snapshot is None or field not in snapshot:setattr(p,field,value)
    source['project_damage_brick_variant_values']=saved
    source['project_damage_brick_variant']=key
