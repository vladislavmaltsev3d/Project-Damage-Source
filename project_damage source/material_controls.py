# показываем материалы рядом с соответствующими частями эффекта
# SPDX-License-Identifier: GPL-3.0-or-later
"""Per-source controls for the material regions the current core exposes."""
import bpy


def experimental(context=None):
    """Preference affects presentation only; never touch source parameters."""
    context = context or bpy.context
    addons = getattr(getattr(context, 'preferences', None), 'addons', None)
    addon = addons.get(__package__) if addons is not None else None
    return bool(addon and getattr(addon.preferences, 'experimental_material_ui', True))


def inline(layout, owner, name, text=None):
    """Material row placed explicitly inside the feature that produces its faces."""
    growth=owner.bl_rna.identifier in {'ProjectDamageIvySettings','ProjectDamageRootsSettings','ProjectDamageGrassSettings'}
    if not growth and (not experimental() or ui_section()!='GEOMETRY'):
        return
    from .material_library import draw_property
    draw_property(layout, owner, name, text=text or owner.bl_rna.properties[name].name)
    layout.separator(factor=.5)


def ui_section(context=None):
    """UI-only state: never write the generator Object to change a tab."""
    context = context or bpy.context
    return getattr(getattr(context, 'window_manager', None),
                   'project_damage_ui_section', 'GEOMETRY')


def centered_heading(layout, text, icon='NONE'):
    """Center the icon and title as one unit, independent of property split."""
    row = layout.row(align=True)
    row.alignment = 'CENTER'
    row.use_property_split = False
    icons=bpy.types.UILayout.bl_rna.functions['label'].parameters['icon'].enum_items
    row.label(text=text, icon=icon if icon in icons else 'NONE')
    from . import editorial
    editorial.button(row,text)

CLASS_PROPS = {0: 'edge_damage_material', 1: 'fracture_l1_material',
               2: 'fracture_l2_material', 3: 'fracture_l3_material',
               4: 'surface_damage_material', 5: 'buildup_material',
               7: 'boolean_break_material'}
LABELS = {0: 'Edge Damage', 1: 'Cracks L1', 2: 'Cracks L2', 3: 'Cracks L3',
          4: 'Surface Damage', 5: 'Buildup / Fragments', 7: 'Boolean Cut'}
                                                                            
