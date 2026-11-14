# служебный редактор раскладки; основной интерфейс задают sidebar.py и panels.py
from . import quality_ui

                                                                              
                                                      
                                                                              

# SPDX-License-Identifier: GPL-3.0-or-later
"""Project Damage 0.15.27 — single-file UI editor.

QUICK START
  Enable Project Damage, open this file in Blender's Text Editor, press Alt+P.
  Edit any layout below, then Alt+P again: no addon rebuild or Blender restart.
  Save this file to keep edits. Run it again after restarting Blender.
  This is an editable UI overlay, NOT an addon installer or standalone addon.
  It uses installed operators/properties; no mesh algorithms are copied here.
  No installation files or scene objects are modified by running the script.

NAVIGATION (search section number or function name)
  01  EDIT FIRST: names, category order, hidden effects, sizing
  02  Shared UI adapters and helpe`rs
  03  Add / Presets / Tools / Settings panels + effect-list rows
  04  Materials and Grease Pencil controls
  05  Tools: mesh preparation / cleanup / advanced masks
  06  Static effects: Edge / Surface / Fracture / Buildup / etc.
  07  Other effects: explicit property-group lists, shared drawing functions
  08  DEV TOOLS panel
  09  Apply / restore UI overlay — leave this section alone normally

EDITING TIPS
  layout.prop(p, 'property_name', text='Visible name') adds a setting.
  layout.operator('project_damage.operator_name', text='Button') adds a button.
  row = layout.row(align=True) places controls side by side.
  box = layout.box() groups controls. Remove/move lines to rearrange them.
  Keep property names, operator IDs and effect/category keys unchanged.
  UI order does NOT change the processing order of damage algorithms.
  Existing DEV TOOLS names/hidden/pinned settings still take precedence.
  To restore stock UI: call restore_ui() or restart Blender without running this.
"""
import bpy
from pathlib import Path
from types import SimpleNamespace
import project_damage as addon
from project_damage import (sidebar, panels, ownership, dev_tools, material_controls,
    breaker as breaker_mod, build, clean_floating, core_loader, live, manual,
    input_prep, paint)
from project_damage.paint import drawing_prop
if not hasattr(bpy.types.Object, 'project_damage'):
    raise RuntimeError('Enable Project Damage in Blender Preferences before running this file')
source = sidebar.source
enabled = sidebar.enabled
initialized = sidebar.initialized



                                                                              
                                                              
                                                                              

                                                         
EFFECTS = (
    ('edge','Edge Damage','chips_enable','MOD_BEVEL'),
    ('surface','Surface Damage','surf_enable','MOD_DISPLACE'),
    ('corner','Corner Damage','corner_enable','VERTEXSEL'),
    ('fracture','Fracture','fract_enable','MOD_EXPLODE'),
    ('fracture_through','Fracture Through','fract_through_enable','MOD_EXPLODE'),
    ('buildup','Buildup / Deposits','build_enable','MOD_BUILD'),
    ('scratch','Scratches Draw','scratch_enable','GREASEPENCIL'),
    ('coating','Coating Peeling','coating_enable','MOD_SOLIDIFY'),
    ('fabric','Fabric Tears','fabric_enable','MOD_CLOTH'),
    ('concrete','Concrete Damage','concrete_enable','MESH_ICOSPHERE'),
    ('hit','Concrete Hit','hit_enable','FORCE_FORCE'),
    ('plaster','Plaster Displacement','plaster_enable','MOD_DISPLACE'),
    ('metal','Metal Deform','metal_enable','MOD_MESHDEFORM'),
    ('wood','Wood Damage','wood_enable','OUTLINER_OB_CURVE'),
    ('lath','Lath Damage','lath_enable','MOD_ARRAY'),
    ('wallpaper','Wallpaper','wallpaper_enable','TEXTURE'),
    ('bullet','Bullet Impacts','bullet_enable','PARTICLE_DATA'),
    ('blast','Building Destruction','blast_enable','MOD_BUILD'),
    ('ground','Ground Cracks','ground_enable','MESH_GRID'),
    ('brick','Brick Damage','brick_enable','MOD_WIREFRAME'),
    ('glass','Broken Glass','glass_enable','MESH_UVSPHERE'),
    ('fissure','Ground Fissures','fissure_enable','MOD_OCEAN'),
    ('boolean','Boolean Break','ui_boolean_enable','MOD_BOOLEAN'),
)
EFFECTS += (('hit_through','Concrete Hit Through','hit_through_enable','FORCE_FORCE'),
            ('glass_through','Broken Glass Through','glass_through_enable','MESH_UVSPHERE'),
            ('debris','Damage Debris','debris_enable','PARTICLES'))
CATALOG={r[0]:r for r in EFFECTS}
TILE_LABELS=dict(zip((r[0] for r in EFFECTS),('Edges','Surface','Corners','Fracture','Fracture Through','Deposits','Scratches','Peeling','Fabric','Concrete','Hit','Plaster','Metal','Wood','Lath','Wallpaper','Bullets','Destruction','Ground','Bricks','Glass','Fissures','Boolean')))
TILE_LABELS.update(blast='Buildings')
TILE_LABELS.update(hit_through='Hit Through',glass_through='Glass Through',debris='Damage Debris')
REMOVED_EFFECTS={'heat','bevel','rust'}
HIDDEN_EFFECTS={'coating'}                                                
ENABLE_PROPS={r[2] for r in EFFECTS} | {'deposit_enable'}


                                                                             
                                                                 
EFFECT_CATEGORIES = (
    ('general', 'General Damage', ('edge','surface','corner','fracture','fracture_through','buildup','debris')),
    ('walls', 'Walls & Layers', ('concrete','hit','hit_through','plaster','brick','lath','wallpaper','coating')),
    ('materials', 'Material Damage', ('wood','metal','fabric','glass','glass_through')),
    ('impacts', 'Impacts & Cuts', ('scratch','bullet','blast','boolean')),
    ('ground', 'Ground Damage', ('ground','fissure')),
)
TOOLS_CATEGORIES = (
    ('PREP', 'Mesh Preparation', 'Input repair, open surfaces and Smart Remesh'),
    ('CLEANUP', 'Cleanup / Trim', 'Remove floating parts and trim generated results'),
    ('MASKS', 'Advanced / Masks', 'Avoid regions, vertex-group masks and edge rules'),
    ('OUTPUT', 'Output', 'Source-data preservation, topology and UV output'),
)


CATEGORY_ROW_HEIGHT = 2.5
COMPACT_COLUMNS = 2
PREVIEW_COLUMNS = 3
PREVIEW_IMAGE_SCALE = 5.0
COMPACT_BUTTON_HEIGHT = 1.15
PREVIEW_BUTTON_HEIGHT = 1.3
PREVIEW_DIRECTORY = Path(addon.__file__).resolve().parent / 'previews'



                                                                              
                                                        
                                                                              

_PREVIEWS = None
_PREVIEW_FILES = {}

def _preview_icon(key, fallback):
    global _PREVIEWS
    path = PREVIEW_DIRECTORY / (key + '.png')
    if path.is_file():
        try:
            stamp = path.stat().st_mtime_ns
            if _PREVIEWS is None:
                _PREVIEWS = bpy.utils.previews.new()
            if _PREVIEW_FILES.get(key) != stamp:
                if key in _PREVIEWS: del _PREVIEWS[key]
                _PREVIEWS.load(key, str(path), 'IMAGE')
                _PREVIEW_FILES[key] = stamp
            return _PREVIEWS[key].icon_id
        except (OSError, RuntimeError, KeyError):
            pass
    icons = bpy.types.UILayout.bl_rna.functions['prop'].parameters['icon'].enum_items
    item = icons.get(fallback)
    return item.value if item is not None else 0

def fold(layout,key,label,closed=True):
    head,body=layout.panel('project_damage_'+key,default_closed=closed)
    head.label(text=label)
    return body

class _Layout:
    """Reuse proven property drawers without legacy apply buttons/enable checkboxes."""
    def __init__(self,layout,key=None,raw=False):
        object.__setattr__(self,'layout',layout)
        object.__setattr__(self,'key',key)
        object.__setattr__(self,'raw',raw)
    def __getattr__(self,name):
        value=getattr(self.layout,name)
        if name in {'row','column','box','split','grid_flow','column_flow'}:
            return lambda *a,**kw:type(self)(value(*a,**kw),self.key,self.raw)
        return value
    def __setattr__(self,name,value):setattr(self.layout,name,value)
    def prop(self,owner,name,**kwargs):
        if name in ENABLE_PROPS:return
        if self.key is not None and not self.raw:
            return dev_tools.draw_property(self.layout,owner,name,self.key,**kwargs)
        return self.layout.prop(owner,name,**kwargs)
    def prop_search(self,owner,name,*args,**kwargs):
        if self.key is not None and not self.raw:
            config=dev_tools.field_config(self.key)
            if name in config.get('hidden',[]) or name in config.get('pinned',[]):return
            label=config.get('labels',{}).get(name)
            if label:kwargs['text']=label
        return self.layout.prop_search(owner,name,*args,**kwargs)
    def operator(self,name,**kwargs):
        if name.startswith('project_damage.apply_'):return SimpleNamespace()
        return self.layout.operator(name,**kwargs)

class _GlobalLayout(_Layout):
    def label(self,**kwargs):
        if kwargs.get('text')=='Mesh':return
        return self.layout.label(**kwargs)
    def prop(self,owner,name,**kwargs):
        if name in {'global_scale','global_detail','draft_enable','draft_quality','seed','all_off','topology','keep_uvs','keep_selections'}:return
        return super().prop(owner,name,**kwargs)

def draw_effect(layout,context,key):
    if key in REMOVED_EFFECTS:return
    dev_tools.draw_pinned(layout,context,key)
    _draw_effect_original(layout,context,key)

def _tab(layout, props, enable_attr):
    """The shared damage-tab body gate. The Enable switch itself lives in the
    panel HEADER (draw_header, below) rather than the body since the
    UI-polish pass - the Blender physics-panel idiom - so this only ever
    needs to build the grayed column everything under it draws into: C4D's
    GetDEnabling grays the whole BODY when the module is off, one to one,
    the header checkbox is drawn ungated and never asked here.

    Inside a live tab a second gate applies, and it is the same everywhere:
    every row that has no effect while the module's PRIMARY field is 0 grays
    too (project_damage_plugin.cpp's GetDEnabling cases). Each tab below puts those rows
    in their own `active`-gated column. Chips is the one asymmetry, faithfully:
    its Detail and Shape rows carry no such case and stay live at Amount 0,
    while its Scale does not."""
    layout.use_property_split = True
    layout.use_property_decorate = False
    col = layout.column()
    col.active = (getattr(props, enable_attr) or (enable_attr=='fract_enable' and props.fract_through_enable)) and not props.all_off
    return col

def _fract_any_amount(p):
    """PROJECT_DAMAGE_FRACT_SCALE/_DETAIL, the Noise pair and the Options group's Depth
    Multiplier all share one GetDEnabling case: ANY level's Amount > 0."""
    return (p.fract_mode == 'PAINT' or any(getattr(p, "fract_l%d_amount" % n) > 0 for n in (1, 2, 3))
            or (p.fract_prox_mode in {'RADIAL', 'GLASS'} and p.fract_prox_collection is not None))

def _has_linked_breaker(props):
    """Mirrors GetDEnabling's InExcludeData::GetObjectCount() == 0 check: a
    row with no object assigned (the user cleared it, or a fresh empty row)
    does not count as a linked breaker."""
    return any(row.obj is not None for row in props.breakers)


                                                                              
                                                     
                                                                              

