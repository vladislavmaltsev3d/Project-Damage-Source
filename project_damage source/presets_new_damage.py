# пресеты для новых генераторов и их вспомогательных объектов
import math

CATEGORY_TITLES={'glass_damage':'Glass Damage','new_damage':'New Damage Generators'}


def create(preset):
    result={}
    glass=(
        ('GLASS_SINGLE_STONE_HIT','glass_hit','One strong stone impact with a broad starburst',.17,1,8),
        ('GLASS_DOUBLE_IMPACT','glass_hit','Two overlapping impact stars',.12,2,11),
        ('GLASS_PELLET_CLUSTER','glass_hit','Five small concentrated punctures',.055,5,15),
        ('GLASS_EDGE_STRIKE','glass_hit','An off-centre edge impact and long radial fractures',.14,1,18),
        ('GLASS_HEAVY_IMPACT','glass_hit','A wide impact opening with several broken rings',.26,1,22),
        ('GLASS_DRAW_TORN_CORNER','glass_drawn','A drawn corner opening with cracks along its rim',.27,1,9),
        ('GLASS_DRAW_LONG_GASH','glass_drawn','An elongated drawn gash in the pane',.23,1,12),
        ('GLASS_DRAW_TWIN_OPENINGS','glass_drawn','Two drawn openings linked by surrounding cracks',.18,2,16),
        ('GLASS_DRAW_CENTRAL_BREACH','glass_drawn','A large irregular opening with a radial crack network',.32,1,21),
        ('GLASS_DRAW_EDGE_SCALLOPS','glass_drawn','A scalloped missing strip beside the pane edge',.16,3,13),
    )
    for key,effect,desc,radius,count,cracks in glass:
        props=dict(glass_paint_impact_size=radius*100,glass_drawn_reach=5.5,glass_paint_holes_enable=effect=='glass_drawn',glass_paint_only=True,glass_seed=17+count,glass_crack_enable=True,glass_crack_width=.38,glass_crack_depth=.75,glass_crack_reach=5.5,glass_radial_min=cracks,glass_radial_max=cracks+5,glass_ring_count=3 if count==1 else 2,glass_paint_hole_cracks=True)
        result[key]=preset('glass_damage',desc,(effect,),props)
    data=(
        ('TORN_METAL_PUNCHED_SHELL','gen_metal','metal_gen','Large petalled holes with a few dents',dict(mode='OBJECT',blasts=2,blast_size_min=.28,blast_size_max=.4,petal_curl=.72,petals_min=5,petals_max=7,tears=0,dents=3,punctures=0)),
        ('TORN_METAL_SHRAPNEL_SKIN','gen_metal','metal_gen','Small punctures around one torn metal opening',dict(mode='OBJECT',blasts=1,blast_size_min=.25,blast_size_max=.35,punctures=28,puncture_size_min=.035,puncture_size_max=.07,tears=3,tear_length_min=.25,tear_length_max=.65,dents=7)),
        ('TORN_METAL_RIPPED_SEAMS','gen_metal','metal_gen','Long torn slots, bent lips and shallow dents',dict(mode='OBJECT',blasts=0,punctures=0,tears=9,tear_length_min=.45,tear_length_max=1.05,tear_width=.045,tear_open=.85,dents=4,crumple=.7)),
        ('SPLIT_WOOD_SNAPPED_BLOCK','gen_wood','wood_gen','Across-grain break with long interlocking splinters',dict(mode='OBJECT',grain_axis='Z',break_mode='SNAPPED',break_angle=15,break_gap=.16,splinter_length_min=.08,splinter_length_max=.7,splinter_width_min=.035,splinter_width_max=.12,splits=1)),
        ('SPLIT_WOOD_CRUSHED_END','gen_wood','wood_gen','One irregular broken end with layered peeled fibres',dict(mode='OBJECT',grain_axis='Z',break_mode='END',break_pos=.22,break_angle=0,break_gap=.08,splinter_length_min=.12,splinter_length_max=.8,splinter_width_min=.04,splinter_width_max=.15,layers=5,splits=2)),
        ('SPLIT_WOOD_FORKED_GRAIN','gen_wood','wood_gen','Long open grain splits and a relatively narrow cross break',dict(mode='OBJECT',grain_axis='Z',break_mode='SNAPPED',break_angle=5,break_gap=.08,splits=3,split_length=.7,split_open=.17,splinter_length_min=.08,splinter_length_max=.32,splinter_width_min=.025,splinter_width_max=.09)),
        ('BROKEN_TILES_CENTRAL_IMPACT','gen_tiles','tile_gen','A central missing cluster with exposed tile adhesive',dict(width=2,height=2,impacts=1,impact_radius=.7,damage=1.1,missing=.8,leftovers=.55,cracks=8,adhesive=.8)),
        ('BROKEN_TILES_SUBWAY_FRACTURE','gen_tiles','tile_gen','Running-bond tiles with a long broken grout network',dict(width=2,height=2,tile_width=.32,tile_height=.16,pattern='RUNNING',impacts=2,impact_radius=.55,missing=.3,cracks=14,follow_grout=.85,crack_length=1.2)),
        ('BROKEN_TILES_LOOSE_FLOOR','gen_tiles','tile_gen','Large floor tiles displaced and tilted around impacts',dict(width=2,height=2,tile_width=.36,tile_height=.36,impacts=3,impact_radius=.5,missing=.2,loose=.9,tilt_angle=14,piece_tilt=8,damage=1.1)),
        ('WEATHERED_STONE_HONEYCOMB','gen_erosion','erosion_gen','Broad sandstone hollows and eroded corners',dict(shape='SOURCE',stone='CUSTOM',edge_round=.06,edge_loss=.6,edge_loss_size=.14,cavities=.8,cavity_size=.22,cavity_depth=.9,pits=.3,roughness=.004)),
        ('WEATHERED_STONE_LAYERED_BLOCK','gen_erosion','erosion_gen','Tilted bedding grooves in softly worn stone',dict(shape='SOURCE',stone='CUSTOM',edge_round=.045,edge_loss=.35,strata=.025,strata_spacing=.12,strata_angle=12,cavities=.22,cavity_size=.12,pits=.2,roughness=.003)),
        ('WEATHERED_STONE_EXPOSED_AGGREGATE','gen_erosion','erosion_gen','Cement worn away to expose coarse aggregate and pitted edges',dict(shape='SOURCE',stone='CUSTOM',edge_round=.025,edge_loss=.75,edge_loss_size=.1,aggregate=.85,aggregate_size=.05,cement_loss=.012,pits=.65,pit_size=.015,cavities=.1)),
        ('BLAST_CRATER_BROKEN_ASPHALT','gen_crater','crater_gen','Compact asphalt crater with lifted torn plates',dict(width=2,height=2,ground_thickness=.28,radius=.48,depth=.22,rim_height=.12,pavement='ASPHALT',pavement_thickness=.045,plate_size=.18,plate_tilt=18,debris=35,debris_size=.09,clod_size=.05,terrain=.008)),
        ('BLAST_CRATER_CONCRETE_SLAB','gen_crater','crater_gen','Concrete slab fractured into large radial plates',dict(width=2,height=2,ground_thickness=.3,radius=.5,depth=.2,rim_height=.08,pavement='CONCRETE',pavement_thickness=.065,plate_size=.27,plate_tilt=12,debris=25,debris_size=.1,slab_size=.65,radial_cracks=9,terrain=.005)),
        ('BLAST_CRATER_SOFT_SOIL','gen_crater','crater_gen','A soft earth bowl with an uneven raised rim and radial ejecta',dict(width=2,height=2,ground_thickness=.32,radius=.5,depth=.24,rim_height=.16,pavement='NONE',clods=.8,clod_size=.075,rays=.8,debris=45,debris_size=.07,irregularity=.8,terrain=.012)),
    )
    for i,(key,effect,group,desc,values) in enumerate(data):
        values=dict(values,seed=20+i)
        values['resolution']={'gen_metal':.018,'gen_wood':.014,'gen_tiles':.008,'gen_erosion':.014,'gen_crater':.014}[effect]
        if effect!='gen_erosion':values['max_verts']=400000
        if effect=='gen_wood':values['splinter_lift']=.22
        result[key]=preset('new_damage',desc,(effect,),settings={group:values})
    return result


