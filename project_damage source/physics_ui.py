# раскладка сворачиваемых разделов физики
# SPDX-License-Identifier: GPL-3.0-or-later
"""Physics tab layout. Explicit rows/props, presentation-only disclosure state.

PREPARE: create fill volumes or detach existing fragments.
FRACTURE: configure the cut, then execute it in the same box.
FINISH: inside-face materials and roughness.
SIMULATE: rigid bodies, colliders and bake.
"""
import bpy
from . import damage_physics as dp, ui_image_hints as hints

SECTIONS = (
    ('fill','Fill Damage',True), ('separate','Separate Pieces',False),
    ('fracture','Create Fracture Pieces',True), ('drawing','Cuts & Guidance',False),
    ('finish','Noisify Shards',False), ('simulation','Rigid Body Simulation',False),
)


# подключаем свойства и действия этого модуля к Blender
def register():
    for key,title,opened in SECTIONS:
        name='project_damage_physics_ui_'+key
        if not hasattr(bpy.types.WindowManager,name):
            setattr(bpy.types.WindowManager,name,bpy.props.BoolProperty(
                name=title,default=opened,options={'SKIP_SAVE'},
                description='Show or hide '+title+' settings. This only changes the interface; it does not rebuild geometry'))


# убираем подключения модуля при выключении аддона
def unregister():
    for key,_,_ in SECTIONS:
        name='project_damage_physics_ui_'+key
        if hasattr(bpy.types.WindowManager,name):delattr(bpy.types.WindowManager,name)


def section(layout,context,key,title,icon,hint=None):
    box=layout.box();box.use_property_split=False;box.use_property_decorate=False
    name='project_damage_physics_ui_'+key
    opened=getattr(context.window_manager,name,True)
    row=box.row(align=True);row.scale_y=1.5
    arrow=row.row(align=True)
    arrow.prop(context.window_manager,name,text='',icon='TRIA_DOWN' if opened else 'TRIA_RIGHT',emboss=False)
    title_row=row.row(align=True);title_row.alignment='CENTER'
    title_row.prop(context.window_manager,name,text=title,icon=icon,emboss=False)
    if hint:hints.button(row,hint,context)
    return box.column(align=True) if opened else None


