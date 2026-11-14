# новый эффект получает свои исходные значения, а не настройки предыдущего
from . import live

PREFIXES={'edge':('chip_','chips_'),'surface':('surf_',),'corner':('corner_',),'buildup':('build_',),'piles':('pile_',),'deposits':('deposit_',),'boolean':('rebar_',),'concrete_v2':('concrete_',),'pipes':('concrete_','pipe_'),'cables':('concrete_','cable_'),'lath_v2':('lath_',),'lath_riven':('lath_',),'lath_framing':('lath_',),'brick_v2':('brick_',),'fabric_solid':('fabric_',),'glass_hit':('glass_',),'glass_drawn':('glass_',),'hit_through':('hit_',)}

def reset(source,key):
    from . import sidebar,dev_tools
    if key in sidebar.FRACTURE_KEYS:return
    previous=live._APPLYING;live._APPLYING=True
    try:
        p=source.project_damage
        prefixes=PREFIXES.get(key,(key+'_',))
        for prop in p.bl_rna.properties:
            name=prop.identifier
            if name in sidebar.ENABLE_PROPS or prop.is_readonly or not name.startswith(prefixes):continue
            if prop.type in {'BOOLEAN','INT','FLOAT','ENUM','STRING','POINTER'}:p.property_unset(name)
        group=getattr(source,'project_damage_'+{'paper_generator':'paper'}.get(key,key),None)
        if group is not None and key in {'foam','weld','grass','gravel','ivy','roots','paper_generator'}:
            for prop in group.bl_rna.properties:
                if prop.identifier in {'rna_type','result','source'} or prop.is_readonly:continue
                if prop.type in {'BOOLEAN','INT','FLOAT','ENUM','STRING','POINTER'}:group.property_unset(prop.identifier)
        if key in {'lath_riven','lath_framing'}:
            from . import riven_lath,stud_framing
            for name,value in (riven_lath.LOOK if key=='lath_riven' else stud_framing.LOOK).items():setattr(p,name,value)
        if key=='lath_v2':p.lath_keys=0.;p.lath_key_size=5.
        if key=='brick':
            for name,value in {'brick_style':'0','brick_brick_mode':'1','brick_surf_mode':'0','brick_behind':'1','brick_damage_depth':0.,'brick_damage_noise':0.,'brick_damage_scale':0.,'brick_detail':.5,'brick_noise_strength':2.,'brick_noise_scale':10.,'brick_noise_detail':4.}.items():setattr(p,name,value)
        if key in {'pipes','cables'}:p.concrete_mode='1'
        if key in {'fabric','fabric_solid'}:
            p.fabric_geometry='SOLIDIFY' if key=='fabric_solid' else 'FLAT'
            p.fabric_solid=key=='fabric_solid';p.fabric_flat=key!='fabric_solid'
            p.fabric_enable=key=='fabric';p.fabric_solid_enable=key=='fabric_solid'
        if key=='glass_drawn':p.glass_paint_holes_enable=True;p.glass_paint_only=True
        if key=='glass_hit':p.glass_paint_holes_enable=False
        from . import generated_geometry
        family = {'surface':'surface_rebar','pipes':'pipe','cables':'cable','concrete_v2':'concrete',
                  'brick_v2':'brick','lath_v2':'lath','lath_riven':'lath','lath_framing':'lath'}.get(key,key)
        if family in generated_geometry.FAMILIES:p.property_unset(family+'_generated_enable')
        dev_tools.apply_effect_defaults(source,key)
        from .paint import DRAW_FIRST
        if key in DRAW_FIRST:setattr(p,DRAW_FIRST[key].lower()+'_mode','1')
    finally:live._APPLYING=previous