def inputs(source,key,context,additive=False):
    import bpy
    from mathutils import Vector
    if not key.startswith('GLASS_'):return
    uid=source.get('project_damage_uid') or source.name
    old=[o for o in bpy.data.objects if o.get('project_damage_glass_preset_source')==source or o.get('project_damage_glass_preset_owner')==uid]
    for obj in old:
        if not additive or obj.get('project_damage_glass_preset_key')==key:bpy.data.objects.remove(obj,do_unlink=True)
    bounds=[Vector(c) for c in source.bound_box]
    lo=Vector([min(p[i] for p in bounds) for i in range(3)]);hi=Vector([max(p[i] for p in bounds) for i in range(3)])
    size=hi-lo;centre=(lo+hi)*.5
    face=lo.y-max(size.y*.001,.0001)
    def pos(x,z):return Vector((centre.x+x*size.x,face,centre.z+z*size.z))
    positions=[(0,0)]
    if key in {'GLASS_DOUBLE_IMPACT','GLASS_DRAW_TWIN_OPENINGS'}:positions=[(-.2,.16),(.2,-.18)]
    if key=='GLASS_PELLET_CLUSTER':positions=[(-.16,.15),(.1,.2),(.22,-.05),(-.07,-.2),(-.23,-.05)]
    if key in {'GLASS_EDGE_STRIKE','GLASS_DRAW_TORN_CORNER'}:positions=[(-.32,.3)]
    if key=='GLASS_DRAW_EDGE_SCALLOPS':positions=[(.38,-.27),(.38,0),(.38,.26)]
    if 'DRAW_' in key:
        gp=bpy.data.grease_pencils.new(source.name+' · Preset Glass Drawing')
        obj=bpy.data.objects.new(gp.name,gp);context.scene.collection.objects.link(obj)
        obj.parent=source;obj['project_damage_paint_source']=source
        ink=bpy.data.materials.get('Project Damage Preset Glass Ink')
        if ink is None:
            ink=bpy.data.materials.new('Project Damage Preset Glass Ink');bpy.data.materials.create_gpencil_data(ink)
            ink.grease_pencil.color=(.05,.5,1,.85)
        gp.materials.append(ink)
        layer=gp.layers.new('Damage Contours',set_active=True);drawing=layer.frames.new(context.scene.frame_current).drawing
        drawing.add_strokes([36]*len(positions))
        coords=[]
        radius={'GLASS_DRAW_TORN_CORNER':.23,'GLASS_DRAW_LONG_GASH':.22,'GLASS_DRAW_TWIN_OPENINGS':.16,'GLASS_DRAW_CENTRAL_BREACH':.29,'GLASS_DRAW_EDGE_SCALLOPS':.16}[key]
        for cx,cz in positions:
            for n in range(36):
                angle=2*math.pi*n/36;rr=radius*(1+.14*math.sin(5*angle)+.07*math.cos(9*angle))
                x=rr*math.cos(angle);z=rr*math.sin(angle)
                if key=='GLASS_DRAW_LONG_GASH':x*=.42;z*=1.7
                coords.extend(pos(cx+x,cz+z))
        drawing.attributes['position'].data.foreach_set('vector',coords)
        for stroke in drawing.strokes:stroke.cyclic=True
        source.project_damage.glass_holes_drawing=obj
        objects=[obj]
    else:
        collection=source.project_damage.glass_hit_collection if additive else None
        if collection is None:
            collection=bpy.data.collections.new(source.name+' · Preset Glass Hits');context.scene.collection.children.link(collection)
        source.project_damage.glass_hit_collection=collection;objects=[]
        for n,(x,z) in enumerate(positions):
            obj=bpy.data.objects.new(source.name+' · Hit '+str(n+1),None);collection.objects.link(obj)
            obj.parent=source;obj.location=pos(x,z);obj.empty_display_type='SPHERE';obj.empty_display_size=.07
            objects.append(obj)
    for obj in objects:
        obj.hide_render=True;obj['project_damage_glass_preset_source']=source;obj['project_damage_glass_preset_owner']=uid;obj['project_damage_glass_preset_key']=key