def _draw_effect_tiles(layout, context, src):
    full = context.window_manager.project_damage_add_view == 'FULL'
                                                                          
    tabs = layout.row(align=True)
    tabs.scale_y = CATEGORY_ROW_HEIGHT
    for key, title, _ in EFFECT_CATEGORIES:
        op = tabs.operator('wm.context_set_enum', text=title, depress=context.window_manager.project_damage_add_category==key)
        op.data_path = 'window_manager.project_damage_add_category'; op.value = key
    layout.prop(context.window_manager, 'project_damage_add_view', expand=True)
    active_category = context.window_manager.project_damage_add_category
    rows = dev_tools.tiles(EFFECTS)
    for category, title, keys in EFFECT_CATEGORIES:
        if category != active_category: continue
        entries = [row for row in rows if row[0] in keys and row[0] not in HIDDEN_EFFECTS]
        if not entries:
            layout.label(text='All effects in this category are hidden in DEV TOOLS', icon='INFO')
            continue
        grid = layout.grid_flow(row_major=True, columns=PREVIEW_COLUMNS if full else COMPACT_COLUMNS,
                              even_columns=True, even_rows=True, align=True)
        for key, label, _, image in entries:
            tile = grid.column(align=True)
            if full:
                card = tile.box()
                preview = card.row(align=True)
                preview.alignment = 'CENTER'
                preview.template_icon(icon_value=_preview_icon(key,image), scale=PREVIEW_IMAGE_SCALE)
                button_row = tile.row(align=True)
                button_row.scale_y = PREVIEW_BUTTON_HEIGHT
                button = button_row.operator('project_damage.effect_action',
                    text=dev_tools.effect_name(key,label), depress=enabled(src,key))
            else:
                tile.scale_y = COMPACT_BUTTON_HEIGHT
                button = tile.operator('project_damage.effect_action',
                    text=dev_tools.effect_name(key,TILE_LABELS[key]),
                    icon=image, depress=enabled(src,key))
            button.effect=key
            button.action='ADD'

def draw_panel_add(self,context):
    from project_damage import core_loader
    layout=self.layout;src=source(context)
    if src is None:
        layout.label(text='Select a source or deformable object',icon='INFO');return
    p=src.project_damage
    layout.label(text=src.name,icon='OBJECT_DATA')
    mismatch=core_loader.version_mismatch()
    if mismatch:layout.label(text='Restart Blender to finish update',icon='ERROR')
    layout.operator('project_damage.update',text='INIT_project_damage',icon='FILE_REFRESH')
    if not initialized(context):return
    row=layout.row(align=True);row.operator('project_damage.select_original',text='Original');row.operator('project_damage.select_result',text='Deformable')
    layout.label(text='Add an effect')
    _draw_effect_tiles(layout,context,src)

def draw_panel_presets(self,context):
    layout=self.layout
    if source(context) is None:return
    box=layout.box();box.label(text='USER_LIBRARY',icon='PRESET')
    row=box.row(align=True);row.menu('PROJECT_DAMAGE_MT_presets',text=bpy.types.PROJECT_DAMAGE_MT_presets.bl_label)
    row.operator('project_damage.add_preset',text='',icon='ADD')
    row.operator('project_damage.add_preset',text='',icon='REMOVE').remove_active=True
    box.label(text='Save and reuse your effect settings')

def draw_panel_tools(self, context):
    from project_damage import input_prep, clean_floating, panels

    layout = self.layout
    src = source(context)
    if src is None:
        return

    p = src.project_damage

                            
    main_row = layout.split(factor=0.5, align=True)

                 
    box = main_row.box()

    row = box.row()
    row.alignment = 'CENTER'
                                                       

    live = box.column(align=True)
    live.prop(p, 'live_update', text='Live Update', icon='FILE_REFRESH')

    delay = live.row(align=True)
    delay.enabled = p.live_update
    delay.prop(p, 'defer_ms', text='Interactive Delay')

           
    box = main_row.box()

                    
                             
                                            

    edges = box.column(align=True)
    edges.prop(p, "edge_threshold")
    sidebar.draw_edge_axes(edges, p)
    tabs=layout.row(align=True);tabs.scale_y=CATEGORY_ROW_HEIGHT
    for key, title, _ in TOOLS_CATEGORIES:
            op=tabs.operator('wm.context_set_enum',text=title,depress=context.window_manager.project_damage_tools_category==key)
            op.data_path='window_manager.project_damage_tools_category';op.value=key
    body=layout.column(align=True)
    body.use_property_split=True;body.use_property_decorate=False
    category=context.window_manager.project_damage_tools_category
    if category=='PREP':
        input_prep.OBJECT_PT_project_damage_input.draw(SimpleNamespace(layout=body),context)
    ''' REMESH REMESHERS remesh remesher retopo
        box=body.box();box.active=not p.quad_remesh;box.prop(p,'smart_remesh')
        sub=box.column(align=True);sub.active=p.smart_remesh
        sub.prop(p,'smart_remesh_density');sub.prop(p,'smart_remesh_grade')
        box=body.box();box.prop(p,'quad_remesh')
        sub=box.column(align=True);sub.active=p.quad_remesh
        sub.row(align=True).prop(p,'quad_remesh_output',expand=True)
        sub.prop(p,'quad_remesh_size')
        if p.quad_remesh:
            box.label(text='Includes Smart Remesh; can add many faces.',icon='INFO')
    '''
    if category=='CLEANUP':
        clean_floating.draw_controls(body,context)
    elif category=='MASKS':
        panels.OBJECT_PT_project_damage_global.draw(SimpleNamespace(layout=_GlobalLayout(body)),context)
    elif category=='OUTPUT':
        for prop in ('preserve_data','keep_uvs','keep_selections','topology'):
            body.prop(p,prop)
        body.operator('project_damage.unwrap_zones',text='Unwrap Material Zones',icon='UV')
        body.separator()
        body.prop(p,'debris_enable',text='Damage Debris')
        body.operator('project_damage.generate_debris',icon='PARTICLES')

def draw_panel_settings(self,context):
    from project_damage import panels
    layout=self.layout;src=source(context)
    if src is None:
        panels.OBJECT_PT_project_damage_breaker.draw(SimpleNamespace(layout=layout),context);return
    p=src.project_damage
    if context.object.type in {'GREASEPENCIL','GPENCIL','CURVE'}:
        button=layout.operator('project_damage.paint_return',text='Finish Drawing + Update',icon='CHECKMARK');button.update=True
    master=layout.column(align=True);master.use_property_split=True;master.use_property_decorate=False
    master.prop(p,'global_scale',text='Master Scale')
    from . import preview_quality
    preview_quality.draw(master,p)
    master.prop(p,'seed');master.prop(p,'all_off',text='Mute All Effects')
    layout.template_list('PROJECT_DAMAGE_UL_effects','',src,'project_damage_effects',src,'project_damage_effect_index',rows=5,maxrows=8)
    row=layout.row(align=True);row.operator('project_damage.effect_action',text='Refresh Selected',icon='FILE_REFRESH').action='APPLY'
    row.operator('project_damage.apply_all_damage',text='Apply All',icon='CHECKMARK')
    rows=src.project_damage_effects
    if rows:
        key=rows[min(src.project_damage_effect_index,len(rows)-1)].key
        if key in CATALOG:
            material_controls.centered_heading(layout,dev_tools.effect_name(key,CATALOG[key][1]),icon=CATALOG[key][3])
            if not enabled(src,key):layout.label(text='Hidden — settings are kept',icon='HIDE_ON')
            draw_effect(layout,context,key)
    else:layout.label(text='Choose an effect in Add',icon='INFO')

def draw_effect_list_item(self,context,layout,data,item,icon,active_data,active_prop,index=0,flt_flag=0):
    src=data;key=item.key
    if key not in CATALOG:return
    _,label,_,image=CATALOG[key]
    row=layout.row(align=True)
    row.label(text=dev_tools.effect_name(key,label),icon=image)
    button=row.operator('project_damage.effect_action',text='',icon='HIDE_OFF' if enabled(src,key) else 'HIDE_ON',emboss=False);button.effect=key;button.action='TOGGLE'
                                                               
    button=row.operator('project_damage.effect_action',text='',icon='FILE_REFRESH',emboss=False);button.effect=key;button.action='APPLY'
    button=row.operator('project_damage.effect_action',text='',icon='X',emboss=False);button.effect=key;button.action='REMOVE'


                                                                              
                                    
                                                                              

CLASS_PROPS = {0: 'edge_damage_material', 1: 'fracture_l1_material',
               2: 'fracture_l2_material', 3: 'fracture_l3_material',
               4: 'surface_damage_material', 5: 'buildup_material',
               7: 'boolean_break_material'}
LABELS = {0: 'Edge Damage', 1: 'Cracks L1', 2: 'Cracks L2', 3: 'Cracks L3',
          4: 'Surface Damage', 5: 'Buildup / Fragments', 7: 'Boolean Cut'}
                                                                            