EFFECT_SLOTS = {
    'bevel': (), 'scratch': (), 'edge': ((0, 'Cut Edges'),), 'surface': ((4, 'Cavities'),),
    'corner': ((4, 'Corner Cavities'),),
    'fracture': ((1, 'Fracture Material'), (0, 'Rim Chips')),
    'fracture_through': ((1, 'Cracks'), (0, 'Rim Chips')),
    'hit_through': ((4,'Impact Crater'),(1,'Chunk Cuts / Cracks')),
    'glass_through': ((4,'Broken Openings'),(1,'Shard Cuts / Cracks'),(0,'Glass Chips')),
    'debris': (),
    'buildup': ((5, 'Bead / Piles / Deposits'),),
                                                                          
                                                            
    'piles': (),
    'deposits': (),
    'coating': ((4, 'Exposed Surface'), (5, 'Flakes')),
    'rust': ((4, 'Corroded Surface'), (5, 'Rust Buildup')),
    'heat': ((4, 'Damaged Surface'), (5, 'Blisters / Buildup')),
    'boolean': ((7, 'Cut Faces'), (0, 'Rim Damage')),
    'fabric': ((4, 'Torn Edges'), (5, 'Threads / Fibres')),
    'concrete': ((4, 'Exposed Concrete'), (1, 'Cracks')),
    'pipes': ((4, 'Exposed Concrete'), (1, 'Cracks')),
    'cables': ((4, 'Exposed Concrete'), (1, 'Cracks')),
    'hit': ((4, 'Impact Crater'), (2, 'Exposed Plates'), (1, 'Cracks')),
    'wood': ((4, 'Exposed Wood'), (1, 'Checks / Splits'), (5, 'Splinters')),
    'lath': ((4, 'Exposed Layers'), (1, 'Cracks')),
    'bullet': ((4, 'Impact Holes'), (1, 'Cracks'), (2, 'Halo / Scorch'), (5, 'Burrs / Splinters')),
    'blast': ((4, 'Blast Cavities'), (2, 'Exposed Structure'), (1, 'Cracks')),
    'ground': ((4, 'Broken Areas'), (1, 'Cracks'), (5, 'Rubble / Heave')),
    'brick': ((1, 'Cracks'), (5, 'Loose Bricks')),
    'glass': ((4, 'Broken Openings'), (1, 'Cracks'), (0, 'Glass Chips')),
    'rusty': ((4,'Hole Walls'),),
    'fissure': ((4, 'Chasm Walls'), (1, 'Branches'), (5, 'Slabs / Rubble')),
}
EXTRA_PROPS = {'erosion_material':'Erosion Surface','rebar_material':'Rebar','rebar_brick_material':'Boolean Fill Bricks','rebar_plank_material':'Boolean Fill Wood','rebar_framing_material':'Boolean Fill Framing','rebar_chips_material':'Boolean Fill Chips','rebar_nail_material':'Boolean Fill Nails','bevel_material':'Bevel Surface', 'wear_material':'Edge Wear', 'scratch_groove_material':'Scratch Grooves', 'scratch_scuff_material':'Scuffed Paint','heat_char_material': 'Heat Char',
               'rebar_mortar_material':'Boolean Fill Mortar',
               'deposit_material': 'Separate Deposits',
               'coating_shell_material': 'Separate Coating',
               'brick_front_material': 'Brick Faces / Sides',
               'brick_break_material': 'Snapped Brick Faces',
               'brick_seam_material': 'Mortar Joints',
               'brick_behind_material': 'Damage Pocket / Backing',
               'brick_render_material': 'Broken Render Edge',
               'lath_material': 'Separate Wooden Laths'}

EXTRA_PROPS['debris_material']='Debris Inner Surface'
EXTRA_PROPS['rbf_inner_material']='Rigid Body Inner Faces'
EFFECT_SLOTS['rbf']=()
EXTRA_PROPS['cellf_inner_material']='Cell Fracture Inner Faces'
EXTRA_PROPS['surface_rebar_material']='Surface Reinforcement'

EXTRA_PROPS['pile_material']='Corner Piles'
EXTRA_PROPS['concrete_rebar_material']='Concrete Reinforcement'
EXTRA_PROPS['rebar_drawn_material']='Hand-Drawn Rebar Material'
EFFECT_SLOTS['cellf']=()

def register_properties(cls, update):
    cls.__annotations__['one_material_all']=bpy.props.BoolProperty(
        name='Add One Material to All',default=False,
        description='Show one material input for all generators; nothing changes until Apply')
    cls.__annotations__['all_generators_material']=bpy.props.PointerProperty(
        name='Material',type=bpy.types.Material,
        description='Material for damaged surfaces. Wood, rebars, pipes, cables, bricks and other generated inserts keep their own materials')
    cls.__annotations__['all_generators_material_applied']=bpy.props.PointerProperty(
        type=bpy.types.Material,options={'HIDDEN'},description='Last explicitly applied shared material')
    for name, label in [(name, LABELS[k]) for k,name in CLASS_PROPS.items()] + list(EXTRA_PROPS.items()):
        cls.__annotations__[name] = bpy.props.PointerProperty(
            name=label, type=bpy.types.Material, update=update,
            description='Optional per-source material. Empty keeps the current/default material; shared damage slots affect all effects using that slot')