# собираем видимые настройки этого раздела
def draw(layout,p,context,module):
    source=dp.ownership.context_source(context)
    if source is None or not hasattr(source,'project_damage_physics') or not dp.module_enabled(p,module):return
    if module=='concrete' and not (p.concrete_enable or p.concrete_v2_enable or p.pipe_damage_enable or p.cable_damage_enable):return
    from . import physics_workflow,boolean_break,material_controls,cell_cuts,inner_rough,uv_zones
    s=dp.settings(source)
    root=layout.column(align=False);root.use_property_split=False;root.use_property_decorate=False
    guide=root.box()
    material_controls.centered_heading(guide,'Prepare → Fracture → Simulate',icon='PHYSICS')
    guide.label(text='1. Geometry — create damage.', icon='MOD_BOOLEAN')
    guide.label(text='2. Fill — restore the missing volume.', icon='MOD_SOLIDIFY')
    guide.label(text='3. Fracture Fills — split into pieces.', icon='MOD_EXPLODE')
    guide.label(text='4. Add colliders, enable physics, play.', icon='PHYSICS')
    row = guide.row(align=True)
    hints.button(row, 'physics_workflow', context)
    op = row.operator('project_damage.editorial_open', text='Workflow with Renders', icon='HELP')
    op.asset = 'physics'
    op.section = 'The complete sequence'
    fills = dp.fills_of(source, module)
    count = len(fills)
    piece_count = len(dp.pieces_of(source))
    guide.label(text='%d fills  |  %d fracture pieces'%(count,piece_count))


    col=section(root,context,'fill','Prepare: Fill Damage' if module not in dp.ADDITIVE else 'Prepare: Detach Geometry','MOD_SOLIDIFY','physics_fill_method' if module not in dp.ADDITIVE else None)
    if col is not None:
        if module in dp.ADDITIVE:
            col.prop(s,'detach_remove',text='Remove Detached Geometry from Wall')
            row=col.row(align=True);row.scale_y=1.5
            row.operator('project_damage.physics_fill',text='Detach '+dp.LABELS[module],icon='MOD_PARTICLES').module=module
            physics_workflow.remove_button(row,source,'FILL',module)
        else:
            row=col.row(align=True);row.label(text='How much depth to restore?',icon='MOD_SOLIDIFY');hints.button(row,'physics_fill_depth',context)
            row=col.row(align=True);row.scale_y=1.5
            for value,label in (('AUTO','Auto'),('WHOLE','Whole Cut'),('SURFACE','Surface Layer')):row.prop_enum(s,'fill_depth',value,text=label)
            if s.fill_depth=='SURFACE':col.prop(s,'surface_depth',text='Surface Depth (cm)')
            col.prop(s,'fill_cracks',text='Include Isolated Thin Cracks')
            col.separator()
            col.label(text='Fill Damage: sockets around structures.')
            if module != 'bullet':col.label(text='Fill V2: local fill; ignores bars / services.')
            row=col.row(align=True);row.scale_y=1.5
            row.operator('project_damage.physics_fill',text='Fill Damage',icon='MOD_SOLIDIFY').module=module
            if module != 'bullet':row.operator('project_damage.physics_fill_v2',text='Fill V2',icon='MOD_BOOLEAN').module=module
            physics_workflow.remove_button(row,source,'FILL',module)
            from . import fill_exact
            if module != 'bullet' and fill_exact.enabled_modules(p):
                row = col.row(align=True)
                row.scale_y = 1.5
                row.operator('project_damage.physics_fill_exact', icon='SELECT_DIFFERENCE').module = module
                col.label(text='Full depth across active solid damage effects.')
            if module != 'bullet' and fill_exact.combined_fill(fills) is not None:
                col.operator('project_damage.physics_separate_exact', icon='MOD_EXPLODE').module = module
            reason=dp.fill_block_reason(source)
            if reason:
                col.label(text='Fill locked: a separated body was moved.',icon='LOCKED')
                col.label(text='Restore its original position, rotation and scale.')
            if module=='hit':
                box=col.box();box.prop(s,'hit_fill_shards');box.prop(s,'hit_fill_center')
                box.operator('project_damage.physics_fill_hole',text='Fill Crater with Shards',icon='MOD_PARTICLES')


    if module in {'boolean','hit'}:
        col=section(root,context,'separate','Separate Pieces','MOD_EXPLODE','physics_separate')
        if col is not None:
            col.label(text='Detach existing islands; no new cuts.',icon='INFO')
            if module=='boolean':boolean_break.draw_physics(col,p)
            else:
                if not p.hit_through_enable:
                    col.prop(s,'hit_separate',text='Separate Fragments',toggle=True,icon='MOD_EXPLODE')
                    if s.hit_separate:
                        col.prop(p,'hit_through_gap',text='Piece Gap');col.prop(p,'hit_through_remove',text='Remove Chunks');col.prop(p,'hit_through_min_piece',text='Min Piece')
                if p.hit_through_enable or s.hit_separate:col.operator('project_damage.physics_separate',icon='MOD_EXPLODE')


    col=section(root,context,'fracture','Fracture: Create Pieces','MOD_EXPLODE','physics_fracture')
    if col is not None:
        targets=dp.fracture_targets(context,source,module)
        fragments=bool(targets and targets[0].get('project_damage_bfrag_of'))
        if fragments:col.label(text='Target: %d selected fragments'%len(targets),icon='RESTRICT_SELECT_OFF')
        elif count:col.label(text='Target: selected fills, otherwise all fills.',icon='MOD_SOLIDIFY')
        else:col.label(text='Create a fill or select a fragment first.',icon='INFO')
        box=col.box();row=box.row(align=True)
        material_controls.centered_heading(row,'Fracture Type',icon='MOD_EXPLODE');hints.button(row,'fracture_type',context)
        for options in ((('AUTO','Auto'),('CONCRETE','Concrete'),('ROCK','Rock')),(('SOIL','Soil'),('WOOD','Wood'),('GLASS','Glass'),('FLAT','Flat Cells'))):
            row=box.row(align=True);row.scale_y=1.5
            for value,label in options:row.prop_enum(s,'material',value,text=label)
        if s.material=='WOOD' or (module=='bullet' and s.material=='AUTO' and dp._bullet_material(source)=='WOOD'):box.prop(s,'grain',expand=True)
        if module=='brick' and s.material=='AUTO':
            box.prop(s,'brick_mortar',expand=True);box.prop(s,'brick_snap')
        if module=='hit':box.prop(s,'hit_fill_shards',text='Pieces');box.prop(s,'hit_fill_center')
        else:box.prop(s,'bullet_piece_size' if module=='bullet' else 'piece_size',text='Piece Size (cm)')
        box.prop(s,'amount_budget',text='Maximum Pieces');box.prop(s,'seed');box.prop(s,'hide_fill',text='Hide Input after Fracture')
        drawing=section(col,context,'drawing','Cuts & Guidance','GREASEPENCIL','physics_drawing')
        if drawing is not None:
            row=drawing.row(align=True);row.scale_y=1.5
            row.prop_enum(s,'cut_mode','AUTO');row.prop_enum(s,'cut_mode','DRAWN')
            cell_cuts.draw_layers(drawing,context,source,physics=True)
        col.separator()
        col.label(text='Ready? Apply the settings with Fracture.',icon='INFO')
        from .material_library import draw_property
        draw_property(col, s, 'inner_material', text='Inside Faces')
        uv_zones.draw_inside_uv(col, context)
        row=col.row(align=True);row.scale_y=1.7
        row.operator('project_damage.physics_fracture',text='Fracture Selected Fragments' if fragments else 'Fracture Fills',icon='MOD_EXPLODE').module=module
        physics_workflow.remove_button(row,source,'PIECES',module)

    col=section(root,context,'finish','Finish - Noisify Shards','SHADING_RENDERED','inner_form')
    if col is not None:
        inner_rough.draw(col, source, 'pieces')
    col=section(root,context,'simulation','Simulate: Rigid Bodies','PHYSICS','physics_proxy')
    if col is not None:
        col.label(text='Create pieces before enabling simulation.',icon='INFO')
        dp.draw_quick_physics(col,context)