EFFECT_SLOTS = {
    'bevel': (), 'scratch': (), 'edge': ((0, 'Cut Edges'),), 'surface': ((4, 'Cavities'),),
    'corner': ((4, 'Corner Cavities'),),
    'fracture': ((1, 'Level 1'), (2, 'Level 2'), (3, 'Level 3'), (0, 'Rim Chips')),
    'fracture_through': ((1, 'Cracks'), (0, 'Rim Chips')),
    'hit_through': ((4,'Impact Crater'),(1,'Chunk Cuts / Cracks')),
    'glass_through': ((4,'Broken Openings'),(1,'Shard Cuts / Cracks'),(0,'Glass Chips')),
    'debris': (),
    'buildup': ((5, 'Buildup'),),
    'coating': ((4, 'Exposed Surface'), (5, 'Flakes')),
    'rust': ((4, 'Corroded Surface'), (5, 'Rust Buildup')),
    'heat': ((4, 'Damaged Surface'), (5, 'Blisters / Buildup')),
    'boolean': ((7, 'Cut Faces'), (0, 'Rim Damage')),
    'fabric': ((4, 'Torn Edges'), (5, 'Threads / Fibres')),
    'concrete': ((4, 'Exposed Concrete'), (1, 'Cracks'), (5, 'Rebar')),
    'hit': ((4, 'Impact Crater'), (2, 'Exposed Plates'), (1, 'Cracks')),
    'wood': ((4, 'Exposed Wood'), (1, 'Checks / Splits'), (5, 'Splinters')),
    'lath': ((4, 'Exposed Layers'), (1, 'Cracks')),
    'bullet': ((4, 'Impact Holes'), (1, 'Cracks'), (2, 'Halo / Scorch'), (5, 'Burrs / Splinters')),
    'blast': ((4, 'Blast Cavities'), (2, 'Exposed Structure'), (1, 'Cracks')),
    'ground': ((4, 'Broken Areas'), (1, 'Cracks'), (5, 'Rubble / Heave')),
    'brick': ((1, 'Cracks'), (5, 'Loose Bricks')),
    'glass': ((4, 'Broken Openings'), (1, 'Cracks'), (0, 'Glass Chips')),
    'fissure': ((4, 'Chasm Walls'), (1, 'Branches'), (5, 'Slabs / Rubble')),
}
EXTRA_PROPS = {'rebar_material':'Rebar','rebar_brick_material':'Boolean Fill Bricks','rebar_plank_material':'Boolean Fill Wood','bevel_material':'Bevel Surface', 'wear_material':'Edge Wear', 'scratch_groove_material':'Scratch Grooves', 'scratch_scuff_material':'Scuffed Paint','heat_char_material': 'Heat Char',
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


def centered_heading(layout, text, icon='NONE'):
    """Center the icon and title as one unit, independent of property split."""
    row = layout.row(align=True)
    row.alignment = 'CENTER'
    row.use_property_split = False
    row.label(text=text, icon=icon)

def draw_materials(layout, props, module):
    if module not in EFFECT_SLOTS:
        return                                                                        
    box = layout.box()
    centered_heading(box, 'Materials', icon='MATERIAL')
    if module=='debris':box.prop(props,'debris_material')
    if module in {'bevel','scratch'}:
        for name in (('bevel','wear') if module=='bevel' else ('scratch_groove','scratch_scuff')):
            box.prop(props,name+'_material')
    if module == 'brick':
        box.prop(props, 'brick_split_materials')
        box.prop(props, 'brick_front_material')
        split = box.column()
        split.enabled = props.brick_split_materials
        for name in ('brick_break_material', 'brick_seam_material', 'brick_behind_material','brick_render_material'):
            split.prop(props, name)
    if module == 'lath':box.prop(props,'lath_material')
    for slot, label in EFFECT_SLOTS[module]:
        box.prop(props, CLASS_PROPS[slot], text=label)
    if module == 'heat':
        box.prop(props, 'heat_char_material')
    if module == 'buildup':
        box.prop(props, 'deposit_material')
    if module == 'coating':
        box.prop(props, 'coating_shell_material')
    if module not in {'bevel','scratch'}:
        box.label(text='Shared slots: Surface / Cracks / Edge / Buildup', icon='INFO')

def drawing_row(layout, p, target, level=1):
    prop = drawing_prop(target, level)
    picker = layout.row(align=True)
    picker.prop(p, prop, text="Drawing")
    remove = picker.row(align=True)
    remove.enabled = getattr(p, prop) is not None
    op = remove.operator('project_damage.paint_delete', text='', icon='TRASH')
    op.target, op.level = target, level
    row = layout.row(align=True)
    for op_id, text in (("project_damage.paint_new", "New Drawing"), ("project_damage.paint_draw", "Draw / Edit")):
        sub = row.row(align=True)
        sub.enabled = op_id != "project_damage.paint_draw" or getattr(p, prop) is not None
        op = sub.operator(op_id, text=text)
        op.target, op.level = target, level


                                                                              
                                                           
                                                                              

def draw_mesh_preparation(self, context):
    s = ownership.source_of(context.object).project_damage_input
    layout = self.layout
    column = layout.column()

                    
    box = column.box()

    row = box.row()
    row.alignment = 'CENTER'
    row.label(text="Basic Settings")

    settings = box.column(align=True)
    settings.prop(s, "mode")
    settings.prop(s, "use_modifiers")

    column.separator()


                 
    box = column.box()

    row = box.row()
    row.alignment = 'CENTER'
    row.label(text="Mesh Repair")

    repair = box.column(align=True)
    repair.active = s.mode == 'AUTO'
    repair.prop(s, "weld_distance")

    column.separator()


                                  
    box = column.box()

    row = box.row()
    row.alignment = 'CENTER'
    row.label(text="Open / Non-manifold Surfaces")

    surface = box.column(align=True)
    surface.active = s.mode != 'CLOSED'
    surface.prop(s, "holes")
    surface.prop(s, "surface_thickness")
    surface.prop(s, "surface_flip")

    column.separator()


                         
    box = column.box()

    row = box.row()
    row.alignment = 'CENTER'
    row.label(text="Additional Settings")

    settings = box.column(align=True)
    settings.prop(s, "restore_ngons")
    settings.prop(s, "thickness_logic")
    settings.prop(s, "transfer_data")

    box.label(
        text="The source object is never modified.",
        icon='INFO'
    )

def draw_cleanup(layout, context):
    s = context.scene.project_damage_clean_floating

                          
    box = layout.box()

    row = box.row()
    row.alignment = 'CENTER'
    row.label(text="Clean Floating Parts", icon='MODIFIER')

                                      
    section = box.box()

    row = section.row()
    row.alignment = 'CENTER'
    row.label(text="Protrusions Above Source Surface", icon='MOD_BOOLEAN')

    settings = section.column(align=True)
    settings.prop(s, "reference")
    settings.prop(s, "allowance")
    settings.prop(s, "use_reference_modifiers")
    settings.prop(s, "flip_open_side")

    section.operator(
        "project_damage.trim_to_source",
        icon='MOD_BOOLEAN'
    )

                    
    section = box.box()

    row = section.row()
    row.alignment = 'CENTER'
    row.label(text="Detached Parts", icon='MESH_DATA')

    settings = section.column(align=True)
    settings.prop(s, "min_part_percent")
    settings.prop(s, "keep_largest_only")
    settings.prop(s, "report_only")

    section.operator(
        "project_damage.clean_floating_parts",
        icon='BRUSH_DATA'
    )

                 
    box.separator()
    box.label(
        text="Result only. Run again after Update.",
        icon='INFO'
    )
    box.label(
        text="Trim first, then Clean. Open sources supported.",
        icon='INFO'
    )


                                                                              
                                                                     
                                                                              

                 
def draw_effect_cleanup(self, context):
    clean_floating.draw_controls(self.layout, context)

                
def draw_effect_global(self, context):
    p = ownership.source_of(context.object).project_damage
    layout = self.layout
    layout.use_property_split = True
    layout.use_property_decorate = False
    layout.prop(p, "all_off")
                                                                          
                                                             
    layout.prop(p, "global_scale")
    from . import preview_quality
    preview_quality.draw(layout,p)
    layout.prop(p, "seed")
    layout.separator()
                                                                     
                                                                      
    layout.label(text="Mesh")
    mesh = layout.column()
    mesh.prop(p, "topology")
    mesh.prop(p, "keep_uvs")
    mesh.prop(p, "keep_selections")
    layout.separator()
                                                                                    
                                                                       
                                                                                 
                                                                         
                                                                        
    pass
    avoid = layout.column()
    pass
    avoid_on = avoid.row(align=True)
    avoid_on.active = bool(p.avoid_attr)
    for prop in ("avoid_chips", "avoid_surf", "avoid_build",
                 "avoid_fract", "avoid_corner"):
        pass                   
    layout.separator()
                                                                                  
                                                                        
                                                                        
                                                                                             
                                                     
    layout.label(text="Damage Mask")
    mask = layout.column()
    mask.prop_search(p, "mask_group", ownership.source_of(context.object), "vertex_groups")
    mask_on = mask.column()
    mask_on.active = bool(p.mask_group) and p.mask_group in ownership.source_of(context.object).vertex_groups
    mask_on.prop(p, "mask_amount")
    mask_on.prop(p, "mask_size")
    box = layout.box()
    box.prop(p, 'draw_mask_enable')
    if p.draw_mask_enable:
        from project_damage.paint import drawing_row
        box.row(align=True).prop(p, 'draw_mask_mode', expand=True)
        drawing_row(box, p, 'DRAWMASK')
        box.prop(p, 'draw_mask_radius')
        box.prop(p, 'draw_mask_soft')
        box.prop(p, 'draw_mask_size')
        box.label(text='Empty drawing: mask ignored.', icon='INFO')
    

               
def draw_effect_chips(self, context):
    p = ownership.source_of(context.object).project_damage
    col = _tab(self.layout, p, "chips_enable")
    colr=col.row()
    colr.scale_y = 2
    colr.row(align=True).prop(p,'chips_mode',text = ' ', expand=True)
    if p.chips_mode == 'PAINT':
        box = col.box()

        from project_damage.paint import drawing_row
        drawing_row(box, p, 'EDGE')

        box.prop(p, 'chips_paint_radius')
        box.label(
            text='Draw near existing edges. Amount sets chip density.',
            icon='INFO'
        )
    box = col.box()

    row = box.row()
    row.alignment = 'CENTER'
    row.label(text="Basic Settings")

    scale = box.column( align = True)
    scale.active = p.amount > 0                                         
    scale.prop(p, "chip_scale")

    scale.prop(p, "chip_seed", text="Seed")
    quality_ui.draw(scale, p, "detail")                                     
    scale.prop(p, "amount")
    scale.prop(p, "offset")
    scale.prop(p, "random_offset")
           
    box = col.box()
    row = box.row()
    row.alignment = 'CENTER'
    row.label(text="Shape")
    size = box.column(align=True)
    size.prop(p, "size_min")
    size.prop(p, "size_max")
    depth = box.column(align=True)
    depth.prop(p, "depth_min")
    depth.prop(p, "depth_max")
    box.prop(p, "stretch")
    box.prop(p, "rim_soft")
    box.separator()
    look = box.column(align=True)
    look.prop(p, 'chip_cluster')
    look.prop(p, 'chip_fine')
    look.prop(p, 'chip_profile')
    look.prop(p, 'chip_asym')
    look.prop(p, 'chip_guard')

                     
def draw_effect_chips_noise(self, context):
    p = ownership.source_of(context.object).project_damage
    layout = self.layout
    layout.use_property_split = True
    layout.use_property_decorate = False

    box = layout.box()

    row = box.row()
    row.alignment = 'CENTER'
    row.label(text="Noise Settings", icon='MOD_NOISE')

    col = box.column(align=True)
    col.active = p.chips_enable and not p.all_off
    col.prop(p, "noise_strength")
    col.prop(p, "noise_scale")
    col.prop(p, "noise_roughness")
    col.prop(p, "noise_distortion")

                 
def draw_effect_surface(self, context):
    p = ownership.source_of(context.object).project_damage
    col = _tab(self.layout, p, "surf_enable")
    row = col.row()
    row.alignment = 'CENTER'
    row.scale_y= 2 
    row.prop(p, 'surf_mode', text = ' ', expand = True)
    if p.surf_mode == 'PAINT':
        box = col.box()

        from project_damage import paint
        paint.drawing_row(box, p, 'SURFACE')

        box.prop(p, 'surf_close_strokes')
        box.prop(p, 'surf_paint_snap')
        box.label(text='Closed contours limit crater bowls.', icon='INFO')
                                                                            
                                                                           
                                                                       
                                                      
    box = col.box()

    row = box.row()
    row.alignment = 'CENTER'
    row.label(text="Basic Settings")

    on = p.surf_amount > 0

    head = box.column()
    head.active = on
    head.prop(p, "surf_scale", text='Scale')
    quality_ui.draw(head, p, "surf_detail", text='Resolution')
    head.prop(p, 'surf_seed', text='Seed')

    box.prop(p, "surf_amount")
    size = box.column(align = True)
    size.prop(p, "surf_size_min")
    size.prop(p, "surf_size_max")
    col.separator()

    depth = box.column(align = True)
    depth.prop(p, "surf_depth_min")
    depth.prop(p, "surf_depth_max")
    col.separator()
                                                                         
                                                                
                                                                      
                                       
    box = col.box()

    row = box.row()
    row.alignment = 'CENTER'
    row.label(text="Secondary Craters")

    sec = box.column()
    sec.active = on and p.surf_mode != 'PAINT'                                                   

                                                    
    sec.prop(p, "surf_sec_amount")

    secfields = sec.column()
    secfields.active = on and p.surf_sec_amount > 0

                                         
    secfields.prop(p, "surf_sec_size")
    secfields.prop(p, "surf_sec_depth")

                       
def draw_effect_surface_noise(self, context):
    p = ownership.source_of(context.object).project_damage
    layout = self.layout
    layout.use_property_split = True
    layout.use_property_decorate = False

    box = layout.box()

    row = box.row()
    row.alignment = 'CENTER'
    row.label(text="Noise Settings", icon='MOD_NOISE')

    col = box.column()
    col.active = p.surf_enable and not p.all_off and p.surf_amount > 0
    col.prop(p, "surf_noise")
    col.prop(p, "surf_noise_in")

              
def draw_effect_rust(self, context):
    obj = ownership.source_of(context.object)
    p = obj.project_damage
    layout = self.layout
    button = layout.row()
    button.scale_y = 1.4
    button.operator("project_damage.apply_rust", icon='SHADING_TEXTURE')
    column = layout.column()
    column.active = p.rust_enable and not p.all_off

    column.prop(p, "rust_mode")
    if p.rust_mode == 'PAINT':
        from project_damage import paint
        paint.drawing_row(column, p, 'RUST')
        column.prop(p, "surf_close_strokes")
        column.prop(p, "surf_paint_snap", text="Projection Distance (cm)")
    column.prop(p, "rust_materials")
    column.prop(p, "rust_intensity")
    column.prop(p, "rust_seed")
    column.separator()

    column.label(text="Spread")
    column.prop(p, "rust_edge_bias")
    column.prop(p, "rust_bottom_bias")
    column.prop(p, "rust_world_gravity")
    gravity = column.column()
    gravity.active = not p.rust_world_gravity
    gravity.prop(p, "rust_gravity")
    column.prop(p, "rust_noise")
    column.prop(p, "rust_noise_scale")
    quality_ui.draw(column, p, "rust_detail")
    column.separator()

    column.label(text="Metal")
    column.prop(p, "rust_crust_start")
    column.prop(p, "rust_crust_height")
    column.prop(p, "rust_crust_rough")
    column.prop(p, "rust_pits")
    pits = column.column()
    pits.active = p.rust_pits > 0
    pits.prop(p, "rust_pit_start")
    pits.prop(p, "rust_pit_size")
    pits.prop(p, "rust_pit_depth")
    column.prop(p, "rust_hole_start")
    holes = column.column()
    holes.active = p.rust_hole_start < 140
    holes.prop(p, "rust_hole_depth")
    holes.prop(p, "rust_flakes")
    holes.prop(p, "rust_flake_size")
    column.separator()

    column.label(text="Paint")
    column.prop(p, "rust_paint")
    paint = column.column()
    paint.active = p.rust_paint
    paint.prop(p, "rust_paint_start")
    paint.prop(p, "rust_band")
    paint.prop(p, "rust_paint_flakes")
    paint.prop(p, "rust_craze")
    paint.prop(p, "rust_craze_cell")
    paint.prop(p, "rust_blisters")
    paint.prop(p, "rust_blister_size")
    paint.label(text="Paint thickness: Coating Peeling > Thickness.", icon='INFO')
    column.separator()

    column.label(text="Streaks")
    column.prop(p, "rust_streak")
    column.prop(p, "rust_streak_length")
    column.label(text="Attributes: project_damage_rust, project_damage_rust_streak.", icon='INFO')

              
def draw_effect_heat(self, context):
    p = ownership.source_of(context.object).project_damage
    button = self.layout.row()
    button.scale_y = 1.4
    button.operator("project_damage.apply_heat", icon='MOD_DISPLACE')
    col = _tab(self.layout, p, "heat_enable")
    col.prop_search(p, "heat_group", ownership.source_of(context.object), "vertex_groups")
    for field in ("heat_seed", "heat_intensity", "heat_diffuse", "heat_spread", "heat_duration", "heat_steps", "heat_char_start", "heat_shrink_depth", "heat_detail", "heat_block_round", "heat_cell_min", "heat_cell_max", "heat_crack_cells", "heat_crack_width", "heat_crack_depth", "heat_blisters", "heat_blister_min", "heat_blister_max", "heat_blister_low", "heat_blister_high", "heat_blister_burst", "heat_char_layer"):
        col.prop(p, field)
    self.layout.label(text="Empty heat group = whole mesh heated.", icon='INFO')

                 
def draw_effect_coating(self, context):
    p = ownership.source_of(context.object).project_damage
    button = self.layout.row()
    button.scale_y = 1.4
    button.operator("project_damage.apply_coating", icon='MOD_SOLIDIFY')
    col = _tab(self.layout, p, "coating_enable")
    for field in ("coating_amount", "coating_thickness", "coating_patch_min",
                  "coating_patch_max", "coating_jagged", "coating_flakes",
                  "coating_curl", "coating_cracks"):
        col.prop(p, field)
    for field in ("coating_seed", "coating_satellites", "coating_stretch",
                  "coating_dir_x", "coating_dir_y", "coating_dir_z",
                  "coating_edge_wrap", "coating_flake_size", "coating_crack_length"):
        col.prop(p, field)
    col.prop(p, "coating_facing_only")
    facing = col.column()
    facing.active = p.coating_facing_only
    for field in ("coating_facing_x", "coating_facing_y", "coating_facing_z", "coating_facing_angle"):
        facing.prop(p, field)
    col.prop(p, "coating_zone_enable")
    zone = col.column()
    zone.active = p.coating_zone_enable
    from project_damage import paint
    paint.drawing_row(zone, p, 'COATING')
    zone.prop(p, "surf_close_strokes")
    zone.prop(p, "surf_paint_snap", text="Projection Distance (cm)")
    for field in ("coating_zone_smooth",
                  "coating_zone_inset", "coating_zone_detail"):
        zone.prop(p, field)
    col.prop(p, "coating_flake_side")
    for field in ("coating_flake_side_x", "coating_flake_side_y", "coating_flake_side_z"):
        col.prop(p, field)
    self.layout.label(text="Inline: peels into the result mesh.", icon='INFO')

                 
def draw_effect_buildup(self, context):
    p = ownership.source_of(context.object).project_damage
    button = self.layout.row()
    button.scale_y = 1.4
    button.operator("project_damage.apply_buildup", icon='MOD_FLUIDSIM')
    col = _tab(self.layout, p, "build_enable")
                                                                                      
                                                                          
    head = col.column()
    head.active = p.build_coverage > 0.0 or p.build_corner_prob > 0.0 or p.deposit_enable
    head.prop(p, "build_scale")
    quality_ui.draw(head, p, "build_detail")
    col.prop(p, "build_coverage")
    col.separator()
                                                                                    
                                                                 
    bead = col.column()
    bead.active = p.build_coverage > 0.0
    bead.label(text="Bead")
    bead.prop(p, "build_width")
    bead.prop(p, "build_height")
    bead.prop(p, "build_irreg")
    col.separator()
    piles = col.column()
    piles.label(text="Corner Piles")
    piles.prop(p, "build_corner_prob")
    on_piles = p.build_corner_prob > 0.0
    pile_fields = piles.column()
    pile_fields.active = on_piles                                                   
    pile_fields.prop(p, "build_corner_size")
    ease = piles.column()
                                                                                 
                                                            
    ease.active = on_piles
    ease.prop(p, "build_blend")
    col.separator()
    col.prop(p, "deposit_enable")
    deposits = col.column()
    deposits.active = p.deposit_enable
    for field in ("deposit_style", "deposit_amount", "deposit_size_min", "deposit_size_max",
                  "deposit_height", "deposit_gravity", "deposit_gravity_direction",
                  "deposit_shelter", "deposit_shelter_radius", "deposit_embed",
                  "deposit_layers", "deposit_porosity", "deposit_separate"):
        deposits.prop(p, field)
    deposits.label(text="Gravity is in source-local space.", icon='INFO')

                  
def draw_effect_fracture(self, context):
    p = ownership.source_of(context.object).project_damage
    col = _tab(self.layout, p, "fract_enable")
    type_box=col.box()
    procedural=type_box.row();procedural.active=p.fract_mode!='PAINT'
    procedural.use_property_split = False
    procedural.prop(p,'fract_type', text = ' ', expand = True)
    
    procedural=type_box.column();procedural.active=p.fract_mode!='PAINT'
    if p.fract_type=='RADIAL':
        procedural.prop(p,'fract_radial_spokes')
        procedural.label(text='Layer Amount controls the number of stars.',icon='INFO')
    elif p.fract_type=='GLASS':
        for field in ('fract_glass_spokes','fract_glass_rings','fract_glass_ring_fill'):
            procedural.prop(p,field)
        procedural.label(text='Layer Amount controls stars; rings use the next layer.',icon='INFO')
    elif p.fract_type=='WOOD':
        grain=procedural.column(align=True)
        for field in ('fract_grain_x','fract_grain_y','fract_grain_z','fract_grain_pull'):grain.prop(p,field)
    if p.fract_mode=='PAINT':type_box.label(text='Paint uses drawn paths; Type is procedural only.',icon='INFO')
    if getattr(self.layout, 'key', None) == 'fracture_through':
        box = col.box()
        box.prop(p, 'fract_through')
        sub = box.column()
        sub.active = p.fract_through
        sub.prop(p, 'fract_through_min_piece')
        sub.label(text='Depth controls penetration; Thickness Cap is ignored.', icon='INFO')
        from project_damage import separate_fragments
        separate_fragments.draw_controls(box, context)
        box.label(text='Alternative to Fracture; shares its settings.', icon='INFO')


                   
    box = col.box()
    box.use_property_split = False
    row = box.row()
    row.scale_y=1.5
    row.alignment = 'CENTER'
    row.prop(p, 'fract_mode', text = ' ', expand = True)



     
    painted = p.fract_mode == 'PAINT'
    if painted:
        from project_damage import paint
        paint.drawing_row(box, p, 'FRACTURE', 1)
        box.label(
            text='Draw Level 1; Levels 2 and 3 branch from it.',
            icon='INFO'
        )

    col.separator()

                    
    box = col.box()

    row = box.row()
    row.alignment = 'CENTER'
    row.label(text="Basic Settings")

    head = box.column(align=True)
    head.active = _fract_any_amount(p)
    head.prop(p, "fract_scale")
    quality_ui.draw(head, p, "fract_detail")

    col.separator()

                     
    for level in (1, 2, 3):
        box = col.box()

        row = box.row()
        row.alignment = 'CENTER'
        row.label(text="Level %d Fractures" % level)

        if painted:
            if level == 1:
                box.prop(p, 'fract_l1_paint_radius')
            else:
                box.prop(p, 'fract_l%d_amount' % level)
        else:
            box.prop(p, "fract_l%d_amount" % level)

                                                 
                                           
        amt_on = (
            (level == 1 or getattr(p, 'fract_l%d_amount' % level) > 0)
            if painted
            else getattr(p, "fract_l%d_amount" % level) > 0
        )

        sub = box.column(align=True)
        sub.active = amt_on

        if not painted:
            sub.prop(p, "fract_l%d_subdiv" % level)

        sub.prop(p, "fract_l%d_width" % level)
        sub.prop(p, "fract_l%d_depth" % level)
        if not painted or level>1:
            sub.prop(p,'fract_l%d_branches'%level)
            branch=sub.column();branch.active=getattr(p,'fract_l%d_branches'%level)>0
            branch.prop(p,'fract_l%d_branch_length'%level)
        if not painted:
            sub.prop(p, 'fract_l%d_length' % level)

        if painted:
            continue                                              

        sub.prop(p, "fract_l%d_bias" % level)
        sub.prop(p, "fract_l%d_chips" % level)

                                                            
        chipfields = sub.column(align=True)
        chipfields.active = getattr(p, "fract_l%d_chips" % level)

        chipfields.prop(p, "fract_l%d_chip_amount" % level)
        chipfields.prop(p, "fract_l%d_chip_size" % level)
        chipfields.prop(p, "fract_l%d_chip_depth" % level)

        col.separator()

                          
def draw_effect_fracture_realism(self, context):
    p = ownership.source_of(context.object).project_damage
    layout = self.layout
    column = layout.column()
    column.active = (p.fract_enable or p.fract_through_enable) and not p.all_off
    column.prop(p, "fract_recipe")
    cap = column.column()
    cap.active = p.fract_recipe == 'IMPROVED' and not (p.fract_through_enable and p.fract_through)
    cap.prop(p, "fract_thickness_cap")

                            
def draw_effect_fracture_proximity(self, context):
    p = ownership.source_of(context.object).project_damage
    layout = self.layout
    column = layout.column()
    column.active = (p.fract_enable or p.fract_through_enable) and not p.all_off
    column.active = column.active and p.fract_mode != 'PAINT'
    effective_mode=p.fract_prox_mode
    column.prop(p, "fract_prox_mode")
    points = column.column()
    points.active = effective_mode != 'OFF'
    points.prop(p, "fract_prox_collection")
    points.prop(p, "fract_prox_use_size")
    radius = points.column()
    radius.active = not p.fract_prox_use_size or p.fract_prox_collection is None
    radius.prop(p, "fract_prox_radius")
    column.separator()

    default = column.column()
    default.active = effective_mode == 'DEFAULT'
    default.prop(p, "fract_prox_strength")
    column.separator()

    radial = column.column()
    radial.active = effective_mode == 'RADIAL'
    radial.label(text="Radial")
    radial.prop(p, "fract_radial_spokes")
    radial.prop(p, "fract_radial_rings")
    radial.prop(p, "fract_radial_ring_fill")
    radial.prop(p, "fract_radial_crush")
    column.separator()

    glass = column.column()
    glass.active = effective_mode == 'GLASS'
    glass.label(text="Glass")
    glass.prop(p, "fract_glass_spokes")
    glass.prop(p, "fract_glass_branch")
    glass.prop(p, "fract_glass_rings")
    glass.prop(p, "fract_glass_ring_fill")
    glass.prop(p, "fract_glass_width")
    glass.prop(p, "fract_glass_depth")
    glass.prop(p, "fract_glass_crush")

                        
def draw_effect_fracture_noise(self, context):
    p = ownership.source_of(context.object).project_damage
    layout = self.layout
    layout.use_property_split = True
    layout.use_property_decorate = False
    col = layout.column()
    col.active = (p.fract_enable or p.fract_through_enable) and not p.all_off and _fract_any_amount(p) and p.fract_mode != 'PAINT'
    col.prop(p, "fract_disp_strength")
    col.prop(p, "fract_disp_scale")

                          
def draw_effect_fracture_options(self, context):
    p = ownership.source_of(context.object).project_damage
    layout = self.layout
    layout.use_property_split = True
    layout.use_property_decorate = False
    col = layout.column()
    col.active = (p.fract_enable or p.fract_through_enable) and not p.all_off
                                                                   
                                                                         
                                                   
    tile = col.column()
    tile.active = _fract_any_amount(p) and p.fract_mode != 'PAINT'
    tile.label(text="Tile Offset")
    tile.prop(p, "fract_tile_offset")
    tile.prop(p, "fract_tile_seed")
    col.separator()
    mult = col.column()
    mult.active = _fract_any_amount(p)                                    
    mult.prop(p, "fract_depth_mult")
                                                                    
                                                                    
    col.prop(p, "fract_rim_soft")
                                                                          
                                                                       
                                                     
    col.separator()
    guides = col.column()
    guides.active = p.fract_mode != 'PAINT'
    guides.label(text="Guide Splines (Procedural)")
    row = guides.row()
    row.template_list("PROJECT_DAMAGE_UL_guides", "", p, "guides", p, "guide_index")
    side = row.column(align=True)
    side.operator("project_damage.guide_add", icon='ADD', text="")
    side.operator("project_damage.guide_remove", icon='REMOVE', text="")

                 
def draw_effect_corners(self, context):
    p = ownership.source_of(context.object).project_damage
    col = _tab(self.layout, p, "corner_enable")

                                                           
    on = p.corner_prob > 0.0


                    
    box = col.box()

    row = box.row()
    row.alignment = 'CENTER'
    row.label(text="Basic Settings")

    head = box.column(align=True)
    head.active = on
    head.prop(p, "corner_scale")
    quality_ui.draw(head, p, "corner_detail")

    box.prop(p, "corner_prob")

    tail = box.column()
    tail.active = on
    tail.prop(p, "corner_min_edges")

    col.separator()


           
    box = col.box()

    row = box.row()
    row.alignment = 'CENTER'
    row.label(text="Shape")

    size = box.column(align=True)
    size.active = on
    size.prop(p, "corner_size_min")
    size.prop(p, "corner_size_max")

    depth = box.column(align=True)
    depth.active = on
    depth.prop(p, "corner_depth_min")
    depth.prop(p, "corner_depth_max")

                                       
    box.prop(p, "corner_rim_soft")

    col.separator()


               
    box = col.box()

    row = box.row()
    row.alignment = 'CENTER'
    row.label(text="Secondary Craters")

    sec = box.column()
    sec.active = on                                                     
    sec.prop(p, "corner_sec_amount")

    secfields = sec.column()
    secfields.active = on and p.corner_sec_amount > 0                   
    secfields.prop(p, "corner_sec_size")
    secfields.prop(p, "corner_sec_depth")

                  
def draw_effect_breakers(self, context):
    p = ownership.source_of(context.object).project_damage
    layout = self.layout
    layout.use_property_split = True
    layout.use_property_decorate = False
    from project_damage import boolean_break, panels
    boolean_break.draw_breaker_list(layout, p, context)
    panels.draw_linked_break_surface(layout, p, context, boolean_options=False)

    linked = _has_linked_breaker(p)
    layout.separator()
                                                                                 
                                                                                           
                                                                   
    live = layout.column()
    live.active = linked
    live.prop(p, "break_rim_enable")
    live.prop(p, "break_scale")
                                                                               
                                                            
    rim = layout.column()
    rim.active = linked and not p.all_off and p.break_rim_enable
    rim.prop(p, "break_surf_dmg")
    rim.prop(p, "break_rim_amount")
                                                                         
    size = rim.column()
    size.active = p.break_rim_amount > 0
    size.prop(p, "break_rim_size")
    size.prop(p, "break_rim_depth")
    from project_damage import displacement_shader
    displacement_shader.draw(layout, p)

               
def draw_effect_rebar(self, context):
    p = ownership.source_of(context.object).project_damage
    layout = self.layout
    layout.use_property_split = True
    layout.use_property_decorate = False
    layout.active = p.rebar_enable
    layout.prop(p, "rebar_preset")
    from project_damage import rebar
    rebar.draw_fill_type(layout,p)
    if p.rebar_fill!='0':
        rebar.draw_fill(layout,p)
        return
    layout.prop(p,"rebar_material")
    layout.prop(p,'rebar_mode')
    layout.prop(p,'rebar_fit')
    if p.rebar_mode=='1':layout.prop(p,'rebar_deform')
    layout.prop(p,'rebar_reach')
    layout.prop(p,'rebar_core')
    layout.prop(p,'rebar_show_all')
    from project_damage import paint
    ink=layout.box();ink.label(text='Hand-Drawn Bars',icon='GREASEPENCIL')
    paint.drawing_row(ink,p,'REBAR')
    ink.prop(p,'rebar_stroke_radius');ink.prop(p,'rebar_stroke_embed')
    layout.prop(p, "rebar_type")
    layout.prop(p, "rebar_layout")
    layout.prop(p, "rebar_seed")

    col = layout.column(heading="Layout")
    from project_damage import break_surface
    break_surface.draw_rebar_axes(col, p)
    cage = p.rebar_layout in ('2', '3')
    grid = not cage
    sub = col.column()
    sub.active = p.rebar_layout == '0'
    sub.prop(p, "rebar_side")
    sub = col.column()
    sub.active = grid
    sub.prop(p, "rebar_angle")
    sub.prop(p, "rebar_outer")
    row = sub.row(align=True)
    row.prop(p, "rebar_bars_u", toggle=True)
    row.prop(p, "rebar_bars_v", toggle=True)
    col.prop(p, "rebar_spacing_u", text="Bar Spacing" if cage else "Spacing U")
    sub = col.column()
    sub.active = grid
    sub.prop(p, "rebar_spacing_v")
    col.prop(p, "rebar_cover")
    col.prop(p, "rebar_amount")
    col.prop(p, "rebar_jitter")
    sub = col.column()
    sub.active = cage
    sub.prop(p, "rebar_tie_spacing")
    sub.prop(p, "rebar_tie_radius")
    s2 = sub.column()
    s2.active = p.rebar_layout == '3'
    s2.prop(p, "rebar_spiral")

    col = layout.column(heading="Bar")
    col.prop(p, "rebar_radius")
    col.prop(p, "rebar_scale")
    sub = col.column()
    sub.active = p.rebar_type in ('0', '1')
    sub.prop(p, "rebar_rib_height")
    sub.prop(p, "rebar_rib_pitch")
    sub = col.column()
    sub.active = p.rebar_type in ('3', '4')
    sub.prop(p, "rebar_twist_pitch")
    col.prop(p, "rebar_sides")
    quality_ui.draw(col, p, "rebar_detail")

    col = layout.column(heading="Exposure")
    col.prop(p, "rebar_embed")
    col.prop(p, "rebar_min_exposed")
    col.prop(p, "rebar_drop_loose")

    col = layout.column(heading="Damage")
    col.prop(p, "rebar_wobble")
    col.prop(p, "rebar_bow")
    col.prop(p, "rebar_bend")
    col.prop(p, "rebar_broken")
    col.prop(p, "rebar_trim")
    col.prop(p, "rebar_neck")

                 
def draw_effect_breaker(self, context):
    br = ownership.source_of(context.object)
    bp = br.project_damage_breaker
    layout = self.layout
    layout.use_property_split = True
    layout.use_property_decorate = False
    layout.prop(bp, "enabled")

                                                                           
    mode_col = layout.column()
    mode_col.active = breaker_mod.effective_mode(br) != 3
    mode_col.prop(bp, "mode")
    layout.prop(bp,'cutter')
    if breaker_mod.effective_mode(br)==3:
        layout.label(text="Custom cutter",icon='MOD_BOOLEAN')
        layout.prop(bp,'custom_sheet')
    layout.prop(bp,'rebar')

    eff = breaker_mod.effective_mode(br)
    PLANE, SPHERE, STRIP = 0, 1, 2
                                                                         
                                              
    sheet = eff in (PLANE, STRIP)
    flip_col = layout.column()
    flip_col.active = sheet or eff == 3
    flip_col.prop(bp, "flip")
    layout.prop(bp, "seed")
    layout.separator()

    layout.label(text="Size")
    size = layout.column()
    lw = size.column()
    lw.active = sheet
    lw.prop(bp, "length")
    lw.prop(bp, "width")
    th = size.column()
    th.active = eff == STRIP or (eff == 3 and bp.custom_sheet == "1")
    th.prop(bp, "thickness")
    rad = size.column()
    rad.active = eff == SPHERE
    rad.prop(bp, "radius")
    quality_ui.draw(size, bp, "resolution")                                       
    layout.separator()

    layout.label(text="Surface Noise")
    noise = layout.column()
                                                                            
    noise.prop(bp, "strength")
    noise.prop(bp, "macro_h")
    noise.prop(bp, "turb_h")
    noise.prop(bp, "fine_h")
    noise.prop(bp, "noise_scale")
    layout.separator()

    layout.label(text="Edge Break-up")
    edge = layout.column()
    edgefields = edge.column()
    edgefields.active = sheet
    edgefields.prop(bp, "rim_amp")
    edgefields.prop(bp, "rim_scale")


                                                                              
                                                                          
                                                                              

EFFECT_GROUPS = {'fabric': (('General', ('enable', 'seed')),
            ('Sheet', ('thickness', 'skin_depth', 'cut_margin', 'detail', 'avoid')),
            ('Interior tears (holes)',
             ('holes_enable',
              'hole_amount',
              'hole_length_min',
              'hole_length_max',
              'hole_width_min',
              'hole_width_max',
              'hole_jagged',
              'hole_lobes',
              'hole_meander',
              'hole_split_max',
              'hole_bridge',
              'hole_dir_mode',
              'hole_dir_x',
              'hole_dir_y',
              'hole_dir_z',
              'hole_dir_jitter')),
            ('Edge tears (slits from an open sheet edge)',
             ('edge_enable',
              'edge_amount',
              'edge_spacing_jitter',
              'edge_length_min',
              'edge_length_max',
              'edge_width_min',
              'edge_width_max',
              'edge_taper',
              'edge_jagged',
              'edge_meander',
              'edge_angle_jitter')),
            ('Threads (master switch + per-kind toggles)',
             ('threads_enable',
              'thread_radius',
              'thread_sides',
              'thread_spacing',
              'bridge_enable',
              'bridge_share',
              'thread_sag',
              'thread_skew',
              'thread_wobble',
              'broken_enable',
              'broken_share',
              'thread_droop',
              'thread_curl',
              'thread_taper',
              'fray_enable',
              'fray_spacing',
              'fray_length_min',
              'fray_length_max',
              'hang_enable',
              'hang_amount',
              'hang_length_min',
              'hang_length_max')),
            ('Grease pencil drawing (closed contours, same input as paint damage)',
             ('paint_holes_enable',
              'paint_edges_enable',
              'paint_only',
              'paint_jagged',
              'paint_rag_size',
              'paint_edge_spacing',
              'paint_edge_length_min',
              'paint_edge_length_max',
              'paint_edge_width_min',
              'paint_edge_width_max',
              'paint_hang_spacing',
              'gravity_x',
              'gravity_y',
              'gravity_z',
              'max_threads'))),
 'concrete': (('General', ('enable', 'seed', 'mode', 'avoid', 'detail')),
              ('Automatic placement',
               ('amount', 'size_min', 'size_max', 'lobes', 'stretch', 'edge_bias')),
              ('Paint mode', ('paint_radius', 'paint_snap', 'paint_close')),
              ('Spall shape',
               ('depth_min',
                'depth_max',
                'jagged',
                'outline_scale',
                'terraces',
                'terrace_skin',
                'terrace_inset',
                'wall_soft',
                'roughness',
                'aggregate_size',
                'thickness_cap')),
              ('Hole (through the mesh)',
               ('hole_enable', 'hole_chance', 'hole_size', 'hole_jagged')),
              ('Cracks around the spall',
               ('crack_enable',
                'crack_amount',
                'crack_length_min',
                'crack_length_max',
                'crack_width',
                'crack_depth',
                'crack_meander',
                'crack_branch',
                'crack_interior')),
              ('Reinforcement bars',
               ('rebar_enable',
                'rebar_amount',
                'rebar_density',
                'rebar_radius',
                'rebar_scale',
                'rebar_spacing_x',
                'rebar_spacing_y',
                'rebar_bars_x',
                'rebar_bars_y',
                'rebar_outer',
                'rebar_angle',
                'rebar_cover',
                'rebar_expose',
                'rebar_embed',
                'rebar_ribs',
                'rebar_sides',
                'rebar_jitter',
                'rebar_wobble',
                'rebar_bend',
                'rebar_broken',
                'up_x',
                'up_y',
                'up_z'))),
 'hit': (('General', ('enable', 'seed', 'avoid', 'detail')),
         ('Placement / proximity', ('aim', 'radius_scale', 'proximity', 'prox_shrink', 'oblique')),
         ('Surface layer (paint / plaster skin that shatters into plates)', ('layer_thickness',)),
         ('Crater core',
          ('core_size',
           'core_depth',
           'core_jagged',
           'roughness',
           'aggregate_size',
           'thickness_cap',
           'hole_enable',
           'hole_size')),
         ('Plates (shattered skin around the crater)',
          ('plate_size', 'plate_growth', 'zone_jagged', 'shatter', 'tilt', 'edge_jagged', 'chip')),
         ('Cracks',
          ('crack_enable',
           'crack_width',
           'crack_depth',
           'radial_count',
           'radial_length_min',
           'radial_length_max',
           'radial_meander',
           'radial_branch',
           'ring_count',
           'ring_fill'))),
 'plaster': (('General', ('enable', 'seed', 'avoid')),
             ('Resolution', ('edge_length', 'max_tris', 'detail')),
             ('Master', ('strength', 'sink', 'thickness_cap')),
             ('Base coat', ('undulation', 'undulation_scale')),
             ('Trowel strokes',
              ('coats',
               'coverage',
               'length_min',
               'length_max',
               'width_min',
               'width_max',
               'angle',
               'angle_random',
               'cross_chance',
               'curve',
               'flatten',
               'tilt',
               'jagged',
               'streaks')),
             ('Ridges (the squeezed-out lip at a stroke rim)',
              ('ridge_height', 'ridge_width', 'ridge_sharp', 'ridge_trail')),
             ('Grain (sand / stipple)', ('grain', 'grain_size', 'pits')),
             ('Hairline cracks',
              ('crack_enable',
               'crack_amount',
               'crack_scale',
               'crack_width',
               'crack_depth',
               'crack_meander',
               'crack_breakup',
               'crack_step',
               'crack_lip',
               'up_x',
               'up_y',
               'up_z'))),
 'wood': (('General', ('enable', 'seed', 'mode', 'avoid', 'detail')),
          ('Grain direction', ('grain_mode', 'grain_x', 'grain_y', 'grain_z')),
          ('Automatic placement',
           ('amount', 'size_min', 'size_max', 'lobes', 'stretch', 'end_bias', 'edge_bias')),
          ('Paint mode', ('paint_radius', 'paint_snap', 'paint_close')),
          ('Torn-out wood',
           ('depth_min',
            'depth_max',
            'layers',
            'layer_inset',
            'fiber_depth_var',
            'thickness_cap',
            'jagged')),
          ('Splinters (fibre strips with pointed ends)',
           ('fiber_width',
            'fiber_jitter',
            'splinter_length',
            'splinter_sharp',
            'splinter_chunk',
            'splinter_ramp',
            'floor_fibers')),
          ('Weathered grain grooves',
           ('groove_enable',
            'groove_spacing',
            'groove_depth',
            'groove_wave',
            'groove_breakup',
            'weather_halo')),
          ('Break through (broken boards)', ('break_enable', 'break_chance', 'break_size')),
          ('Splits / checks (cracks along the grain)',
           ('check_enable',
            'check_amount',
            'check_global',
            'check_length_min',
            'check_length_max',
            'check_width',
            'check_depth',
            'check_meander')),
          ('Knots', ('knot_amount', 'knot_size', 'knot_stretch', 'knot_depth', 'knot_checks')),
          ('Loose splinters (solids added back)',
           ('sliver_enable',
            'sliver_amount',
            'sliver_length',
            'sliver_width',
            'sliver_thick',
            'sliver_lift',
            'sliver_embed'))),
 'lath': (('General', ('enable', 'seed', 'mode', 'avoid', 'detail')),
          ('Automatic placement',
           ('amount', 'size_min', 'size_max', 'lobes', 'stretch', 'edge_bias')),
          ('Paint mode', ('paint_radius', 'paint_snap', 'paint_close')),
          ('Plaster',
           ('plaster_min',
            'plaster_max',
            'jagged',
            'outline_scale',
            'crumble',
            'crumble_size',
            'terraces',
            'terrace_skin',
            'terrace_inset',
            'wall_soft',
            'roughness',
            'thickness_cap')),
          ('Cavity behind the laths', ('cavity_depth', 'keys', 'key_size')),
          ('Hole (through the mesh)',
           ('hole_enable', 'hole_chance', 'hole_size', 'hole_jagged', 'hole_laths')),
          ('Plaster cracks around the patch',
           ('crack_enable',
            'crack_amount',
            'crack_length_min',
            'crack_length_max',
            'crack_width',
            'crack_depth',
            'crack_meander',
            'crack_branch',
            'crack_interior')),
          ('Laths',
           ('lath_enable',
            'lath_amount',
            'lath_width',
            'lath_thickness',
            'lath_gap',
            'lath_scale',
            'lath_angle',
            'lath_length',
            'lath_embed',
            'lath_jitter',
            'lath_tilt',
            'lath_warp',
            'lath_edge',
            'lath_missing',
            'lath_broken',
            'lath_bend',
            'lath_splinter',
            'up_x',
            'up_y',
            'up_z')),
          ('Smoothing and optimization', ('cavity_smooth', 'rim_smooth', 'lath_optimize')),
          ('Clean walls and holes',
           ('wall_slope', 'cavity_slope', 'hole_noise_scale', 'hole_detail'))),
 'wallpaper': (('General', ('enable', 'seed')),
               ('Paper and strips',
                ('thickness',
                 'strip_width',
                 'seam_offset',
                 'seam_dir_x',
                 'seam_dir_y',
                 'seam_dir_z',
                 'wall_angle',
                 'detail',
                 'avoid')),
               ('Seams', ('seams_enable', 'seam_width', 'seam_depth', 'seam_open')),
               ('Torn strips (automatic placement)',
                ('tears_enable',
                 'tear_amount',
                 'tear_length_min',
                 'tear_length_max',
                 'tear_width_min',
                 'tear_width_max',
                 'tear_diagonal',
                 'tear_from_top',
                 'tear_side',
                 'tear_jagged',
                 'seam_crack',
                 'seam_crack_length')),
               ('Peeled flaps',
                ('flaps_enable',
                 'flap_amount',
                 'flap_fold',
                 'flap_fold_radius',
                 'flap_curl',
                 'flap_curl_focus',
                 'flap_cone',
                 'flap_wave',
                 'flap_ragged')),
               ('Lifted seam edges',
                ('lifts_enable',
                 'lift_amount',
                 'lift_length_min',
                 'lift_length_max',
                 'lift_width_min',
                 'lift_width_max',
                 'lift_curl')),
               ('Grease pencil', ('paint_enable', 'paint_reach', 'paint_jagged', 'seam_snap'))),
 'bullet': (('General', ('enable', 'seed', 'mode', 'material', 'avoid', 'detail')),
            ('Size and penetration (all materials)',
             ('size_min',
              'size_max',
              'size_bias',
              'penetration',
              'penetration_var',
              'thickness_cap')),
            ('Shot direction', ('shot_mode', 'oblique', 'shot_x', 'shot_y', 'shot_z', 'elongate')),
            ('Automatic placement',
             ('amount', 'clusters', 'cluster_share', 'cluster_spread', 'min_spacing')),
            ('Paint mode',
             ('paint_spacing', 'paint_spread', 'paint_snap', 'paint_close', 'paint_single')),
            ('Halo (rust / scorch ring)',
             ('halo_enable', 'halo_scale', 'halo_depth', 'halo_jagged')),
            ('Pits (spatter dings / pock marks around each impact)',
             ('pit_amount', 'pit_size', 'pit_spread')),
            ('Metal',
             ('metal_hole_jagged',
              'metal_dish',
              'metal_dish_width',
              'metal_lip_height',
              'metal_lip_width',
              'metal_petals',
              'metal_petal_tear',
              'metal_curl',
              'metal_exit_burr',
              'metal_dent_chance')),
            ('Wood',
             ('grain_mode',
              'grain_x',
              'grain_y',
              'grain_z',
              'wood_crush',
              'wood_splinter',
              'wood_fiber_width',
              'wood_exit_scale',
              'wood_exit_depth',
              'wood_slivers',
              'wood_sliver_length',
              'wood_split_chance',
              'wood_split_length')),
            ('Concrete / stone',
             ('conc_spall_chance',
              'conc_spall_scale',
              'conc_spall_depth',
              'conc_skin_depth',
              'conc_bore_depth',
              'conc_jagged',
              'conc_roughness',
              'conc_aggregate',
              'conc_exit_scale',
              'conc_cracks',
              'conc_crack_length',
              'crack_width',
              'crack_depth'))),
 'blast': (('General', ('enable', 'seed', 'avoid', 'detail')),
           ('Placement / proximity',
            ('radius_scale', 'proximity', 'prox_shrink', 'jagged', 'noise_scale')),
           ('Building layout', ('floor_height', 'floor_offset', 'slab_thickness', 'wall_depth')),
           ("Layers, as normalized blast distance (1 = the empty's sphere)",
            ('core_size',
             'room_size',
             'room_facets',
             'partition_spacing',
             'partition_thickness',
             'slab_break',
             'slab_droop',
             'wall_size',
             'fringe',
             'fringe_density',
             'spall_reach',
             'spall_cover')),
           ('Masonry',
            ('brick_length',
             'brick_height',
             'joint_width',
             'joint_depth',
             'brick_jitter',
             'plaster_depth')),
           ('Cracks',
            ('crack_enable', 'crack_count', 'crack_length', 'crack_width', 'crack_depth'))),
 'ground': (('General',
             ('enable', 'seed', 'mode', 'avoid', 'detail', 'up_x', 'up_y', 'up_z', 'max_slope')),
            ('Automatic placement',
             ('amount', 'size_min', 'size_max', 'area_chance', 'area_size', 'area_stretch')),
            ('Paint mode', ('paint_snap', 'paint_close', 'paint_jitter', 'paint_branch')),
            ('Crack network',
             ('crack_enable',
              'crack_amount',
              'crack_generations',
              'crack_length_min',
              'crack_length_max',
              'crack_width',
              'crack_width_var',
              'crack_taper',
              'crack_depth',
              'crack_meander',
              'crack_kink',
              'crack_branch',
              'crack_join',
              'crack_chip')),
            ('Damaged area: alligator plates',
             ('area_jagged',
              'rim_fade',
              'plate_size',
              'plate_center_scale',
              'plate_jagged',
              'gap_width',
              'gap_depth',
              'plate_sink',
              'plate_tilt')),
            ('Pothole: missing plates',
             ('pothole_chance',
              'pothole_size',
              'pothole_loose',
              'pothole_depth',
              'roughness',
              'aggregate_size',
              'thickness_cap')),
            ('Rubble: loose chunks lying in the pothole',
             ('rubble_enable', 'rubble_amount', 'rubble_size_min', 'rubble_size_max')),
            ('Heave: one side of a crack lifted and tilted up (ref 9)',
             ('heave_chance', 'heave_height', 'heave_width', 'heave_gap'))),
 'brick': (('Brick construction', ('brick_mode',)),
           ('Style', ('style',)),
           ('General', ('enable', 'seed', 'mode', 'avoid', 'detail')),
           ('Automatic placement',
            ('amount', 'size_min', 'size_max', 'lobes', 'stretch', 'edge_bias')),
           ('Paint mode', ('paint_radius', 'paint_snap', 'paint_close')),
           ('Render (the coat over the bricks)',
            ('render_min',
             'render_max',
             'jagged',
             'outline_scale',
             'crumble',
             'crumble_size',
             'terraces',
             'terrace_skin',
             'terrace_inset',
             'wall_soft',
             'roughness',
             'render_stick',
             'thickness_cap')),
           ('Bricks: size, joints, bond',
            ('bond',
             'brick_length',
             'brick_height',
             'brick_width',
             'mortar',
             'mortar_depth',
             'brick_scale',
             'bond_offset')),
           ('Brick surface',
            ('face_jitter', 'face_tilt', 'brick_rough', 'chip', 'chip_size', 'joint_rough')),
           ('Knocked-out bricks',
            ('layers',
             'break_size',
             'layer_inset',
             'break_jagged',
             'missing',
             'broken',
             'broken_rough',
             'collar_fill')),
           ('Loose bricks', ('loose', 'loose_depth', 'loose_tilt', 'loose_out', 'loose_out_dist')),
           ('Hole (through the mesh)', ('hole_enable', 'hole_chance', 'hole_size')),
           ('Render cracks around the patch',
            ('crack_enable',
             'crack_amount',
             'crack_length_min',
             'crack_length_max',
             'crack_width',
             'crack_depth',
             'crack_meander',
             'crack_branch',
             'crack_interior',
             'up_x',
             'up_y',
             'up_z')),
           ('Material Regions', ('split_materials',)),
           ('Render edge', ('sharp_edge',)),
           ('Behind the brick layers', ('behind', 'damage_depth', 'damage_noise', 'damage_scale')),
           ('Corners, joints and topology', ('corners', 'joint_fill', 'remesh', 'remesh_size'))),
 'glass': (('Thickness and openings', ('auto_hole', 'auto_hole_thickness')),
           ('General', ('enable', 'seed')),
           ('Pane', ('thickness', 'skin_depth', 'cut_margin', 'detail', 'avoid')),
           ('Automatic impacts',
            ('impact_enable',
             'impact_amount',
             'hole_chance',
             'hole_size_min',
             'hole_size_max',
             'hole_lobes',
             'impact_spacing')),
           ('Shards along the opening',
            ('radial_min',
             'radial_max',
             'radial_jitter',
             'spike_chance',
             'spike_length',
             'spike_width',
             'spike_split',
             'spike_lean',
             'notch_jitter',
             'blowout',
             'blowout_reach',
             'edge_kink',
             'shard_crack')),
           ('Cracks (grooves cut into the faces; the pane stays one piece)',
            ('crack_enable',
             'crack_width',
             'crack_depth',
             'crack_both_sides',
             'crack_reach',
             'crack_reach_jitter',
             'crack_meander',
             'crack_branch',
             'crack_taper',
             'crush_size',
             'ring_count',
             'ring_first',
             'ring_growth',
             'ring_fill',
             'ring_jitter',
             'ring_bow')),
           ('Conchoidal chips on the exit face along the opening',
            ('chip_enable', 'chip_chance', 'chip_spacing', 'chip_size', 'chip_depth')),
           ('Grease pencil drawing (same contour input as paint damage)',
            ('paint_holes_enable',
             'paint_impacts_enable',
             'paint_only',
             'paint_shard_width',
             'paint_hole_cracks',
             'paint_point_size',
             'paint_impact_size',
             'paint_point_hole',
             'max_cracks'))),
 'fissure': (('General', ('enable', 'seed', 'avoid', 'detail', 'up_x', 'up_y', 'up_z')),
             ('The main fissure',
              ('width',
               'width_random',
               'depth',
               'depth_random',
               'taper',
               'jagged',
               'edge_scale',
               'wall_lean',
               'wall_rough',
               'floor_shape',
               'thickness_cap')),
             ('Tilted slabs along the rims',
              ('slab_enable',
               'slab_rows',
               'slab_length_min',
               'slab_length_max',
               'slab_width_min',
               'slab_width_max',
               'slab_row_falloff',
               'slab_thick_min',
               'slab_thick_max',
               'tilt_min',
               'tilt_max',
               'sink_chance',
               'roll',
               'flat_chance',
               'side_bias',
               'slab_gap',
               'slab_jagged',
               'slab_skew')),
             ('Side branches',
              ('branch_enable',
               'branch_density',
               'branch_length_min',
               'branch_length_max',
               'branch_width',
               'branch_depth',
               'branch_meander',
               'branch_angle',
               'branch_fork')),
             ('Rubble',
              ('rubble_enable',
               'rubble_density',
               'rubble_size_min',
               'rubble_size_max',
               'rubble_wedged'))),
 'bevel': (('General', ('enable', 'seed', 'avoid', 'detail')),
           ('Which edges',
            ('angle',
             'drawn_only',
             'paint_radius',
             'paint_align',
             'paint_cover',
             'paint_partial',
             'paint_taper',
             'max_turn')),
           ('Profile',
            ('profile',
             'roundness',
             'fillet_top',
             'fillet_side',
             'size_mode',
             'size_top',
             'size_side',
             'max_share',
             'taper',
             'taper_flip',
             'round_corners')),
           ('Edge wear layer',
            ('wear_enable',
             'wear_uneven',
             'wear_uneven_scale',
             'wear_rough',
             'wear_chips',
             'wear_chip_min',
             'wear_chip_max',
             'wear_chip_depth',
             'wear_chip_stretch',
             'wear_big',
             'wear_mask_width',
             'wear_mask_noise',
             'wear_mask_scale',
             'up_x',
             'up_y',
             'up_z'))),
 'scratch': (('Stroke interpretation', ('mode',)),
             ('Size', ('size_scale',)),
             ('General', ('enable', 'seed', 'style', 'avoid', 'detail')),
             ('Placement around the strokes',
              ('density',
               'max_scratches',
               'spread',
               'falloff',
               'angle_jitter',
               'cross_share',
               'along',
               'paint_snap')),
             ('Shape',
              ('length_min',
               'length_max',
               'length_bias',
               'width',
               'width_var',
               'depth',
               'depth_var',
               'profile',
               'taper',
               'gouge',
               'curve',
               'wobble',
               'gaps',
               'wrap_angle')),
             ('Bundles (several parallel lines dragged together, like grit)',
              ('bundle_chance', 'bundle_min', 'bundle_max', 'bundle_gap', 'bundle_thin')),
             ('Scuff band (paper-thin cut that only carries a selection)',
              ('scuff_enable', 'scuff_width', 'scuff_depth')),
             ('Float mask', ('mask_falloff',))),
 'metal': (('General', ('enable', 'seed', 'avoid', 'style')),
           ('Resolution', ('edge_length', 'detail', 'max_tris')),
           ('Contact', ('depth_scale', 'clearance', 'proximity', 'proximity_depth')),
           ('Dent shape', ('falloff', 'falloff_min', 'radius_max', 'profile', 'bulge')),
           ('Sheet vs solid', ('sheet_thickness', 'thickness_cap')),
           ('Creases (radial buckles in the dent walls)',
            ('crease', 'crease_spacing', 'crease_length', 'crease_sharp', 'crease_branch')),
           ('Crumple (small facets on the most deformed metal)', ('crumple', 'crumple_scale')),
           ('Grain', ('roughness',)),
           ('Placement / proximity', ('crush_aim', 'crush_proximity')),
           ('Crush', ('crush_depth', 'crush_length', 'crush_hinge', 'crush_skew')),
           ('Accordion folds (the length the crush took out buckles up)',
            ('fold_size', 'fold', 'fold_sharp', 'fold_wander', 'fold_lift')),
           ('Crumpled facets', ('crush_crumple', 'crush_facet'))),
 'debris': (('Debris extraction',
             ('seed', 'min_volume', 'min_thickness', 'chunk_size', 'chunk_jitter', 'max_pieces')),
            ('Inner surface UV', ('inner_uv_scale',)),
            ('Shading', ('smooth_angle',)))}

_EFFECT_LABELS = {
    'debris': 'Damage Debris',
    'bevel': 'Edge Bevel / Wear', 'scratch': 'Scratches Draw', 'metal': 'Metal Deform',
    'fabric': 'Fabric Tears', 'concrete': 'Concrete Damage', 'hit': 'Concrete Hit',
    'plaster': 'Plaster Displacement', 'wood': 'Wood Damage',
    'lath': 'Lath Damage', 'wallpaper': 'Wallpaper',
    'bullet': 'Bullet Impacts', 'blast': 'Building Destruction',
    'ground': 'Ground Cracks', 'brick': 'Brick Damage',
    'glass': 'Broken Glass', 'fissure': 'Ground Fissures',
}



def _effect_draw(self, context):
    p = ownership.source_of(context.object).project_damage
    module = self.effect_module
    layout = self.layout
    layout.use_property_split = True
    layout.use_property_decorate = False
    col = layout.column()
    col.enabled = not p.all_off
    if module in {'hit','glass'} and getattr(layout,'key',None)==module+'_through':
        box=col.box();box.prop(p,module+'_through')
        sub=box.column(align=True);sub.active=getattr(p,module+'_through')
        fields=('through_gap','through_max_wall','chunk_depth','through_remove','through_min_piece') if module=='hit' else ('through_gap','through_rings','through_fall','through_min_piece')
        for field in fields:sub.prop(p,module+'_'+field)
        from project_damage import separate_fragments
        separate_fragments.draw_controls(box,context)
        box.label(text='Alternative asset; shares the original settings.',icon='INFO')
    if module=='debris':
        col.label(text='Enable a cutting effect first. Source − result becomes debris.',icon='INFO')
        col.operator('project_damage.generate_debris',text='Generate / Refresh Debris',icon='FILE_REFRESH')
    op = col.operator('project_damage.apply_effect', text='Apply ' + _EFFECT_LABELS[module], icon='MOD_DISPLACE')
    op.module = module
    from project_damage import material_controls
    mat_module=module+'_through' if module in {'hit','glass'} and getattr(layout,'key',None)==module+'_through' else module
    material_controls.draw(col, p, mat_module)
    if module in {'bevel','scratch'}:
        from project_damage.paint import drawing_row
        drawing_row(col,p,module.upper())
        col.prop(p,'bevel_preset' if module=='bevel' else 'scratch_style')
        if module=='scratch':
            col.label(text='Custom: editable shape settings')
        else:
            col.operator('project_damage.recipe_defaults').module=module
        col.label(text='Empty drawing: all sharp edges' if module=='bevel' else 'Draw strokes on the surface')
    if module == 'metal':
        col.prop(p,'metal_style')
        col.prop(p,'metal_collection' if p.metal_style=='0' else 'metal_crush_collection')
        if p.metal_style=='1': col.operator('project_damage.add_hit',text='Add Crush Empty',icon='ADD').module='metal_crush'
        col.label(text='Preserves source face materials and UVs')
    if module == 'hit':
        col.prop(p, 'hit_collection')
        col.operator('project_damage.add_hit', icon='ADD')
        col.label(text='Move impact empties near the surface')
    if module=='glass':
        from project_damage import input_prep
        col.prop(input_prep.settings(ownership.source_of(context.object)),'thickness_logic')
    if module == 'blast':
        col.prop(p, 'blast_collection')
        col.operator('project_damage.add_hit', text='Add Blast Empty', icon='ADD').module = 'blast'
        col.label(text='Local +Z is the building up axis')
    if module == 'bullet':
        col.prop(p, 'bullet_material')
        col.operator('project_damage.bullet_preset')
    if module in {'concrete', 'wood', 'lath', 'bullet', 'ground', 'brick'}:
        col.prop(p, module + '_mode', text='Placement')
        if int(getattr(p, module + '_mode')) == 1:
            from project_damage.paint import drawing_row
            drawing_row(col, p, module.upper())
    if module == 'fissure':
        from project_damage.paint import drawing_row
        drawing_row(col, p, 'FISSURE')
        col.label(text='Draw a path above the ground; paint only')
    if module == 'glass':
        from project_damage.paint import drawing_row
        col.label(text='Closed openings')
        drawing_row(col, p, 'GLASS_HOLES')
        col.label(text='Impact dots / crack strokes')
        drawing_row(col, p, 'GLASS_IMPACTS')
    if module == 'wallpaper':
        from project_damage.paint import drawing_row
        drawing_row(col, p, 'WALLPAPER')
        materials = col.box()
        material_controls.centered_heading(materials,'Materials',icon='MATERIAL')
        materials.prop(p, 'wallpaper_material')
        materials.prop(p, 'wallpaper_back_material')
        materials.prop(p, 'wall_material')
    if module == 'fabric':
        from project_damage.paint import drawing_row
        col.prop_search(p, 'fabric_group', ownership.source_of(context.object), 'vertex_groups', text='Tear Mask')
        col.label(text='Drawn holes')
        drawing_row(col, p, 'FABRIC_HOLES')
        col.label(text='Drawn torn edges')
        drawing_row(col, p, 'FABRIC_EDGES')

def _effect_group_draw(self, context):
    p = ownership.source_of(context.object).project_damage
    layout = self.layout
    layout.use_property_split = True
    layout.use_property_decorate = False
    col = layout.column()
    active=getattr(p,self.effect_module+'_enable')
    if self.effect_module in {'hit','glass'}:active=active or getattr(p,self.effect_module+'_through_enable')
    col.enabled = active and not p.all_off
    for field in self.effect_fields:
        if self.effect_module=='scratch' and p.scratch_mode=='1' and field in {'density','max_scratches','spread','falloff','angle_jitter','cross_share','along'}:continue
        if self.effect_module=='scratch' and field=='mode':
            col.prop(p,'scratch_mode');continue
        if self.effect_module=='scratch' and p.scratch_style!='3' and field not in {'enable','seed','style','avoid','detail','density','max_scratches','spread','size_scale','paint_snap','wrap_angle'}:continue
        if self.effect_module == 'metal':
            shared={'enable','seed','avoid','style','edge_length','detail','max_tris','sheet_thickness','roughness'}
            crush={'crush_aim','crush_proximity','crush_depth','crush_length','crush_hinge','crush_skew','fold_size','fold','fold_sharp','fold_wander','fold_lift','crush_crumple','crush_facet'}
            if field not in shared and ((p.metal_style=='1') != (field in crush)): continue
        if field not in {'enable', 'mode'}:
            if self.effect_module=='bevel' and field in {'size_top','size_side'} and p.bevel_size_mode=='1':
                col.prop(p,'bevel_'+field,text='Top Share' if field=='size_top' else 'Side Share')
            else:
                col.prop(p, self.effect_module + '_' + field)

def _draw_effect_original(layout,context,key,raw=False):
    if key in REMOVED_EFFECTS:return
    from project_damage import panels
    mapping={
        'edge':('chips','chips_noise'),'surface':('surface','surface_noise'),
        'corner':('corners',),'fracture':('fracture','fracture_realism','fracture_proximity','fracture_noise','fracture_options'),
        'fracture_through':('fracture','fracture_realism','fracture_proximity','fracture_noise','fracture_options'),
        'buildup':('buildup',),'rust':('rust',),'heat':('heat',),'coating':('coating',),
        'boolean':('breakers',),'rebar':('rebar',),
    }
    if key in mapping:
        classes=[getattr(panels,'OBJECT_PT_project_damage_'+name) for name in mapping[key]]
        for i,cls in enumerate(classes):
            inline_noise='noise' in cls.bl_label.casefold() or 'noise' in cls.__name__.casefold()
            target=layout if i==0 or inline_noise else fold(layout,'effect_'+key+'_'+str(i),cls.bl_label)
            if target is None: continue
            if inline_noise:
                target.separator(); target.label(text=cls.bl_label)
            cls.draw(SimpleNamespace(layout=_Layout(target,key,raw)),context)
    else:
        module={'hit_through':'hit','glass_through':'glass'}.get(key,key)
        _effect_draw(SimpleNamespace(layout=_Layout(layout,key,raw),effect_module=module),context)
        for i,(label,fields) in enumerate(EFFECT_GROUPS.get(module,())):
            target=fold(layout,'effect_'+key+'_'+str(i+1),label)
            if target is not None:
                _effect_group_draw(SimpleNamespace(layout=_Layout(target,key,raw),effect_module=module,effect_fields=fields),context)
    if key=='boolean':
        src=source(context)
        head,body=layout.panel('project_damage_boolean_rebar',default_closed=False)
        head.prop(src.project_damage,'rebar_enable',text='Rebar / Internal Fill')
        if body is not None:
            panels.OBJECT_PT_project_damage_rebar.draw(SimpleNamespace(layout=_Layout(body,key,raw)),context)


                                                                              
                                                    
                                                                              

def draw_dev_tools(self, context):
    from project_damage import sidebar
    layout = self.layout
    layout.operator('project_damage.export_dev_settings', text='Save All Settings for AI', icon='EXPORT')
    src, key = dev_tools.selected(context)
    if key is None:
        layout.label(text='Select an effect in the hierarchy', icon='INFO')
        dev_tools.button(layout,'Show All Tiles','SHOW_ALL')
        dev_tools.button(layout,'Reset UI Layout','RESET_LAYOUT','LOOP_BACK')
        return
    state = context.window_manager.project_damage_dev
    layout.label(text=dev_tools.effect_name(key,sidebar.CATALOG[key][1]))
    row = layout.row(align=True)
    dev_tools.button(row, 'Save Defaults', 'SAVE_DEFAULTS', 'FILE_TICK')
    dev_tools.button(row, 'Reset Defaults', 'RESET_DEFAULTS', 'LOOP_BACK')
    layout.label(text='New setups only; does not enable the effect')
    layout.prop(state,'edit_layout')
    if not state.edit_layout:
        return
    box = layout.box();box.label(text='Effect / Add Tile')
    box.prop(state,'effect_label');dev_tools.button(box,'Set Name','RENAME_EFFECT')
    row=box.row(align=True)
    dev_tools.button(row,'Earlier','TILE_UP','TRIA_UP');dev_tools.button(row,'Later','TILE_DOWN','TRIA_DOWN')
    row=box.row(align=True)
    dev_tools.button(row,'Show Tile' if key in dev_tools.data()['layout'].get('hidden',[]) else 'Hide Tile','TOGGLE_TILE')
    dev_tools.button(row,'Show All Tiles','SHOW_ALL')
    box=layout.box();box.label(text='Parameter / Control')
    box.prop(state,'field');box.prop(state,'field_label');dev_tools.button(box,'Set Control Name','RENAME_FIELD')
    hidden=state.field in dev_tools.field_config(key).get('hidden',[])
    dev_tools.button(box,'Show Control' if hidden else 'Hide Control','TOGGLE_FIELD')
    row=box.row(align=True);dev_tools.button(row,'Pin to Quick Controls','PIN');dev_tools.button(row,'Unpin','UNPIN')
    row=box.row(align=True);dev_tools.button(row,'Earlier','FIELD_UP','TRIA_UP');dev_tools.button(row,'Later','FIELD_DOWN','TRIA_DOWN')
    layout.label(text='Order affects UI only, not damage processing')
    dev_tools.button(layout,'Reset UI Layout','RESET_LAYOUT','LOOP_BACK')


                                                                              
                                                                       
                                                                              

_BACKUP_KEY = '_single_file_ui_originals'
def _replace(owner, name, value):
    backups = getattr(addon, _BACKUP_KEY, None)
    if backups is None:
        backups = []; setattr(addon, _BACKUP_KEY, backups)
    if not any(o is owner and n == name for o, n, _ in backups):
        backups.append((owner, name, getattr(owner, name)))
    setattr(owner, name, value)

def _redraw():
    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas: area.tag_redraw()

def restore_ui():
    global _PREVIEWS
    for owner, name, value in reversed(getattr(addon, _BACKUP_KEY, [])):
        setattr(owner, name, value)
    if hasattr(addon, _BACKUP_KEY): delattr(addon, _BACKUP_KEY)
    if _PREVIEWS is not None:
        bpy.utils.previews.remove(_PREVIEWS); _PREVIEWS = None
    _PREVIEW_FILES.clear()
    _redraw()
    print('Project Damage: installed UI restored')

def apply_ui():
                                                                           
    for name, value in [('EFFECTS', EFFECTS), ('CATALOG', CATALOG),
                        ('TILE_LABELS', TILE_LABELS), ('HIDDEN_EFFECTS', HIDDEN_EFFECTS),
                        ('EFFECT_CATEGORIES', EFFECT_CATEGORIES), ('TOOLS_CATEGORIES', TOOLS_CATEGORIES),
                        ('_draw_effect_original', _draw_effect_original), ('draw_effect', draw_effect),
                        ('_draw_effect_tiles', _draw_effect_tiles)]:
        _replace(sidebar, name, value)
    for owner, callback in [
        (sidebar.OBJECT_PT_project_damage, draw_panel_add),
        (sidebar.OBJECT_PT_project_damage_presets, draw_panel_presets),
        (sidebar.OBJECT_PT_project_damage_tools, draw_panel_tools),
        (sidebar.OBJECT_PT_project_damage_settings, draw_panel_settings),
        (dev_tools.OBJECT_PT_project_damage_dev_tools, draw_dev_tools),
        (input_prep.OBJECT_PT_project_damage_input, draw_mesh_preparation)]:
        _replace(owner, 'draw', callback)
    _replace(sidebar.PROJECT_DAMAGE_UL_effects, 'draw_item', draw_effect_list_item)
    _replace(material_controls, 'draw', draw_materials)
    _replace(material_controls, 'centered_heading', centered_heading)
    _replace(paint, 'drawing_row', drawing_row)
    _replace(clean_floating, 'draw_controls', draw_cleanup)
    _replace(panels.OBJECT_PT_project_damage_cleanup, 'draw', draw_effect_cleanup)
    _replace(panels.OBJECT_PT_project_damage_global, 'draw', draw_effect_global)
    _replace(panels.OBJECT_PT_project_damage_chips, 'draw', material_controls.with_materials(draw_effect_chips, 'edge'))
    _replace(panels.OBJECT_PT_project_damage_chips_noise, 'draw', draw_effect_chips_noise)
    _replace(panels.OBJECT_PT_project_damage_surface, 'draw', material_controls.with_materials(draw_effect_surface, 'surface'))
    _replace(panels.OBJECT_PT_project_damage_surface_noise, 'draw', draw_effect_surface_noise)
    _replace(panels.OBJECT_PT_project_damage_rust, 'draw', material_controls.with_materials(draw_effect_rust, 'rust'))
    _replace(panels.OBJECT_PT_project_damage_heat, 'draw', material_controls.with_materials(draw_effect_heat, 'heat'))
    _replace(panels.OBJECT_PT_project_damage_coating, 'draw', material_controls.with_materials(draw_effect_coating, 'coating'))
    _replace(panels.OBJECT_PT_project_damage_buildup, 'draw', material_controls.with_materials(draw_effect_buildup, 'buildup'))
    _replace(panels.OBJECT_PT_project_damage_fracture, 'draw', material_controls.with_materials(draw_effect_fracture, 'fracture'))
    _replace(panels.OBJECT_PT_project_damage_fracture_realism, 'draw', draw_effect_fracture_realism)
    _replace(panels.OBJECT_PT_project_damage_fracture_proximity, 'draw', draw_effect_fracture_proximity)
    _replace(panels.OBJECT_PT_project_damage_fracture_noise, 'draw', draw_effect_fracture_noise)
    _replace(panels.OBJECT_PT_project_damage_fracture_options, 'draw', draw_effect_fracture_options)
    _replace(panels.OBJECT_PT_project_damage_corners, 'draw', material_controls.with_materials(draw_effect_corners, 'corner'))
    _replace(panels.OBJECT_PT_project_damage_breakers, 'draw', material_controls.with_materials(draw_effect_breakers, 'boolean'))
    _replace(panels.OBJECT_PT_project_damage_rebar, 'draw', draw_effect_rebar)
    _replace(panels.OBJECT_PT_project_damage_breaker, 'draw', draw_effect_breaker)
    _redraw()
    print('Project Damage: single-file UI applied. Edit and Alt+P to refresh.')

if __name__ == '__main__':
    apply_ui()