def overrides(source, existing=None):
    result = dict(existing or {})
    from . import fracture_stack, wall_stack
    fractures = fracture_stack.layers(source)
    canonical = fractures[0][1] if fractures else source.project_damage
    editing_wall = wall_stack.edited(source) is not None
    for cls, name in CLASS_PROPS.items():
        if editing_wall and name in wall_stack.MATERIALS and (cls == 4 or not fractures):
            mat = source.get(wall_stack.BASE_MATERIALS, {}).get(name)
        else:
            mat = getattr(canonical if cls in (1,2,3) else source.project_damage, name)
        if mat is not None:
            result[cls] = mat
                                                                            
    if source.project_damage.fract_enable or source.project_damage.fract_through_enable:
        mat=canonical.fracture_l1_material
        if mat is not None:
            for slot in (1,2,3):result[slot]=mat
    return result

def explicit_materials(props):
    names = list(CLASS_PROPS.values()) + list(EXTRA_PROPS) + [
        'wallpaper_material', 'wallpaper_back_material', 'wall_material','pipe_material','cable_material']
    return tuple(mat for name in names if (mat := getattr(props, name)) is not None)

# собираем видимые настройки этого раздела
def draw(layout, props, module):
    if ui_section()!='MATERIAL':return
    if not has_materials(module):return
    box=layout.box();box.use_property_split=False
    centered_heading(box,'Material Override' if experimental() else 'Materials',icon='MATERIAL')
    row=box.row(align=True);row.scale_y=1.5
    row.prop(props,'one_material_all',text='Add One Material to All',toggle=True,icon='MATERIAL')
    if props.one_material_all:
        from .material_library import draw_property
        draw_property(box,props,'all_generators_material',text='Material')
        row=box.row(align=True);row.scale_y=1.5
        row.enabled=props.all_generators_material is not None
        row.operator('project_damage.material_all',text='Apply to All Damage Surfaces',icon='CHECKMARK')
        return
    if experimental():return                                                                         
    if module=='brick' and props.brick_v2_enable:box.prop(props,'brick_split_materials',text='Separate Material Regions')
    from .material_library import draw_property
    owner=getattr(props.id_data,'project_damage_'+module) if module in {'weld','grass','gravel','foam'} else props
    for name,label in material_fields(module):
        if name=='surface_secondary_material':
            if props.surf_sec_amount<=0:continue
            row=box.row(align=True)
            draw_property(row,owner,name,text=label)
            row.operator('project_damage.surface_material_from_cavity',text='Take from Cavity',icon='DUPLICATE')
        else:draw_property(box,owner,name,text=label)
    if module=='surface' and props.surf_type=='0' and props.surface_rebar_enable:
        draw_property(box,props,'surface_rebar_material',text='Reinforcement')
    if module=='foam':
        from . import foam
        foam.draw_material_settings(layout,bpy.context)
    if module=='boolean' and props.pipe_break_enable:
        draw_property(box,props,'pipe_material',text='Pipe Material')
    if module=='boolean' and props.cable_break_enable:
        draw_property(box,props,'cable_material',text='Cable Material')
    if module=='boolean' and props.rebar_enable:
        name={'0':'rebar_material','1':'rebar_brick_material','2':'rebar_plank_material','3':'rebar_framing_material','4':'rebar_chips_material','5':'rebar_plank_material'}[props.rebar_fill]
        draw_property(box,props,name)
        if props.rebar_fill=='0' and not props.boolean_central_rebar and props.rebar_drawing is not None:
            draw_property(box,props,'rebar_drawn_material')
        if props.rebar_fill=='3':draw_property(box,props,'rebar_nail_material')
        if props.rebar_fill=='1' and not props.rebar_brick_3d:draw_property(box,props,'rebar_mortar_material')
    return
def with_materials(draw_method, module):
    def draw_panel(self, context):
        from . import ownership
        source = ownership.context_source(context)
        active_module = 'fracture_through' if module=='fracture' and getattr(self.layout,'key',None) in {'fracture_through','fracture_volume'} else module
        draw(self.layout, source.project_damage, active_module)
        draw_method(self, context)
    return draw_panel


_MATERIAL_FIELDS = {}


def is_material_property(owner,name):
                                                                          
                                                                       
    rna=owner.bl_rna
    key=(rna.as_pointer(), name)
    if key not in _MATERIAL_FIELDS:
        prop=rna.properties.get(name)
        _MATERIAL_FIELDS[key]=bool(prop is not None and prop.type=='POINTER' and prop.fixed_type.identifier=='Material')
    return _MATERIAL_FIELDS[key]


def material_fields(module):
    if module=='metaltear':return [('metaltear_material','Torn Edges')]
    if module=='peeling':return [('peeling_material','Exposed Substrate'),('peeling_back_material','Coating Underside')]+([('peeling_wallpaper_material','Wallpaper')])
    if module=="clay":return [("clay_material","Clay Material")]
    if module=="erosion":return [("erosion_material","Erosion Surface")]
    if module in {'weld','grass','gravel','foam'}:return [('material','Material')]
    if module in {'glass_hit','glass_drawn'}:return [('surface_damage_material','Broken Openings'),('fracture_l1_material','Cracks')]
    if module=='cables':return [('surface_damage_material','Exposed Concrete'),('fracture_l1_material','Cracks'),('cable_material','Cable Material')]
    if module=='pipes':return [('surface_damage_material','Exposed Concrete'),('fracture_l1_material','Cracks'),('pipe_material','Pipe Material')]
    if module=='lath_framing':module='lath'
    if module=='wallhole':module='brick'
    module = {'fabric_solid':'fabric','concrete_v2':'concrete','lath_v2':'lath','lath_riven':'lath','brick_v2':'brick','glass_hit':'glass','glass_drawn':'glass'}.get(module,module)
    if module=='surface':return [('surface_damage_material','Cavities')]
    if module=='piles':return [('pile_material','Corner Piles')]
    if module=='deposits':return [('deposit_material','Deposits')]
    if module=='concrete':return [('surface_damage_material','Exposed Concrete'),('fracture_l1_material','Cracks'),('concrete_rebar_material','Reinforcement')]
    if module == 'hang':return [('boolean_break_material','Hole Walls'),('rebar_material','Bars'),('hang_cable_material','Cables')]
    from .fracture_assets import TYPE_BY_ASSET
    if module in TYPE_BY_ASSET:module='fracture'
    if module in {'fracture','fracture_through','fracture_volume'}:
        return [('fracture_l1_material','Fracture Material'),('edge_damage_material','Rim Chips')]
    if module=='wallpaper':return [('wallpaper_material','Printed Paper'),('wallpaper_back_material','Paper Backing'),('wall_material','Exposed Wall')]
    if module=='scratch':return [('scratch_groove_material','Scratch Grooves'),('scratch_scuff_material','Scuffed Paint')]
    if module=='bevel':return [('bevel_material','Bevel Surface'),('wear_material','Edge Wear')]
    if module=='dbevel':return [('bevel_material','Chamfer'),('wear_material','Broken Concrete')]
    fields=[(CLASS_PROPS[slot],label) for slot,label in EFFECT_SLOTS.get(module,())]
    extras={'brick':('brick_front_material','brick_break_material','brick_seam_material','brick_behind_material','brick_render_material'),
            'lath':('lath_material',),'debris':('debris_material',),'rbf':('rbf_inner_material',),'cellf':('cellf_inner_material',),
            'deposits':('deposit_material',),'coating':('coating_shell_material',)}
    fields.extend((name,EXTRA_PROPS[name]) for name in extras.get(module,()))
    return fields


def has_materials(module):return bool(material_fields(module)) or (experimental() and module in {'ivy','roots'})


def selected_sources(context):
    from . import ownership
    objects=list(context.selected_objects)
    if not objects:objects=[ownership.context_source(context)]
    sources=[]
    for obj in objects:
        src=ownership.source_of(obj)
        if src is not None and src.type=='MESH' and src not in sources:sources.append(src)
    return sources


# общий бетон не заменяет дерево, металл, гвозди и другие вставки
DAMAGE_MATERIAL_FIELDS = {
    'edge_damage_material','fracture_l1_material','fracture_l2_material','fracture_l3_material',
    'surface_damage_material','surface_secondary_material','boolean_break_material',
    'bevel_material','wear_material','scratch_groove_material','scratch_scuff_material',
    'erosion_material','clay_material','heat_char_material','metaltear_material',
    'brick_behind_material','brick_render_material','wall_material','peeling_material',
    'cellf_inner_material','rbf_inner_material','debris_material',
}


def _damage_material_mask(source,obj):
    import numpy as np
    from . import build,baked_materials,material_update
    if baked_materials.fragment(obj) or any(obj.get(k) for k in baked_materials._FRAGMENT_STAMPS):
        return baked_materials.inner_mask(obj)
    if not (obj.get(build.RESULT_STAMP) or obj.get('project_damage_baked_surface')):
        return np.zeros(len(obj.data.polygons),bool)
    mask=np.zeros(len(obj.data.polygons),bool)
    for field in DAMAGE_MATERIAL_FIELDS:
        part=material_update.body_mask(obj.data,source,field)
        if part is not None:mask |= part
    return mask


def apply_shared_output_material(source):
    p=source.project_damage
    material=p.all_generators_material_applied if p.one_material_all else None
    if material is None:return
    from . import ownership,material_update
    uid=source.get('project_damage_uid')
    for obj in bpy.data.objects:
        if obj.type!='MESH' or obj==source:continue
        if not (getattr(obj,'project_damage_og_object',None)==source or
                (uid and any(obj.get(k)==uid for k in ownership.STAMPS))):continue
        mask=_damage_material_mask(source,obj)
        if mask.any():material_update.assign(obj,material,mask)


def assign_all(context, sources, material):
    from . import ownership, material_update, fracture_stack
    objects=0
    with material_update.shader_edit(context):
        for src in sources:
            p=src.project_damage
            p.one_material_all=True
            p.all_generators_material_applied=material
            for name in DAMAGE_MATERIAL_FIELDS:
                if hasattr(p,name):setattr(p,name,material)
            fracture_stack.save(src)
            for row in src.project_damage_effects:
                if fracture_stack.VALUES in row:
                    for name in ('fracture_l1_material','fracture_l2_material','fracture_l3_material'):
                        row[fracture_stack.VALUES][name]=material
            uid=src.get('project_damage_uid')
            for obj in bpy.data.objects:
                if obj.type!='MESH' or obj==src:continue
                owned=(getattr(obj,'project_damage_og_object',None)==src or
                       (uid and any(obj.get(k)==uid for k in ownership.STAMPS)))
                if not owned:continue
                mask=_damage_material_mask(src,obj)
                if mask.any():
                    material_update.assign(obj, material, mask)
                    objects+=1
    return objects


# действие «Apply Material to Damage Surfaces»
class PROJECT_DAMAGE_OT_material_all(bpy.types.Operator):
    bl_idname='project_damage.material_all'
    bl_label='Apply Material to Damage Surfaces'
    bl_options={'REGISTER','UNDO'}

    @classmethod
    def poll(cls,context):
        from . import ownership
        src=ownership.context_source(context)
        return context.mode=='OBJECT' and src is not None and src.type=='MESH' and src.project_damage.all_generators_material is not None

    def execute(self,context):
        from . import ownership
        src=ownership.context_source(context)
        sources=selected_sources(context)
        try:count=assign_all(context,sources,src.project_damage.all_generators_material)
        except (RuntimeError,ValueError) as exc:
            self.report({'ERROR'},str(exc));return {'CANCELLED'}
        self.report({'INFO'},'Assigned material on %d setups / %d generated objects; geometry unchanged'%(len(sources),count))
        return {'FINISHED'}


TUBE_MATERIAL_FIELDS = ('pipe_material', 'cable_material')


def automatic_material_keys(field):
    marker = '_pd_auto_' + field
    return marker, marker + '_type'


def restore_automatic_material(props, field, values):
    for key in automatic_material_keys(field):
        if key in values:
            props[key] = values[key]
        elif key in props:
            del props[key]


def _automatic_material_changed(props, field, material, kind):
    name_key, type_key = automatic_material_keys(field)
    return props.get(name_key) == material.name and props.get(type_key) != kind


def _set_automatic_material(props, field, material, kind):
    setattr(props, field, material)
    name_key, type_key = automatic_material_keys(field)
    props[name_key] = material.name
    props[type_key] = kind


def remember_defaults(source, result):
    """Keep the selectors in sync with actually generated default materials."""
    import numpy as np
    from . import boolean_stack, live, mesh_io, result_transfer, material_update, material_uv, wall_stack
    previous=live._APPLYING;live._APPLYING=True
    p=source.project_damage

    def assign(field, material):
        if material is None:
            return
        current = getattr(p, field)
        base = material_uv.base_material(material)
        if field in TUBE_MATERIAL_FIELDS:
            kind = getattr(p, field.replace('_material', '_type'))
            if current is None or _automatic_material_changed(p, field, current, kind):
                _set_automatic_material(p, field, base, kind)
        elif current is None:
            setattr(p, field, base)

    try:
        mesh=result.data
        indices=np.empty(len(mesh.polygons),dtype=np.int32);mesh.polygons.foreach_get('material_index',indices)
        dedicated=np.zeros(len(indices),dtype=np.bool_)
        for _,attr,_ in result_transfer.MATERIAL_REGIONS:
            mask=material_update.face_values(mesh,attr)
            if mask is not None:dedicated|=mask.astype(bool)
        boolean = boolean_stack.edited(source)
        wall = wall_stack.edited(source)
        scope = {}
        if boolean_stack.SERIAL in source:
            owners = material_update.face_values(mesh, boolean_stack.FACE_OWNER)
            serial = boolean[boolean_stack.INSTANCE] if boolean is not None else 0
            scope['boolean_break_material'] = owners == serial if owners is not None else np.full(len(indices), serial == 0)
        if wall_stack.SERIAL in source:
            owners = material_update.face_values(mesh, 'project_damage_wall_instance')
            serial = wall[wall_stack.INSTANCE] if wall is not None else 0
            mask = owners == serial if owners is not None else np.full(len(indices), serial == 0)
            scope.update(dict.fromkeys(wall_stack.MATERIALS, mask))

        def region(attr, shared=False, owner_mask=None):
            attribute=mesh.attributes.get(attr)
            if attribute is None or attribute.domain!='FACE' or len(attribute.data)!=len(indices):return None
            mask=np.empty(len(indices),dtype=np.bool_);attribute.data.foreach_get('value',mask)
            if shared:mask&=~dedicated
            if owner_mask is not None:mask&=owner_mask
            valid=indices[mask]
            if not len(valid):return None
            counts=np.bincount(valid);slot=int(counts.argmax())
            return mesh.materials[slot] if slot<len(mesh.materials) else None
        for cls,field in CLASS_PROPS.items():
            assign(field,region(mesh_io.CLASS_NAMES[cls],shared=True,owner_mask=scope.get(field)))
        for _,attr,field in result_transfer.MATERIAL_REGIONS:assign(field,region(attr))
        mapping=dict(material_update.OUTPUTS)
        if p.rebar_enable:mapping[material_update.REBAR_FIELDS.get(p.rebar_fill,'rebar_material')]=('rebar',0)
        for field,(kind,slot) in mapping.items():
            obj = material_update.output_object(source, kind, field)
            if obj is not None and slot < len(obj.data.materials):
                assign(field, obj.data.materials[slot])
    finally:live._APPLYING=previous


def automatic_default(props,field,kind,factory):
    from . import live
    material=getattr(props,field)
    if material is not None and not _automatic_material_changed(props, field, material, kind):
        return material
    material=factory(kind)
    if material is not None and isinstance(props, bpy.types.PropertyGroup):
        previous=live._APPLYING;live._APPLYING=True
        try:
            _set_automatic_material(props, field, material, kind)
        finally:live._APPLYING=previous
    return material
