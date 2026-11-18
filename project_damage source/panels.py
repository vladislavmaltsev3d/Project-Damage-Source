# панели отдельных эффектов и общих инструментов
from . import generated_geometry, through_holes
from . import quality_ui
# ProjectDamage for Blender - worn edge damage cut into mesh objects.
# Copyright (C) 2026 Vladislav Maltsev - Lazy3D
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <https://www.gnu.org/licenses/>.
from . import output_collections
from . import ui_image_hints
import bpy
from types import SimpleNamespace
from .fracture_assets import TYPE_BY_ASSET, effective_type, proximity_mode
from . import ownership

from . import breaker as breaker_mod
from . import build
from . import clean_floating
from . import core_loader
from . import live
from . import manual
from . import avoid_tools
from . import effect_layout


def draw_mesh_preparation_tools(self, context):
    s = ownership.context_source(context).project_damage_input
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

def draw_cleanup_tools(layout, context):
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

class _ProjectDamagePanel(bpy.types.Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "⚒ Project Damage"
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        return (ownership.context_source(context) is not None and ownership.context_source(context).type == 'MESH'
                and not ownership.context_source(context).get(build.RESULT_STAMP)
                and not ownership.context_source(context).get(build.DEPOSIT_STAMP)
                and not ownership.context_source(context).get(build.COATING_STAMP))



# раздел «project_damage»
class OBJECT_PT_project_damage(_ProjectDamagePanel):
    bl_idname = "OBJECT_PT_project_damage"
    bl_label = "project_damage"
    bl_options = set()                                                   

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False
        source = ownership.context_source(context)
        p = source.project_damage
        identity = layout.box()
        identity.label(text="Original: " + source.name, icon='OBJECT_DATA')
        row = identity.row(align=True)
        row.operator('project_damage.select_original', text='Edit Original')
        result = build.result_of(source)
        sub = row.row(align=True)
        sub.enabled = result is not None
        sub.operator('project_damage.select_result', text='Edit Deformable')
        if result is not None:
            identity.label(text="Deformable: " + result.name, icon='MESH_DATA')
        if context.mode == 'EDIT_MESH' and context.object == result:
            identity.label(text='INIT_project_damage regenerates from Original', icon='INFO')
                                                                        
                                                                          
                                                                           
                                              
         
                                                                             
                                                                          
                                                                           
                                           
        mm = core_loader.version_mismatch()
        if mm is not None:
            row = layout.row()
            row.alert = True
            row.label(text="Addon %s but core %s - restart Blender to finish "
                           "the update" % mm, icon='ERROR')
        row = layout.row()
        row.scale_y = 1.4
        row.operator("project_damage.update", icon='FILE_REFRESH')
        row = layout.row()
        row.scale_y = 1.4
        row.operator("project_damage.apply_all_damage", icon='CHECKMARK')
        layout.prop(p, "preserve_data")
                                                                      
                                                                       
                                                                         
                                                               
                                                         
        layout.prop(p, "live_update")
        delay = layout.column()
        delay.active = p.live_update
        delay.prop(p, "defer_ms")
        library = layout.box()
        library.label(text="USER_LIBRARY")
        row = library.row(align=True)
        row.menu("PROJECT_DAMAGE_MT_presets", text=bpy.types.PROJECT_DAMAGE_MT_presets.bl_label)
        row.operator("project_damage.add_preset", text="", icon='ADD')
        row.operator("project_damage.add_preset", text="", icon='REMOVE').remove_active = True
        layout.separator()
        res = build.result_of(ownership.context_source(context))
        if res is not None:
            layout.label(text="Result: %s" % res.name, icon='OUTLINER_OB_MESH')


class OBJECT_PT_project_damage_cleanup(_ProjectDamagePanel):
    bl_parent_id = "OBJECT_PT_project_damage"
    bl_label = "Cleanup / Trim"

    def draw(self, context):
        draw_cleanup_tools(self.layout, context)


# раздел «Global»
class OBJECT_PT_project_damage_global(_ProjectDamagePanel):
    bl_parent_id = "OBJECT_PT_project_damage"
    bl_label = "Global"

    def draw(self, context):
        p = ownership.context_source(context).project_damage
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
        mask.prop_search(p, "mask_group", ownership.context_source(context), "vertex_groups")
        mask_on = mask.column()
        mask_on.active = bool(p.mask_group) and p.mask_group in ownership.context_source(context).vertex_groups
        mask_on.prop(p, "mask_amount")
        mask_on.prop(p, "mask_size")
        box = layout.box()
        box.prop(p, 'draw_mask_enable')
        if p.draw_mask_enable:
            from .paint import drawing_row
            box.row(align=True).prop(p, 'draw_mask_mode', expand=True)
            drawing_row(box, p, 'DRAWMASK')
            box.prop(p, 'draw_mask_radius')
            box.prop(p, 'draw_mask_soft')
            box.prop(p, 'draw_mask_size')
            box.label(text='Empty drawing: mask ignored.', icon='INFO')


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
    its Resolution and Shape rows carry no such case and stay live at Amount 0,
    while its Scale does not."""
    layout.use_property_split = True
    layout.use_property_decorate = False
    col = layout.column()
    col.active = (getattr(props, enable_attr) or (enable_attr=='fract_enable' and props.fract_through_enable)) and not props.all_off
    return col


# раздел «Edge Damage»
class OBJECT_PT_project_damage_chips(_ProjectDamagePanel):
    bl_parent_id = "OBJECT_PT_project_damage"
    bl_label = "Edge Damage"

    def draw_header(self, context):
        self.layout.prop(ownership.context_source(context).project_damage, "chips_enable", text="")

    def draw(self, context):
        p = ownership.context_source(context).project_damage
        col = _tab(self.layout, p, "chips_enable")
        colr=col.row()
        colr.scale_y = 2
        colr.row(align=True).prop(p,'chips_mode',text = ' ', expand=True)
        if p.chips_mode == 'PAINT':
            box = col.box()

            from .paint import drawing_row
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
        box.prop(p, "amount",text="Chip Count")
        scale.prop(p, "chip_scale")

        scale.prop(p, "chip_seed", text="Seed")
        quality_ui.draw(scale, p, "detail")                                     
        scale.prop(p, "offset",text="Offset")
        scale.prop(p, "random_offset",text="Randomize Offset")
               
        box = col.box()
        row = box.row()
        row.alignment = 'CENTER'
        row.label(text="Shape")
        size = box.column(align=True)
        material_controls.inline(box, p, 'edge_damage_material', text='Cut Edges')
        size.prop(p, "size_min")
        size.prop(p, "size_max")
        depth = box.column(align=True)
        depth.prop(p, "depth_min")
        depth.prop(p, "depth_max")
        box.prop(p, "stretch")
        box.prop(p, "rim_soft")
                                                                            
        box.separator()
        look = box.column(align=True)
        look.prop(p,'chip_cluster')
        look.prop(p,'chip_fine')
        look.prop(p,'chip_profile')
        look.prop(p,'chip_asym')
        look.prop(p,'chip_guard')

                   


# раздел « »
class OBJECT_PT_project_damage_chips_noise(_ProjectDamagePanel):
                                                                                     
                                                                                     
                                                                              
    bl_parent_id = "OBJECT_PT_project_damage_chips"
    bl_label = " "

    def draw(self, context):
        p = ownership.context_source(context).project_damage
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


# раздел «Surface Damage»
class OBJECT_PT_project_damage_surface(_ProjectDamagePanel):
    bl_parent_id = "OBJECT_PT_project_damage"
    bl_label = "Surface Damage"

    def draw_header(self, context):
        self.layout.prop(ownership.context_source(context).project_damage, "surf_enable", text="")

    def draw(self, context):
        p = ownership.context_source(context).project_damage
        col = _tab(self.layout, p, "surf_enable")
        row = col.row()
        row.alignment = 'CENTER'
        row.scale_y= 2 
        row.prop(p, 'surf_mode', text = ' ', expand = True)
        if p.surf_mode == 'PAINT':
            box = col.box()

            from . import paint
            paint.drawing_row(box, p, 'SURFACE')

            box.prop(p, 'surf_close_strokes')
            box.prop(p, 'surf_paint_snap')
            box.label(text='Closed contours limit knife cuts.' if p.surf_type=='1' else 'Closed contours limit crater bowls.', icon='INFO')
                                                                                
                                                                               
                                                                           
                                                          
        box = col.box()

        row = box.row()
        row.alignment = 'CENTER'
        row.label(text="Basic Settings")

        basic=box
        row = box.row(align=True)
        row.scale_y=1.5
        row.prop_enum(p,'surf_type','0',text='Concrete')
        row.prop_enum(p,'surf_type','1',text='Wood Knife')
        if p.surf_type == '1':
            box = col.box()
            row = box.row(); row.alignment = 'CENTER'; row.label(text='Wood Knife')
            boxc=box.column(align=True)
            boxc.prop(p,'surf_grain_x',text='Grain X')
            boxc.prop(p,'surf_grain_y',text='Grain Y')
            boxc.prop(p,'surf_grain_z',text='Grain Z')
            boxc.prop(p,'surf_knife_jitter'); boxc.prop(p,'surf_knife_back')
            boxc.label(text='Grain direction uses local object axes.', icon='INFO')
            boxc.label(text='Shading v2 keeps knife ridges crisp (up to 25°).', icon='INFO')

        box=basic
        on = p.surf_amount > 0

        head = box.column(align=True)
        head.active = on
        material_controls.inline(box, p, 'surface_damage_material', text='Cavities')
        head.prop(p, "surf_scale", text='Scale')
        quality_ui.draw(head, p, "surf_detail", text='Resolution')
        head.prop(p, "surf_seed", text='Seed')

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
        row.label(text="Surface Shape", icon="MESH_DATA")
        look = box.column(align=True)
        look.active = on
        for field in ("surf_cluster", "surf_size_bias", "surf_depth_follow", "surf_guard"):
            look.prop(p, field)
        col.separator()
                                                                             
                                                                    
                                                                          
                                           
        if p.surf_type=='0':
            if generated_geometry.draw(col, p, 'surface_rebar'):
                box = col.box()
                material_controls.centered_heading(box, 'Reinforcement Mesh', icon='MOD_WIREFRAME')
                box.prop(p, 'surface_rebar_enable', text='Reinforcement Mesh', toggle=True, icon='MOD_WIREFRAME')
                if p.surface_rebar_enable:
                    settings=box.column(align=True)
                    material_controls.inline(settings, p, 'surface_rebar_material', text='Reinforcement')
                    quality_ui.draw(settings, p, 'surface_rebar_resolution', slider=True)
                    settings.prop(p, 'surface_rebar_amount', text='Amount')
                    settings.prop(p, 'surface_rebar_radius', text='Radius (cm)')
                    settings.prop(p, 'surface_rebar_cover', text='Deepness (cm)')


# раздел « »
class OBJECT_PT_project_damage_surface_noise(_ProjectDamagePanel):
                                                                                     
                                                                              
    bl_parent_id = "OBJECT_PT_project_damage_surface"
    bl_label = " "

    def draw(self, context):
        p = ownership.context_source(context).project_damage
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False

        box = layout.box()

        row = box.row()
        row.alignment = 'CENTER'
        row.label(text="Noise Settings", icon='MOD_NOISE')

        col = box.column(align=True)
        col.active = p.surf_enable and not p.all_off and p.surf_amount > 0
        col.prop(p, "surf_noise",text='Outline Noise')
        col.prop(p, "surf_noise_in",text='Floor Noise')

# раздел «Rust»
class OBJECT_PT_project_damage_rust(_ProjectDamagePanel):
    bl_parent_id = "OBJECT_PT_project_damage"
    bl_label = "Rust"

    def draw_header(self, context):
        self.layout.prop(ownership.context_source(context).project_damage, "rust_enable", text="")

    def draw(self, context):
        obj = ownership.context_source(context)
        p = obj.project_damage
        layout = self.layout
        button = layout.row()
        button.scale_y = 1.4
        button.operator("project_damage.apply_rust", icon='SHADING_TEXTURE')
        column = layout.column()
        column.active = p.rust_enable and not p.all_off

        column.prop(p, "rust_mode")
        if p.rust_mode == 'PAINT':
            from . import paint
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


# раздел «Heat Damage & Charring»
class OBJECT_PT_project_damage_heat(_ProjectDamagePanel):
    bl_parent_id = "OBJECT_PT_project_damage"
    bl_label = "Heat Damage & Charring"

    def draw_header(self, context):
        self.layout.prop(ownership.context_source(context).project_damage, "heat_enable", text="")

    def draw(self, context):
        p = ownership.context_source(context).project_damage
        button = self.layout.row()
        button.scale_y = 1.4
        button.operator("project_damage.apply_heat", icon='MOD_DISPLACE')
        col = _tab(self.layout, p, "heat_enable")
        col.prop_search(p, "heat_group", ownership.context_source(context), "vertex_groups")
        for field in ("heat_seed", "heat_intensity", "heat_diffuse", "heat_spread", "heat_duration", "heat_steps", "heat_char_start", "heat_shrink_depth", "heat_detail", "heat_block_round", "heat_cell_min", "heat_cell_max", "heat_crack_cells", "heat_crack_width", "heat_crack_depth", "heat_blisters", "heat_blister_min", "heat_blister_max", "heat_blister_low", "heat_blister_high", "heat_blister_burst", "heat_char_layer"):
            col.prop(p, field)
        self.layout.label(text="Empty heat group = whole mesh heated.", icon='INFO')


# раздел «Coating Peeling»
class OBJECT_PT_project_damage_coating(_ProjectDamagePanel):
    bl_parent_id = "OBJECT_PT_project_damage"
    bl_label = "Coating Peeling"

    def draw_header(self, context):
        self.layout.prop(ownership.context_source(context).project_damage, "coating_enable", text="")

    def draw(self, context):
        p = ownership.context_source(context).project_damage
        button = self.layout.row()
        button.scale_y = 1.4
        button.operator("project_damage.apply_coating", icon='MOD_SOLIDIFY')
        col = _tab(self.layout, p, "coating_enable")
        if material_controls.experimental():
            box=effect_layout.section(col, 'Coating and Flakes')
            material_controls.inline(box, p, 'surface_damage_material', text='Exposed Surface')
            material_controls.inline(box, p, 'buildup_material', text='Inline Flakes')
            material_controls.inline(box, p, 'coating_shell_material', text='Separate Coating')
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
        from . import paint
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


# раздел «Bead»
class OBJECT_PT_project_damage_buildup(_ProjectDamagePanel):
                                                                         
                                
    bl_parent_id = "OBJECT_PT_project_damage"
    bl_label = "Bead"

    def draw_header(self, context):
        self.layout.prop(ownership.context_source(context).project_damage, "build_enable", text="")

    def draw(self, context):
        p = ownership.context_source(context).project_damage
        self.layout.use_property_split = False
        self.layout.use_property_decorate = False
        button = self.layout.row()
        button.scale_y = 1.4
        button.operator("project_damage.apply_buildup", icon='MOD_FLUIDSIM')
        col = _tab(self.layout, p, "build_enable")
        col.use_property_split = False
        col.use_property_decorate = False

                                                                               
        drawn = p.build_mode == 'PAINT'
        colr = col.row()
        colr.scale_y = 2
        colr.row(align=True).prop(p, 'build_mode', text=' ', expand=True)
        if drawn:
            box = col.box()
            from .paint import drawing_row
            drawing_row(box, p, 'BEAD')
            box.prop(p, 'build_paint_radius')
            box.label(text='Draw along the inside edges to cover. Empty drawing: no bead.', icon='INFO')

        box = col.box()
        row = box.row()
        row.alignment = 'CENTER'
        row.label(text="Basic Settings", icon='SETTINGS')
        box.prop(p,"build_coverage",text="Edges Covered")
        head = box.column()
        head.active = drawn or p.build_coverage > 0.0
        head.prop(p, "build_scale")
        quality_ui.draw(head, p, "build_detail", text='Resolution')
        if not drawn:
            row = box.row()
            row.scale_y = 1.5
                                                              
            pass
        col.separator()

        box = col.box()
        row = box.row()
        row.alignment = 'CENTER'
        row.label(text="Shape", icon='MOD_CURVE')
        shape = box.column()
        shape.active = drawn or p.build_coverage > 0.0
        row = shape.row(align=True)
        material_controls.inline(box, p, 'buildup_material', text='Bead')
        row.prop_enum(p, 'build_shape', '0', text='Rounded')
        row.prop_enum(p, 'build_shape', '1', text='Fillet')
        shape.prop(p, "build_width")
        shape.prop(p, "build_height")
        shape.prop(p, "build_irreg")
        shape.prop(p, "build_lumps")
        col.separator()


# раздел «Corner Piles»
class OBJECT_PT_project_damage_piles(_ProjectDamagePanel):
    bl_parent_id = "OBJECT_PT_project_damage"
    bl_label = "Corner Piles"

    def draw_header(self, context):
        self.layout.prop(ownership.context_source(context).project_damage, "pile_enable", text="")

    def draw(self, context):
        p=ownership.context_source(context).project_damage
        layout=self.layout
        box=effect_layout.section(layout,'General')
        material_controls.inline(box,p,'pile_material',text='Corner Piles')
        box.prop(p,'build_corner_prob',text='Corners Filled')
        box.prop(p,'pile_scale',text='Scale')
        quality_ui.draw(box, p,'pile_detail',text='Resolution')
        if p.build_corner_prob>0:
            box=effect_layout.section(layout,'Shape');col=box.column(align=True)
            col.prop(p,'build_corner_size',text='Pile Size')
            col.prop(p,'pile_height',text='Height')
            col.prop(p,'pile_lumps',text='Lumps')
            col.prop(p,'build_irreg',text='Waviness')
            col.prop(p,'build_blend',text='Blend Into Surface')


# раздел «Deposits»
class OBJECT_PT_project_damage_deposits(_ProjectDamagePanel):
    bl_parent_id = "OBJECT_PT_project_damage"
    bl_label = "Deposits"

    def draw_header(self, context):
        self.layout.prop(ownership.context_source(context).project_damage, "deposit_enable", text="")

    def draw(self, context):
        p=ownership.context_source(context).project_damage
        layout=self.layout
        box=effect_layout.section(layout,'General')
        box.prop(p,'deposit_style',text='Type',expand=True)
        material_controls.inline(box,p,'deposit_material',text='Deposits')
        box.prop(p,'deposit_scale',text='Scale')
        quality_ui.draw(box, p,'deposit_detail',text='Resolution')
        box.separator(factor=.5)
        box.prop(p,'deposit_amount',text='Amount')
        if p.deposit_amount>0:
            box=effect_layout.section(layout,'Shape');col=box.column(align=True)
            col.prop(p, 'deposit_size_min')
            col.prop(p, 'deposit_size_max')
            col.prop(p, 'deposit_height')
            col.prop(p, 'deposit_layers')
            col.prop(p, 'deposit_porosity')
            col.prop(p, 'deposit_embed')
            box=effect_layout.section(layout,'Placement');col=box.column(align=True)
            col.prop(p, 'deposit_gravity')
            if p.deposit_gravity>0:col.prop(p,'deposit_gravity_direction')
            col.prop(p, 'deposit_shelter')
            col.prop(p, 'deposit_shelter_radius')
        
def _fract_any_amount(p):
    """PROJECT_DAMAGE_FRACT_SCALE/_DETAIL, the Noise pair and the Options group's Depth
    Multiplier all share one GetDEnabling case: ANY level's Amount > 0."""
    return (p.fract_mode == 'PAINT' or any(getattr(p, "fract_l%d_amount" % n) > 0 for n in (1, 2, 3))
            or (proximity_mode(p) in {'RADIAL', 'GLASS'} and p.fract_prox_collection is not None)
            or (p.fract_helper_drawing is not None and (p.fract_physics or (p.fract_through_enable and p.fract_through))))




                                                                              
                                                                      
                                                                           
                                                                              
                                                                        

_EFFECT_LABELS = {
    'rbf': 'Rigid Body Fracture',
    'cellf': 'Cell Fracture',
    'debris': 'Damage Debris',
    'bevel': 'Edge Bevel / Wear', 'scratch': 'Scratches Draw', 'metal': 'Metal Deform',
    'fabric': 'Fabric Tears', 'concrete': 'Concrete Damage', 'hit': 'Concrete Hit',
    'plaster': 'Plaster Displacement', 'wood': 'Wood Damage',
    'lath': 'Lath Damage', 'wallpaper': 'Wallpaper',
    'bullet': 'Bullet Impacts', 'blast': 'Building Destruction',
    'ground': 'Ground Cracks', 'brick': 'Brick Damage',
    'glass': 'Broken Glass', 'fissure': 'Ground Fissures',
    'rusty': 'Rusty Holes',
}


def _effect_header(self, context):
    p = ownership.context_source(context).project_damage
    self.layout.prop(p, self.effect_module + '_enable', text='')


def draw_impact_deform(layout, context):
    """Impact Deform controls: impact_deform_tools.draw."""
    from . import impact_deform_tools
    impact_deform_tools.draw(layout, context)


def draw_fracture_helpers(layout, p):
    from . import paint
    material_controls.centered_heading(layout,'Cut Helpers',icon='GREASEPENCIL')
    paint.drawing_row(layout,p,'FRACT_HELPER')
    layout.prop(p,'fract_helper_snap',text='Projection Distance')


def draw_fragment_controls(layout,p,context,key):
    from . import separate_fragments
    box=layout.box()
    material_controls.centered_heading(box,'Fragments to Separate Objects',icon='MOD_EXPLODE')
    through=key in {'fracture_through','fracture_volume','hit_through'}
    if through:
        prop='hit_through' if key=='hit_through' else 'fract_through'
        row=box.row(align=True);row.scale_y=1.5
        row.prop(p,prop,text='Fragments to Separate Objects',toggle=True,icon='MOD_EXPLODE')
        if not getattr(p,prop):return
        if key=='hit_through':
            box.prop(p,'hit_through_gap',text='Piece Gap')
            box.prop(p,'hit_through_max_wall',text='Max Wall Thickness')
            box.prop(p,'hit_chunk_depth',text='Chunk Depth')
            box.prop(p,'hit_through_remove',text='Remove Chunks')
            box.prop(p,'hit_through_min_piece',text='Min Piece')
        else:
            box.prop(p,'fract_through_min_piece',text='Min Piece')
            if key=='fracture_volume':
                box.prop(p,'fract_volume_size',text='Inner Piece Size')
                box.prop(p,'fract_volume_rough',text='Inner Roughness')
            draw_fracture_helpers(box,p)
    else:
        row=box.row(align=True);row.scale_y=1.5
        row.prop(p,'fract_physics',text='Non-through Chunks (WIP)',toggle=True,icon='MOD_EXPLODE')
        if not p.fract_physics:return
        box.prop(p,'fract_physics_depth',text='Chunk Depth')
        box.prop(p,'fract_physics_close',text='Close Chunks')
        box.prop(p,'fract_through_min_piece',text='Min Piece')
        draw_fracture_helpers(box,p)
    separate_fragments.draw_controls(box,context)


def draw_glass_pieces(layout, p, context):
    """Hit Glass / Glass Drawn: Make Pass Through, Fill Hole, Separate to Pieces."""
    from . import material_controls
    box = layout.box()
    material_controls.centered_heading(box, 'Pieces for Physics', icon='MOD_EXPLODE')
    row = box.row(align=True); row.scale_y = 1.5
                                                                      
    if p.glass_pass_through:
        sub = box.column(align=True)
        sub.prop(p, 'glass_through_gap', text='Piece Gap')
        sub.prop(p, 'glass_through_rings', text='Shard Rings')
        sub.prop(p, 'glass_through_fall', text='Remove Shards')
        sub.prop(p, 'glass_through_min_piece', text='Min Piece')
    row = box.row(align=True); row.scale_y = 1.5
    row.prop(p, 'glass_fill_hole', text='Fill Hole', toggle=True, icon='MOD_PARTICLES')
    if p.glass_fill_hole:
        sub = box.column(align=True)
        sub.prop(p, 'glass_fill_count')
        sub.prop(p, 'glass_fill_center')
        sub.prop(p, 'glass_fill_origin')
        if not p.glass_pass_through:
            sub.prop(p, 'glass_through_gap', text='Piece Gap')
    row = box.row(align=True); row.scale_y = 1.5
    separate = row.operator('project_damage.separate_fragments', text='Separate to Pieces', icon='MOD_EXPLODE')
    separate.origin_mode = 'MASS'
    separate.glass = True
    row.operator('project_damage.remove_glass_pieces', text='', icon='TRASH')
    box.label(text='One closed object per piece, origin at its centre of mass, no parent', icon='INFO')
                                                                                  
    from . import damage_physics
    damage_physics.draw_quick_physics(box, context)


def _effect_draw(self, context):
    from . import material_controls
    p = ownership.context_source(context).project_damage
    module = self.effect_module
    layout = self.layout
    layout.use_property_split = True
    layout.use_property_decorate = False
    col = layout.column()
    col.enabled = not p.all_off
    if module=='rbf':
        from . import paint
        ui_image_hints.prop(col,p,'rbf_material',text='Pattern')
        col.label(text='Creates separate meshes from the original; no physics is added.',icon='INFO')
        col.label(text='Cut guides');paint.drawing_row(col,p,'RBF_CUTS')
        col.label(text='Impact guides');paint.drawing_row(col,p,'RBF_IMPACTS')
        col.prop(p,'rbf_impact_collection')
    if module=='cellf':
        from . import paint
        ui_image_hints.prop(col,p,'cellf_material',text='Material')
        col.label(text='Solid pieces with rough break faces; no physics is added.',icon='INFO')
        row=col.row(align=True)
        row.prop(p,'cellf_prox_collection',text='Proximity')
        row.operator('project_damage.cellf_add_point',text='',icon='ADD')
        sub=col.row();sub.enabled=p.cellf_prox_collection is not None
        sub.prop(p,'cellf_prox_use_size')
        from .cell_cuts import draw_layers
        draw_layers(col,context,ownership.context_source(context))
    if module=='debris':
        col.label(text='Enable a cutting effect first. Source − result becomes debris.',icon='INFO')
        col.operator('project_damage.generate_debris',text='Generate / Refresh Debris',icon='FILE_REFRESH')
    if module in {'cellf','rbf','debris'}:
        from . import inner_rough
        inner_rough.draw(col, ownership.context_source(context), module)
    if module == 'concrete' and p.concrete_v2_enable:
        col.operator('project_damage.apply_concrete_v2', text='Update Concrete Damage V2', icon='MOD_DISPLACE')
    else:
        op = col.operator('project_damage.apply_effect', text='Apply ' + _EFFECT_LABELS[module], icon='MOD_DISPLACE')
        op.module = module
    from . import material_controls
    mat_module=module+'_through' if module in {'hit','glass'} and getattr(layout,'key',None)==module+'_through' else module
    material_controls.draw(col, p, mat_module)
    if module in {'bevel','scratch'}:
        from .paint import drawing_row
        drawing_row(col,p,module.upper())
                                                                         
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
        colb=col.box()
        colb.prop(p, 'hit_collection')
        colb.operator('project_damage.add_hit', icon='ADD')
        colb.alert = True
        colb.label(text='Move impact empties near the surface')
    if module=='glass':
        from . import input_prep
        from .params import glass_straight_through
        if glass_straight_through(p):
            col.label(text='Cuts straight through the mesh', icon='INFO')
        else:
            col.prop(input_prep.settings(ownership.context_source(context)),'thickness_logic')
    if module == 'blast':
        col.prop(p, 'blast_collection')
        col.operator('project_damage.add_hit', text='Add Blast Empty', icon='ADD').module = 'blast'
        col.label(text='Local +Z is the building up axis')
    if module == 'bullet':
        rowc=col.row()
        rowc.alignment = 'CENTER'
        rowc.scale_y=1.45
        rowc.label(text = 'Distribution Mode')
                                       
                                                     
    if module in {'concrete','lath','brick'}:
        row=col.row(align=True);row.scale_y=1.5
        row.prop(p,module+'_model_type',text='',expand=True)
    if module in {'concrete', 'wood', 'lath', 'bullet', 'ground', 'brick', 'rusty'}:
        row=col.row(align=True);row.scale_y=1.5
        if module in {'lath','brick'}:
            row.prop_enum(p,module+'_mode','0',text='Automatic')
            row.prop_enum(p,module+'_mode','1',text='Draw')
        else:row.prop(p, module + '_mode', text='', expand=True)
        if module not in {'lath','brick'} and getattr(p,module+'_mode')=='2':
            from . import damage_proximity
            damage_proximity.draw(col,p,module)
        if int(getattr(p, module + '_mode')) == 1:
            from .paint import drawing_row
            drawing_row(col, p, module.upper())
    if module == 'fissure':
        from .paint import drawing_row
        drawing_row(col, p, 'FISSURE')
        col.label(text='Draw a path above the ground; paint only')
    if module == 'glass':
        from .paint import drawing_row
        if p.glass_hit_enable:
            col.prop(p, 'glass_hit_collection', text='Hit Empties')
        else:
            col.label(text='Holes: closed contours')
            drawing_row(col, p, 'GLASS_HOLES')
            if p.glass_drawn_enable:
                col.prop(p, 'glass_drawn_reach', text='Crack Reach')
            else:
                col.label(text='Impact dots / crack strokes')
                drawing_row(col, p, 'GLASS_IMPACTS')
    if module == 'wallpaper':
        from .paint import drawing_row
        drawing_row(col, p, 'WALLPAPER')
    if module == 'rusty' and p.rusty_mode == '1':
        col.label(text='Dots, closed outlines and open trails', icon='GREASEPENCIL')
                                                                          
                                                             
                                                                          

# раздел «Hanging Rebar Holes»
class OBJECT_PT_project_damage_hang(_ProjectDamagePanel):
    bl_idname = 'OBJECT_PT_project_damage_hang'
    bl_parent_id = 'OBJECT_PT_project_damage'
    bl_label = 'Hanging Rebar Holes'
    effect_module = 'hang'

    def draw(self, context):
        p = ownership.context_source(context).project_damage
        layout = self.layout
        layout.use_property_split = False
        layout.use_property_decorate = False
        box = layout.box()
        box.enabled = p.hang_enable and not p.all_off
        effect_layout.heading(box, 'General')
        col = box.column(align=True)
        col.prop(p,'hang_sides',text='Resolution')
        col.prop(p,'hang_master_scale',text='Master Scale')
        col.prop(p,'hang_seed',text='Seed')
        row=col.row(align=True);row.scale_y=1.5
        row.prop(p,'hang_mode',text='Hole Source',expand=True)
        if p.hang_mode=='0':
            box=effect_layout.section(layout,'Distribution');col=box.column(align=True)
            material_controls.inline(col,p,'boolean_break_material',text='Hole Walls')
            col.prop(p,'hang_amount',text='Amount')
            col.prop(p, 'hang_thickness', text='Thickness')
            col.prop(p,'hang_edge_wall_depth')
            col.prop(p,'hang_size_min',text='Size Min')
            col.prop(p,'hang_size_max',text='Size Max')
            col.prop(p,'hang_jagged',text='Jagged')
            col.prop(p,'hang_flare',text='Flare')
            col.prop(p,'hang_roughness',text='Roughness')
            noise=_ui_disclosure(box,'hang_noise_open','Procedural Noise')
            if noise is not None:
                from . import asset_noise
                asset_noise.draw(noise,p,'hang')
        else:
            box=effect_layout.section(layout,'Breakers')
            from .boolean_break import draw_breaker_list
            draw_breaker_list(box,p,context)
            draw_linked_break_surface(layout,p,context,boolean_options=False)
        from .recording_tools import toggle_box
        if generated_geometry.draw(layout, p, 'hang'):
            col=toggle_box(layout,p,'hang_bars_enable','Reinforcement Bars')
            if col is not None:
                material_controls.inline(col,p,'rebar_material',text='Bars')
                quality_ui.draw(col, p,'hang_rebar_resolution',slider=True)
                col.prop(p,'hang_bar_spacing',text='Spacing')
                col.prop(p,'hang_bar_radius',text='Radius')
                col.prop(p,'hang_bar_depth',text='Depth')
                col.prop(p,'hang_bar_broken',text='Broken')
                col.prop(p,'hang_droop',text='Droop')
                col.prop(p,'hang_bar_bow',text='Bow')
            col=toggle_box(layout,p,'hang_cables_enable','Cables')
            if col is not None:
                material_controls.inline(col,p,'hang_cable_material',text='Cables')
                col.prop(p,'hang_cable_amount',text='Amount')
                col.prop(p,'hang_cable_radius',text='Radius')
                col.prop(p,'hang_cable_length',text='Length')
                col.prop(p,'hang_cable_sag',text='Sag')
                col.prop(p,'hang_cable_loops',text='Loops')


def draw_fabric_tear_shape(layout,p):
    from .boolean_break import _raw
    fibres=effect_layout.section(layout,'Fabric Fibres')
    row=fibres.row(align=True);row.scale_y=1.5
    row.prop_enum(p,'fabric_fibre_model','LOOSE',text='Loose Fibres')
    row.prop_enum(p,'fabric_fibre_model','WOVEN',text='Woven Fibres')
    if p.fabric_paint_only:
        if effect_layout.visible(p,'fabric_yarn_size'):
            fibres.prop(p,'fabric_yarn_size',text='Yarn Size (cm)')
        if effect_layout.visible(p,'fabric_hole_dir_mode'):
            fibres.prop(p,'fabric_hole_dir_mode',text='Fibre Direction')
        for axis in ('x','y','z'):
            if effect_layout.visible(p,'fabric_hole_dir_'+axis):
                fibres.prop(p,'fabric_hole_dir_'+axis,text='Fibre Direction '+axis.upper())
        return
    box=effect_layout.section(layout,'Tear Shape')
    _raw(box).prop(p,'fabric_tear_preset',text='Preset',expand=False)
    row=box.row(align=True);row.scale_y=1.5
    row.prop_enum(p,'fabric_tear_style','0',text='Lenses')
    row.prop_enum(p,'fabric_tear_style','1',text='Noise Contours')
    if p.fabric_tear_style=='1':
        col=box.column(align=True)
        col.prop(p,'fabric_shape_noise_scale',text='Contour Noise Scale (cm)')
        col.prop(p,'fabric_shape_noise_strength',text='Contour Irregularity',slider=True)
    box.prop(p,'fabric_yarn_size',text='Yarn Size (cm)')


class OBJECT_PT_project_damage_fabric(_ProjectDamagePanel):
    bl_idname = "OBJECT_PT_project_damage_fabric"
    bl_parent_id = "OBJECT_PT_project_damage"
    bl_label = _EFFECT_LABELS["fabric"]
    effect_module = "fabric"

    def draw(self, context):
        from .asset_updates import toggle_box
        p=ownership.context_source(context).project_damage
        layout=self.layout
        layout.use_property_split=False
        layout.use_property_decorate=False
        _effect_draw(SimpleNamespace(layout=layout,effect_module='fabric'),context)
        geometry=effect_layout.section(layout,'Fabric Geometry')
        row=geometry.row(align=True);row.scale_y=1.5
        row.operator('project_damage.fabric_geometry',text='Flat',icon='MESH_PLANE',depress=p.fabric_geometry=='FLAT').mode='FLAT'
        row.operator('project_damage.fabric_geometry',text='Solidify',icon='MOD_SOLIDIFY',depress=p.fabric_geometry=='SOLIDIFY').mode='SOLIDIFY'
        ui_image_hints.button(row,'fabric_mode')
        if p.fabric_solid_enable:geometry.prop(p.id_data.project_damage_input,'surface_thickness',text='Open Sheet Thickness (cm)')
        from .paint import drawing_row
        row=layout.row(align=True);row.scale_y=1.5
        row.operator('project_damage.fabric_placement',text='Procedural',depress=not p.fabric_paint_only).mode='PROCEDURAL'
        row.operator('project_damage.fabric_placement',text='Draw',depress=p.fabric_paint_only).mode='DRAW'
        if p.fabric_paint_only:
            box=effect_layout.section(layout,'Draw Tears')
            material_controls.inline(box, p, 'surface_damage_material', text='Torn Edges')
            row=box.row(align=True);row.alignment='CENTER'
            row.prop(p,'fabric_paint_holes_enable',text='Draw Holes',toggle=True,icon='GREASEPENCIL')
            if p.fabric_paint_holes_enable:drawing_row(box,p,'FABRIC_HOLES')
            row=box.row(align=True);row.alignment='CENTER'
            row.prop(p,'fabric_paint_edges_enable',text='Draw Torn Edges',toggle=True,icon='GREASEPENCIL')
            if p.fabric_paint_edges_enable:drawing_row(box,p,'FABRIC_EDGES')
            col=box.column(align=True)
            if effect_layout.visible(p,'surf_paint_snap'):col.prop(p,'surf_paint_snap',text='Projection Distance (cm)')
            if effect_layout.visible(p,'fabric_paint_jagged'):col.prop(p,'fabric_paint_jagged',text='Rim Roughness')
            if effect_layout.visible(p,'fabric_paint_rag_size'):col.prop(p,'fabric_paint_rag_size',text='Rim Detail Size (cm)')
            if effect_layout.visible(p,'fabric_paint_edge_spacing'):col.prop(p,'fabric_paint_edge_spacing',text='Edge Slit Spacing')
            if effect_layout.visible(p,'fabric_paint_edge_length_min'):col.prop(p,'fabric_paint_edge_length_min',text='Edge Slit Length Min')
            if effect_layout.visible(p,'fabric_paint_edge_length_max'):col.prop(p,'fabric_paint_edge_length_max',text='Edge Slit Length Max')
            if effect_layout.visible(p,'fabric_paint_edge_width_min'):col.prop(p,'fabric_paint_edge_width_min',text='Edge Slit Width Min')
            if effect_layout.visible(p,'fabric_paint_edge_width_max'):col.prop(p,'fabric_paint_edge_width_max',text='Edge Slit Width Max')
        box=effect_layout.section(layout,'General');col=box.column(align=True)
        if effect_layout.visible(p,'fabric_detail'):quality_ui.draw(col, p,'fabric_detail',text='Resolution')
        if effect_layout.visible(p,'fabric_seed'):col.prop(p,'fabric_seed',text='Seed')
        if effect_layout.visible(p,'fabric_layer_depth'):
            row=box.row(align=True)
            row.prop(p,'fabric_layer_depth',text='Panel Thickness Limit (cm)')
            row.operator('project_damage.setting_info',text='',icon='INFO').text='Panel Thickness Limit controls whether nearby back faces belong to the same cloth panel: 0 affects only the connected side; raise it to include the back of the same panel.'
        if p.fabric_solid_enable:
            col=box.column(align=True)
            col.prop(p,'fabric_solid_bevel',text='Cut Edge Bevel')
            if p.fabric_solid_bevel>0:col.prop(p,'fabric_solid_bevel_segments',text='Bevel Segments')
        draw_fabric_tear_shape(layout,p)
        if not p.fabric_paint_only:
            distribution=effect_layout.section(layout,'Noise Distribution')
            distribution.prop(p,'fabric_distribution_scale',text='Noise Scale (cm)')
            distribution.prop(p,'fabric_distribution_strength',text='Clustering',slider=True)
        if not p.fabric_paint_only:
            col=toggle_box(layout,p,'fabric_holes_enable','Interior Tears')
            if col is not None:
                material_controls.inline(col, p, 'surface_damage_material', text='Torn Edges')
                if effect_layout.visible(p,'fabric_hole_amount'):col.prop(p,'fabric_hole_amount',text='Hole Amount')
                if effect_layout.visible(p,'fabric_hole_length_min'):col.prop(p,'fabric_hole_length_min',text='Hole Length Min (cm)')
                if effect_layout.visible(p,'fabric_hole_length_max'):col.prop(p,'fabric_hole_length_max',text='Hole Length Max (cm)')
                if effect_layout.visible(p,'fabric_hole_width_min'):col.prop(p,'fabric_hole_width_min',text='Hole Width Min (cm)')
                if effect_layout.visible(p,'fabric_hole_width_max'):col.prop(p,'fabric_hole_width_max',text='Hole Width Max (cm)')
                if p.fabric_tear_style=='0' and effect_layout.visible(p,'fabric_hole_jagged'):col.prop(p,'fabric_hole_jagged',text='Hole Jagged')
                if p.fabric_tear_style=='0' and effect_layout.visible(p,'fabric_hole_lobes'):col.prop(p,'fabric_hole_lobes',text='Hole Lobes')
                if p.fabric_tear_style=='0' and effect_layout.visible(p,'fabric_hole_meander'):col.prop(p,'fabric_hole_meander',text='Hole Meander')
                if p.fabric_tear_style=='0' and effect_layout.visible(p,'fabric_hole_split_max'):col.prop(p,'fabric_hole_split_max',text='Hole Split Max')
                if p.fabric_tear_style=='0' and effect_layout.visible(p,'fabric_hole_bridge'):col.prop(p,'fabric_hole_bridge',text='Hole Bridge (cm)')
                if effect_layout.visible(p,'fabric_hole_dir_mode'):col.prop(p,'fabric_hole_dir_mode',text='Hole Dir Mode')
                if effect_layout.visible(p,'fabric_hole_dir_x'):col.prop(p,'fabric_hole_dir_x',text='Hole Dir X')
                if effect_layout.visible(p,'fabric_hole_dir_y'):col.prop(p,'fabric_hole_dir_y',text='Hole Dir Y')
                if effect_layout.visible(p,'fabric_hole_dir_z'):col.prop(p,'fabric_hole_dir_z',text='Hole Dir Z')
                if effect_layout.visible(p,'fabric_hole_dir_jitter'):col.prop(p,'fabric_hole_dir_jitter',text='Hole Dir Jitter')
            col=toggle_box(layout,p,'fabric_edge_enable','Edge Tears')
            if col is not None:
                material_controls.inline(col, p, 'surface_damage_material', text='Torn Edges')
                if effect_layout.visible(p,'fabric_edge_amount'):col.prop(p,'fabric_edge_amount',text='Edge Amount')
                if effect_layout.visible(p,'fabric_edge_spacing_jitter'):col.prop(p,'fabric_edge_spacing_jitter',text='Edge Spacing Jitter')
                if effect_layout.visible(p,'fabric_edge_length_min'):col.prop(p,'fabric_edge_length_min',text='Edge Length Min (cm)')
                if effect_layout.visible(p,'fabric_edge_length_max'):col.prop(p,'fabric_edge_length_max',text='Edge Length Max (cm)')
                if effect_layout.visible(p,'fabric_edge_width_min'):col.prop(p,'fabric_edge_width_min',text='Edge Width Min (cm)')
                if effect_layout.visible(p,'fabric_edge_width_max'):col.prop(p,'fabric_edge_width_max',text='Edge Width Max (cm)')
                if p.fabric_tear_style=='0' and effect_layout.visible(p,'fabric_edge_taper'):col.prop(p,'fabric_edge_taper',text='Edge Taper')
                if p.fabric_tear_style=='0' and effect_layout.visible(p,'fabric_edge_jagged'):col.prop(p,'fabric_edge_jagged',text='Edge Jagged')
                if p.fabric_tear_style=='0' and effect_layout.visible(p,'fabric_edge_meander'):col.prop(p,'fabric_edge_meander',text='Edge Meander')
                if effect_layout.visible(p,'fabric_edge_angle_jitter'):col.prop(p,'fabric_edge_angle_jitter',text='Edge Angle Jitter')
        threads=toggle_box(layout,p,'fabric_threads_enable','Threads','CURVE_DATA')
        if threads is not None:
            col=threads
            material_controls.inline(threads, p, 'buildup_material', text='Threads / Fibres')
            if effect_layout.visible(p,'fabric_thread_radius'):col.prop(p,'fabric_thread_radius',text='Thread Radius (cm)')
            if effect_layout.visible(p,'fabric_thread_sides'):col.prop(p,'fabric_thread_sides',text='Thread Sides')
            if effect_layout.visible(p,'fabric_thread_spacing'):col.prop(p,'fabric_thread_spacing',text='Thread Spacing (cm)')
            col=toggle_box(threads,p,'fabric_bridge_enable','Bridge Threads',"CURVE_DATA")
            if col is not None:
                if effect_layout.visible(p,'fabric_bridge_share'):col.prop(p,'fabric_bridge_share',text='Bridge Share')
                if effect_layout.visible(p,'fabric_thread_sag'):col.prop(p,'fabric_thread_sag',text='Thread Sag')
                if effect_layout.visible(p,'fabric_thread_skew'):col.prop(p,'fabric_thread_skew',text='Thread Skew')
                if effect_layout.visible(p,'fabric_thread_wobble'):col.prop(p,'fabric_thread_wobble',text='Thread Wobble')
            col=toggle_box(threads,p,'fabric_broken_enable','Broken Threads',"CURVE_DATA")
            if col is not None:
                if effect_layout.visible(p,'fabric_broken_share'):col.prop(p,'fabric_broken_share',text='Broken Share')
                if effect_layout.visible(p,'fabric_thread_droop'):col.prop(p,'fabric_thread_droop',text='Thread Droop')
                if effect_layout.visible(p,'fabric_thread_curl'):col.prop(p,'fabric_thread_curl',text='Thread Curl')
                if effect_layout.visible(p,'fabric_thread_taper'):col.prop(p,'fabric_thread_taper',text='Thread Taper')
            col=toggle_box(threads,p,'fabric_fray_enable','Edge Fray',"CURVE_DATA")
            if col is not None:
                if effect_layout.visible(p,'fabric_fray_spacing'):col.prop(p,'fabric_fray_spacing',text='Fray Spacing (cm)')
                if effect_layout.visible(p,'fabric_fray_length_min'):col.prop(p,'fabric_fray_length_min',text='Fray Length Min (cm)')
                if effect_layout.visible(p,'fabric_fray_length_max'):col.prop(p,'fabric_fray_length_max',text='Fray Length Max (cm)')
            if p.fabric_solid_enable and (not p.fabric_paint_only or p.fabric_paint_edges_enable):
                col=toggle_box(threads,p,'fabric_hang_enable','Hanging Threads',"CURVE_DATA")
                if col is not None:
                    if effect_layout.visible(p,'fabric_paint_hang_spacing'):col.prop(p,'fabric_paint_hang_spacing',text='Hang Spacing (cm)')
                    if effect_layout.visible(p,'fabric_hang_amount'):col.prop(p,'fabric_hang_amount',text='Hang Amount')
                    if effect_layout.visible(p,'fabric_hang_length_min'):col.prop(p,'fabric_hang_length_min',text='Hang Length Min (cm)')
                    if effect_layout.visible(p,'fabric_hang_length_max'):col.prop(p,'fabric_hang_length_max',text='Hang Length Max (cm)')
            col=threads.column(align=True)
            if effect_layout.visible(p,'fabric_gravity_x'):col.prop(p,'fabric_gravity_x',text='Gravity X')
            if effect_layout.visible(p,'fabric_gravity_y'):col.prop(p,'fabric_gravity_y',text='Gravity Y')
            if effect_layout.visible(p,'fabric_gravity_z'):col.prop(p,'fabric_gravity_z',text='Gravity Z')
            if effect_layout.visible(p,'fabric_max_threads'):col.prop(p,'fabric_max_threads',text='Max Threads')



                                                                          
                                                               
                                                                          
def _ui_disclosure(layout,key,title):
    box=layout.box();wm=bpy.context.window_manager
    opened=getattr(wm,'project_damage_'+key)
    row=box.row(align=True);row.alignment='CENTER';row.scale_y=1.5
    row.prop(wm,'project_damage_'+key,text=title,icon='TRIA_DOWN' if opened else 'TRIA_RIGHT',emboss=False)
    from . import editorial
    editorial.button(row,title)
    return box.column(align=True) if opened else None


class OBJECT_PT_project_damage_concrete(_ProjectDamagePanel):
    bl_idname = "OBJECT_PT_project_damage_concrete"
    bl_parent_id = "OBJECT_PT_project_damage"
    bl_label = _EFFECT_LABELS["concrete"]
    effect_module = "concrete"

    def draw(self, context):
        from .asset_updates import toggle_box
        p=ownership.context_source(context).project_damage
        layout=self.layout
        layout.use_property_split=False
        layout.use_property_decorate=False
        _effect_draw(SimpleNamespace(layout=layout,effect_module='concrete'),context)
        row=layout.row(align=True);row.scale_y=1.5
        row.prop(p,'concrete_wall_type',expand=True)
        if p.concrete_wall_type=='WOOD':
            box=effect_layout.section(layout,'Wooden Skin');col=box.column(align=True)
            col.prop(p,'concrete_wood_width',text='Plank Width (cm)')
            col.prop(p,'concrete_wood_splinter',text='Splinter Length (cm)')
            col.prop(p,'concrete_wood_angle',text='Grain Angle')
        box=effect_layout.section(layout,'General');col=box.column(align=True)
        if effect_layout.visible(p,'concrete_amount'):col.prop(p,'concrete_amount',text='Amount')
        if effect_layout.visible(p,'concrete_detail'):quality_ui.draw(col, p,'concrete_detail',text='Resolution')
        if effect_layout.visible(p,'concrete_seed'):col.prop(p,'concrete_seed',text='Seed')
        col.separator(type="LINE")
        if p.concrete_v2_enable and p.concrete_wall_type!='WOOD':
            col.prop(p,'concrete_smooth_width',text='Smooth Slope')
            col.separator(type="LINE")
        if effect_layout.visible(p,'concrete_size_min'):col.prop(p,'concrete_size_min',text='Size Min (cm)')
        if effect_layout.visible(p,'concrete_size_max'):col.prop(p,'concrete_size_max',text='Size Max (cm)')
        if effect_layout.visible(p,'concrete_lobes'):col.prop(p,'concrete_lobes',text='Lobes')
        if effect_layout.visible(p,'concrete_stretch'):col.prop(p,'concrete_stretch',text='Stretch')
        if effect_layout.visible(p,'concrete_edge_bias'):col.prop(p,'concrete_edge_bias',text='Edge Bias')
        if p.concrete_mode=="1":
            box=effect_layout.section(layout,"Paint Settings");col=box.column(align=True)
            if effect_layout.visible(p,'concrete_paint_radius'):col.prop(p,'concrete_paint_radius',text='Paint Radius (cm)')
            if effect_layout.visible(p,'concrete_paint_snap'):col.prop(p,'concrete_paint_snap',text='Paint Snap (cm)')
            if effect_layout.visible(p,'concrete_paint_close'):ui_image_hints.prop(col,p,'concrete_paint_close',text='Paint Close')
        box=effect_layout.section(layout,'Damaged Zone');col=box.column(align=True)
        material_controls.inline(col, p, 'surface_damage_material', text='Exposed Wood' if p.concrete_wall_type=='WOOD' else 'Exposed Concrete')
        if effect_layout.visible(p,'concrete_depth_min'):col.prop(p,'concrete_depth_min',text='Depth Min (cm)')
        if effect_layout.visible(p,'concrete_depth_max'):col.prop(p,'concrete_depth_max',text='Depth Max (cm)')
        if p.concrete_wall_type!='WOOD' and effect_layout.visible(p,'concrete_wall_soft'):col.prop(p,'concrete_wall_soft',text='Wall Soft')
        col.prop(p,'concrete_jagged',text='Outline Irregularity')
        if effect_layout.visible(p,'concrete_outline_scale'):col.prop(p,'concrete_outline_scale',text='Outline Scale')
        if effect_layout.visible(p,'concrete_terraces'):col.prop(p,'concrete_terraces',text='Terraces')
        if effect_layout.visible(p,'concrete_terrace_skin'):col.prop(p,'concrete_terrace_skin',text='Terrace Skin')
        if effect_layout.visible(p,'concrete_terrace_inset'):col.prop(p,'concrete_terrace_inset',text='Terrace Inset')
        if effect_layout.visible(p,'concrete_thickness_cap'):col.prop(p,'concrete_thickness_cap',text='Thickness Cap')
        col.prop(p,'concrete_edge_wall_depth')
        col=_ui_disclosure(box,'concrete_spall_open','Spall Surface')
        if col is not None:
            col.prop(p,'concrete_aggregate_size',text='Size (cm)')
            quality_ui.draw(col, p,'concrete_surface_detail',text='Detail')
            col.prop(p,'concrete_surface_distortion',text='Distortion')
            col.prop(p,'concrete_surface_roughness',text='Roughness')
            col.prop(p,'concrete_surface_strength',text='Strength')
        if generated_geometry.draw(layout, p, 'concrete'):
            col=toggle_box(layout,p,'concrete_rebar_enable','Reinforcement Bars')
            if col is not None:
                material_controls.inline(col, p, 'concrete_rebar_material', text='Rebar')
                if effect_layout.visible(p,'concrete_rebar_amount'):col.prop(p,'concrete_rebar_amount',text='Rebar Amount')
                if effect_layout.visible(p,'concrete_rebar_density'):col.prop(p,'concrete_rebar_density',text='Rebar Density')
                if effect_layout.visible(p,'concrete_rebar_radius'):col.prop(p,'concrete_rebar_radius',text='Rebar Radius (cm)')
                if effect_layout.visible(p,'concrete_rebar_scale'):col.prop(p,'concrete_rebar_scale',text='Rebar Scale')
                if effect_layout.visible(p,'concrete_rebar_spacing_x'):col.prop(p,'concrete_rebar_spacing_x',text='Rebar Spacing X (cm)')
                if effect_layout.visible(p,'concrete_rebar_spacing_y'):col.prop(p,'concrete_rebar_spacing_y',text='Rebar Spacing Y (cm)')
                if effect_layout.visible(p,'concrete_rebar_bars_x'):col.prop(p,'concrete_rebar_bars_x',text='Rebar Bars X')
                if effect_layout.visible(p,'concrete_rebar_bars_y'):col.prop(p,'concrete_rebar_bars_y',text='Rebar Bars Y')
                if effect_layout.visible(p,'concrete_rebar_outer'):col.prop(p,'concrete_rebar_outer',text='Rebar Outer')
                if effect_layout.visible(p,'concrete_rebar_angle'):col.prop(p,'concrete_rebar_angle',text='Rebar Angle')
                if effect_layout.visible(p,'concrete_rebar_cover'):col.prop(p,'concrete_rebar_cover',text='Rebars Depth (cm)')
                if effect_layout.visible(p,'concrete_rebar_expose'):col.prop(p,'concrete_rebar_expose',text='Rebar Expose')
                if effect_layout.visible(p,'concrete_rebar_embed'):col.prop(p,'concrete_rebar_embed',text='Rebar Embed (cm)')
                if effect_layout.visible(p,'concrete_rebar_ribs'):col.prop(p,'concrete_rebar_ribs',text='Rebar Ribs')
                quality_ui.draw(col, p,'concrete_rebar_resolution',slider=True)
                if effect_layout.visible(p,'concrete_rebar_sides'):col.prop(p,'concrete_rebar_sides',text='Rebar Sides')
                if effect_layout.visible(p,'concrete_rebar_jitter'):col.prop(p,'concrete_rebar_jitter',text='Rebar Jitter')
                if effect_layout.visible(p,'concrete_rebar_wobble'):col.prop(p,'concrete_rebar_wobble',text='Rebar Wobble')
                if effect_layout.visible(p,'concrete_rebar_bend'):col.prop(p,'concrete_rebar_bend',text='Rebar Bend')
                if effect_layout.visible(p,'concrete_rebar_broken'):col.prop(p,'concrete_rebar_broken',text='Rebar Broken')
                if effect_layout.visible(p,'concrete_up_x'):col.prop(p,'concrete_up_x',text='Up X')
                if effect_layout.visible(p,'concrete_up_y'):col.prop(p,'concrete_up_y',text='Up Y')
                if effect_layout.visible(p,'concrete_up_z'):col.prop(p,'concrete_up_z',text='Up Z')
        col=toggle_box(layout,p,'concrete_crack_enable','Cracks Around the Damage')
        if col is not None:
            material_controls.inline(col, p, 'fracture_l1_material', text='Cracks')
            if effect_layout.visible(p,'concrete_crack_amount'):col.prop(p,'concrete_crack_amount',text='Crack Amount')
            if effect_layout.visible(p,'concrete_crack_length_min'):col.prop(p,'concrete_crack_length_min',text='Crack Length Min (cm)')
            if effect_layout.visible(p,'concrete_crack_length_max'):col.prop(p,'concrete_crack_length_max',text='Crack Length Max (cm)')
            if effect_layout.visible(p,'concrete_crack_width'):col.prop(p,'concrete_crack_width',text='Crack Width (cm)')
            if effect_layout.visible(p,'concrete_crack_depth'):col.prop(p,'concrete_crack_depth',text='Crack Depth (cm)')
            if effect_layout.visible(p,'concrete_crack_meander'):col.prop(p,'concrete_crack_meander',text='Crack Meander')
            if effect_layout.visible(p,'concrete_crack_branch'):col.prop(p,'concrete_crack_branch',text='Crack Branch')
            if effect_layout.visible(p,'concrete_crack_interior'):col.prop(p,'concrete_crack_interior',text='Crack Interior')
        col=toggle_box(layout,p,'concrete_hole_enable','Through Holes')
        if col is not None and through_holes.draw_controls(col, p, 'concrete'):
            if effect_layout.visible(p,'concrete_hole_chance'):col.prop(p,'concrete_hole_chance',text='Hole Chance')
            if effect_layout.visible(p,'concrete_hole_size'):col.prop(p,'concrete_hole_size',text='Hole Size')
            if effect_layout.visible(p,'concrete_hole_jagged'):col.prop(p,'concrete_hole_jagged',text='Hole Jagged')



                                                          
                                                                          
class PROJECT_DAMAGE_OT_toggle_hit_core(bpy.types.Operator):
    bl_idname = 'project_damage.toggle_hit_core'
    bl_label = 'Enable / Disable Crater Core'
    bl_description = 'Disable sets Core Size to 0. Enable restores its default value (0.32)'
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return ownership.context_source(context) is not None

    def execute(self, context):
        p = ownership.context_source(context).project_damage
        p.hit_core_size = 0.0 if p.hit_core_size > 0.0 else p.bl_rna.properties['hit_core_size'].default
        return {'FINISHED'}


class OBJECT_PT_project_damage_hit(_ProjectDamagePanel):
    bl_idname = "OBJECT_PT_project_damage_hit"
    bl_parent_id = "OBJECT_PT_project_damage"
    bl_label = _EFFECT_LABELS["hit"]
    effect_module = "hit"

    def draw(self, context):
        p=ownership.context_source(context).project_damage
        layout=self.layout
        from .asset_updates import toggle_box
        layout.use_property_split=False;layout.use_property_decorate=False
        layout.column(align=True).prop(p,'hit_radius_scale',text='Proximity Radius')
        _effect_draw(SimpleNamespace(layout=layout,effect_module='hit'),context)
        box=effect_layout.section(layout,'General');col=box.column(align=True)
        if effect_layout.visible(p,'hit_detail'):quality_ui.draw(col, p,'hit_detail',text='Resolution')
        if effect_layout.visible(p,'hit_seed'):col.prop(p,'hit_seed',text='Seed')
        if effect_layout.visible(p,'hit_layer_thickness'):col.prop(p,'hit_layer_thickness',text='Layer Thickness')
        box=effect_layout.section(layout,'Plates');col=box.column(align=True)
        material_controls.inline(col, p, 'fracture_l2_material', text='Exposed Plates')
        if effect_layout.visible(p,'hit_plate_size'):col.prop(p,'hit_plate_size',text='Plate Size')
        if effect_layout.visible(p,'hit_plate_growth'):col.prop(p,'hit_plate_growth',text='Plate Growth')
        if effect_layout.visible(p,'hit_zone_jagged'):col.prop(p,'hit_zone_jagged',text='Zone Jagged')
        if effect_layout.visible(p,'hit_shatter'):col.prop(p,'hit_shatter',text='Shatter')
        if effect_layout.visible(p,'hit_tilt'):col.prop(p,'hit_tilt',text='Tilt')
        if effect_layout.visible(p,'hit_edge_jagged'):col.prop(p,'hit_edge_jagged',text='Edge Jagged')
        if effect_layout.visible(p,'hit_chip'):col.prop(p,'hit_chip',text='Chip')
        box=effect_layout.section(layout,'Crater Core')
        row=box.row(align=True)
        row.scale_y=1.5
        enabled=p.hit_core_size>0.0
        row.operator('project_damage.toggle_hit_core',
                     text='Disable Crater Core' if enabled else 'Enable Crater Core',
                     icon='CHECKMARK' if enabled else 'ADD', depress=enabled)
        col=box.column(align=True)
        material_controls.inline(col, p, 'surface_damage_material', text='Impact Crater')
        if effect_layout.visible(p,'hit_core_size'):col.prop(p,'hit_core_size',text='Core Size')
        if effect_layout.visible(p,'hit_core_depth'):col.prop(p,'hit_core_depth',text='Core Depth')
        if effect_layout.visible(p,'hit_core_jagged'):col.prop(p,'hit_core_jagged',text='Core Jagged')
        if effect_layout.visible(p,'hit_roughness'):col.prop(p,'hit_roughness',text='Roughness')
        if effect_layout.visible(p,'hit_aggregate_size'):col.prop(p,'hit_aggregate_size',text='Aggregate Size')
        if effect_layout.visible(p,'hit_thickness_cap'):col.prop(p,'hit_thickness_cap',text='Thickness Cap')
        if effect_layout.visible(p,'hit_hole_enable'):col.prop(p,'hit_hole_enable',text='Hole Enable')
        if p.hit_hole_enable and through_holes.draw_controls(col, p, 'hit'):
            if effect_layout.visible(p,'hit_hole_size'):col.prop(p,'hit_hole_size',text='Hole Size')
        col=toggle_box(layout,p,'hit_crack_enable','Cracks')
        if col is not None:
            material_controls.inline(col, p, 'fracture_l1_material', text='Cracks')
            if effect_layout.visible(p,'hit_crack_width'):col.prop(p,'hit_crack_width',text='Crack Width')
            if effect_layout.visible(p,'hit_crack_depth'):col.prop(p,'hit_crack_depth',text='Crack Depth')
            if effect_layout.visible(p,'hit_radial_count'):col.prop(p,'hit_radial_count',text='Radial Count')
            if effect_layout.visible(p,'hit_radial_length_min'):col.prop(p,'hit_radial_length_min',text='Radial Length Min')
            if effect_layout.visible(p,'hit_radial_length_max'):col.prop(p,'hit_radial_length_max',text='Radial Length Max')
            if effect_layout.visible(p,'hit_radial_meander'):col.prop(p,'hit_radial_meander',text='Radial Meander')
            if effect_layout.visible(p,'hit_radial_branch'):col.prop(p,'hit_radial_branch',text='Radial Branch')
            if effect_layout.visible(p,'hit_ring_count'):col.prop(p,'hit_ring_count',text='Ring Count')
            if effect_layout.visible(p,'hit_ring_fill'):col.prop(p,'hit_ring_fill',text='Ring Fill')



                                                                          
                                                              
                                                                          
class OBJECT_PT_project_damage_plaster(_ProjectDamagePanel):
    bl_idname = "OBJECT_PT_project_damage_plaster"
    bl_parent_id = "OBJECT_PT_project_damage"
    bl_label = _EFFECT_LABELS["plaster"]
    effect_module = "plaster"

    def draw(self, context):
        from .asset_updates import toggle_box
        p=ownership.context_source(context).project_damage
        layout=self.layout
        layout.use_property_split=False
        layout.use_property_decorate=False
        _effect_draw(SimpleNamespace(layout=layout,effect_module='plaster'),context)
        from .asset_updates import draw_plaster_zone
        draw_plaster_zone(layout,context,p)
        box=effect_layout.section(layout,'General');col=box.column(align=True)
        if effect_layout.visible(p,'plaster_master_scale'):col.prop(p,'plaster_master_scale',text='Master Scale')
        if effect_layout.visible(p,'plaster_seed'):col.prop(p,'plaster_seed',text='Seed')
        if effect_layout.visible(p,'plaster_detail'):quality_ui.draw(col, p,'plaster_detail',text='Resolution')
        if effect_layout.visible(p,'plaster_max_tris'):col.prop(p,'plaster_max_tris',text='Max Tris')
        if effect_layout.visible(p,'plaster_edge_length'):col.prop(p,'plaster_edge_length',text='Edge Length')
        box=effect_layout.section(layout,'Noise Settings');col=box.column(align=True)
        if effect_layout.visible(p,'plaster_strength'):col.prop(p,'plaster_strength',text='Strength')
        if effect_layout.visible(p,'plaster_sink'):col.prop(p,'plaster_sink',text='Sink')
        if effect_layout.visible(p,'plaster_thickness_cap'):col.prop(p,'plaster_thickness_cap',text='Thickness Cap')
        if effect_layout.visible(p,'plaster_undulation'):col.prop(p,'plaster_undulation',text='Undulation')
        if effect_layout.visible(p,'plaster_undulation_scale'):col.prop(p,'plaster_undulation_scale',text='Undulation Scale')
        box=effect_layout.section(layout,'Trowel Strokes');col=box.column(align=True)
        if effect_layout.visible(p,'plaster_coats'):col.prop(p,'plaster_coats',text='Coats')
        if effect_layout.visible(p,'plaster_coverage'):col.prop(p,'plaster_coverage',text='Coverage')
        if effect_layout.visible(p,'plaster_length_min'):col.prop(p,'plaster_length_min',text='Length Min')
        if effect_layout.visible(p,'plaster_length_max'):col.prop(p,'plaster_length_max',text='Length Max')
        if effect_layout.visible(p,'plaster_width_min'):col.prop(p,'plaster_width_min',text='Width Min')
        if effect_layout.visible(p,'plaster_width_max'):col.prop(p,'plaster_width_max',text='Width Max')
        if effect_layout.visible(p,'plaster_angle'):col.prop(p,'plaster_angle',text='Angle')
        if effect_layout.visible(p,'plaster_angle_random'):col.prop(p,'plaster_angle_random',text='Angle Random')
        if effect_layout.visible(p,'plaster_cross_chance'):col.prop(p,'plaster_cross_chance',text='Cross Chance')
        if effect_layout.visible(p,'plaster_curve'):col.prop(p,'plaster_curve',text='Curve')
        if effect_layout.visible(p,'plaster_flatten'):col.prop(p,'plaster_flatten',text='Flatten')
        if effect_layout.visible(p,'plaster_tilt'):col.prop(p,'plaster_tilt',text='Tilt')
        if effect_layout.visible(p,'plaster_jagged'):col.prop(p,'plaster_jagged',text='Jagged')
        if effect_layout.visible(p,'plaster_streaks'):col.prop(p,'plaster_streaks',text='Streaks')
        box=effect_layout.section(layout,'Ridges');col=box.column(align=True)
        if effect_layout.visible(p,'plaster_ridge_height'):col.prop(p,'plaster_ridge_height',text='Ridge Height')
        if effect_layout.visible(p,'plaster_ridge_width'):col.prop(p,'plaster_ridge_width',text='Ridge Width')
        if effect_layout.visible(p,'plaster_ridge_sharp'):col.prop(p,'plaster_ridge_sharp',text='Ridge Sharp')
        if effect_layout.visible(p,'plaster_ridge_trail'):col.prop(p,'plaster_ridge_trail',text='Ridge Trail')
        box=effect_layout.section(layout,'Grain');col=box.column(align=True)
        if effect_layout.visible(p,'plaster_grain'):col.prop(p,'plaster_grain',text='Grain')
        if effect_layout.visible(p,'plaster_grain_size'):col.prop(p,'plaster_grain_size',text='Grain Size')
        if effect_layout.visible(p,'plaster_pits'):col.prop(p,'plaster_pits',text='Pits')
        col=toggle_box(layout,p,'plaster_crack_enable','Hairline Cracks')
        if col is not None:
            if effect_layout.visible(p,'plaster_crack_amount'):col.prop(p,'plaster_crack_amount',text='Crack Amount')
            if effect_layout.visible(p,'plaster_crack_scale'):col.prop(p,'plaster_crack_scale',text='Crack Scale')
            if effect_layout.visible(p,'plaster_crack_width'):col.prop(p,'plaster_crack_width',text='Crack Width')
            if effect_layout.visible(p,'plaster_crack_depth'):col.prop(p,'plaster_crack_depth',text='Crack Depth')
            if effect_layout.visible(p,'plaster_crack_meander'):col.prop(p,'plaster_crack_meander',text='Crack Meander')
            if effect_layout.visible(p,'plaster_crack_breakup'):col.prop(p,'plaster_crack_breakup',text='Crack Breakup')
            if effect_layout.visible(p,'plaster_crack_step'):col.prop(p,'plaster_crack_step',text='Crack Step')
            if effect_layout.visible(p,'plaster_crack_lip'):col.prop(p,'plaster_crack_lip',text='Crack Lip')
            if effect_layout.visible(p,'plaster_up_x'):col.prop(p,'plaster_up_x',text='Up X')
            if effect_layout.visible(p,'plaster_up_y'):col.prop(p,'plaster_up_y',text='Up Y')
            if effect_layout.visible(p,'plaster_up_z'):col.prop(p,'plaster_up_z',text='Up Z')



                                                                          
                                                           
                                                                          
class OBJECT_PT_project_damage_wood(_ProjectDamagePanel):
    bl_idname = "OBJECT_PT_project_damage_wood"
    bl_parent_id = "OBJECT_PT_project_damage"
    bl_label = _EFFECT_LABELS["wood"]
    effect_module = "wood"

    def draw(self, context):
        p = ownership.context_source(context).project_damage
        layout = self.layout
        layout.use_property_split = False
        layout.use_property_decorate = False

        box = layout.box()
        row = box.row(align=True)
        row.alignment = 'CENTER'
        _effect_draw(SimpleNamespace(layout=box, effect_module="wood"), context)

                 
        if (
            effect_layout.visible(p, "wood_seed")
            or effect_layout.visible(p, "wood_avoid")
            or effect_layout.visible(p, "wood_detail")
        ):
            box = layout.box()
            box.enabled = p.wood_enable and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="General", icon=effect_layout.heading_icon("General"))
            col = box.column(align=True)
            quality_ui.draw(col, p, "wood_detail", text="Resolution")
            col.prop(p, "wood_seed", text="Seed")
            pass

                         
        if (
            effect_layout.visible(p, "wood_grain_mode")
            or effect_layout.visible(p, "wood_grain_x")
            or effect_layout.visible(p, "wood_grain_y")
            or effect_layout.visible(p, "wood_grain_z")
        ):
            box = layout.box()
            box.enabled = p.wood_enable and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Grain direction", icon=effect_layout.heading_icon("Grain direction"))
            col = box.column(align=True)
            col.prop(p, "wood_grain_mode", text="Grain Mode")
            col.prop(p, "wood_grain_x", text="Grain X")
            col.prop(p, "wood_grain_y", text="Grain Y")
            col.prop(p, "wood_grain_z", text="Grain Z")

                             
        if (
            p.wood_mode != '1' and (
            effect_layout.visible(p, "wood_amount")
            or effect_layout.visible(p, "wood_size_min")
            or effect_layout.visible(p, "wood_size_max")
            or effect_layout.visible(p, "wood_lobes")
            or effect_layout.visible(p, "wood_stretch")
            or effect_layout.visible(p, "wood_end_bias")
            or effect_layout.visible(p, "wood_edge_bias"))
        ):
            box = layout.box()
            box.enabled = p.wood_enable and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Automatic placement", icon=effect_layout.heading_icon("Automatic placement"))
            col = box.column(align=True)
            col.prop(p, "wood_amount", text="Amount")
            col.prop(p, "wood_size_min", text="Size Min")
            col.prop(p, "wood_size_max", text="Size Max")
            col.prop(p, "wood_lobes", text="Lobes")
            col.prop(p, "wood_stretch", text="Stretch")
            col.prop(p, "wood_end_bias", text="End Bias")
            col.prop(p, "wood_edge_bias", text="Edge Bias")

                    
        if (
            p.wood_mode == '1' and (
            effect_layout.visible(p, "wood_paint_radius")
            or effect_layout.visible(p, "wood_paint_snap")
            or effect_layout.visible(p, "wood_paint_close"))
        ):
            box = layout.box()
            box.enabled = p.wood_enable and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Paint mode", icon=effect_layout.heading_icon("Paint mode"))
            col = box.column(align=True)
            col.prop(p, "wood_paint_radius", text="Paint Radius")
            col.prop(p, "wood_paint_snap", text="Paint Snap")
            col.prop(p, "wood_paint_close", text="Paint Close")

                       
        if (
            effect_layout.visible(p, "wood_depth_min")
            or effect_layout.visible(p, "wood_depth_max")
            or effect_layout.visible(p, "wood_layers")
            or effect_layout.visible(p, "wood_layer_inset")
            or effect_layout.visible(p, "wood_fiber_depth_var")
            or effect_layout.visible(p, "wood_thickness_cap")
            or effect_layout.visible(p, "wood_jagged")
        ):
            box = layout.box()
            box.enabled = p.wood_enable and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Torn-out wood", icon=effect_layout.heading_icon("Torn-out wood"))
            col = box.column(align=True)
            material_controls.inline(box, p, 'surface_damage_material', text='Exposed Wood')
            col.prop(p, "wood_depth_min", text="Depth Min")
            col.prop(p, "wood_depth_max", text="Depth Max")
            col.prop(p, "wood_layers", text="Layers")
            col.prop(p, "wood_layer_inset", text="Layer Inset")
            col.prop(p, "wood_fiber_depth_var", text="Fiber Depth Var")
            col.prop(p, "wood_thickness_cap", text="Thickness Cap")
            col.prop(p, "wood_jagged", text="Jagged")

                                                    
        if (
            effect_layout.visible(p, "wood_fiber_width")
            or effect_layout.visible(p, "wood_fiber_jitter")
            or effect_layout.visible(p, "wood_splinter_length")
            or effect_layout.visible(p, "wood_splinter_sharp")
            or effect_layout.visible(p, "wood_splinter_chunk")
            or effect_layout.visible(p, "wood_splinter_ramp")
            or effect_layout.visible(p, "wood_floor_fibers")
        ):
            box = layout.box()
            box.enabled = p.wood_enable and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Splinters (fibre strips with pointed ends)", icon=effect_layout.heading_icon("Splinters (fibre strips with pointed ends)"))
            col = box.column(align=True)
            col.prop(p, "wood_fiber_width", text="Fiber Width")
            col.prop(p, "wood_fiber_jitter", text="Fiber Jitter")
            col.prop(p, "wood_splinter_length", text="Splinter Length")
            col.prop(p, "wood_splinter_sharp", text="Splinter Sharp")
            col.prop(p, "wood_splinter_chunk", text="Splinter Chunk")
            col.prop(p, "wood_splinter_ramp", text="Splinter Ramp")
            col.prop(p, "wood_floor_fibers", text="Floor Fibers")

                                 
        if (
            effect_layout.visible(p, "wood_groove_enable")
            or effect_layout.visible(p, "wood_groove_spacing")
            or effect_layout.visible(p, "wood_groove_depth")
            or effect_layout.visible(p, "wood_groove_wave")
            or effect_layout.visible(p, "wood_groove_breakup")
            or effect_layout.visible(p, "wood_weather_halo")
        ):
            box = layout.box()
            box.enabled = p.wood_enable and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Weathered grain grooves", icon=effect_layout.heading_icon("Weathered grain grooves"))
            col = box.column(align=True)
            col.prop(p, "wood_groove_enable", text="Groove Enable")
            col.prop(p, "wood_groove_spacing", text="Groove Spacing")
            col.prop(p, "wood_groove_depth", text="Groove Depth")
            col.prop(p, "wood_groove_wave", text="Groove Wave")
            col.prop(p, "wood_groove_breakup", text="Groove Breakup")
            col.prop(p, "wood_weather_halo", text="Weather Halo")

                                       
        if (
            effect_layout.visible(p, "wood_break_enable")
            or effect_layout.visible(p, "wood_break_chance")
            or effect_layout.visible(p, "wood_break_size")
        ):
            box = layout.box()
            box.enabled = p.wood_enable and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Break through (broken boards)", icon=effect_layout.heading_icon("Break through (broken boards)"))
            col = box.column(align=True)
            col.prop(p, "wood_break_enable", text="Break Enable")
            if p.wood_break_enable and through_holes.draw_controls(col, p, 'wood'):
                col.prop(p, "wood_break_chance", text="Break Chance")
                col.prop(p, "wood_break_size", text="Break Size")

                                                  
        if (
            effect_layout.visible(p, "wood_check_enable")
            or effect_layout.visible(p, "wood_check_amount")
            or effect_layout.visible(p, "wood_check_global")
            or effect_layout.visible(p, "wood_check_length_min")
            or effect_layout.visible(p, "wood_check_length_max")
            or effect_layout.visible(p, "wood_check_width")
            or effect_layout.visible(p, "wood_check_depth")
            or effect_layout.visible(p, "wood_check_meander")
        ):
            box = layout.box()
            box.enabled = p.wood_enable and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Splits / checks (cracks along the grain)", icon=effect_layout.heading_icon("Splits / checks (cracks along the grain)"))
            col = box.column(align=True)
            material_controls.inline(box, p, 'fracture_l1_material', text='Checks / Splits')
            col.prop(p, "wood_check_enable", text="Check Enable")
            col.prop(p, "wood_check_amount", text="Check Amount")
            col.prop(p, "wood_check_global", text="Check Global")
            col.prop(p, "wood_check_length_min", text="Check Length Min")
            col.prop(p, "wood_check_length_max", text="Check Length Max")
            col.prop(p, "wood_check_width", text="Check Width")
            col.prop(p, "wood_check_depth", text="Check Depth")
            col.prop(p, "wood_check_meander", text="Check Meander")

               
        if (
            effect_layout.visible(p, "wood_knot_amount")
            or effect_layout.visible(p, "wood_knot_size")
            or effect_layout.visible(p, "wood_knot_stretch")
            or effect_layout.visible(p, "wood_knot_depth")
            or effect_layout.visible(p, "wood_knot_checks")
        ):
            box = layout.box()
            box.enabled = p.wood_enable and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Knots", icon=effect_layout.heading_icon("Knots"))
            col = box.column(align=True)
            col.prop(p, "wood_knot_amount", text="Knot Amount")
            col.prop(p, "wood_knot_size", text="Knot Size")
            col.prop(p, "wood_knot_stretch", text="Knot Stretch")
            col.prop(p, "wood_knot_depth", text="Knot Depth")
            col.prop(p, "wood_knot_checks", text="Knot Checks")

                                             
        if (
            effect_layout.visible(p, "wood_sliver_enable")
            or effect_layout.visible(p, "wood_sliver_amount")
            or effect_layout.visible(p, "wood_sliver_length")
            or effect_layout.visible(p, "wood_sliver_width")
            or effect_layout.visible(p, "wood_sliver_thick")
            or effect_layout.visible(p, "wood_sliver_lift")
            or effect_layout.visible(p, "wood_sliver_embed")
        ):
            box = layout.box()
            box.enabled = p.wood_enable and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Loose splinters (solids added back)", icon=effect_layout.heading_icon("Loose splinters (solids added back)"))
            col = box.column(align=True)
            material_controls.inline(box, p, 'buildup_material', text='Splinters')
            col.prop(p, "wood_sliver_enable", text="Sliver Enable")
            col.prop(p, "wood_sliver_amount", text="Sliver Amount")
            col.prop(p, "wood_sliver_length", text="Sliver Length")
            col.prop(p, "wood_sliver_width", text="Sliver Width")
            col.prop(p, "wood_sliver_thick", text="Sliver Thick")
            col.prop(p, "wood_sliver_lift", text="Sliver Lift")
            col.prop(p, "wood_sliver_embed", text="Sliver Embed")


                                                                          
                                                           
                                                                          
class OBJECT_PT_project_damage_lath(_ProjectDamagePanel):
    bl_idname = "OBJECT_PT_project_damage_lath"
    bl_parent_id = "OBJECT_PT_project_damage"
    bl_label = _EFFECT_LABELS["lath"]
    effect_module = "lath"

    def draw(self, context):
        from .asset_updates import toggle_box
        p=ownership.context_source(context).project_damage
        layout=self.layout
        layout.use_property_split=False
        layout.use_property_decorate=False
        _effect_draw(SimpleNamespace(layout=layout,effect_module='lath'),context)
        box=effect_layout.section(layout,'General');col=box.column(align=True)
        if p.lath_generated_enable:col.prop(p,'lath_wood_master_scale')
        if effect_layout.visible(p,'lath_detail'):quality_ui.draw(col, p,'lath_detail',text='Resolution')
        if effect_layout.visible(p,'lath_seed'):col.prop(p,'lath_seed',text='Seed')
        box=effect_layout.section(layout,'Automatic Placement');col=box.column(align=True)
        if effect_layout.visible(p,'lath_amount'):col.prop(p,'lath_amount',text='Amount')
        if effect_layout.visible(p,'lath_size_min'):col.prop(p,'lath_size_min',text='Size Min')
        if effect_layout.visible(p,'lath_size_max'):col.prop(p,'lath_size_max',text='Size Max')
        if effect_layout.visible(p,'lath_lobes'):col.prop(p,'lath_lobes',text='Lobes')
        if effect_layout.visible(p,'lath_stretch'):col.prop(p,'lath_stretch',text='Stretch')
        if effect_layout.visible(p,'lath_edge_bias'):col.prop(p,'lath_edge_bias',text='Edge Bias')
        if p.lath_mode=="1":
            box=effect_layout.section(layout,"Paint Settings");col=box.column(align=True)
            if effect_layout.visible(p,'lath_paint_radius'):col.prop(p,'lath_paint_radius',text='Paint Radius')
            if effect_layout.visible(p,'lath_paint_snap'):col.prop(p,'lath_paint_snap',text='Paint Snap')
            if effect_layout.visible(p,'lath_paint_close'):col.prop(p,'lath_paint_close',text='Paint Close')
        box=effect_layout.section(layout,'Plaster');col=box.column(align=True)
        material_controls.inline(col, p, 'surface_damage_material', text='Exposed Layers')
        if effect_layout.visible(p,'lath_wall_soft'):col.prop(p,'lath_wall_soft',text='Wall Soft')
        if p.lath_v2_enable:
            col.separator(factor=.5)
            col.prop(p,'lath_smooth_width',text='Smooth Walls')
            col.separator(factor=.5)
        if p.lath_framing_enable:
            layer = 'Studs'
        elif p.lath_v2_enable and not p.lath_riven_enable:
            layer = 'Planks'
        else:
            layer = 'Laths'
        if effect_layout.visible(p, 'lath_plaster_min'):
            col.prop(p, 'lath_plaster_min', text=f'Plaster Min / {layer} Depth Min')
        if effect_layout.visible(p, 'lath_plaster_max'):
            col.prop(p, 'lath_plaster_max', text=f'Plaster Max / {layer} Depth Max')
        if effect_layout.visible(p,'lath_jagged'):col.prop(p,'lath_jagged',text='Jagged')
        if effect_layout.visible(p,'lath_outline_scale'):col.prop(p,'lath_outline_scale',text='Outline Scale')
        if effect_layout.visible(p,'lath_crumble'):col.prop(p,'lath_crumble',text='Crumble')
        if effect_layout.visible(p,'lath_crumble_size'):col.prop(p,'lath_crumble_size',text='Crumble Size')
        if effect_layout.visible(p,'lath_terraces'):col.prop(p,'lath_terraces',text='Terraces')
        if effect_layout.visible(p,'lath_terrace_skin'):col.prop(p,'lath_terrace_skin',text='Terrace Skin')
        if effect_layout.visible(p,'lath_terrace_inset'):col.prop(p,'lath_terrace_inset',text='Terrace Inset')
        if effect_layout.visible(p,'lath_roughness'):col.prop(p,'lath_roughness',text='Roughness')
        if effect_layout.visible(p,'lath_thickness_cap'):col.prop(p,'lath_thickness_cap',text='Thickness Cap')
        col.prop(p,'lath_edge_wall_depth')
        if not p.lath_framing_enable:
            col=_ui_disclosure(box,'riven_cavity_open' if p.lath_riven_enable else 'lath_cavity_open','Cavity Behind the Laths')
            if col is not None:
                col.prop(p,'lath_cavity_depth',text='Cavity Depth')
                col.prop(p,'lath_keys',text='Keys')
                col.prop(p,'lath_key_size',text='Key Size')
        back=_ui_disclosure(box,'framing_back_open' if p.lath_framing_enable else 'riven_back_open' if p.lath_riven_enable else 'lath_back_open','Back Surface Noise')
        if back is not None:
            row=back.row(align=True);row.scale_y=1.5
            for preset,label in (('CONCRETE','Concrete'),('SOFT','Soft Concrete'),('FLAT','Flat')):
                row.operator('project_damage.back_noise_preset',text=label).preset=preset
            from . import asset_noise
            asset_noise.draw(back,p,'lath')
        if generated_geometry.draw(layout, p, 'lath'):
            if p.lath_riven_enable:draw_riven_studs(layout,p)
            if p.lath_framing_enable:draw_stud_framing_settings(layout,p)
            col=None if p.lath_framing_enable else toggle_box(layout,p,'lath_lath_enable','Planks' if p.lath_v2_enable else 'Laths')
            if col is not None:
                material_controls.inline(col, p, 'lath_material', text='Wooden Laths')
                if effect_layout.visible(p,'lath_lath_amount'):col.prop(p,'lath_lath_amount',text='Lath Amount')
                if effect_layout.visible(p,'lath_lath_width'):col.prop(p,'lath_lath_width',text='Lath Width')
                if effect_layout.visible(p,'lath_lath_thickness'):col.prop(p,'lath_lath_thickness',text='Lath Thickness')
                if effect_layout.visible(p,'lath_lath_gap'):col.prop(p,'lath_lath_gap',text='Lath Gap')
                if effect_layout.visible(p,'lath_lath_scale'):col.prop(p,'lath_lath_scale',text='Lath Scale')
                if effect_layout.visible(p,'lath_lath_angle'):col.prop(p,'lath_lath_angle',text='Lath Angle')
                if effect_layout.visible(p,'lath_lath_length'):col.prop(p,'lath_lath_length',text='Lath Length')
                if effect_layout.visible(p,'lath_lath_embed'):col.prop(p,'lath_lath_embed',text='Lath Embed')
                if effect_layout.visible(p,'lath_lath_jitter'):col.prop(p,'lath_lath_jitter',text='Lath Jitter')
                if effect_layout.visible(p,'lath_lath_tilt'):col.prop(p,'lath_lath_tilt',text='Lath Tilt')
                if effect_layout.visible(p,'lath_lath_warp'):col.prop(p,'lath_lath_warp',text='Lath Warp')
                if effect_layout.visible(p,'lath_lath_edge'):col.prop(p,'lath_lath_edge',text='Lath Edge')
                if effect_layout.visible(p,'lath_lath_missing'):col.prop(p,'lath_lath_missing',text='Lath Missing')
                if effect_layout.visible(p,'lath_lath_broken'):col.prop(p,'lath_lath_broken',text='Lath Broken')
                if effect_layout.visible(p,'lath_lath_bend'):col.prop(p,'lath_lath_bend',text='Lath Bend')
                if effect_layout.visible(p,'lath_lath_splinter'):col.prop(p,'lath_lath_splinter',text='Lath Splinter')
                if effect_layout.visible(p,'lath_up_x'):col.prop(p,'lath_up_x',text='Up X')
                if effect_layout.visible(p,'lath_up_y'):col.prop(p,'lath_up_y',text='Up Y')
                if effect_layout.visible(p,'lath_up_z'):col.prop(p,'lath_up_z',text='Up Z')
                if p.lath_riven_enable:draw_riven_settings(col,p)
        col=toggle_box(layout,p,'lath_crack_enable','Plaster Cracks')
        if col is not None:
            material_controls.inline(col, p, 'fracture_l1_material', text='Cracks')
            if effect_layout.visible(p,'lath_crack_amount'):col.prop(p,'lath_crack_amount',text='Crack Amount')
            if effect_layout.visible(p,'lath_crack_length_min'):col.prop(p,'lath_crack_length_min',text='Crack Length Min')
            if effect_layout.visible(p,'lath_crack_length_max'):col.prop(p,'lath_crack_length_max',text='Crack Length Max')
            if effect_layout.visible(p,'lath_crack_width'):col.prop(p,'lath_crack_width',text='Crack Width')
            if effect_layout.visible(p,'lath_crack_depth'):col.prop(p,'lath_crack_depth',text='Crack Depth')
            if effect_layout.visible(p,'lath_crack_meander'):col.prop(p,'lath_crack_meander',text='Crack Meander')
            if effect_layout.visible(p,'lath_crack_branch'):col.prop(p,'lath_crack_branch',text='Crack Branch')
            if effect_layout.visible(p,'lath_crack_interior'):col.prop(p,'lath_crack_interior',text='Crack Interior')
        holes=_ui_disclosure(layout,'lath_holes_open','Through Holes')
        col=toggle_box(holes,p,'lath_hole_enable','Enable Through Holes') if holes is not None else None
        if col is not None and through_holes.draw_controls(col, p, 'lath'):
            if effect_layout.visible(p,'lath_hole_chance'):col.prop(p,'lath_hole_chance',text='Hole Chance')
            if effect_layout.visible(p,'lath_hole_size'):col.prop(p,'lath_hole_size',text='Hole Size')
            if effect_layout.visible(p,'lath_hole_jagged'):col.prop(p,'lath_hole_jagged',text='Hole Jagged')
            if p.lath_generated_enable and effect_layout.visible(p,'lath_hole_laths'):col.prop(p,'lath_hole_laths',text='Hole Laths')
        box=effect_layout.section(layout,'Smoothing and Optimization');col=box.column(align=True)
        if effect_layout.visible(p,'lath_cavity_smooth'):col.prop(p,'lath_cavity_smooth',text='Cavity Smooth')
        if effect_layout.visible(p,'lath_rim_smooth'):col.prop(p,'lath_rim_smooth',text='Rim Smooth')
        if p.lath_generated_enable and effect_layout.visible(p,'lath_lath_optimize'):col.prop(p,'lath_lath_optimize',text='Lath Optimize')
        box=effect_layout.section(layout,'Clean Walls and Holes');col=box.column(align=True)
        if effect_layout.visible(p,'lath_wall_slope'):col.prop(p,'lath_wall_slope',text='Wall Slope')
        if effect_layout.visible(p,'lath_cavity_slope'):col.prop(p,'lath_cavity_slope',text='Cavity Slope')
        if effect_layout.visible(p,'lath_hole_noise_scale'):col.prop(p,'lath_hole_noise_scale',text='Hole Noise Scale')
        if effect_layout.visible(p,'lath_hole_detail'):quality_ui.draw(col, p,'lath_hole_detail',text='Hole Resolution')



                                                                          
                                                                
                                                                          
# действие «Add Wallpaper Finder»
class PROJECT_DAMAGE_OT_wallpaper_finder_add(bpy.types.Operator):
    bl_idname = 'project_damage.wallpaper_finder_add'
    bl_label = 'Add Wallpaper Finder'
    bl_description = 'Create an Empty at the 3D cursor and use its position to choose the wallpaper side'
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        source = ownership.context_source(context)
        return (context.mode == 'OBJECT' and source is not None and source.type == 'MESH'
                and source.project_damage.wallpaper_enable)

    def execute(self, context):
        from mathutils import Matrix
        from .params import unit_factor

        source = ownership.context_source(context)
        props = source.project_damage
        collection = props.wallpaper_finders
        if collection is None:
            collection = bpy.data.collections.new(source.name + ' Wallpaper Finders')
            context.scene.collection.children.link(collection)
            props.wallpaper_finders = collection
            if props.wallpaper_finder is not None and props.wallpaper_finder.name not in collection.objects:
                collection.objects.link(props.wallpaper_finder)
        finder = None
        if finder is None:
            finder = bpy.data.objects.new(source.name + ' Wallpaper Finder', None)
            finder.empty_display_type = 'SPHERE'
            finder.empty_display_size = 10.0 / unit_factor(context)
            finder.show_in_front = True
            collection.objects.link(finder)
            finder.parent = source
            finder.matrix_world = Matrix.Translation(context.scene.cursor.location)
            finder.project_damage_og_object = source
            props.wallpaper_finder = None
            context.view_layer.update()
        if finder.name in context.view_layer.objects:
            for selected in context.selected_objects:
                selected.select_set(False)
            finder.hide_set(False)
            finder.select_set(True)
            context.view_layer.objects.active = finder
        return {'FINISHED'}


class OBJECT_PT_project_damage_wallpaper(_ProjectDamagePanel):
    bl_idname = "OBJECT_PT_project_damage_wallpaper"
    bl_parent_id = "OBJECT_PT_project_damage"
    bl_label = _EFFECT_LABELS["wallpaper"]
    effect_module = "wallpaper"

    def draw(self, context):
        from .asset_updates import toggle_box
        p=ownership.context_source(context).project_damage
        layout=self.layout
        layout.use_property_split=False
        layout.use_property_decorate=False
        _effect_draw(SimpleNamespace(layout=layout,effect_module='wallpaper'),context)
        layout.prop(p,'wallpaper_master_scale')
        if p.wallpaper_paint_enable:
            box=effect_layout.section(layout,"Drawing Settings");col=box.column(align=True)
            if effect_layout.visible(p,'wallpaper_paint_reach'):col.prop(p,'wallpaper_paint_reach',text='Paint Reach')
            if effect_layout.visible(p,'wallpaper_paint_jagged'):col.prop(p,'wallpaper_paint_jagged',text='Paint Jagged')
            if effect_layout.visible(p,'wallpaper_seam_snap'):col.prop(p,'wallpaper_seam_snap',text='Seam Snap')
        box = effect_layout.section(layout, 'Wallpaper Finder')
        row = box.row(align=True)
        row.prop(p, 'wallpaper_finders', text='Finders')
        row.operator('project_damage.wallpaper_finder_add', text='', icon='ADD')
        if p.wallpaper_finder is not None:
            box.prop(p, 'wallpaper_finder', text='Legacy Finder')
        box.operator('project_damage.wallpaper_finder_add', text='Add Wallpaper Finder', icon='EMPTY_DATA')
        hint = box.row(align=True)
        hint.label(text='Place the Finder on the side to cover.', icon='INFO')
        ui_image_hints.button(hint, 'wallpaper_finder')
        box=effect_layout.section(layout,'General');col=box.column(align=True)
        if effect_layout.visible(p,'wallpaper_detail'):quality_ui.draw(col, p,'wallpaper_detail',text='Resolution')
        if effect_layout.visible(p,'wallpaper_seed'):col.prop(p,'wallpaper_seed',text='Seed')
        box=effect_layout.section(layout,'Paper and Strips');col=box.column(align=True)
        material_controls.inline(col, p, 'wallpaper_material', text='Printed Paper')
        material_controls.inline(col, p, 'wallpaper_back_material', text='Paper Backing')
        material_controls.inline(col, p, 'wall_material', text='Exposed Wall')
        if effect_layout.visible(p,'wallpaper_thickness'):col.prop(p,'wallpaper_thickness',text='Thickness')
        if effect_layout.visible(p,'wallpaper_strip_width'):col.prop(p,'wallpaper_strip_width',text='Strip Width')
        if effect_layout.visible(p,'wallpaper_seam_offset'):col.prop(p,'wallpaper_seam_offset',text='Seam Offset')
        if effect_layout.visible(p,'wallpaper_seam_dir_x'):col.prop(p,'wallpaper_seam_dir_x',text='Seam Dir X')
        if effect_layout.visible(p,'wallpaper_seam_dir_y'):col.prop(p,'wallpaper_seam_dir_y',text='Seam Dir Y')
        if effect_layout.visible(p,'wallpaper_seam_dir_z'):col.prop(p,'wallpaper_seam_dir_z',text='Seam Dir Z')
        if effect_layout.visible(p,'wallpaper_wall_angle'):col.prop(p,'wallpaper_wall_angle',text='Wall Angle')
        col=toggle_box(layout,p,'wallpaper_seams_enable','Seams')
        if col is not None:
            if effect_layout.visible(p,'wallpaper_seam_width'):col.prop(p,'wallpaper_seam_width',text='Seam Width')
            if effect_layout.visible(p,'wallpaper_seam_depth'):col.prop(p,'wallpaper_seam_depth',text='Seam Depth')
            if effect_layout.visible(p,'wallpaper_seam_open'):col.prop(p,'wallpaper_seam_open',text='Seam Open')
        col = toggle_box(layout, p, 'wallpaper_tears_enable', 'Tears',
                         show_content=p.wallpaper_drawing is not None)
        if col is not None and p.wallpaper_tears_enable:
            col.prop(p, 'wallpaper_flap_mode', expand=True)
            if p.wallpaper_flap_mode != 'EMPTIES':
                col.prop(p,'wallpaper_tear_amount',text='Flaps Amount')
            if p.wallpaper_flap_mode != 'AUTO':
                row = col.row(align=True)
                row.prop(p, 'wallpaper_flap_collection', text='Flap Empties')
                row.operator('project_damage.wallpaper_flap_add', text='', icon='ADD')
            else:
                col.operator('project_damage.wallpaper_flap_add', text='Add Flap Empty', icon='EMPTY_DATA')
            col.label(text='Move Empty: strip / height. Scale: tear length.', icon='INFO')
            if effect_layout.visible(p,'wallpaper_tear_length_min'):col.prop(p,'wallpaper_tear_length_min',text='Tear Length Min')
            if effect_layout.visible(p,'wallpaper_tear_length_max'):col.prop(p,'wallpaper_tear_length_max',text='Tear Length Max')
            if effect_layout.visible(p,'wallpaper_tear_width_min'):col.prop(p,'wallpaper_tear_width_min',text='Tear Width Min')
            if effect_layout.visible(p,'wallpaper_tear_width_max'):col.prop(p,'wallpaper_tear_width_max',text='Tear Width Max')
            if effect_layout.visible(p,'wallpaper_tear_diagonal'):col.prop(p,'wallpaper_tear_diagonal',text='Tear Diagonal')
            col.prop(p, 'wallpaper_tear_diagonal_randomness', slider=True)
            if effect_layout.visible(p,'wallpaper_tear_from_top'):col.prop(p,'wallpaper_tear_from_top',text='Tear From Top')
            if p.wallpaper_flap_mode != 'EMPTIES':
                col.prop(p, 'wallpaper_tear_from_top_randomness', slider=True)
            if effect_layout.visible(p,'wallpaper_tear_side'):col.prop(p,'wallpaper_tear_side',text='Tear Side')
            if effect_layout.visible(p,'wallpaper_tear_jagged'):col.prop(p,'wallpaper_tear_jagged',text='Tear Jagged')
            if effect_layout.visible(p,'wallpaper_seam_crack'):col.prop(p,'wallpaper_seam_crack',text='Seam Crack')
            if effect_layout.visible(p,'wallpaper_seam_crack_length'):col.prop(p,'wallpaper_seam_crack_length',text='Seam Crack Length')
        if col is not None:
            flaps = toggle_box(col, p, 'wallpaper_flaps_enable', 'Flaps')
            if flaps is not None:
                if effect_layout.visible(p,'wallpaper_flap_amount'):flaps.prop(p,'wallpaper_flap_amount',text='Hanging Paper')
                if effect_layout.visible(p,'wallpaper_flap_fold'):flaps.prop(p,'wallpaper_flap_fold',text='Flap Fold')
                if effect_layout.visible(p,'wallpaper_flap_fold_radius'):flaps.prop(p,'wallpaper_flap_fold_radius',text='Flap Fold Radius')
                if effect_layout.visible(p,'wallpaper_flap_curl'):flaps.prop(p,'wallpaper_flap_curl',text='Flap Curl')
                flaps.prop(p, 'wallpaper_flap_curl_fold_randomness', slider=True)
                if effect_layout.visible(p,'wallpaper_flap_curl_focus'):flaps.prop(p,'wallpaper_flap_curl_focus',text='Flap Curl Focus')
                if effect_layout.visible(p,'wallpaper_flap_cone'):flaps.prop(p,'wallpaper_flap_cone',text='Flap Cone')
                if effect_layout.visible(p,'wallpaper_flap_wave'):flaps.prop(p,'wallpaper_flap_wave',text='Flap Wave')
                flaps.prop(p, 'wallpaper_flap_wave_randomness', slider=True)
                if effect_layout.visible(p,'wallpaper_flap_ragged'):flaps.prop(p,'wallpaper_flap_ragged',text='Flap Ragged')
        col=toggle_box(layout,p,'wallpaper_lifts_enable','Lifts')
        if col is not None:
            if effect_layout.visible(p,'wallpaper_lift_amount'):col.prop(p,'wallpaper_lift_amount',text='Lift Amount')
            if effect_layout.visible(p,'wallpaper_lift_length_min'):col.prop(p,'wallpaper_lift_length_min',text='Lift Length Min')
            if effect_layout.visible(p,'wallpaper_lift_length_max'):col.prop(p,'wallpaper_lift_length_max',text='Lift Length Max')
            if effect_layout.visible(p,'wallpaper_lift_width_min'):col.prop(p,'wallpaper_lift_width_min',text='Lift Width Min')
            if effect_layout.visible(p,'wallpaper_lift_width_max'):col.prop(p,'wallpaper_lift_width_max',text='Lift Width Max')
            if effect_layout.visible(p,'wallpaper_lift_curl'):col.prop(p,'wallpaper_lift_curl',text='Lift Curl')



                                                                          
                                                             
                                                                          
class OBJECT_PT_project_damage_bullet(_ProjectDamagePanel):
    bl_idname = "OBJECT_PT_project_damage_bullet"
    bl_parent_id = "OBJECT_PT_project_damage"
    bl_label = _EFFECT_LABELS["bullet"]
    effect_module = "bullet"

    def draw(self, context):
        p=ownership.context_source(context).project_damage
        layout=self.layout
        from .asset_updates import toggle_box
        row=layout.row(align=True);row.scale_y=1.5
        row.prop(p,'bullet_material',text='Material Type',expand=True)
        _effect_draw(SimpleNamespace(layout=layout,effect_module='bullet'),context)
        box=effect_layout.section(layout,'General');col=box.column(align=True)
        if effect_layout.visible(p,'bullet_detail'):quality_ui.draw(col, p,'bullet_detail',text='Resolution')
        if effect_layout.visible(p,'bullet_seed'):col.prop(p,'bullet_seed',text='Seed')
        if p.bullet_mode == '0':
            box=effect_layout.section(layout,'Distribute');col=box.column(align=True)
            if effect_layout.visible(p,'bullet_amount'):col.prop(p,'bullet_amount',text='Amount')
            if effect_layout.visible(p,'bullet_clusters'):col.prop(p,'bullet_clusters',text='Clusters')
            if effect_layout.visible(p,'bullet_cluster_share'):col.prop(p,'bullet_cluster_share',text='Cluster Share')
            if effect_layout.visible(p,'bullet_cluster_spread'):col.prop(p,'bullet_cluster_spread',text='Cluster Spread (cm)')
            if effect_layout.visible(p,'bullet_min_spacing'):col.prop(p,'bullet_min_spacing',text='Min Spacing')
        if p.bullet_mode=="1":
            box=effect_layout.section(layout,"Paint Settings");col=box.column(align=True)
            if effect_layout.visible(p,'bullet_paint_spacing'):col.prop(p,'bullet_paint_spacing',text='Paint Spacing (cm)')
            if effect_layout.visible(p,'bullet_paint_spread'):col.prop(p,'bullet_paint_spread',text='Paint Spread (cm)')
            if effect_layout.visible(p,'bullet_paint_snap'):col.prop(p,'bullet_paint_snap',text='Paint Snap (cm)')
            if effect_layout.visible(p,'bullet_paint_close'):col.prop(p,'bullet_paint_close',text='Paint Close')
            if effect_layout.visible(p,'bullet_paint_single'):col.prop(p,'bullet_paint_single',text='Paint Single (cm)')
        box=effect_layout.section(layout,'Size and Penetration');col=box.column(align=True)
        material_controls.inline(col, p, 'surface_damage_material', text='Impact Holes')
        if effect_layout.visible(p,'bullet_size_min'):col.prop(p,'bullet_size_min',text='Size Min (cm)')
        if effect_layout.visible(p,'bullet_size_max'):col.prop(p,'bullet_size_max',text='Size Max (cm)')
        if effect_layout.visible(p,'bullet_size_bias'):col.prop(p,'bullet_size_bias',text='Size Bias')
        if effect_layout.visible(p,'bullet_penetration'):col.prop(p,'bullet_penetration',text='Penetration (cm)')
        if effect_layout.visible(p,'bullet_penetration_var'):col.prop(p,'bullet_penetration_var',text='Penetration Var')
        if effect_layout.visible(p,'bullet_thickness_cap'):col.prop(p,'bullet_thickness_cap',text='Thickness Cap')
        box=effect_layout.section(layout,'Shot Direction');col=box.column(align=True)
        if effect_layout.visible(p,'bullet_shot_mode'):col.prop(p,'bullet_shot_mode',text='Shot Mode')
        if effect_layout.visible(p,'bullet_shot_x'):col.prop(p,'bullet_shot_x',text='Shot X')
        if effect_layout.visible(p,'bullet_shot_y'):col.prop(p,'bullet_shot_y',text='Shot Y')
        if effect_layout.visible(p,'bullet_shot_z'):col.prop(p,'bullet_shot_z',text='Shot Z')
        if effect_layout.visible(p,'bullet_oblique'):col.prop(p,'bullet_oblique',text='Oblique')
        if effect_layout.visible(p,'bullet_elongate'):col.prop(p,'bullet_elongate',text='Elongate')
        col=toggle_box(layout,p,'bullet_halo_enable','Halo')
        if col is not None:
            material_controls.inline(col, p, 'fracture_l2_material', text='Halo / Scorch')
            if effect_layout.visible(p,'bullet_halo_scale'):col.prop(p,'bullet_halo_scale',text='Halo Scale')
            if effect_layout.visible(p,'bullet_halo_depth'):col.prop(p,'bullet_halo_depth',text='Halo Depth (cm)')
            if effect_layout.visible(p,'bullet_halo_jagged'):col.prop(p,'bullet_halo_jagged',text='Halo Jagged')
        box=effect_layout.section(layout,'Pits');col=box.column(align=True)
        if effect_layout.visible(p,'bullet_pit_amount'):col.prop(p,'bullet_pit_amount',text='Pit Amount')
        if effect_layout.visible(p,'bullet_pit_size'):col.prop(p,'bullet_pit_size',text='Pit Size')
        if effect_layout.visible(p,'bullet_pit_spread'):col.prop(p,'bullet_pit_spread',text='Pit Spread')
        if p.bullet_material in ('0',):
            box=effect_layout.section(layout,'Metal Settings');col=box.column(align=True)
            material_controls.inline(col, p, 'buildup_material', text='Burrs / Splinters')
            if effect_layout.visible(p,'bullet_metal_hole_jagged'):col.prop(p,'bullet_metal_hole_jagged',text='Metal Hole Jagged')
            if effect_layout.visible(p,'bullet_metal_dish'):col.prop(p,'bullet_metal_dish',text='Metal Dish')
            if effect_layout.visible(p,'bullet_metal_dish_width'):col.prop(p,'bullet_metal_dish_width',text='Metal Dish Width')
            if effect_layout.visible(p,'bullet_metal_lip_height'):col.prop(p,'bullet_metal_lip_height',text='Metal Lip Height')
            if effect_layout.visible(p,'bullet_metal_lip_width'):col.prop(p,'bullet_metal_lip_width',text='Metal Lip Width')
            if effect_layout.visible(p,'bullet_metal_petals'):col.prop(p,'bullet_metal_petals',text='Metal Petals')
            if effect_layout.visible(p,'bullet_metal_petal_tear'):col.prop(p,'bullet_metal_petal_tear',text='Metal Petal Tear')
            if effect_layout.visible(p,'bullet_metal_curl'):col.prop(p,'bullet_metal_curl',text='Metal Curl')
            if effect_layout.visible(p,'bullet_metal_exit_burr'):col.prop(p,'bullet_metal_exit_burr',text='Metal Exit Burr')
            if effect_layout.visible(p,'bullet_metal_dent_chance'):col.prop(p,'bullet_metal_dent_chance',text='Metal Dent Chance')
        if p.bullet_material in ('1',):
            box=effect_layout.section(layout,'Wood Settings');col=box.column(align=True)
            material_controls.inline(box, p, 'buildup_material', text='Wood Splinters')
            if effect_layout.visible(p,'bullet_grain_mode'):col.prop(p,'bullet_grain_mode',text='Grain Mode')
            if effect_layout.visible(p,'bullet_grain_x'):col.prop(p,'bullet_grain_x',text='Grain X')
            if effect_layout.visible(p,'bullet_grain_y'):col.prop(p,'bullet_grain_y',text='Grain Y')
            if effect_layout.visible(p,'bullet_grain_z'):col.prop(p,'bullet_grain_z',text='Grain Z')
            if effect_layout.visible(p,'bullet_wood_crush'):col.prop(p,'bullet_wood_crush',text='Wood Crush')
            if effect_layout.visible(p,'bullet_wood_splinter'):col.prop(p,'bullet_wood_splinter',text='Wood Splinter')
            if effect_layout.visible(p,'bullet_wood_fiber_width'):col.prop(p,'bullet_wood_fiber_width',text='Wood Fiber Width')
            if effect_layout.visible(p,'bullet_wood_exit_scale'):col.prop(p,'bullet_wood_exit_scale',text='Wood Exit Scale')
            if effect_layout.visible(p,'bullet_wood_exit_depth'):col.prop(p,'bullet_wood_exit_depth',text='Wood Exit Depth')
            if effect_layout.visible(p,'bullet_wood_slivers'):col.prop(p,'bullet_wood_slivers',text='Wood Slivers')
            if effect_layout.visible(p,'bullet_wood_sliver_length'):col.prop(p,'bullet_wood_sliver_length',text='Wood Sliver Length')
            if effect_layout.visible(p,'bullet_wood_split_chance'):col.prop(p,'bullet_wood_split_chance',text='Wood Split Chance')
            if effect_layout.visible(p,'bullet_wood_split_length'):col.prop(p,'bullet_wood_split_length',text='Wood Split Length')
        if p.bullet_material in ('2', '3'):
            box=effect_layout.section(layout,'Concrete / Stone Settings');col=box.column(align=True)
            material_controls.inline(col, p, 'fracture_l1_material', text='Cracks')
            if effect_layout.visible(p,'bullet_conc_spall_chance'):col.prop(p,'bullet_conc_spall_chance',text=' Spall Chance')
            if effect_layout.visible(p,'bullet_conc_spall_scale'):col.prop(p,'bullet_conc_spall_scale',text=' Spall Scale')
            if effect_layout.visible(p,'bullet_conc_spall_depth'):col.prop(p,'bullet_conc_spall_depth',text=' Spall Depth')
            if effect_layout.visible(p,'bullet_conc_skin_depth'):col.prop(p,'bullet_conc_skin_depth',text=' Skin Depth')
            if effect_layout.visible(p,'bullet_conc_bore_depth'):col.prop(p,'bullet_conc_bore_depth',text=' Bore Depth')
            if effect_layout.visible(p,'bullet_conc_jagged'):col.prop(p,'bullet_conc_jagged',text=' Jagged')
            if effect_layout.visible(p,'bullet_conc_roughness'):col.prop(p,'bullet_conc_roughness',text=' Roughness')
            if effect_layout.visible(p,'bullet_conc_aggregate'):col.prop(p,'bullet_conc_aggregate',text=' Aggregate')
            if effect_layout.visible(p,'bullet_conc_exit_scale'):col.prop(p,'bullet_conc_exit_scale',text=' Exit Scale')
            if effect_layout.visible(p,'bullet_conc_cracks'):col.prop(p,'bullet_conc_cracks',text=' Cracks')
            if effect_layout.visible(p,'bullet_crack_width'):col.prop(p,'bullet_crack_width',text='Crack Width (cm)')
            if effect_layout.visible(p,'bullet_crack_depth'):col.prop(p,'bullet_crack_depth',text='Crack Depth (cm)')
            if effect_layout.visible(p,'bullet_conc_crack_length'):col.prop(p,'bullet_conc_crack_length',text=' Crack Length')
            col.scale_y=1.2



                                                                          
                                                            
                                                                          
class OBJECT_PT_project_damage_blast(_ProjectDamagePanel):
    bl_idname = "OBJECT_PT_project_damage_blast"
    bl_parent_id = "OBJECT_PT_project_damage"
    bl_label = _EFFECT_LABELS["blast"]
    effect_module = "blast"

    def draw(self, context):
        p = ownership.context_source(context).project_damage
        layout = self.layout
        layout.use_property_split = False
        layout.use_property_decorate = False

        box = layout.box()
        row = box.row(align=True)
        row.alignment = 'CENTER'
        _effect_draw(SimpleNamespace(layout=box, effect_module="blast"), context)

                 
        if (
            effect_layout.visible(p, "blast_seed")
            or effect_layout.visible(p, "blast_avoid")
            or effect_layout.visible(p, "blast_detail")
        ):
            box = layout.box()
            box.enabled = p.blast_enable and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="General", icon=effect_layout.heading_icon("General"))
            col = box.column(align=True)
            quality_ui.draw(col, p, "blast_detail", text="Resolution")
            col.prop(p, "blast_seed", text="Seed")
            pass

                               
        if (
            effect_layout.visible(p, "blast_radius_scale")
            or effect_layout.visible(p, "blast_proximity")
            or effect_layout.visible(p, "blast_prox_shrink")
            or effect_layout.visible(p, "blast_jagged")
            or effect_layout.visible(p, "blast_noise_scale")
        ):
            box = layout.box()
            box.enabled = p.blast_enable and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Placement / proximity", icon=effect_layout.heading_icon("Placement / proximity"))
            col = box.column(align=True)
            material_controls.inline(box, p, 'surface_damage_material', text='Blast Cavities')
            col.prop(p, "blast_radius_scale", text="Radius Scale")
            col.prop(p, "blast_proximity", text="Proximity")
            col.prop(p, "blast_prox_shrink", text="Prox Shrink")
            col.prop(p, "blast_jagged", text="Jagged")
            col.prop(p, "blast_noise_scale", text="Noise Scale")

                         
        if (
            effect_layout.visible(p, "blast_floor_height")
            or effect_layout.visible(p, "blast_floor_offset")
            or effect_layout.visible(p, "blast_slab_thickness")
            or effect_layout.visible(p, "blast_wall_depth")
        ):
            box = layout.box()
            box.enabled = p.blast_enable and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Building layout", icon=effect_layout.heading_icon("Building layout"))
            col = box.column(align=True)
            material_controls.inline(box, p, 'fracture_l2_material', text='Exposed Structure')
            col.prop(p, "blast_floor_height", text="Floor Height (cm)")
            col.prop(p, "blast_floor_offset", text="Floor Offset (cm)")
            col.prop(p, "blast_slab_thickness", text="Slab Thickness (cm)")
            col.prop(p, "blast_wall_depth", text="Wall Depth (cm)")

                                                                       
        if (
            effect_layout.visible(p, "blast_core_size")
            or effect_layout.visible(p, "blast_room_size")
            or effect_layout.visible(p, "blast_room_facets")
            or effect_layout.visible(p, "blast_partition_spacing")
            or effect_layout.visible(p, "blast_partition_thickness")
            or effect_layout.visible(p, "blast_slab_break")
            or effect_layout.visible(p, "blast_slab_droop")
            or effect_layout.visible(p, "blast_wall_size")
            or effect_layout.visible(p, "blast_fringe")
            or effect_layout.visible(p, "blast_fringe_density")
            or effect_layout.visible(p, "blast_spall_reach")
            or effect_layout.visible(p, "blast_spall_cover")
        ):
            box = layout.box()
            box.enabled = p.blast_enable and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Layers, as normalized blast distance (1 = the empty's sphere)", icon=effect_layout.heading_icon("Layers, as normalized blast distance (1 = the empty's sphere)"))
            col = box.column(align=True)
            col.prop(p, "blast_core_size", text="Core Size")
            col.prop(p, "blast_room_size", text="Room Size")
            col.prop(p, "blast_room_facets", text="Room Facets")
            col.prop(p, "blast_partition_spacing", text="Partition Spacing (cm)")
            col.prop(p, "blast_partition_thickness", text="Partition Thickness (cm)")
            col.prop(p, "blast_slab_break", text="Slab Break")
            col.prop(p, "blast_slab_droop", text="Slab Droop")
            col.prop(p, "blast_wall_size", text="Wall Size")
            col.prop(p, "blast_fringe", text="Fringe")
            col.prop(p, "blast_fringe_density", text="Fringe Density")
            col.prop(p, "blast_spall_reach", text="Spall Reach")
            col.prop(p, "blast_spall_cover", text="Spall Deepness")

                 
        if (
            effect_layout.visible(p, "blast_brick_length")
            or effect_layout.visible(p, "blast_brick_height")
            or effect_layout.visible(p, "blast_joint_width")
            or effect_layout.visible(p, "blast_joint_depth")
            or effect_layout.visible(p, "blast_brick_jitter")
            or effect_layout.visible(p, "blast_plaster_depth")
        ):
            box = layout.box()
            box.enabled = p.blast_enable and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Masonry", icon=effect_layout.heading_icon("Masonry"))
            col = box.column(align=True)
            col.prop(p, "blast_brick_length", text="Brick Length (cm)")
            col.prop(p, "blast_brick_height", text="Brick Height (cm)")
            col.prop(p, "blast_joint_width", text="Joint Width (cm)")
            col.prop(p, "blast_joint_depth", text="Joint Depth (cm)")
            col.prop(p, "blast_brick_jitter", text="Brick Jitter")
            col.prop(p, "blast_plaster_depth", text="Plaster Depth (cm)")

                
        if (
            effect_layout.visible(p, "blast_crack_enable")
            or effect_layout.visible(p, "blast_crack_count")
            or effect_layout.visible(p, "blast_crack_length")
            or effect_layout.visible(p, "blast_crack_width")
            or effect_layout.visible(p, "blast_crack_depth")
        ):
            box = layout.box()
            box.enabled = p.blast_enable and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Cracks", icon=effect_layout.heading_icon("Cracks"))
            col = box.column(align=True)
            material_controls.inline(box, p, 'fracture_l1_material', text='Cracks')
            col.prop(p, "blast_crack_enable", text="Crack Enable")
            col.prop(p, "blast_crack_count", text="Crack Count")
            col.prop(p, "blast_crack_length", text="Crack Length")
            col.prop(p, "blast_crack_width", text="Crack Width (cm)")
            col.prop(p, "blast_crack_depth", text="Crack Depth (cm)")


                                                                          
                                                             
                                                                          
class OBJECT_PT_project_damage_ground(_ProjectDamagePanel):
    bl_idname = "OBJECT_PT_project_damage_ground"
    bl_parent_id = "OBJECT_PT_project_damage"
    bl_label = _EFFECT_LABELS["ground"]
    effect_module = "ground"

    def draw(self, context):
        from .asset_updates import toggle_box
        p=ownership.context_source(context).project_damage
        layout=self.layout
        layout.use_property_split=False
        layout.use_property_decorate=False
        _effect_draw(SimpleNamespace(layout=layout,effect_module='ground'),context)
        box=effect_layout.section(layout,'General');col=box.column(align=True)
        col.prop(p,'ground_depth',text='Depth (cm)')
        if effect_layout.visible(p,'ground_detail'):quality_ui.draw(col, p,'ground_detail',text='Resolution')
        if effect_layout.visible(p,'ground_seed'):col.prop(p,'ground_seed',text='Seed')
        if effect_layout.visible(p,'ground_max_slope'):col.prop(p,'ground_max_slope',text='Max Slope')
        row=box.row(align=True);row.scale_y=1.5
        row.prop(p,'ground_axis_x',text='X',toggle=True)
        row.prop(p,'ground_axis_y',text='Y',toggle=True)
        row.prop(p,'ground_axis_z',text='Z',toggle=True)
        if not (p.ground_axis_x or p.ground_axis_y or p.ground_axis_z):
            box.label(text='Assign at least one axis',icon='INFO')
        box=effect_layout.section(layout,'Automatic Placement');col=box.column(align=True)
        if effect_layout.visible(p,'ground_amount'):col.prop(p,'ground_amount',text='Amount')
        if effect_layout.visible(p,'ground_size_min'):col.prop(p,'ground_size_min',text='Size Min (cm)')
        if effect_layout.visible(p,'ground_size_max'):col.prop(p,'ground_size_max',text='Size Max (cm)')
        if effect_layout.visible(p,'ground_area_chance'):col.prop(p,'ground_area_chance',text='Area Chance')
        if effect_layout.visible(p,'ground_area_size'):col.prop(p,'ground_area_size',text='Area Size')
        if effect_layout.visible(p,'ground_area_stretch'):col.prop(p,'ground_area_stretch',text='Area Stretch')
        if p.ground_mode=="1":
            box=effect_layout.section(layout,"Paint Settings");col=box.column(align=True)
            if effect_layout.visible(p,'ground_paint_snap'):col.prop(p,'ground_paint_snap',text='Paint Snap (cm)')
            if effect_layout.visible(p,'ground_paint_close'):col.prop(p,'ground_paint_close',text='Paint Close')
            if effect_layout.visible(p,'ground_paint_jitter'):col.prop(p,'ground_paint_jitter',text='Paint Jitter (cm)')
            if effect_layout.visible(p,'ground_paint_branch'):col.prop(p,'ground_paint_branch',text='Paint Branch')
        box=effect_layout.section(layout,'Damaged Area — Alligator Plates');col=box.column(align=True)
        material_controls.inline(col, p, 'surface_damage_material', text='Broken Areas')
        if effect_layout.visible(p,'ground_area_jagged'):col.prop(p,'ground_area_jagged',text='Area Jagged')
        if effect_layout.visible(p,'ground_rim_fade'):col.prop(p,'ground_rim_fade',text='Rim Fade')
        if effect_layout.visible(p,'ground_plate_size'):col.prop(p,'ground_plate_size',text='Plate Size (cm)')
        if effect_layout.visible(p,'ground_plate_center_scale'):col.prop(p,'ground_plate_center_scale',text='Plate Center Scale')
        if effect_layout.visible(p,'ground_plate_jagged'):col.prop(p,'ground_plate_jagged',text='Plate Jagged')
        if effect_layout.visible(p,'ground_gap_width'):col.prop(p,'ground_gap_width',text='Gap Width (cm)')
        if effect_layout.visible(p,'ground_gap_depth'):col.prop(p,'ground_gap_depth',text='Gap Depth (cm)')
        if effect_layout.visible(p,'ground_plate_sink'):col.prop(p,'ground_plate_sink',text='Plate Sink (cm)')
        if effect_layout.visible(p,'ground_plate_tilt'):col.prop(p,'ground_plate_tilt',text='Plate Tilt (cm)')
        col=toggle_box(layout,p,'ground_crack_enable','Crack Network')
        if col is not None:
            material_controls.inline(col, p, 'fracture_l1_material', text='Cracks')
            if effect_layout.visible(p,'ground_crack_amount'):col.prop(p,'ground_crack_amount',text='Crack Amount')
            if effect_layout.visible(p,'ground_crack_generations'):col.prop(p,'ground_crack_generations',text='Crack Generations')
            if effect_layout.visible(p,'ground_crack_length_min'):col.prop(p,'ground_crack_length_min',text='Crack Length Min (cm)')
            if effect_layout.visible(p,'ground_crack_length_max'):col.prop(p,'ground_crack_length_max',text='Crack Length Max (cm)')
            if effect_layout.visible(p,'ground_crack_width'):col.prop(p,'ground_crack_width',text='Crack Width (cm)')
            if effect_layout.visible(p,'ground_crack_width_var'):col.prop(p,'ground_crack_width_var',text='Crack Width Var')
            if effect_layout.visible(p,'ground_crack_taper'):col.prop(p,'ground_crack_taper',text='Crack Taper')
            if effect_layout.visible(p,'ground_crack_depth'):col.prop(p,'ground_crack_depth',text='Crack Depth (cm)')
            if effect_layout.visible(p,'ground_crack_meander'):col.prop(p,'ground_crack_meander',text='Crack Meander')
            if effect_layout.visible(p,'ground_crack_kink'):col.prop(p,'ground_crack_kink',text='Crack Kink')
            if effect_layout.visible(p,'ground_crack_branch'):col.prop(p,'ground_crack_branch',text='Crack Branch')
            if effect_layout.visible(p,'ground_crack_join'):col.prop(p,'ground_crack_join',text='Crack Join')
            if effect_layout.visible(p,'ground_crack_chip'):col.prop(p,'ground_crack_chip',text='Crack Chip')
        box=effect_layout.section(layout,'Potholes — Missing Plates');col=box.column(align=True)
        if effect_layout.visible(p,'ground_pothole_chance'):col.prop(p,'ground_pothole_chance',text='Pothole Chance')
        if effect_layout.visible(p,'ground_pothole_size'):col.prop(p,'ground_pothole_size',text='Pothole Size')
        if effect_layout.visible(p,'ground_pothole_loose'):col.prop(p,'ground_pothole_loose',text='Pothole Loose')
        if effect_layout.visible(p,'ground_pothole_depth'):col.prop(p,'ground_pothole_depth',text='Pothole Depth (cm)')
        if effect_layout.visible(p,'ground_roughness'):col.prop(p,'ground_roughness',text='Roughness')
        if effect_layout.visible(p,'ground_aggregate_size'):col.prop(p,'ground_aggregate_size',text='Aggregate Size (cm)')
        if effect_layout.visible(p,'ground_thickness_cap'):col.prop(p,'ground_thickness_cap',text='Thickness Cap')
        if generated_geometry.draw(layout, p, 'ground'):
            col=toggle_box(layout,p,'ground_rubble_enable','Rubble')
            if col is not None:
                material_controls.inline(col, p, 'buildup_material', text='Rubble / Heave')
                if effect_layout.visible(p,'ground_rubble_amount'):col.prop(p,'ground_rubble_amount',text='Rubble Amount')
                if effect_layout.visible(p,'ground_rubble_size_min'):col.prop(p,'ground_rubble_size_min',text='Rubble Size Min (cm)')
                if effect_layout.visible(p,'ground_rubble_size_max'):col.prop(p,'ground_rubble_size_max',text='Rubble Size Max (cm)')
            box=effect_layout.section(layout,'Heave');col=box.column(align=True)
            if effect_layout.visible(p,'ground_heave_chance'):col.prop(p,'ground_heave_chance',text='Heave Chance')
            if effect_layout.visible(p,'ground_heave_height'):col.prop(p,'ground_heave_height',text='Heave Height (cm)')
            if effect_layout.visible(p,'ground_heave_width'):col.prop(p,'ground_heave_width',text='Heave Width (cm)')
            if effect_layout.visible(p,'ground_heave_gap'):col.prop(p,'ground_heave_gap',text='Heave Gap (cm)')



                                                                          
                                                            
                                                                          
def draw_brick_back_surface(layout, p, *, classic=False):
    from . import asset_noise

    box = layout.box()
    title = 'Back Surface Noise' if classic else 'Back Surface'
    effect_layout.heading(box, title, 'TEXTURE')
    if classic:
        material_controls.inline(box, p, 'brick_behind_material', text='Damage Pocket / Backing')
    box.prop(p, 'brick_back_noise_enable', toggle=True, icon='TEXTURE')
    if not p.brick_back_noise_enable:
        return

    if not classic:
        row = box.row(align=True)
        row.scale_y = 1.5
        row.prop_enum(p, 'brick_surf_mode', '0', text='Procedural')
        row.prop_enum(p, 'brick_surf_mode', '1', text='Texture')

    if classic:
        col = box.column(align=True)
        col.prop(p, 'brick_noise_strength', text='Strength (cm)')
        if p.brick_noise_strength > 0.0:
            quality_ui.draw(col, p, 'brick_back_resolution', text='Back Surface Resolution')
            col.prop(p, 'brick_noise_scale', text='Noise Scale (cm)')
            quality_ui.draw(col, p, 'brick_noise_detail', text='Detail')
    elif p.brick_surf_mode == '0':
        quality_ui.draw(box, p, 'brick_back_resolution', text='Back Surface Resolution')
        asset_noise.draw(box, p, 'brick')
    else:
        col = box.column(align=True)
        col.prop(p, 'brick_tex_path', text='Height Texture')
        col.prop(p, 'brick_tex_scale', text='Texture Scale (cm)')
        col.prop(p, 'brick_tex_strength', text='Texture Depth (cm)')
        col.prop(p, 'brick_tex_edge', text='Mesh Edge Size (cm)')
        if p.brick_tex_edge <= 0.0:
            quality_ui.draw(col, p, 'brick_back_resolution', text='Back Surface Resolution')
        col.prop(p, 'brick_tex_offset_x', text='Offset X (cm)')
        col.prop(p, 'brick_tex_offset_y', text='Offset Y (cm)')
        col.prop(p, 'brick_tex_offset_z', text='Offset Z (cm)')


def draw_classic_brick(layout,context,p):
    from .recording_tools import toggle_box
    _effect_draw(SimpleNamespace(layout=layout,effect_module='brick'),context)
    box=effect_layout.section(layout,'General');col=box.column(align=True)
    quality_ui.draw(col, p,'brick_detail',text='Resolution')
    col.prop(p,'brick_seed',text='Seed')
    box=effect_layout.section(layout,'Damage Settings');col=box.column(align=True)
    if p.brick_mode!='1':col.prop(p,'brick_amount',text='Amount')
    col.prop(p,'brick_size_min',text='Size Min (cm)')
    col.prop(p,'brick_size_max',text='Size Max (cm)')
    col.prop(p,'brick_lobes',text='Lobes')
    col.prop(p,'brick_stretch',text='Stretch')
    col.prop(p,'brick_edge_bias',text='Edge Bias')
    col.separator(type='LINE')
    axes=col.row(align=True)
    axes.prop(p,'brick_up_x',text='Bricks Orientation X')
    axes.prop(p,'brick_up_y',text='Y')
    axes.prop(p,'brick_up_z',text='Z')
    if p.brick_mode=='1':
        box=effect_layout.section(layout,'Paint Settings');col=box.column(align=True)
        col.prop(p,'brick_paint_radius',text='Paint Radius (cm)')
        col.prop(p,'brick_paint_snap',text='Paint Snap (cm)')
        col.prop(p,'brick_paint_close',text='Close Strokes')
    box=effect_layout.section(layout,'Surface Layer Depth');col=box.column(align=True)
    material_controls.inline(col,p,'brick_render_material',text='Broken Surface Edge')
    col.prop(p,'brick_render_min')
    col.prop(p,'brick_render_max')
    col.prop(p,'brick_jagged',text='Jagged')
    col.prop(p,'brick_outline_scale',text='Outline Scale')
    col.prop(p,'brick_crumble',text='Crumble')
    col.prop(p,'brick_crumble_size',text='Crumble Size (cm)')
    if not p.brick_sharp_edge:col.prop(p,'brick_render_stick',text='Surface Remnants')
    col.prop(p,'brick_thickness_cap',text='Thickness Cap')
    col.prop(p,'brick_edge_wall_depth')
    draw_brick_back_surface(layout, p, classic=True)
    if generated_geometry.draw(layout, p, 'brick'):
        box=effect_layout.section(layout,'Bricks Settings');col=box.column(align=True)
        material_controls.inline(col,p,'brick_front_material',text='Brick Faces / Sides')
        material_controls.inline(col,p,'brick_seam_material',text='Mortar Joints')
        col.prop(p,'brick_bond',text='Bond',expand=True)
        col.prop(p,'brick_brick_scale',text='Brick Scale')
        col.separator(type='LINE')
        col.prop(p,'brick_brick_length',text='Brick Length (cm)')
        col.prop(p,'brick_brick_height',text='Brick Height (cm)')
        col.prop(p,'brick_brick_width',text='Brick Width (cm)')
        col.prop(p,'brick_mortar',text='Mortar (cm)')
        col.prop(p,'brick_mortar_depth',text='Mortar Depth (cm)')
        col.prop(p,'brick_bond_offset',text='Bond Offset')
        sub=_ui_disclosure(box,'brick_surface_open','Bricks Surface')
        if sub is not None:
            sub.prop(p,'brick_face_jitter',text='Face Jitter (cm)')
            sub.prop(p,'brick_face_tilt',text='Face Tilt')
            sub.prop(p,'brick_brick_rough',text='Brick Rough')
            sub.prop(p,'brick_chip',text='Chip')
            sub.prop(p,'brick_chip_size',text='Chip Size (cm)')
            sub.prop(p,'brick_joint_rough',text='Joint Rough')
        sub=_ui_disclosure(box,'brick_knocked_open','Knocked Out Bricks')
        if sub is not None:
            material_controls.inline(sub,p,'brick_break_material',text='Snapped Brick Faces')
            sub.prop(p,'brick_break_size',text='Break Size')
            sub.prop(p,'brick_layer_inset',text='Layer Inset')
            sub.prop(p,'brick_break_jagged',text='Break Jagged')
            sub.prop(p,'brick_missing',text='Missing')
            sub.prop(p,'brick_broken',text='Broken')
            sub.prop(p,'brick_broken_rough',text='Broken Rough')
            sub.prop(p,'brick_collar_fill',text='Collar Fill')
        sub=_ui_disclosure(box,'brick_loose_open','Loose Bricks')
        if sub is not None:
            sub.prop(p,'brick_loose',text='Loose')
            sub.prop(p,'brick_loose_depth',text='Loose Depth (cm)')
            sub.prop(p,'brick_loose_tilt',text='Loose Tilt')
            sub.prop(p,'brick_loose_out',text='Loose Out')
            sub.prop(p,'brick_loose_out_dist',text='Loose Out Dist (cm)')
    col=toggle_box(layout,p,'brick_hole_enable','Through Holes')
    if col is not None and through_holes.draw_controls(col, p, 'brick'):
        col.prop(p,'brick_hole_chance',text='Hole Chance')
        col.prop(p,'brick_hole_size',text='Hole Size')
    col=toggle_box(layout,p,'brick_crack_enable','Cracks')
    if col is not None:
        material_controls.inline(col,p,'fracture_l1_material',text='Cracks')
        col.prop(p,'brick_crack_amount',text='Crack Amount')
        col.prop(p,'brick_crack_length_min',text='Crack Length Min (cm)')
        col.prop(p,'brick_crack_length_max',text='Crack Length Max (cm)')
        col.prop(p,'brick_crack_width',text='Crack Width (cm)')
        col.prop(p,'brick_crack_depth',text='Crack Depth (cm)')
        col.prop(p,'brick_crack_meander',text='Crack Meander')
        col.prop(p,'brick_crack_branch',text='Crack Branch')
        col.prop(p,'brick_crack_interior',text='Crack Interior')
    box=effect_layout.section(layout,'Corners, Joints and Topology');col=box.column(align=True)
    if p.brick_generated_enable:
        col.prop(p,'brick_corners',text='Corners')
        col.prop(p,'brick_joint_fill',text='Joint Fill')
    col.prop(p,'brick_remesh',text='Remesh')
    if p.brick_remesh:col.prop(p,'brick_remesh_size',text='Remesh Size (cm)')


class OBJECT_PT_project_damage_brick(_ProjectDamagePanel):
    bl_idname = "OBJECT_PT_project_damage_brick"
    bl_parent_id = "OBJECT_PT_project_damage"
    bl_label = _EFFECT_LABELS["brick"]
    effect_module = "brick"

    def draw(self, context):
        p = ownership.context_source(context).project_damage
        layout = self.layout
        layout.use_property_split = False
        layout.use_property_decorate = False

        if p.brick_enable and not p.brick_v2_enable:
            draw_classic_brick(layout,context,p)
            return

        box = layout.box()
        row = box.row(align=True)
        row.alignment = 'CENTER'
        _effect_draw(SimpleNamespace(layout=box, effect_module="brick"), context)

        if p.brick_v2_enable:
            box = layout.box()
            effect_layout.heading(box, 'Smooth Profile', 'MOD_SMOOTH')
            box.prop(p, 'brick_smooth_width', text='Smooth Slope')

                                   
        draw_brick_back_surface(layout, p)

        if (
            p.brick_generated_enable and effect_layout.visible(p, "brick_brick_mode")
        ):
            box = layout.box()
            box.enabled = (p.brick_enable or p.brick_v2_enable) and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Brick construction", icon=effect_layout.heading_icon("Brick construction"))
            col = box.column(align=True)
            col.prop(p, "brick_brick_mode", text="Brick Mode")

               
        if not p.brick_v2_enable and effect_layout.visible(p, "brick_style"):
            box = layout.box()
            box.enabled = (p.brick_enable or p.brick_v2_enable) and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Style", icon=effect_layout.heading_icon("Style"))
            col = box.column(align=True)
            col.prop(p, "brick_style", text="Style")

                 
        if (
            effect_layout.visible(p, "brick_seed")
            or effect_layout.visible(p, "brick_avoid")
            or effect_layout.visible(p, "brick_detail")
        ):
            box = layout.box()
            box.enabled = (p.brick_enable or p.brick_v2_enable) and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="General", icon=effect_layout.heading_icon("General"))
            col = box.column(align=True)
            quality_ui.draw(col, p, "brick_detail", text="Resolution")
            col.prop(p, "brick_seed", text="Seed")
            pass

                             
        if (
            p.brick_mode != '1' and (
            effect_layout.visible(p, "brick_amount")
            or effect_layout.visible(p, "brick_size_min")
            or effect_layout.visible(p, "brick_size_max")
            or effect_layout.visible(p, "brick_lobes")
            or effect_layout.visible(p, "brick_stretch")
            or effect_layout.visible(p, "brick_edge_bias"))
        ):
            box = layout.box()
            box.enabled = (p.brick_enable or p.brick_v2_enable) and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Automatic placement", icon=effect_layout.heading_icon("Automatic placement"))
            col = box.column(align=True)
            col.prop(p, "brick_amount", text="Amount")
            col.prop(p, "brick_size_min", text="Size Min (cm)")
            col.prop(p, "brick_size_max", text="Size Max (cm)")
            col.prop(p, "brick_lobes", text="Lobes")
            col.prop(p, "brick_stretch", text="Stretch")
            col.prop(p, "brick_edge_bias", text="Edge Bias")

                    
        if (
            p.brick_mode == '1' and (
            effect_layout.visible(p, "brick_paint_radius")
            or effect_layout.visible(p, "brick_paint_snap")
            or effect_layout.visible(p, "brick_paint_close"))
        ):
            box = layout.box()
            box.enabled = (p.brick_enable or p.brick_v2_enable) and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Paint mode", icon=effect_layout.heading_icon("Paint mode"))
            col = box.column(align=True)
            col.prop(p, "brick_paint_radius", text="Paint Radius (cm)")
            col.prop(p, "brick_paint_snap", text="Paint Snap (cm)")
            col.prop(p, "brick_paint_close", text="Paint Close")

                                           
        if (
            effect_layout.visible(p, "brick_render_min")
            or effect_layout.visible(p, "brick_render_max")
            or effect_layout.visible(p, "brick_jagged")
            or effect_layout.visible(p, "brick_outline_scale")
            or effect_layout.visible(p, "brick_crumble")
            or effect_layout.visible(p, "brick_crumble_size")
            or effect_layout.visible(p, "brick_terraces")
            or effect_layout.visible(p, "brick_terrace_skin")
            or effect_layout.visible(p, "brick_terrace_inset")
            or effect_layout.visible(p, "brick_wall_soft")
            or effect_layout.visible(p, "brick_roughness")
            or effect_layout.visible(p, "brick_render_stick")
            or effect_layout.visible(p, "brick_thickness_cap")
        ):
            box = layout.box()
            box.enabled = (p.brick_enable or p.brick_v2_enable) and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Render (the coat over the bricks)", icon=effect_layout.heading_icon("Render (the coat over the bricks)"))
            col = box.column(align=True)
            material_controls.inline(box, p, 'brick_render_material', text='Broken Render Edge')
            col.prop(p, "brick_render_min")
            col.prop(p, "brick_render_max")
            col.prop(p, "brick_jagged", text="Jagged")
            col.prop(p, "brick_outline_scale", text="Outline Scale")
            col.prop(p, "brick_crumble", text="Crumble")
            col.prop(p, "brick_crumble_size", text="Crumble Size (cm)")
            col.prop(p, "brick_terraces", text="Terraces")
            col.prop(p, "brick_terrace_skin", text="Terrace Skin")
            col.prop(p, "brick_terrace_inset", text="Terrace Inset")
            col.prop(p, "brick_wall_soft", text="Wall Soft")
            col.prop(p, "brick_roughness", text="Roughness")
            col.prop(p, "brick_render_stick", text="Render Stick")
            col.prop(p, "brick_thickness_cap", text="Thickness Cap")
            col.prop(p,"brick_edge_wall_depth")

                                    
        if generated_geometry.draw(layout, p, 'brick'):
            if (
                effect_layout.visible(p, "brick_bond")
                or effect_layout.visible(p, "brick_brick_length")
                or effect_layout.visible(p, "brick_brick_height")
                or effect_layout.visible(p, "brick_brick_width")
                or effect_layout.visible(p, "brick_mortar")
                or effect_layout.visible(p, "brick_mortar_depth")
                or effect_layout.visible(p, "brick_brick_scale")
                or effect_layout.visible(p, "brick_bond_offset")
            ):
                box = layout.box()
                box.enabled = (p.brick_enable or p.brick_v2_enable) and not p.all_off
                row = box.row(align=True)
                row.alignment = 'CENTER'
                row.label(text="Bricks: size, joints, bond", icon=effect_layout.heading_icon("Bricks: size, joints, bond"))
                col = box.column(align=True)
                material_controls.inline(box, p, 'brick_front_material', text='Brick Faces / Sides')
                material_controls.inline(box, p, 'brick_seam_material', text='Mortar Joints')
                col.prop(p, "brick_bond", text="Bond")
                col.prop(p, "brick_brick_length", text="Brick Length (cm)")
                col.prop(p, "brick_brick_height", text="Brick Height (cm)")
                col.prop(p, "brick_brick_width", text="Brick Width (cm)")
                col.prop(p, "brick_mortar", text="Mortar (cm)")
                col.prop(p, "brick_mortar_depth", text="Mortar Depth (cm)")
                col.prop(p, "brick_brick_scale", text="Brick Scale")
                col.prop(p, "brick_bond_offset", text="Bond Offset")

                       
            if (
                effect_layout.visible(p, "brick_face_jitter")
                or effect_layout.visible(p, "brick_face_tilt")
                or effect_layout.visible(p, "brick_brick_rough")
                or effect_layout.visible(p, "brick_chip")
                or effect_layout.visible(p, "brick_chip_size")
                or effect_layout.visible(p, "brick_joint_rough")
            ):
                box = layout.box()
                box.enabled = (p.brick_enable or p.brick_v2_enable) and not p.all_off
                row = box.row(align=True)
                row.alignment = 'CENTER'
                row.label(text="Brick surface", icon=effect_layout.heading_icon("Brick surface"))
                col = box.column(align=True)
                col.prop(p, "brick_face_jitter", text="Face Jitter (cm)")
                col.prop(p, "brick_face_tilt", text="Face Tilt")
                col.prop(p, "brick_brick_rough", text="Brick Rough")
                col.prop(p, "brick_chip", text="Chip")
                col.prop(p, "brick_chip_size", text="Chip Size (cm)")
                col.prop(p, "brick_joint_rough", text="Joint Rough")

                            
            show_layers = p.brick_brick_mode == '0' and effect_layout.visible(p, 'brick_layers')
            if (
                show_layers
                or effect_layout.visible(p, "brick_break_size")
                or effect_layout.visible(p, "brick_layer_inset")
                or effect_layout.visible(p, "brick_break_jagged")
                or effect_layout.visible(p, "brick_missing")
                or effect_layout.visible(p, "brick_broken")
                or effect_layout.visible(p, "brick_broken_rough")
                or effect_layout.visible(p, "brick_collar_fill")
            ):
                box = layout.box()
                box.enabled = (p.brick_enable or p.brick_v2_enable) and not p.all_off
                row = box.row(align=True)
                row.alignment = 'CENTER'
                row.label(text="Knocked-out bricks", icon=effect_layout.heading_icon("Knocked-out bricks"))
                col = box.column(align=True)
                material_controls.inline(box, p, 'brick_break_material', text='Snapped Brick Faces')
                if show_layers:
                    col.prop(p, "brick_layers", text="Layers")
                col.prop(p, "brick_break_size", text="Break Size")
                col.prop(p, "brick_layer_inset", text="Layer Inset")
                col.prop(p, "brick_break_jagged", text="Break Jagged")
                col.prop(p, "brick_missing", text="Missing")
                col.prop(p, "brick_broken", text="Broken")
                col.prop(p, "brick_broken_rough", text="Broken Rough")
                col.prop(p, "brick_collar_fill", text="Collar Fill")

                      
            if (
                effect_layout.visible(p, "brick_loose")
                or effect_layout.visible(p, "brick_loose_depth")
                or effect_layout.visible(p, "brick_loose_tilt")
                or effect_layout.visible(p, "brick_loose_out")
                or effect_layout.visible(p, "brick_loose_out_dist")
            ):
                box = layout.box()
                box.enabled = (p.brick_enable or p.brick_v2_enable) and not p.all_off
                row = box.row(align=True)
                row.alignment = 'CENTER'
                row.label(text="Loose bricks", icon=effect_layout.heading_icon("Loose bricks"))
                col = box.column(align=True)
                if p.brick_brick_mode == '0':
                    material_controls.inline(box, p, 'buildup_material', text='Carved Loose Bricks')
                col.prop(p, "brick_loose", text="Loose")
                col.prop(p, "brick_loose_depth", text="Loose Depth (cm)")
                col.prop(p, "brick_loose_tilt", text="Loose Tilt")
                col.prop(p, "brick_loose_out", text="Loose Out")
                col.prop(p, "brick_loose_out_dist", text="Loose Out Dist (cm)")

                                 
        if (
            effect_layout.visible(p, "brick_hole_enable")
            or effect_layout.visible(p, "brick_hole_chance")
            or effect_layout.visible(p, "brick_hole_size")
        ):
            box = layout.box()
            box.enabled = (p.brick_enable or p.brick_v2_enable) and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Hole (through the mesh)", icon=effect_layout.heading_icon("Hole (through the mesh)"))
            col = box.column(align=True)
            col.prop(p, "brick_hole_enable", text="Hole Enable")
            if p.brick_hole_enable and through_holes.draw_controls(col, p, 'brick'):
                col.prop(p, "brick_hole_chance", text="Hole Chance")
                col.prop(p, "brick_hole_size", text="Hole Size")

                                        
        if (
            effect_layout.visible(p, "brick_crack_enable")
            or effect_layout.visible(p, "brick_crack_amount")
            or effect_layout.visible(p, "brick_crack_length_min")
            or effect_layout.visible(p, "brick_crack_length_max")
            or effect_layout.visible(p, "brick_crack_width")
            or effect_layout.visible(p, "brick_crack_depth")
            or effect_layout.visible(p, "brick_crack_meander")
            or effect_layout.visible(p, "brick_crack_branch")
            or effect_layout.visible(p, "brick_crack_interior")
            or effect_layout.visible(p, "brick_up_x")
            or effect_layout.visible(p, "brick_up_y")
            or effect_layout.visible(p, "brick_up_z")
        ):
            box = layout.box()
            box.enabled = (p.brick_enable or p.brick_v2_enable) and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Render cracks around the patch", icon=effect_layout.heading_icon("Render cracks around the patch"))
            col = box.column(align=True)
            material_controls.inline(box, p, 'fracture_l1_material', text='Cracks')
            col.prop(p, "brick_crack_enable", text="Crack Enable")
            col.prop(p, "brick_crack_amount", text="Crack Amount")
            col.prop(p, "brick_crack_length_min", text="Crack Length Min (cm)")
            col.prop(p, "brick_crack_length_max", text="Crack Length Max (cm)")
            col.prop(p, "brick_crack_width", text="Crack Width (cm)")
            col.prop(p, "brick_crack_depth", text="Crack Depth (cm)")
            col.prop(p, "brick_crack_meander", text="Crack Meander")
            col.prop(p, "brick_crack_branch", text="Crack Branch")
            col.prop(p, "brick_crack_interior", text="Crack Interior")
            col.prop(p, "brick_up_x", text="Up X")
            col.prop(p, "brick_up_y", text="Up Y")
            col.prop(p, "brick_up_z", text="Up Z")

                          
        if (
            effect_layout.visible(p, "brick_split_materials")
        ):
            box = layout.box()
            box.enabled = (p.brick_enable or p.brick_v2_enable) and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Material Regions", icon=effect_layout.heading_icon("Material Regions"))
            col = box.column(align=True)
            col.prop(p, "brick_split_materials", text="Separate Material Regions")

                     
        if (
            effect_layout.visible(p, "brick_sharp_edge")
        ):
            box = layout.box()
            box.enabled = (p.brick_enable or p.brick_v2_enable) and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Render edge", icon=effect_layout.heading_icon("Render edge"))
            col = box.column(align=True)
            col.prop(p, "brick_sharp_edge", text="Sharp Edge")

                                 
        if (
            effect_layout.visible(p, "brick_behind")
            or effect_layout.visible(p, "brick_damage_depth")
            or effect_layout.visible(p, "brick_damage_noise")
            or effect_layout.visible(p, "brick_damage_scale")
        ):
            box = layout.box()
            box.enabled = (p.brick_enable or p.brick_v2_enable) and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Behind the brick layers", icon=effect_layout.heading_icon("Behind the brick layers"))
            col = box.column(align=True)
            material_controls.inline(box, p, 'brick_behind_material', text='Damage Pocket / Backing')
            col.prop(p, "brick_behind", text="Behind")
            col.prop(p, "brick_damage_depth", text="Damage Depth (cm)")
            col.prop(p, "brick_damage_noise", text="Damage Noise (cm)")
            col.prop(p, "brick_damage_scale", text="Damage Scale (cm)")

                                      
        if (
            effect_layout.visible(p, "brick_corners")
            or effect_layout.visible(p, "brick_joint_fill")
            or effect_layout.visible(p, "brick_remesh")
            or effect_layout.visible(p, "brick_remesh_size")
        ):
            box = layout.box()
            box.enabled = (p.brick_enable or p.brick_v2_enable) and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Corners, joints and topology", icon=effect_layout.heading_icon("Corners, joints and topology"))
            col = box.column(align=True)
            if p.brick_generated_enable:
                col.prop(p, "brick_corners", text="Corners")
                col.prop(p, "brick_joint_fill", text="Joint Fill")
            col.prop(p, "brick_remesh", text="Remesh")
            col.prop(p, "brick_remesh_size", text="Remesh Size (cm)")


                                                                          
                                                            
                                                                          
def draw_glass_asset(layout,context,p):
    from .recording_tools import toggle_box
    from .paint import drawing_row
    layout.scale_y=1.2
    if p.glass_hit_enable:
        box=effect_layout.section(layout,'Impact Points');row=box.row(align=True)
        row.prop(p,'glass_hit_collection',text='Hit Empties')
        row.operator('project_damage.glass_add_impact',text='',icon='ADD')
        box.prop(p,'glass_paint_impact_size',text='Proximity Radius (cm)')
    else:
        box=effect_layout.section(layout,'Drawing')
        drawing_row(box,p,'GLASS_HOLES')
        box.prop(p,'glass_drawn_reach',text='Crack Reach')
        drawing=effect_layout.section(box,'Grease Pencil Drawing')
        drawing.prop(p,'glass_paint_shard_width',text='Shard Width (cm)')
        drawing.prop(p,'glass_max_cracks',text='Max Cracks')
    box=effect_layout.section(layout,'General');col=box.column(align=True)
    col.prop(p,'glass_master_scale',text='Master Scale')
    quality_ui.draw(col, p,'glass_detail',text='Resolution')
    col.prop(p,'glass_seed',text='Seed')
    box=effect_layout.section(layout,'Form Settings');col=box.column(align=True)
    material_controls.inline(col,p,'surface_damage_material',text='Broken Openings')
    if p.glass_hit_enable:col.prop(p,'glass_hole_lobes',text='Hole Lobes')
    shards=effect_layout.section(box,'Shards Along the Opening');col=shards.column(align=True)
    col.prop(p,'glass_radial_min',text='Radial Min')
    col.prop(p,'glass_radial_max',text='Radial Max')
    col.prop(p,'glass_radial_jitter',text='Radial Jitter')
    col.prop(p,'glass_spike_chance',text='Spike Chance')
    col.prop(p,'glass_spike_length',text='Spike Length')
    col.prop(p,'glass_spike_width',text='Spike Width')
    col.prop(p,'glass_spike_split',text='Spike Split')
    col.prop(p,'glass_spike_lean',text='Spike Lean')
    col.prop(p,'glass_notch_jitter',text='Notch Jitter')
    col.prop(p,'glass_blowout',text='Blowout')
    col.prop(p,'glass_blowout_reach',text='Blowout Reach')
    col.prop(p,'glass_edge_kink',text='Edge Kink')
    col.prop(p,'glass_shard_crack',text='Shard Crack')
    col=toggle_box(layout,p,'glass_crack_enable','Cracks')
    if col is not None:
        material_controls.inline(col,p,'fracture_l1_material',text='Cracks')
        col.prop(p,'glass_crack_width',text='Crack Width (cm)')
        col.prop(p,'glass_crack_depth',text='Crack Depth')
        col.prop(p,'glass_crack_both_sides',text='Crack Both Sides')
        if not p.glass_drawn_enable:col.prop(p,'glass_crack_reach',text='Crack Reach')
        col.prop(p,'glass_crack_reach_jitter',text='Crack Reach Jitter')
        col.prop(p,'glass_crack_meander',text='Crack Meander')
        col.prop(p,'glass_crack_branch',text='Crack Branch')
        col.prop(p,'glass_crack_taper',text='Crack Taper')
        col.prop(p,'glass_crush_size',text='Crush Size')
        col.prop(p,'glass_ring_count',text='Ring Count')
        col.prop(p,'glass_ring_first',text='Ring First')
        col.prop(p,'glass_ring_growth',text='Ring Growth')
        col.prop(p,'glass_ring_fill',text='Ring Fill')
        col.prop(p,'glass_ring_jitter',text='Ring Jitter')
        col.prop(p,'glass_ring_bow',text='Ring Bow')


class OBJECT_PT_project_damage_glass(_ProjectDamagePanel):
    bl_idname = "OBJECT_PT_project_damage_glass"
    bl_parent_id = "OBJECT_PT_project_damage"
    bl_label = _EFFECT_LABELS["glass"]
    effect_module = "glass"

    def draw(self, context):
        p = ownership.context_source(context).project_damage
        layout = self.layout
        layout.use_property_split = False
        layout.use_property_decorate = False

        if p.glass_hit_enable or p.glass_drawn_enable:
            draw_glass_asset(layout,context,p)
            return

        box = layout.box()
        row = box.row(align=True)
        row.alignment = 'CENTER'
        _effect_draw(SimpleNamespace(layout=box, effect_module="glass"), context)

                                                                             
                                                   
        from .params import glass_straight_through
        straight = glass_straight_through(p)

                                
        if not straight and (
            effect_layout.visible(p, "glass_auto_hole")
            or effect_layout.visible(p, "glass_auto_hole_thickness")
        ):
            box = layout.box()
            box.enabled = (p.glass_enable or p.glass_through_enable or p.glass_hit_enable or p.glass_drawn_enable) and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Thickness and openings", icon=effect_layout.heading_icon("Thickness and openings"))
            col = box.column(align=True)
            col.prop(p, "glass_auto_hole", text="Auto Hole")
            col.prop(p, "glass_auto_hole_thickness", text="Auto Hole Thickness (cm)")

                 
        if (
            effect_layout.visible(p, "glass_seed")
        ):
            box = layout.box()
            box.enabled = (p.glass_enable or p.glass_through_enable or p.glass_hit_enable or p.glass_drawn_enable) and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="General", icon=effect_layout.heading_icon("General"))
            col = box.column(align=True)
            col.prop(p, "glass_seed", text="Seed")

              
        if (
            effect_layout.visible(p, "glass_thickness")
            or effect_layout.visible(p, "glass_skin_depth")
            or effect_layout.visible(p, "glass_cut_margin")
            or effect_layout.visible(p, "glass_detail")
            or effect_layout.visible(p, "glass_avoid")
        ):
            box = layout.box()
            box.enabled = (p.glass_enable or p.glass_through_enable or p.glass_hit_enable or p.glass_drawn_enable) and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Pane", icon=effect_layout.heading_icon("Pane"))
            col = box.column(align=True)
            if not straight:
                material_controls.inline(box, p, 'surface_damage_material', text='Broken Openings')
                col.prop(p, "glass_thickness", text="Thickness (cm)")
                col.prop(p, "glass_skin_depth", text="Skin Depth (cm)")
                col.prop(p, "glass_cut_margin", text="Cut Margin")
            quality_ui.draw(col, p, "glass_detail", text="Resolution")
            pass

                           
        if (
            effect_layout.visible(p, "glass_impact_enable")
            or effect_layout.visible(p, "glass_impact_amount")
            or effect_layout.visible(p, "glass_hole_chance")
            or effect_layout.visible(p, "glass_hole_size_min")
            or effect_layout.visible(p, "glass_hole_size_max")
            or effect_layout.visible(p, "glass_hole_lobes")
            or effect_layout.visible(p, "glass_impact_spacing")
        ):
            box = layout.box()
            box.enabled = (p.glass_enable or p.glass_through_enable or p.glass_hit_enable or p.glass_drawn_enable) and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Automatic impacts", icon=effect_layout.heading_icon("Automatic impacts"))
            col = box.column(align=True)
            col.prop(p, "glass_impact_enable", text="Impact Enable")
            col.prop(p, "glass_impact_amount", text="Impact Amount")
            col.prop(p, "glass_hole_chance", text="Hole Chance")
            col.prop(p, "glass_hole_size_min", text="Hole Size Min (cm)")
            col.prop(p, "glass_hole_size_max", text="Hole Size Max (cm)")
            col.prop(p, "glass_hole_lobes", text="Hole Lobes")
            col.prop(p, "glass_impact_spacing", text="Impact Spacing")

                                  
        if (
            effect_layout.visible(p, "glass_radial_min")
            or effect_layout.visible(p, "glass_radial_max")
            or effect_layout.visible(p, "glass_radial_jitter")
            or effect_layout.visible(p, "glass_spike_chance")
            or effect_layout.visible(p, "glass_spike_length")
            or effect_layout.visible(p, "glass_spike_width")
            or effect_layout.visible(p, "glass_spike_split")
            or effect_layout.visible(p, "glass_spike_lean")
            or effect_layout.visible(p, "glass_notch_jitter")
            or effect_layout.visible(p, "glass_blowout")
            or effect_layout.visible(p, "glass_blowout_reach")
            or effect_layout.visible(p, "glass_edge_kink")
            or effect_layout.visible(p, "glass_shard_crack")
        ):
            box = layout.box()
            box.enabled = (p.glass_enable or p.glass_through_enable or p.glass_hit_enable or p.glass_drawn_enable) and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Shards along the opening", icon=effect_layout.heading_icon("Shards along the opening"))
            col = box.column(align=True)
            col.prop(p, "glass_radial_min", text="Radial Min")
            col.prop(p, "glass_radial_max", text="Radial Max")
            col.prop(p, "glass_radial_jitter", text="Radial Jitter")
            col.prop(p, "glass_spike_chance", text="Spike Chance")
            col.prop(p, "glass_spike_length", text="Spike Length")
            col.prop(p, "glass_spike_width", text="Spike Width")
            col.prop(p, "glass_spike_split", text="Spike Split")
            col.prop(p, "glass_spike_lean", text="Spike Lean")
            col.prop(p, "glass_notch_jitter", text="Notch Jitter")
            col.prop(p, "glass_blowout", text="Blowout")
            col.prop(p, "glass_blowout_reach", text="Blowout Reach")
            col.prop(p, "glass_edge_kink", text="Edge Kink")
            col.prop(p, "glass_shard_crack", text="Shard Crack")

                                                                       
        if (
            effect_layout.visible(p, "glass_crack_enable")
            or effect_layout.visible(p, "glass_crack_width")
            or effect_layout.visible(p, "glass_crack_depth")
            or effect_layout.visible(p, "glass_crack_both_sides")
            or effect_layout.visible(p, "glass_crack_reach")
            or effect_layout.visible(p, "glass_crack_reach_jitter")
            or effect_layout.visible(p, "glass_crack_meander")
            or effect_layout.visible(p, "glass_crack_branch")
            or effect_layout.visible(p, "glass_crack_taper")
            or effect_layout.visible(p, "glass_crush_size")
            or effect_layout.visible(p, "glass_ring_count")
            or effect_layout.visible(p, "glass_ring_first")
            or effect_layout.visible(p, "glass_ring_growth")
            or effect_layout.visible(p, "glass_ring_fill")
            or effect_layout.visible(p, "glass_ring_jitter")
            or effect_layout.visible(p, "glass_ring_bow")
        ):
            box = layout.box()
            box.enabled = (p.glass_enable or p.glass_through_enable or p.glass_hit_enable or p.glass_drawn_enable) and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Cracks (grooves cut into the faces; the pane stays one piece)", icon=effect_layout.heading_icon("Cracks (grooves cut into the faces; the pane stays one piece)"))
            col = box.column(align=True)
            material_controls.inline(box, p, 'fracture_l1_material', text='Cracks')
            col.prop(p, "glass_crack_enable", text="Crack Enable")
            col.prop(p, "glass_crack_width", text="Crack Width (cm)")
            col.prop(p, "glass_crack_depth", text="Crack Depth")
            col.prop(p, "glass_crack_both_sides", text="Crack Both Sides")
            col.prop(p, "glass_crack_reach", text="Crack Reach")
            col.prop(p, "glass_crack_reach_jitter", text="Crack Reach Jitter")
            col.prop(p, "glass_crack_meander", text="Crack Meander")
            col.prop(p, "glass_crack_branch", text="Crack Branch")
            col.prop(p, "glass_crack_taper", text="Crack Taper")
            col.prop(p, "glass_crush_size", text="Crush Size")
            col.prop(p, "glass_ring_count", text="Ring Count")
            col.prop(p, "glass_ring_first", text="Ring First")
            col.prop(p, "glass_ring_growth", text="Ring Growth")
            col.prop(p, "glass_ring_fill", text="Ring Fill")
            col.prop(p, "glass_ring_jitter", text="Ring Jitter")
            col.prop(p, "glass_ring_bow", text="Ring Bow")

                                                             
        if (
            effect_layout.visible(p, "glass_chip_enable")
            or effect_layout.visible(p, "glass_chip_chance")
            or effect_layout.visible(p, "glass_chip_spacing")
            or effect_layout.visible(p, "glass_chip_size")
            or effect_layout.visible(p, "glass_chip_depth")
        ):
            box = layout.box()
            box.enabled = (p.glass_enable or p.glass_through_enable or p.glass_hit_enable or p.glass_drawn_enable) and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Conchoidal chips on the exit face along the opening", icon=effect_layout.heading_icon("Conchoidal chips on the exit face along the opening"))
            col = box.column(align=True)
            material_controls.inline(box, p, 'edge_damage_material', text='Glass Chips')
            col.prop(p, "glass_chip_enable", text="Chip Enable")
            col.prop(p, "glass_chip_chance", text="Chip Chance")
            col.prop(p, "glass_chip_spacing", text="Chip Spacing (cm)")
            col.prop(p, "glass_chip_size", text="Chip Size (cm)")
            col.prop(p, "glass_chip_depth", text="Chip Depth")

                                                                    
        if (
            effect_layout.visible(p, "glass_paint_holes_enable")
            or effect_layout.visible(p, "glass_paint_impacts_enable")
            or effect_layout.visible(p, "glass_paint_only")
            or effect_layout.visible(p, "glass_paint_shard_width")
            or effect_layout.visible(p, "glass_paint_hole_cracks")
            or effect_layout.visible(p, "glass_paint_point_size")
            or effect_layout.visible(p, "glass_paint_impact_size")
            or effect_layout.visible(p, "glass_paint_point_hole")
            or effect_layout.visible(p, "glass_max_cracks")
        ):
            box = layout.box()
            box.enabled = (p.glass_enable or p.glass_through_enable or p.glass_hit_enable or p.glass_drawn_enable) and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Grease pencil drawing (same contour input as paint damage)", icon=effect_layout.heading_icon("Grease pencil drawing (same contour input as paint damage)"))
            col = box.column(align=True)
            col.prop(p, "glass_paint_holes_enable", text="Paint Holes Enable")
            col.prop(p, "glass_paint_impacts_enable", text="Paint Impacts Enable")
            col.prop(p, "glass_paint_only", text="Paint Only")
            col.prop(p, "glass_paint_shard_width", text="Paint Shard Width (cm)")
            col.prop(p, "glass_paint_hole_cracks", text="Paint Hole Cracks")
            col.prop(p, "glass_paint_point_size", text="Paint Point Size (cm)")
            col.prop(p, "glass_paint_impact_size", text="Paint Impact Size (cm)")
            col.prop(p, "glass_paint_point_hole", text="Paint Point Hole")
            col.prop(p, "glass_max_cracks", text="Max Cracks")


                                                                          
                                                              
                                                                          
class OBJECT_PT_project_damage_fissure(_ProjectDamagePanel):
    bl_idname = "OBJECT_PT_project_damage_fissure"
    bl_parent_id = "OBJECT_PT_project_damage"
    bl_label = _EFFECT_LABELS["fissure"]
    effect_module = "fissure"

    def draw(self, context):
        p=ownership.context_source(context).project_damage;layout=self.layout
        _effect_draw(SimpleNamespace(layout=layout,effect_module='fissure'),context)
        box=effect_layout.section(layout,'General');col=box.column(align=True)
        col.prop(p,'fissure_depth',text='Depth (cm)')
        col.prop(p,'fissure_master_scale',text='Master Scale')
        quality_ui.draw(col, p,'fissure_detail',text='Resolution')
        col.prop(p,'fissure_seed',text='Seed')
        col.prop(p,'fissure_up_x');col.prop(p,'fissure_up_y');col.prop(p,'fissure_up_z')
        box=effect_layout.section(layout,'The Main Fissure');col=box.column(align=True)
        material_controls.inline(col,p,'surface_damage_material',text='Chasm Walls')
        col.prop(p, 'fissure_width')
        col.prop(p, 'fissure_width_random')
        col.prop(p, 'fissure_depth_random')
        col.prop(p, 'fissure_taper')
        col.prop(p, 'fissure_jagged')
        col.prop(p, 'fissure_edge_scale')
        col.prop(p, 'fissure_wall_lean')
        col.prop(p, 'fissure_wall_rough')
        col.prop(p, 'fissure_floor_shape')
        col.prop(p, 'fissure_thickness_cap')
        if generated_geometry.draw(layout, p, 'fissure'):
            col=recording_tools.toggle_box(layout,p,'fissure_slab_enable','Tilted Slabs')
            if col is not None:
                material_controls.inline(col,p,'buildup_material',text='Slabs / Rubble')
                col.prop(p, 'fissure_slab_rows')
                col.prop(p, 'fissure_slab_length_min')
                col.prop(p, 'fissure_slab_length_max')
                col.prop(p, 'fissure_slab_width_min')
                col.prop(p, 'fissure_slab_width_max')
                col.prop(p, 'fissure_slab_row_falloff')
                col.prop(p, 'fissure_slab_thick_min')
                col.prop(p, 'fissure_slab_thick_max')
                col.prop(p, 'fissure_tilt_min')
                col.prop(p, 'fissure_tilt_max')
                col.prop(p, 'fissure_sink_chance')
                col.prop(p, 'fissure_roll')
                col.prop(p, 'fissure_flat_chance')
                col.prop(p, 'fissure_side_bias')
                col.prop(p, 'fissure_slab_gap')
                col.prop(p, 'fissure_slab_jagged')
                col.prop(p, 'fissure_slab_skew')
        col=recording_tools.toggle_box(layout,p,'fissure_branch_enable','Side Branches')
        if col is not None:
            material_controls.inline(col,p,'fracture_l1_material',text='Branches')
            col.prop(p, 'fissure_branch_density')
            col.prop(p, 'fissure_branch_length_min')
            col.prop(p, 'fissure_branch_length_max')
            col.prop(p, 'fissure_branch_width')
            col.prop(p, 'fissure_branch_depth')
            col.prop(p, 'fissure_branch_meander')
            col.prop(p, 'fissure_branch_angle')
            col.prop(p, 'fissure_branch_fork')
        if p.fissure_generated_enable:
            col=recording_tools.toggle_box(layout,p,'fissure_rubble_enable','Rubble')
            if col is not None:
                material_controls.inline(col,p,'buildup_material',text='Slabs / Rubble')
                col.prop(p, 'fissure_rubble_density')
                col.prop(p, 'fissure_rubble_size_min')
                col.prop(p, 'fissure_rubble_size_max')
                col.prop(p, 'fissure_rubble_wedged')
        box = layout.box()
        material_controls.centered_heading(box, 'Realism', icon='MOD_EXPLODE')
        box.prop(p, 'fissure_realistic', text='Realistic Broken Concrete', toggle=True, icon='MOD_EXPLODE')
        if p.fissure_realistic:
            col = box.column(align=True)
            col.prop(p, 'fissure_edge_damage', text='Edge Damage')
            col.prop(p, 'fissure_face_rough', text='Face Roughness')
            col.prop(p, 'fissure_split_chance', text='Corner Breaks')
            col.prop(p, 'fissure_surface_cracks', text='Surface Cracks')
            if p.fissure_generated_enable:col.prop(p, 'fissure_debris', text='Debris')
            if p.fissure_generated_enable and p.fissure_debris > 0:
                col.prop(p, 'fissure_debris_size', text='Debris Size')



                                                                          
                                                            
                                                                          
class OBJECT_PT_project_damage_bevel(_ProjectDamagePanel):
    bl_idname = "OBJECT_PT_project_damage_bevel"
    bl_parent_id = "OBJECT_PT_project_damage"
    bl_label = _EFFECT_LABELS["bevel"]
    effect_module = "bevel"

    def draw(self, context):
        p = ownership.context_source(context).project_damage
        layout = self.layout
        layout.use_property_split = False
        layout.use_property_decorate = False

        box = layout.box()
        row = box.row(align=True)
        row.alignment = 'CENTER'
        _effect_draw(SimpleNamespace(layout=box, effect_module="bevel"), context)

                 
        if (
            effect_layout.visible(p, "bevel_seed")
            or effect_layout.visible(p, "bevel_avoid")
            or effect_layout.visible(p, "bevel_detail")
        ):
            box = layout.box()
            box.enabled = p.bevel_enable and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="General", icon=effect_layout.heading_icon("General"))
            col = box.column(align=True)
            col.prop(p, "bevel_seed", text="Seed")
            pass
            quality_ui.draw(col, p, "bevel_detail", text="Resolution")

                     
        if (
            effect_layout.visible(p, "bevel_angle")
            or effect_layout.visible(p, "bevel_drawn_only")
            or effect_layout.visible(p, "bevel_paint_radius")
            or effect_layout.visible(p, "bevel_paint_align")
            or effect_layout.visible(p, "bevel_paint_cover")
            or effect_layout.visible(p, "bevel_paint_partial")
            or effect_layout.visible(p, "bevel_paint_taper")
            or effect_layout.visible(p, "bevel_max_turn")
        ):
            box = layout.box()
            box.enabled = p.bevel_enable and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Which edges", icon=effect_layout.heading_icon("Which edges"))
            col = box.column(align=True)
            col.prop(p, "bevel_angle", text="Angle")
            col.prop(p, "bevel_drawn_only", text="Drawn Only")
            col.prop(p, "bevel_paint_radius", text="Paint Radius (cm)")
            col.prop(p, "bevel_paint_align", text="Paint Align")
            col.prop(p, "bevel_paint_cover", text="Paint Deepness")
            col.prop(p, "bevel_paint_partial", text="Paint Partial")
            col.prop(p, "bevel_paint_taper", text="Paint Taper (cm)")
            col.prop(p, "bevel_max_turn", text="Max Turn")

                 
        if (
            effect_layout.visible(p, "bevel_profile")
            or effect_layout.visible(p, "bevel_roundness")
            or effect_layout.visible(p, "bevel_fillet_top")
            or effect_layout.visible(p, "bevel_fillet_side")
            or effect_layout.visible(p, "bevel_size_mode")
            or effect_layout.visible(p, "bevel_size_top")
            or effect_layout.visible(p, "bevel_size_side")
            or effect_layout.visible(p, "bevel_max_share")
            or effect_layout.visible(p, "bevel_taper")
            or effect_layout.visible(p, "bevel_taper_flip")
            or effect_layout.visible(p, "bevel_round_corners")
        ):
            box = layout.box()
            box.enabled = p.bevel_enable and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Profile", icon=effect_layout.heading_icon("Profile"))
            col = box.column(align=True)
            ui_image_hints.prop(col,p, "bevel_profile", text="Profile")
            col.prop(p, "bevel_roundness", text="Roundness")
            col.prop(p, "bevel_fillet_top", text="Fillet Top")
            col.prop(p, "bevel_fillet_side", text="Fillet Side")
            col.prop(p, "bevel_size_mode", text="Size Mode")
            col.prop(p, "bevel_size_top", text="Size Top (cm)")
            col.prop(p, "bevel_size_side", text="Size Side (cm)")
            col.prop(p, "bevel_max_share", text="Max Share")
            col.prop(p, "bevel_taper", text="Taper")
            col.prop(p, "bevel_taper_flip", text="Taper Flip")
            col.prop(p, "bevel_round_corners", text="Round Corners")

                         
        if (
            effect_layout.visible(p, "bevel_wear_enable")
            or effect_layout.visible(p, "bevel_wear_uneven")
            or effect_layout.visible(p, "bevel_wear_uneven_scale")
            or effect_layout.visible(p, "bevel_wear_rough")
            or effect_layout.visible(p, "bevel_wear_chips")
            or effect_layout.visible(p, "bevel_wear_chip_min")
            or effect_layout.visible(p, "bevel_wear_chip_max")
            or effect_layout.visible(p, "bevel_wear_chip_depth")
            or effect_layout.visible(p, "bevel_wear_chip_stretch")
            or effect_layout.visible(p, "bevel_wear_big")
            or effect_layout.visible(p, "bevel_wear_mask_width")
            or effect_layout.visible(p, "bevel_wear_mask_noise")
            or effect_layout.visible(p, "bevel_wear_mask_scale")
            or effect_layout.visible(p, "bevel_up_x")
            or effect_layout.visible(p, "bevel_up_y")
            or effect_layout.visible(p, "bevel_up_z")
        ):
            box = layout.box()
            box.enabled = p.bevel_enable and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Edge wear layer", icon=effect_layout.heading_icon("Edge wear layer"))
            col = box.column(align=True)
            col.prop(p, "bevel_wear_enable", text="Wear Enable")
            col.prop(p, "bevel_wear_uneven", text="Wear Uneven")
            col.prop(p, "bevel_wear_uneven_scale", text="Wear Uneven Scale (cm)")
            col.prop(p, "bevel_wear_rough", text="Wear Rough")
            col.prop(p, "bevel_wear_chips", text="Wear Chips")
            col.prop(p, "bevel_wear_chip_min", text="Wear Chip Min (cm)")
            col.prop(p, "bevel_wear_chip_max", text="Wear Chip Max (cm)")
            col.prop(p, "bevel_wear_chip_depth", text="Wear Chip Depth")
            col.prop(p, "bevel_wear_chip_stretch", text="Wear Chip Stretch")
            col.prop(p, "bevel_wear_big", text="Wear Big")
            col.prop(p, "bevel_wear_mask_width", text="Wear Mask Width (cm)")
            col.prop(p, "bevel_wear_mask_noise", text="Wear Mask Noise")
            col.prop(p, "bevel_wear_mask_scale", text="Wear Mask Scale (cm)")
            col.prop(p, "bevel_up_x", text="Up X")
            col.prop(p, "bevel_up_y", text="Up Y")
            col.prop(p, "bevel_up_z", text="Up Z")


                                                                          
                                                              
                                                                          
class OBJECT_PT_project_damage_scratch(_ProjectDamagePanel):
    bl_idname = "OBJECT_PT_project_damage_scratch"
    bl_parent_id = "OBJECT_PT_project_damage"
    bl_label = _EFFECT_LABELS["scratch"]
    effect_module = "scratch"

    def draw(self, context):
        p = ownership.context_source(context).project_damage
        layout = self.layout
        layout.use_property_split = False
        layout.use_property_decorate = False

        box = layout.box()
        row = box.row(align=True)
        row.alignment = 'CENTER'
        _effect_draw(SimpleNamespace(layout=box, effect_module="scratch"), context)

                               
        if (
            effect_layout.visible(p, "scratch_mode")
        ):
            box = layout.box()
            box.enabled = p.scratch_enable and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Stroke interpretation", icon=effect_layout.heading_icon("Stroke interpretation"))
            col = box.column(align=True)
            col.prop(p, "scratch_mode", text="Mode")

              
        if (
            effect_layout.visible(p, "scratch_size_scale")
        ):
            box = layout.column()
                                                             
                                      
                                     
                                                                            
                                         
                                                                 

                 
        if (
            effect_layout.visible(p, "scratch_seed")
            or effect_layout.visible(p, "scratch_style")
            or effect_layout.visible(p, "scratch_avoid")
            or effect_layout.visible(p, "scratch_detail")
        ):
            box = layout.box()
            box.enabled = p.scratch_enable and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="General", icon=effect_layout.heading_icon("General"))
            col = box.column(align=True)
            col.prop(p, "scratch_style", text="Style")
            col.separator()
            col.prop(p, "scratch_size_scale", text="Size Scale")
            quality_ui.draw(col, p, "scratch_detail", text="Resolution")
            col.prop(p, "scratch_seed", text="Seed")

                            
        if (
            effect_layout.visible(p, "scratch_density")
            or effect_layout.visible(p, "scratch_max_scratches")
            or effect_layout.visible(p, "scratch_spread")
            or effect_layout.visible(p, "scratch_falloff")
            or effect_layout.visible(p, "scratch_angle_jitter")
            or effect_layout.visible(p, "scratch_cross_share")
            or effect_layout.visible(p, "scratch_along")
            or effect_layout.visible(p, "scratch_paint_snap")
        ):
            box = layout.box()
            box.enabled = p.scratch_enable and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Placement Settings", icon=effect_layout.heading_icon("Placement Settings"))
            col = box.column(align=True)
            col.prop(p, "scratch_density", text="Density")
            col.prop(p, "scratch_max_scratches", text="Max Scratches")
            col.prop(p, "scratch_spread", text="Spread (cm)")
            col.prop(p, "scratch_falloff", text="Falloff")
            col.prop(p, "scratch_angle_jitter", text="Angle Jitter")
            col.prop(p, "scratch_cross_share", text="Cross Share")
            col.prop(p, "scratch_along", text="Along")
            col.prop(p, "scratch_paint_snap", text="Paint Snap (cm)")

               
        if (
            effect_layout.visible(p, "scratch_length_min")
            or effect_layout.visible(p, "scratch_length_max")
            or effect_layout.visible(p, "scratch_length_bias")
            or effect_layout.visible(p, "scratch_width")
            or effect_layout.visible(p, "scratch_width_var")
            or effect_layout.visible(p, "scratch_depth")
            or effect_layout.visible(p, "scratch_depth_var")
            or effect_layout.visible(p, "scratch_profile")
            or effect_layout.visible(p, "scratch_taper")
            or effect_layout.visible(p, "scratch_gouge")
            or effect_layout.visible(p, "scratch_curve")
            or effect_layout.visible(p, "scratch_wobble")
            or effect_layout.visible(p, "scratch_gaps")
            or effect_layout.visible(p, "scratch_wrap_angle")
        ):
            box = layout.box()
            box.enabled = p.scratch_enable and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Shape", icon=effect_layout.heading_icon("Shape"))
            col = box.column(align=True)
            material_controls.inline(box, p, 'scratch_groove_material', text='Scratch Grooves')
            col.prop(p, "scratch_length_min", text="Length Min (cm)")
            col.prop(p, "scratch_length_max", text="Length Max (cm)")
            col.prop(p, "scratch_length_bias", text="Length Bias")
            col.prop(p, "scratch_width", text="Width (cm)")
            col.prop(p, "scratch_width_var", text="Width Var")
            col.prop(p, "scratch_depth", text="Depth (cm)")
            col.prop(p, "scratch_depth_var", text="Depth Var")
            col.prop(p, "scratch_profile", text="Profile")
            col.prop(p, "scratch_taper", text="Taper")
            col.prop(p, "scratch_gouge", text="Gouge")
            col.prop(p, "scratch_curve", text="Curve")
            col.prop(p, "scratch_wobble", text="Wobble")
            col.prop(p, "scratch_gaps", text="Gaps")
            col.prop(p, "scratch_wrap_angle", text="Wrap Angle")

                                                                      
        if (
            effect_layout.visible(p, "scratch_bundle_chance")
            or effect_layout.visible(p, "scratch_bundle_min")
            or effect_layout.visible(p, "scratch_bundle_max")
            or effect_layout.visible(p, "scratch_bundle_gap")
            or effect_layout.visible(p, "scratch_bundle_thin")
        ):
            box = layout.box()
            box.enabled = p.scratch_enable and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Bundles (several parallel lines dragged together, like grit)", icon=effect_layout.heading_icon("Bundles (several parallel lines dragged together, like grit)"))
            col = box.column(align=True)
            col.prop(p, "scratch_bundle_chance", text="Bundle Chance")
            col.prop(p, "scratch_bundle_min", text="Bundle Min")
            col.prop(p, "scratch_bundle_max", text="Bundle Max")
            col.prop(p, "scratch_bundle_gap", text="Bundle Gap (cm)")
            col.prop(p, "scratch_bundle_thin", text="Bundle Thin")

        if material_controls.experimental() and p.scratch_style != '3':
            box=effect_layout.section(layout, 'Scuff Band')
            material_controls.inline(box, p, 'scratch_scuff_material', text='Scuffed Paint')

                                                                   
        if (
            effect_layout.visible(p, "scratch_scuff_enable")
            or effect_layout.visible(p, "scratch_scuff_width")
            or effect_layout.visible(p, "scratch_scuff_depth")
        ):
            box = layout.box()
            box.enabled = p.scratch_enable and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Scuff band (paper-thin cut that only carries a selection)", icon=effect_layout.heading_icon("Scuff band (paper-thin cut that only carries a selection)"))
            col = box.column(align=True)
            material_controls.inline(box, p, 'scratch_scuff_material', text='Scuffed Paint')
            col.prop(p, "scratch_scuff_enable", text="Scuff Enable")
            col.prop(p, "scratch_scuff_width", text="Scuff Width")
            col.prop(p, "scratch_scuff_depth", text="Scuff Depth (cm)")

                    
        if (
            effect_layout.visible(p, "scratch_mask_falloff")
        ):
            box = layout.box()
            box.enabled = p.scratch_enable and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Float mask", icon=effect_layout.heading_icon("Float mask"))
            col = box.column(align=True)
            col.prop(p, "scratch_mask_falloff", text="Mask Falloff (cm)")


                                                                          
                                                            
                                                                          
class OBJECT_PT_project_damage_metal(_ProjectDamagePanel):
    bl_idname = "OBJECT_PT_project_damage_metal"
    bl_parent_id = "OBJECT_PT_project_damage"
    bl_label = _EFFECT_LABELS["metal"]
    effect_module = "metal"

    def draw(self, context):
        p = ownership.context_source(context).project_damage
        layout = self.layout
        layout.use_property_split = False
        layout.use_property_decorate = False

        box = layout.box()
        row = box.row(align=True)
        row.alignment = 'CENTER'
        _effect_draw(SimpleNamespace(layout=box, effect_module="metal"), context)

                 
        if (
            effect_layout.visible(p, "metal_seed")
            or effect_layout.visible(p, "metal_avoid")
            or effect_layout.visible(p, "metal_style")
        ):
            box = layout.box()
            box.enabled = p.metal_enable and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="General", icon=effect_layout.heading_icon("General"))
            col = box.column(align=True)
            col.prop(p, "metal_seed", text="Seed")
            pass
            col.prop(p, "metal_style", text="Style")

                    
        if (
            effect_layout.visible(p, "metal_edge_length")
            or effect_layout.visible(p, "metal_detail")
            or effect_layout.visible(p, "metal_max_tris")
        ):
            box = layout.box()
            box.enabled = p.metal_enable and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Resolution", icon=effect_layout.heading_icon("Resolution"))
            col = box.column(align=True)
            col.prop(p, "metal_edge_length", text="Edge Length (cm)")
            quality_ui.draw(col, p, "metal_detail", text="Resolution")
            col.prop(p, "metal_max_tris", text="Max Tris")

                 
        if (
            effect_layout.visible(p, "metal_depth_scale")
            or effect_layout.visible(p, "metal_clearance")
            or effect_layout.visible(p, "metal_proximity")
            or effect_layout.visible(p, "metal_proximity_depth")
        ):
            box = layout.box()
            box.enabled = p.metal_enable and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Contact", icon=effect_layout.heading_icon("Contact"))
            col = box.column(align=True)
            col.prop(p, "metal_depth_scale", text="Depth Scale")
            col.prop(p, "metal_clearance", text="Clearance (cm)")
            col.prop(p, "metal_proximity", text="Proximity (cm)")
            col.prop(p, "metal_proximity_depth", text="Proximity Depth (cm)")

                    
        if (
            effect_layout.visible(p, "metal_falloff")
            or effect_layout.visible(p, "metal_falloff_min")
            or effect_layout.visible(p, "metal_radius_max")
            or effect_layout.visible(p, "metal_profile")
            or effect_layout.visible(p, "metal_bulge")
        ):
            box = layout.box()
            box.enabled = p.metal_enable and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Dent shape", icon=effect_layout.heading_icon("Dent shape"))
            col = box.column(align=True)
            col.prop(p, "metal_falloff", text="Falloff")
            col.prop(p, "metal_falloff_min", text="Falloff Min (cm)")
            col.prop(p, "metal_radius_max", text="Radius Max (cm)")
            col.prop(p, "metal_profile", text="Profile")
            col.prop(p, "metal_bulge", text="Bulge")

                        
        if (
            effect_layout.visible(p, "metal_sheet_thickness")
            or effect_layout.visible(p, "metal_thickness_cap")
        ):
            box = layout.box()
            box.enabled = p.metal_enable and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Sheet vs solid", icon=effect_layout.heading_icon("Sheet vs solid"))
            col = box.column(align=True)
            col.prop(p, "metal_sheet_thickness", text="Sheet Thickness (cm)")
            col.prop(p, "metal_thickness_cap", text="Thickness Cap")

                                                    
        if (
            effect_layout.visible(p, "metal_crease")
            or effect_layout.visible(p, "metal_crease_spacing")
            or effect_layout.visible(p, "metal_crease_length")
            or effect_layout.visible(p, "metal_crease_sharp")
            or effect_layout.visible(p, "metal_crease_branch")
        ):
            box = layout.box()
            box.enabled = p.metal_enable and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Creases (radial buckles in the dent walls)", icon=effect_layout.heading_icon("Creases (radial buckles in the dent walls)"))
            col = box.column(align=True)
            col.prop(p, "metal_crease", text="Crease")
            col.prop(p, "metal_crease_spacing", text="Crease Spacing (cm)")
            col.prop(p, "metal_crease_length", text="Crease Length")
            col.prop(p, "metal_crease_sharp", text="Crease Sharp")
            col.prop(p, "metal_crease_branch", text="Crease Branch")

                                                           
        if (
            effect_layout.visible(p, "metal_crumple")
            or effect_layout.visible(p, "metal_crumple_scale")
        ):
            box = layout.box()
            box.enabled = p.metal_enable and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Crumple (small facets on the most deformed metal)", icon=effect_layout.heading_icon("Crumple (small facets on the most deformed metal)"))
            col = box.column(align=True)
            col.prop(p, "metal_crumple", text="Crumple")
            col.prop(p, "metal_crumple_scale", text="Crumple Scale (cm)")

               
        if (
            effect_layout.visible(p, "metal_roughness")
        ):
            box = layout.box()
            box.enabled = p.metal_enable and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Grain", icon=effect_layout.heading_icon("Grain"))
            col = box.column(align=True)
            col.prop(p, "metal_roughness", text="Roughness (cm)")

                               
        if (
            effect_layout.visible(p, "metal_crush_aim")
            or effect_layout.visible(p, "metal_crush_proximity")
        ):
            box = layout.box()
            box.enabled = p.metal_enable and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Placement / proximity", icon=effect_layout.heading_icon("Placement / proximity"))
            col = box.column(align=True)
            col.prop(p, "metal_crush_aim", text="Crush Aim")
            col.prop(p, "metal_crush_proximity", text="Crush Proximity")

               
        if (
            effect_layout.visible(p, "metal_crush_depth")
            or effect_layout.visible(p, "metal_crush_length")
            or effect_layout.visible(p, "metal_crush_hinge")
            or effect_layout.visible(p, "metal_crush_skew")
        ):
            box = layout.box()
            box.enabled = p.metal_enable and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Crush", icon=effect_layout.heading_icon("Crush"))
            col = box.column(align=True)
            col.prop(p, "metal_crush_depth", text="Crush Depth")
            col.prop(p, "metal_crush_length", text="Crush Length")
            col.prop(p, "metal_crush_hinge", text="Crush Hinge")
            col.prop(p, "metal_crush_skew", text="Crush Skew")

                                                                    
        if (
            effect_layout.visible(p, "metal_fold_size")
            or effect_layout.visible(p, "metal_fold")
            or effect_layout.visible(p, "metal_fold_sharp")
            or effect_layout.visible(p, "metal_fold_wander")
            or effect_layout.visible(p, "metal_fold_lift")
        ):
            box = layout.box()
            box.enabled = p.metal_enable and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Accordion folds (the length the crush took out buckles up)", icon=effect_layout.heading_icon("Accordion folds (the length the crush took out buckles up)"))
            col = box.column(align=True)
            col.prop(p, "metal_fold_size", text="Fold Size (cm)")
            col.prop(p, "metal_fold", text="Fold")
            col.prop(p, "metal_fold_sharp", text="Fold Sharp")
            col.prop(p, "metal_fold_wander", text="Fold Wander")
            col.prop(p, "metal_fold_lift", text="Fold Lift")

                         
        if (
            effect_layout.visible(p, "metal_crush_crumple")
            or effect_layout.visible(p, "metal_crush_facet")
        ):
            box = layout.box()
            box.enabled = p.metal_enable and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Crumpled facets", icon=effect_layout.heading_icon("Crumpled facets"))
            col = box.column(align=True)
            col.prop(p, "metal_crush_crumple", text="Crush Crumple")
            col.prop(p, "metal_crush_facet", text="Crush Facet (cm)")


                                                                          
                                                             
                                                                          
class OBJECT_PT_project_damage_debris(_ProjectDamagePanel):
    bl_idname = "OBJECT_PT_project_damage_debris"
    bl_parent_id = "OBJECT_PT_project_damage"
    bl_label = _EFFECT_LABELS["debris"]
    effect_module = "debris"

    def draw(self, context):
        p = ownership.context_source(context).project_damage
        layout = self.layout
        layout.use_property_split = False
        layout.use_property_decorate = False

        box = layout.box()
        row = box.row(align=True)
        row.alignment = 'CENTER'
        _effect_draw(SimpleNamespace(layout=box, effect_module="debris"), context)

                           
        if (
            effect_layout.visible(p, "debris_seed")
            or effect_layout.visible(p, "debris_min_volume")
            or effect_layout.visible(p, "debris_min_thickness")
            or effect_layout.visible(p, "debris_chunk_size")
            or effect_layout.visible(p, "debris_chunk_jitter")
            or effect_layout.visible(p, "debris_max_pieces")
        ):
            box = layout.box()
            box.enabled = p.debris_enable and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Debris extraction", icon=effect_layout.heading_icon("Debris extraction"))
            col = box.column(align=True)
            col.prop(p, "debris_seed", text="Seed")
            col.prop(p, "debris_min_volume", text="Min Volume (cm³)")
            col.prop(p, "debris_min_thickness", text="Min Thickness (cm)")
            col.prop(p, "debris_chunk_size", text="Chunk Size (cm)")
            col.prop(p, "debris_chunk_jitter", text="Chunk Jitter")
            col.prop(p, "debris_max_pieces", text="Max Pieces")

                          
        if (
            effect_layout.visible(p, "debris_inner_uv_scale")
        ):
            box = layout.box()
            box.enabled = p.debris_enable and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Inner surface UV", icon=effect_layout.heading_icon("Inner surface UV"))
            col = box.column(align=True)
            material_controls.inline(box, p, 'debris_material', text='Inner Surface')
            col.prop(p, "debris_inner_uv_scale", text="Inner UV Scale (cm)")

                 
        if (
            effect_layout.visible(p, "debris_smooth_angle")
        ):
            box = layout.box()
            box.enabled = p.debris_enable and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Shading", icon=effect_layout.heading_icon("Shading"))
            col = box.column(align=True)
            col.prop(p, "debris_smooth_angle", text="Smooth Angle (degrees)")


                                                                          
                                                          
                                                                          
class OBJECT_PT_project_damage_rbf(_ProjectDamagePanel):
    bl_idname = "OBJECT_PT_project_damage_rbf"
    bl_parent_id = "OBJECT_PT_project_damage"
    bl_label = _EFFECT_LABELS["rbf"]
    effect_module = "rbf"

    def draw(self, context):
        p = ownership.context_source(context).project_damage
        layout = self.layout
        layout.use_property_split = False
        layout.use_property_decorate = False

        box = layout.box()
        row = box.row(align=True)
        row.alignment = 'CENTER'
        _effect_draw(SimpleNamespace(layout=box, effect_module="rbf"), context)

                
        if (
            effect_layout.visible(p, "rbf_seed")
            or effect_layout.visible(p, "rbf_pieces")
            or effect_layout.visible(p, "rbf_max_pieces")
            or effect_layout.visible(p, "rbf_gap")
            or effect_layout.visible(p, "rbf_size_variation")
            or effect_layout.visible(p, "rbf_min_piece")
        ):
            box = layout.box()
            box.enabled = p.rbf_enable and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Pieces", icon=effect_layout.heading_icon("Pieces"))
            col = box.column(align=True)
            col.prop(p, "rbf_seed", text="Seed")
            col.prop(p, "rbf_pieces", text="Pieces")
            col.prop(p, "rbf_max_pieces", text="Max Pieces")
            col.prop(p, "rbf_gap", text="Gap")
            col.prop(p, "rbf_size_variation", text="Size Variation")
            col.prop(p, "rbf_min_piece", text="Min Piece")

                          
        if (
            effect_layout.visible(p, "rbf_cut_spacing")
            or effect_layout.visible(p, "rbf_impact_radius")
            or effect_layout.visible(p, "rbf_impact_density")
            or effect_layout.visible(p, "rbf_rubble")
        ):
            box = layout.box()
            box.enabled = p.rbf_enable and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Cuts and impacts", icon=effect_layout.heading_icon("Cuts and impacts"))
            col = box.column(align=True)
            col.prop(p, "rbf_cut_spacing", text="Cut Spacing")
            col.prop(p, "rbf_impact_radius", text="Impact Radius")
            col.prop(p, "rbf_impact_density", text="Impact Density")
            col.prop(p, "rbf_rubble", text="Rubble")

                          
        if (
            effect_layout.visible(p, "rbf_grain_x")
            or effect_layout.visible(p, "rbf_grain_y")
            or effect_layout.visible(p, "rbf_grain_z")
            or effect_layout.visible(p, "rbf_grain_stretch")
            or effect_layout.visible(p, "rbf_weave_stretch")
            or effect_layout.visible(p, "rbf_glass_spokes")
            or effect_layout.visible(p, "rbf_glass_ring_growth")
        ):
            box = layout.box()
            box.enabled = p.rbf_enable and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Material pattern", icon=effect_layout.heading_icon("Material pattern"))
            col = box.column(align=True)
            col.prop(p, "rbf_grain_x", text="Grain X")
            col.prop(p, "rbf_grain_y", text="Grain Y")
            col.prop(p, "rbf_grain_z", text="Grain Z")
            col.prop(p, "rbf_grain_stretch", text="Grain Stretch")
            col.prop(p, "rbf_weave_stretch", text="Weave Stretch")
            col.prop(p, "rbf_glass_spokes", text="Glass Spokes")
            col.prop(p, "rbf_glass_ring_growth", text="Glass Ring Growth")

                      
        if (
            effect_layout.visible(p, "rbf_inner_uv_scale")
            or effect_layout.visible(p, "rbf_smooth_angle")
        ):
            box = layout.box()
            box.enabled = p.rbf_enable and not p.all_off
            row = box.row(align=True)
            row.alignment = 'CENTER'
            row.label(text="Surface data", icon=effect_layout.heading_icon("Surface data"))
            col = box.column(align=True)
            col.prop(p, "rbf_inner_uv_scale", text="Inner Uv Scale")
            col.prop(p, "rbf_smooth_angle", text="Smooth Angle")


                                                                          
                                                                           
                                                                          
class OBJECT_PT_project_damage_cellf(_ProjectDamagePanel):
    bl_idname = "OBJECT_PT_project_damage_cellf"
    bl_parent_id = "OBJECT_PT_project_damage"
    bl_label = _EFFECT_LABELS["cellf"]
    effect_module = "cellf"

    def draw(self, context):
        p = ownership.context_source(context).project_damage
        layout = self.layout
        layout.use_property_split = False
        layout.use_property_decorate = False

        box = layout.box()
        _effect_draw(SimpleNamespace(layout=box, effect_module="cellf"), context)

        box=layout.box()
        material_controls.centered_heading(box,'Pieces',icon='MOD_EXPLODE')
        col=box.column(align=True)
        material_controls.inline(col,p,'cellf_inner_material',text='Inner Faces')
        col.prop(p,'cellf_pieces',text='Pieces')
        col.prop(p,'cellf_gap',text='Gap')
        col.prop(p,'cellf_size_variation',text='Size Variation')
        col.prop(p,'cellf_seed',text='Seed')
        box=layout.box()
        material_controls.centered_heading(box,'Break Faces',icon='FACESEL')
        col=box.column(align=True)
        col.prop(p,'cellf_roughness',text='Roughness')
        if p.cellf_material!='1' and p.cellf_roughness>1.25:
            col.label(text='High roughness can skip folded pieces',icon='ERROR')
        relief=col.column(align=True)
        relief.enabled=p.cellf_roughness>0 if p.cellf_material=='1' else p.cellf_roughness!=0
        relief.prop(p,'cellf_rough_scale',text='Rough Scale')
        quality_ui.draw(relief, p,'cellf_detail',text='Detail')
        if p.cellf_material!='1':
            row=col.row(align=True);row.scale_y=1.5
            row.prop(p,'cellf_planar',text='Through Thickness',expand=True)
            if p.cellf_planar=='0':col.prop(p,'cellf_planar_ratio',text='Planar Ratio')
        else:
            col.label(text='Glass cracks span the pane thickness',icon='INFO')
        if p.cellf_prox_collection is not None:
            box=layout.box()
            material_controls.centered_heading(box,'Proximity',icon='EMPTY_AXIS')
            col=box.column(align=True)
            col.prop(p,'cellf_prox_radius',text='Radius')
            if p.cellf_prox_use_size:
                col.label(text='Empties use their own size for Radius',icon='INFO')
            col.prop(p,'cellf_prox_density',text='Density')
            col.prop(p,'cellf_prox_falloff',text='Falloff')
            col.prop(p,'cellf_rubble',text='Rubble')
            col.prop(p,'cellf_keep_remainder',text='Intact Remainder',toggle=True)
            if p.cellf_keep_remainder:col.prop(p,'cellf_remainder_reach',text='Remainder Reach')
        if p.cellf_material=='0':
            box=layout.box()
            material_controls.centered_heading(box,'Wood Pattern',icon='OUTLINER_OB_CURVE')
            col=box.column(align=True)
            row=col.row(align=True)
            row.prop(p,'cellf_grain_x',text='X')
            row.prop(p,'cellf_grain_y',text='Y')
            row.prop(p,'cellf_grain_z',text='Z')
            col.prop(p,'cellf_grain_stretch',text='Grain Stretch')
            col.prop(p,'cellf_splinter',text='Splinter')
        elif p.cellf_material=='1':
            box=layout.box()
            material_controls.centered_heading(box,'Glass Pattern',icon='MESH_UVSPHERE')
            col=box.column(align=True)
            col.prop(p,'cellf_glass_spokes',text='Radial Cracks')
            col.prop(p,'cellf_glass_ring_growth',text='Ring Spacing')
            col.prop(p,'cellf_glass_branch',text='Branching')
            col.prop(p,'cellf_glass_wander',text='Wander')
            col.prop(p,'cellf_glass_hole',text='Hole')
        box=layout.box()
        material_controls.centered_heading(box,'Surface Data',icon='GROUP_UVS')
        col=box.column(align=True)
        col.prop(p,'cellf_inner_uv_scale',text='Inner UV Scale')
        col.prop(p,'cellf_smooth_angle',text='Smooth Angle')


# раздел «Rusty Holes»
class OBJECT_PT_project_damage_rusty(_ProjectDamagePanel):
    bl_idname='OBJECT_PT_project_damage_rusty'
    bl_parent_id='OBJECT_PT_project_damage'
    bl_label='Rusty Holes'
    effect_module='rusty'
    def draw(self,context):
        p=ownership.context_source(context).project_damage;layout=self.layout
        _effect_draw(SimpleNamespace(layout=layout,effect_module='rusty'),context)
        box=effect_layout.section(layout,'General');col=box.column(align=True)
        col.prop(p,'rusty_master_scale',text='Master Scale')
        quality_ui.draw(col, p,'rusty_detail',text='Resolution')
        col.prop(p,'rusty_seed',text='Seed')
        if p.rusty_mode=='0':
            box=effect_layout.section(layout,'Placement');col=box.column(align=True)
            col.prop(p, 'rusty_amount')
            col.prop(p, 'rusty_size_min')
            col.prop(p, 'rusty_size_max')
            col.prop(p, 'rusty_size_bias')
            col.prop(p, 'rusty_min_spacing')
        else:
            box=effect_layout.section(layout,'Paint Settings');col=box.column(align=True)
            col.prop(p, 'rusty_size_min')
            col.prop(p, 'rusty_size_max')
            col.prop(p, 'rusty_paint_size')
            col.prop(p, 'rusty_paint_spacing')
            col.prop(p, 'rusty_paint_gaps')
            col.prop(p, 'rusty_paint_crack')
            col.prop(p, 'rusty_paint_single')
            col.prop(p, 'rusty_paint_close')
            col.prop(p, 'rusty_paint_snap')
        box=effect_layout.section(layout,'Hole Shape');col=box.column(align=True)
        material_controls.inline(col,p,'surface_damage_material',text='Hole Walls')
        col.prop(p,'rusty_thickness',text='Thickness')
        col.prop(p, 'rusty_lobes')
        col.prop(p, 'rusty_lobe_spread')
        col.prop(p, 'rusty_jagged')
        col.prop(p, 'rusty_roughness')
        col.prop(p, 'rusty_scallop')
        col.prop(p, 'rusty_merge')
        col.prop(p, 'rusty_lace')
        col.separator(factor=.5)
        col.prop(p,'rusty_min_bridge',text='Min Bridge')
        col.prop(p,'rusty_stretch',text='Stretch')
        col.prop(p,'rusty_flow',text='Flow')
        box=effect_layout.section(layout,'Pits');col=box.column(align=True)
        col.prop(p,'rusty_pit_amount')
        if p.rusty_pit_amount>0:
            col.prop(p,'rusty_pit_size');col.prop(p,'rusty_pit_spread')
        box=effect_layout.section(layout,'Cracks');col=box.column(align=True)
        col.prop(p,'rusty_crack_amount')
        if p.rusty_crack_amount>0:
            col.prop(p, 'rusty_crack_length')
            col.prop(p, 'rusty_crack_meander')
            col.prop(p, 'rusty_crack_fork')
            col.prop(p, 'rusty_crack_width')
            col.prop(p, 'rusty_crack_pits')


_EFFECT_PANELS = (OBJECT_PT_project_damage_hang,
    OBJECT_PT_project_damage_fabric,
    OBJECT_PT_project_damage_concrete,
    OBJECT_PT_project_damage_hit,
    OBJECT_PT_project_damage_plaster,
    OBJECT_PT_project_damage_wood,
    OBJECT_PT_project_damage_lath,
    OBJECT_PT_project_damage_wallpaper,
    OBJECT_PT_project_damage_bullet,
    OBJECT_PT_project_damage_blast,
    OBJECT_PT_project_damage_ground,
    OBJECT_PT_project_damage_brick,
    OBJECT_PT_project_damage_glass,
    OBJECT_PT_project_damage_fissure,
    OBJECT_PT_project_damage_rusty,
    OBJECT_PT_project_damage_bevel,
    OBJECT_PT_project_damage_scratch,
    OBJECT_PT_project_damage_metal,
    OBJECT_PT_project_damage_debris,
    OBJECT_PT_project_damage_rbf,
    OBJECT_PT_project_damage_cellf,
)





def draw_ivy_settings(layout, context):
    from .ivy import target_for
    from . import material_controls
    layout.use_property_split = False
    layout.use_property_decorate = False
    s = target_for(context).project_damage_ivy
    simple = not s.advanced_controls
    box = layout.box()
    material_controls.centered_heading(box, 'Seeds & Guides', icon='EMPTY_AXIS')
    column = box.column(align=True)
    row=column.row(align=True)
    row.prop(s,'seeds',text='Seeds')
    row.operator('project_damage.add_growth_seed',text='',icon='ADD').kind='ivy'
    column.prop(s, 'guide')
    row = box.row(align=True);row.scale_y = 1.5
    row.operator('project_damage.ivy_draw', text='New Drawing', icon='ADD').action='NEW'
    edit = row.row(align=True);edit.enabled=s.guide is not None
    edit.operator('project_damage.ivy_draw', text='Draw / Edit', icon='GREASEPENCIL').action='EDIT'
    edit.operator('project_damage.ivy_draw', text='', icon='TRASH').action='DELETE'
    if simple and s.guide is not None:
        column = box.column(align=True)
        column.prop(s, 'follow_guides')
        sub = column.column(align=True);sub.enabled = s.follow_guides
        sub.prop(s, 'guide_strength')
    hanging_box = box.box()
    material_controls.centered_heading(hanging_box, 'Hanging Ivy Draw', icon='CURVE_BEZCURVE')
    column = hanging_box.column(align=True)
    ui_image_hints.prop(column,s, 'curve')
    row = hanging_box.row(align=True);row.scale_y = 1.5
    row.operator('project_damage.ivy_draw', text='New Curve', icon='ADD').action='CURVE_NEW'
    edit = row.row(align=True);edit.enabled=s.curve is not None
    edit.operator('project_damage.ivy_draw', text='Draw / Edit', icon='CURVE_BEZCURVE').action='CURVE_EDIT'
    edit.operator('project_damage.ivy_draw', text='', icon='TRASH').action='CURVE_DELETE'
    if s.curve is not None:
        column = hanging_box.column(align=True)
        column.prop(s, 'hang_density')
        column.prop(s, 'hang_length')
    if context.object is not None and context.object.type in {'GREASEPENCIL','GPENCIL','CURVE'}:
        box.operator('project_damage.ivy_draw', text='Finish Drawing', icon='CHECKMARK').action='FINISH'
    column = box.column(align=True)
    if simple:
        box = layout.box()
        material_controls.centered_heading(box, 'Ivy', icon='OUTLINER_OB_CURVES')
        column = box.column(align=True)
        material_controls.inline(box, s, 'branch_material', text='Branches')
        column.prop(s, 'master_scale')
        quality_ui.draw(column, s, 'resolution',text='Resolution')
        column.prop(s, 'seed',text='Random Seed')
        column.separator(factor=.5)
        column.prop(s, 'amount')
        column.prop(s, 'branching_amount')
        column.prop(s, 'randomness')
        column.prop(s, 'climb')
        column.prop(s, 'droop')
        if material_controls.experimental(context):
            from .ivy import draw_shader_settings
            draw_shader_settings(box, s.branch_material)
        column.prop(s, 'branch_thickness')
        box = layout.box()
        material_controls.centered_heading(box, 'Leaves', icon='MESH_PLANE')
        column = box.column(align=True)
        material_controls.inline(box, s, 'leaf_material', text='Leaves')
        column.prop(s, 'density')
        if material_controls.experimental(context):
            from .ivy import draw_shader_settings
            draw_shader_settings(box, s.leaf_material)
        column.prop(s, 'simple_leaf_size')
        column.prop(s, 'leaf_tilt')
        column.prop(s, 'stem_length')
        column.prop(s, 'leaf_uv_direction')
        layout.prop(s, 'advanced_controls', icon='PREFERENCES')
        return
    layout.prop(s, 'advanced_controls', icon='PREFERENCES')
    box=layout.box();material_controls.centered_heading(box,'General',icon='SETTINGS')
    column=box.column(align=True)
    column.prop(s,'master_scale')
    quality_ui.draw(column, s,'resolution',text='Resolution')
    column.prop(s,'seed',text='Random Seed')
    column.separator(factor=.5)
    box = layout.box()
    material_controls.centered_heading(box, 'Growth', icon='OUTLINER_OB_CURVES')
    column = box.column(align=True)
    column.prop(s, 'size')
    column.prop(s, 'primary_weight')
    column.prop(s, 'random_weight')
    column.prop(s, 'gravity_weight')
    ui_image_hints.prop(column,s, 'adhesion_weight')
    if s.guide is not None:
        column.prop(s, 'guide_weight')
        column.prop(s, 'guide_radius')
    column.prop(s, 'branching_probability')
    column.prop(s, 'max_float_length')
    column.prop(s, 'max_adhesion_distance')
    column.prop(s, 'max_nodes')
    box = layout.box()
    material_controls.centered_heading(box, 'Branches', icon='CURVE_DATA')
    column = box.column(align=True)
    material_controls.inline(box, s, 'branch_material', text='Branches')
    if material_controls.experimental(context):
        from .ivy import draw_shader_settings
        draw_shader_settings(box, s.branch_material)
    column.prop(s, 'branch_size')
    column.prop(s, 'branch_sides')
    column.prop(s, 'uv_scale')
    box = layout.box()
    material_controls.centered_heading(box, 'Leaves', icon='MESH_PLANE')
    column = box.column(align=True)
    material_controls.inline(box, s, 'leaf_material', text='Leaves')
    if material_controls.experimental(context):
        from .ivy import draw_shader_settings
        draw_shader_settings(box, s.leaf_material)
    column.prop(s, 'leaf_size')
    if s.leaf_size > 0:
        column.prop(s, 'leaf_density')
        if s.leaf_density > 0:
            column.prop(s, 'leaf_density_multiplier')
            column.prop(s, 'leaf_tilt')
            column.prop(s, 'leaf_offset')
            column.prop(s, 'stem_length')
            if s.stem_length > 0:
                column.prop(s, 'stem_thickness')
            column.prop(s, 'stem_attach')
            column.prop(s, 'leaf_uv_direction')
            column.prop(s, 'leaf_maturity')
            column.prop(s, 'adhesion_smoothing')


# раздел «Fracture»
class OBJECT_PT_project_damage_fracture(_ProjectDamagePanel):
    bl_parent_id = "OBJECT_PT_project_damage"
    bl_label = "Fracture"

    def draw_header(self, context):
        self.layout.prop(
            ownership.context_source(context).project_damage,
            "fract_enable",
            text=""
        )

    def draw(self, context):
        p = ownership.context_source(context).project_damage
        col = _tab(self.layout, p, "fract_enable")
        locked=getattr(self.layout,'key',None) in TYPE_BY_ASSET or getattr(self.layout,'key',None)=='fracture'
        typ=effective_type(p,getattr(self.layout,'key',None))
        mode_box=col.box();mode_box.use_property_split=False
        mode_row=mode_box.row(align=True);mode_row.alignment='CENTER'
        mode_row.scale_y=1.5
                                                                         
        for value, label in (('PROCEDURAL','Procedural'),('PAINT','Paint')):
            mode_row.prop_enum(p,'fract_mode',value,text=label)
        ui_image_hints.button(mode_row,'fracture_placement')
        if p.fract_mode!='PAINT':
            row=mode_box.row(align=True);row.scale_y=1.5
            row.prop_enum(p,'fract_surface','CLASSIC',text='Classic')
            row.prop_enum(p,'fract_surface','ROUNDED',text='Rounded')
            ui_image_hints.button(row,'fracture_surface')
        show_style=(p.fract_mode=='PROCEDURAL' and typ in {'RADIAL','GLASS','WOOD'}) or (typ=='WOOD' and p.fract_mode=='PROXIMITY')
        type_box=col.box() if not locked or show_style else None
        procedural=type_box.column() if type_box is not None else col.column()
        procedural.active=p.fract_mode!='PAINT'
        if not locked and p.fract_mode!='PROXIMITY':
            types=procedural.row(align=True);types.use_property_split=False
            for item in p.bl_rna.properties['fract_type'].enum_items:
                types.prop_enum(p,'fract_type',item.identifier,text=item.name)
        if typ=='RADIAL' and p.fract_mode=='PROCEDURAL':
            procedural.prop(p,'fract_radial_spokes')
            procedural.label(text='Layer Amount controls the number of stars.',icon='INFO')
        elif typ=='GLASS' and p.fract_mode=='PROCEDURAL':
            for field in ('fract_glass_spokes','fract_glass_rings','fract_glass_ring_fill'):
                procedural.prop(p,field)
            procedural.label(text='Layer Amount controls stars; rings use the next layer.',icon='INFO')
        elif typ=='WOOD' and p.fract_mode!='PAINT':
            grain=procedural.column(align=True)
            for field in ('fract_grain_x','fract_grain_y','fract_grain_z','fract_grain_pull'):grain.prop(p,field)
        if p.fract_mode=='PAINT' and not locked:type_box.label(text='Paint uses drawn paths; Type is procedural only.',icon='INFO')
                                                                          
        box = mode_box
        painted = p.fract_mode == 'PAINT'

        if painted:
            from . import paint
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
        material_controls.inline(box, p, 'fracture_l1_material', text='Fractures — All Layers')
        head.prop(p, "fract_paint_scale" if painted else "fract_scale")
        quality_ui.draw(head, p, "fract_detail")
        if p.fract_mode != 'PAINT':
            head.label(text = 'Broken Tiles Offset', icon = 'EMPTY_SINGLE_ARROW')
            head.prop(p, 'fract_tile_offset')
            head.prop(p, 'fract_tile_seed')
            head.prop(p, 'fract_depth_mult')
            head.prop(p, 'fract_rim_soft')
        look = box.column(align=True)
        look.active = _fract_any_amount(p)
        look.label(text='Crack Edges', icon='MOD_EDGESPLIT')
        look.prop(p, 'fract_rim_jag')
        look.prop(p, 'fract_flare')

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
                    box.prop(p, 'fract_l%d_paint_amount' % level)
            else:
                box.prop(p, "fract_l%d_amount" % level)

                                                     
                                               
            amt_on = (
                (level == 1 or getattr(p, 'fract_l%d_paint_amount' % level) > 0)
                if painted
                else getattr(p, "fract_l%d_amount" % level) > 0
            )
            if level==1 and p.fract_helper_drawing is not None and (p.fract_physics or (p.fract_through_enable and p.fract_through)):
                amt_on=True                                                      

            sub = box.column(align=True)
            sub.active = amt_on

            if not painted:
                sub.prop(p, "fract_l%d_subdiv" % level)

            sub.prop(p, "fract_l%d_width" % level)
            sub.prop(p, "fract_l%d_depth" % level)
            if painted and level>1 and amt_on:
                shape=sub.box()
                row=shape.row();row.alignment='CENTER'
                row.label(text='Path Shape',icon='MOD_DISPLACE')
                shape.prop(p,'fract_l%d_paint_length_min'%level)
                shape.prop(p,'fract_l%d_paint_length'%level)
                shape.prop(p,'fract_l%d_paint_distortion'%level,text='Distortion')
                if getattr(p,'fract_l%d_paint_distortion'%level)>0:
                    shape.prop(p,'fract_l%d_paint_distortion_scale'%level,text='Distortion Scale (cm)')
            if not painted or level>1:
                sub.prop(p,'fract_l%d_branches'%level)
                if getattr(p,'fract_l%d_branches'%level)>0:
                    sub.column(align=True).prop(p,'fract_l%d_branch_length'%level)
            if not painted:
                sub.prop(p, 'fract_l%d_length' % level)

            if painted:
                continue                                              

            sub.prop(p, "fract_l%d_bias" % level)
            sub.prop(p, "fract_l%d_chips" % level)

                                                                
            chipfields = sub.column(align=True)
            chipfields.active = getattr(p, "fract_l%d_chips" % level)

            material_controls.inline(chipfields, p, 'edge_damage_material', text='Rim Chips')
            chipfields.prop(p, "fract_l%d_chip_amount" % level)
            chipfields.prop(p, "fract_l%d_chip_size" % level)
            chipfields.prop(p, "fract_l%d_chip_depth" % level)

            col.separator()

class OBJECT_PT_project_damage_fracture_realism(_ProjectDamagePanel):
    bl_parent_id = "OBJECT_PT_project_damage_fracture"
    bl_label = "Realism"

    def draw(self, context):
        p = ownership.context_source(context).project_damage
        layout = self.layout
        column = layout.column()
        column.active = (p.fract_enable or p.fract_through_enable) and not p.all_off
        column.prop(p, "fract_recipe")
        cap = column.column()
        cap.active = p.fract_recipe == 'IMPROVED' and not ((p.fract_through_enable and p.fract_through) or (not p.fract_through_enable and p.fract_physics))
        cap.prop(p, "fract_thickness_cap")


def draw_fracture_proximity_asset(layout,context,key):
    p=ownership.context_source(context).project_damage
    box=effect_layout.section(layout,'Basic Settings');col=box.column(align=True)
    material_controls.inline(box, p, 'fracture_l1_material', text='Fractures — All Layers')
    material_controls.inline(box, p, 'edge_damage_material', text='Rim Chips')
    col.prop(p,'fract_scale',text='Scale')
    quality_ui.draw(col, p,'fract_detail',text='Resolution')
    if key=='fracture_proximity_default':
        col.prop(p,'fract_proximity_amount',text='Amount')
        col.prop(p,'fract_l1_width',text='Width')
        col.prop(p,'fract_l1_depth',text='Depth')
        col.prop(p,'fract_l1_length',text='Length')
        col.prop(p,'fract_l1_branches',text='Branches per Line')
        if p.fract_l1_branches>0:col.prop(p,'fract_l1_branch_length',text='Branch Length')
    box=effect_layout.section(layout,'Proximity')
    OBJECT_PT_project_damage_fracture_proximity.draw(SimpleNamespace(layout=box),context)


class OBJECT_PT_project_damage_fracture_proximity(_ProjectDamagePanel):
    bl_parent_id = "OBJECT_PT_project_damage_fracture"
    bl_label = "Proximity"

    def draw(self, context):
        p = ownership.context_source(context).project_damage
        layout = self.layout
        column = layout.column()
        column.active = (p.fract_enable or p.fract_through_enable) and not p.all_off
        column.active = column.active and p.fract_mode != 'PAINT'
        from .fracture_assets import PROXIMITY_ASSETS,active_key
        locked=active_key(p) in PROXIMITY_ASSETS
        effective_mode=proximity_mode(p) if locked else p.fract_prox_mode
        if not locked:ui_image_hints.prop(column,p, "fract_prox_mode")
        if effective_mode == 'OFF' or p.fract_mode == 'PAINT':
            return
        points = column.column()
        points.active = effective_mode != 'OFF'
        ui_image_hints.prop(points,p, "fract_prox_collection")
        points.operator('project_damage.fracture_point_add',text='Add Proximity Point',icon='ADD')
        points.prop(p, "fract_prox_use_size")
        radius = points.column()
        radius.active = not p.fract_prox_use_size or p.fract_prox_collection is None
        radius.prop(p, "fract_prox_radius")
        column.separator()

        if effective_mode=='DEFAULT' and not locked:
            default = column.column()
            default.active = effective_mode == 'DEFAULT'
            default.prop(p, "fract_prox_strength")
        column.separator()

        if effective_mode=='RADIAL':
            radial = column.column()
            radial.active = effective_mode == 'RADIAL'
            radial.label(text="Radial")
            radial.prop(p,'fract_l1_width',text='Line Width (cm)')
            radial.prop(p,'fract_l1_depth',text='Line Depth (cm)')
            radial.prop(p, "fract_radial_spokes")
            radial.prop(p, "fract_radial_rings")
            radial.prop(p, "fract_radial_ring_fill")
            radial.prop(p, "fract_radial_crush")
        column.separator()

        if effective_mode=='GLASS':
            glass = column.column()
            glass.active = effective_mode == 'GLASS'
            glass.label(text="Glass")
            glass.prop(p,'fract_glass_width',text='Line Width (cm)')
            glass.prop(p,'fract_glass_depth',text='Line Depth (cm)')
            glass.prop(p, "fract_glass_spokes")
            glass.prop(p, "fract_glass_branch")
            glass.prop(p, "fract_glass_rings")
            glass.prop(p, "fract_glass_ring_fill")
            glass.prop(p, "fract_glass_crush")


class OBJECT_PT_project_damage_fracture_noise(_ProjectDamagePanel):
                                                                                     
                                                                     
    bl_parent_id = "OBJECT_PT_project_damage_fracture"
    bl_label = " "

    def draw(self, context):
        p = ownership.context_source(context).project_damage
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False
        col = layout.column()
        col.active = (p.fract_enable or p.fract_through_enable) and not p.all_off and _fract_any_amount(p) and p.fract_mode != 'PAINT'
        col.prop(p, "fract_disp_strength")
        col.prop(p, "fract_disp_scale")


class PROJECT_DAMAGE_OT_fracture_point_add(bpy.types.Operator):
    bl_idname='project_damage.fracture_point_add'
    bl_label='Add Proximity Point'
    bl_description='Create a sphere Empty at the 3D cursor in the fracture points collection; create the collection if needed'
    bl_options={'REGISTER','UNDO'}

    @classmethod
    def poll(cls,context):
        source=ownership.context_source(context)
        return context.mode=='OBJECT' and source is not None and source.type=='MESH'

    def execute(self,context):
        source=ownership.context_source(context)
        p=source.project_damage
        collection=p.fract_prox_collection
        if collection is not None and collection.library is not None:
            self.report({'ERROR'},'Choose a local editable points collection')
            return {'CANCELLED'}
        if collection is None:
            collection=bpy.data.collections.new(source.name+' Fracture Points')
            context.scene.collection.children.link(collection)
            p.fract_prox_collection=collection
        elif collection != context.scene.collection and collection not in list(_scene_collections(context.scene.collection)):
            context.scene.collection.children.link(collection)
        point=bpy.data.objects.new(source.name+' Fracture Point',None)
        point.empty_display_type='SPHERE'
        from .params import unit_factor
        point.empty_display_size=max(.001,p.fract_prox_radius/unit_factor(context))
        point.location=context.scene.cursor.location
        collection.objects.link(point)
                                                                              
        live._mark_dirty(source)
        return {'FINISHED'}


def _scene_collections(root):
    for child in root.children:
        yield child
        yield from _scene_collections(child)


class PROJECT_DAMAGE_UL_guides(bpy.types.UIList):
    """Rows of ProjectDamageProps.guides. No enable tick per row - the C4D list has the
    Object Manager tick for that, and Blender's own object visibility is not
    the same thing; remove the row instead."""
    def draw_item(self, context, layout, data, item, icon, active_data, active_prop):
        layout.prop(item, "obj", text="")


class PROJECT_DAMAGE_OT_guide_add(bpy.types.Operator):
    bl_idname = "project_damage.guide_add"
    bl_label = "Add Guide Spline"
    bl_description = "Add an empty row to link a curve as a crack guide"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return ownership.context_source(context) is not None and ownership.context_source(context).type == 'MESH'

    def execute(self, context):
        p = ownership.context_source(context).project_damage
        p.guides.add()
        p.guide_index = len(p.guides) - 1
        return {'FINISHED'}


class PROJECT_DAMAGE_OT_guide_remove(bpy.types.Operator):
    bl_idname = "project_damage.guide_remove"
    bl_label = "Remove Guide Spline"
    bl_description = "Remove the selected guide row"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return (ownership.context_source(context) is not None and ownership.context_source(context).type == 'MESH'
                and len(ownership.context_source(context).project_damage.guides) > 0)

    def execute(self, context):
        p = ownership.context_source(context).project_damage
        idx = min(p.guide_index, len(p.guides) - 1)
        p.guides.remove(idx)
        p.guide_index = max(0, idx - 1)
        live.touch_prop(p, context)
        return {'FINISHED'}


                                                                      
                                                                                     
                                                       
                   

                             
                                                             
                             
                                         
                                             
                              
                                                                                  
                                                                       
                                                                             
                                                       
                            
                                                                               
                                         
                                                                        
                                                                        
                                      
                                                                              
                                                                           
                                                         
                                                                          
                                                                                   


class OBJECT_PT_project_damage_corners(_ProjectDamagePanel):
    bl_parent_id = "OBJECT_PT_project_damage"
    bl_label = "Corner Damage"

    def draw_header(self, context):
        self.layout.prop(ownership.context_source(context).project_damage, "corner_enable", text="")

    def draw(self, context):
        p = ownership.context_source(context).project_damage
        col = _tab(self.layout, p, "corner_enable")

                                                               
        on = p.corner_mode == 'PAINT' or p.corner_prob > 0.0

                                                                  
        colr = col.row()
        colr.scale_y = 2
        colr.row(align=True).prop(p, 'corner_mode', text=' ', expand=True)
        if p.corner_mode == 'PAINT':
            box = col.box()
            from .paint import drawing_row
            drawing_row(box, p, 'CORNER')
            box.prop(p, 'corner_paint_radius')
            box.label(text='Draw over the corners to break. Empty drawing: none break.', icon='INFO')

               
        box = col.box()
        row = box.row()
        row.alignment = 'CENTER'
        row.label(text="Corner Style")
        row = box.row(align=True)
        row.prop_enum(p, 'corner_style', '0', text='Round Crater')
        row.prop_enum(p, 'corner_style', '1', text='Broken Corner')
        look = box.column(align=True)
        look.active = on and p.corner_style == '1'
        look.prop(p, "corner_rough")
        look.prop(p, "corner_tilt")
        look.prop(p, "corner_flakes")

                        
        box = col.box()

        row = box.row()
        row.alignment = 'CENTER'
        row.label(text="Basic Settings")

        if p.corner_mode != 'PAINT':
            box.prop(p,"corner_prob",text="Corners Broken")
        head = box.column(align=True)
        head.active = on
        head.prop(p, "corner_scale")
        quality_ui.draw(head, p, "corner_detail")


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
        material_controls.inline(box, p, 'surface_damage_material', text='Corner Cavities')
        size.prop(p, "corner_size_min")
        size.prop(p, "corner_size_max")

        depth = box.column(align=True)
        depth.active = on
        depth.prop(p, "corner_depth_min")
        depth.prop(p, "corner_depth_max")

                                           
        box.prop(p, "corner_rim_soft")

        col.separator()


                   


def _has_linked_breaker(props):
    """Mirrors GetDEnabling's InExcludeData::GetObjectCount() == 0 check: a
    row with no object assigned (the user cleared it, or a fresh empty row)
    does not count as a linked breaker."""
    return any(row.obj is not None for row in props.breakers)


def _bricks_disclosure(layout, wm, prop, title):
    """UI-only boxes, initially closed; never change generator settings."""
    box = layout.box()
    row = box.row(align=True)
    row.alignment = 'CENTER'
    row.prop(wm, prop, text=title, emboss=False,
             icon='TRIA_DOWN' if getattr(wm, prop) else 'TRIA_RIGHT')
    from . import editorial
    editorial.button(row,title)
    return box.column(align=True) if getattr(wm, prop) else None


def draw_boolean_bricks(layout, context):
    """Compatibility entry point; the shared Boolean interface is below."""
    draw_boolean_controls(layout, context)


def draw_linked_break_surface(layout, p, context, *, boolean_options=True):
    from . import break_surface, displacement_shader, effect_layout
    wm = context.window_manager
    surface = effect_layout.section(layout, 'Break Surface')
    material_controls.inline(surface, p, 'boolean_break_material', text='Cut Faces' if boolean_options else 'Hole Walls')
    surface.prop(p, 'break_share_surface', toggle=True, icon='LINKED')
    br = break_surface.active_cutter(p)
    if br is None:
        surface.label(text='Add or select a Breaker to edit its surface.', icon='INFO')
    else:
        row = surface.row(align=True)
        row.context_pointer_set('project_damage_breaker_obj', br)
        row.menu('PROJECT_DAMAGE_MT_bricks_surface_presets', text='Preset', icon='PRESET')
        ui_image_hints.button(row, 'break_surface_styles')
        row.operator('project_damage.break_surface_copy', text='', icon='DUPLICATE')
        bp = br.project_damage_breaker
        col = surface.column(align=True)
        col.prop(bp, 'strength', text='Strength')
        col.prop(bp, 'noise_scale', text='Noise Scale')
        col.prop(bp, 'surf_smooth', text='Smoothness')
        col.prop(p, 'break_smooth_inside', slider=True)
        quality_ui.draw(col, bp, 'surf_detail', text='Detail')
        col.prop(bp, 'surf_roughness', text='Roughness')
        col.prop(bp, 'surf_crease', text='Crease')
        col.prop(bp, 'surf_warp', text='Warp')
        break_surface.draw_wood_fields(col, bp)
        noise = _bricks_disclosure(surface, wm, 'project_damage_bricks_noise_open', 'Noise Layers')
        if noise is not None:
            noise.prop(bp, 'macro_h', text='Macro Height')
            noise.prop(bp, 'macro_scale', text='Macro Scale')
            noise.prop(bp, 'turb_h', text='Turbulence Height')
            noise.prop(bp, 'turb_scale', text='Turbulence Scale')
            noise.prop(bp, 'fine_h', text='Fine Height')
            noise.prop(bp, 'fine_scale', text='Fine Scale')
                                                                          
        if break_surface._breaker_mode(br) in (0, 2):
            edge = _bricks_disclosure(surface, wm, 'project_damage_bricks_edge_open', 'Edge Breakup')
            if edge is not None:
                edge.prop(bp, 'rim_amp', text='Amount')
                edge.prop(bp, 'rim_scale', text='Scale')
    if not boolean_options:
        return
    rim = effect_layout.section(surface, 'Rim Damage')
    rim.prop(p, 'break_rim_enable', text='Rim Damage', toggle=True, icon='MOD_EXPLODE')
    if p.break_rim_enable:
        col = rim.column(align=True)
        material_controls.inline(col, p, 'edge_damage_material', text='Rim Damage')
        col.prop(p, 'break_surf_dmg', text='Surface Damage')
        col.prop(p, 'break_rim_amount', text='Amount')
        if p.break_rim_amount > 0:
            col.prop(p, 'break_rim_size', text='Size')
            col.prop(p, 'break_rim_depth', text='Depth')
    displacement_shader.draw(surface, p)


def draw_boolean_controls(layout, context):
    """Shared cutter/surface controls, then the selected asset's own settings."""
    from . import boolean_auto, boolean_break, break_surface, effect_layout
    source = ownership.context_source(context)
    p = source.project_damage
    wm = context.window_manager
    layout.use_property_split = False
    layout.use_property_decorate = False

    cutters = effect_layout.section(layout, 'Cutters')
    boolean_break.draw_breaker_list(cutters, p, context)
    if boolean_break.variant(p) in boolean_auto.SUPPORTED and boolean_auto.helper_of(source) is not None:
        cutters.operator('project_damage.boolean_auto_select', icon='RESTRICT_SELECT_OFF')

    general=effect_layout.section(layout,'General')
    col=general.column(align=True)
    col.prop(p,'break_scale',text='Scale')
    col.prop(p,'boolean_quality',text='Resolution')
    draw_linked_break_surface(layout, p, context)

    variant=boolean_break.variant(p)
    if variant=='SIMPLE':return
    if not generated_geometry.draw(layout, p, 'boolean'):
        if variant=='REBAR':draw_hand_drawn_rebars(layout,p)
        return
    if variant=='CENTRAL':
        draw_boolean_central_rebar(layout,p)
        return
    if variant=='REBAR':
        draw_boolean_rebars(layout,p)
        return
    if variant in {'PLANKS','LATHS'}:
        draw_boolean_planks(layout,p)
        return
    if variant=='FRAMING':
        draw_boolean_framing(layout,p)
        return
    if variant=='CHIPS':
        draw_boolean_chips(layout,p)
        return
    if variant in {'PIPES','CABLES'}:
        draw_boolean_tubes(layout,p,variant)
        return
    bricks = effect_layout.section(layout, 'Brick Layer')
    col = bricks.column(align=True)
    material_controls.inline(bricks, p, 'rebar_brick_material', text='Bricks')
    material_controls.inline(bricks, p, 'rebar_mortar_material', text='Mortar')
    col.prop(p, 'rebar_scale', text='Scale')
    if not p.boolean_stable_volume:ui_image_hints.prop(col,p, 'rebar_cover', text='Deepness')
    if not draw_boolean_volume_placement(bricks,p):
        placement = effect_layout.section(bricks, 'Placement Logic')
        col = placement.column(align=True)
        row = col.row(align=True); row.scale_y = 1.5
        ui_image_hints.prop(row,p, 'rebar_mode', text='Placement', expand=True)
        row = col.row(align=True); row.scale_y = 1.5
        ui_image_hints.prop(row,p, 'rebar_fit', text='Fit', expand=True)
        row = col.row(align=True); row.scale_y = 1.5
        ui_image_hints.prop(row,p, 'rebar_layout', text='Layout', expand=True)
        break_surface.draw_rebar_axes(col, p)
        row = col.row(align=True); row.scale_y = 1.5
        ui_image_hints.prop(row,p, 'rebar_side', text='Face', expand=True)

    col = bricks.column(align=True)
    col.prop(p, 'rebar_seed', text='Seed')

    if not p.boolean_stable_volume and p.rebar_mode == '1':
        col.prop(p, 'rebar_deform', text='Deform *')



    bricks.separator(type='LINE')
    col = bricks.column(align=True)
    col.prop(p, 'rebar_brick_length', text='Brick Length')
    col.prop(p, 'rebar_brick_height', text='Brick Height')
    col.prop(p, 'rebar_brick_depth', text='Brick Depth')
    col.prop(p, 'rebar_mortar', text='Mortar')
    col.prop(p, 'rebar_missing', text='Missing')
    col.prop(p, 'rebar_brick_broken', text='Broken')
    col.prop(p, 'rebar_chamfer', text='Chamfer')
    if not p.boolean_stable_volume:col.prop(p, 'rebar_brick_jitter', text='Jitter')
    if p.boolean_stable_volume:col.prop(p,'rebar_angle',text='Grid Rotation')

    if not p.boolean_stable_volume:
        models = bricks.box()
        row = models.row(align=True); row.scale_y = 1.5
        ui_image_hints.prop(row,p, 'rebar_brick_3d', text='3D Bricks (Experimental)', toggle=True, icon='MESH_CUBE')
        if p.rebar_brick_3d:
            models.column(align=True).prop(p, 'rebar_brick_collection', text='Brick Collection')
        else:
            joints = bricks.box()
            row = joints.row(align=True); row.scale_y = 1.5
            row.prop(p, 'rebar_mortar_joints', text='Mortar Joints', toggle=True, icon='MOD_BUILD')
            if p.rebar_mortar_joints:
                joints.column(align=True).prop(p, 'rebar_mortar_recess', text='Mortar Recess')
    if p.boolean_stable_volume:
        joints=bricks.box();row=joints.row(align=True);row.scale_y=1.5
        row.prop(p,'rebar_mortar_joints',text='Mortar Joints',toggle=True,icon='MOD_BUILD')
        if p.rebar_mortar_joints:joints.prop(p,'rebar_mortar_recess',text='Mortar Recess')
    bricks.column(align=True).prop(p, 'rebar_drop_loose', text='Drop Loose')
                                                                    
                                                                               
                                                  


def _boolean_layout_choices(layout,p, bars=False):
    from . import effect_layout,break_surface
    box=effect_layout.section(layout,'Placement Logic')
    col=box.column(align=True)
    row=col.row(align=True);row.scale_y=1.5;ui_image_hints.prop(row,p,'rebar_mode',text='Placement',expand=True)
    row=col.row(align=True);row.scale_y=1.5;ui_image_hints.prop(row,p,'rebar_fit',text='Fit',expand=True)
    row=col.row(align=True);row.scale_y=1.5;ui_image_hints.prop(row,p,'rebar_layout',text='Layout',expand=True)
    break_surface.draw_rebar_axes(col,p)
    if p.rebar_layout=='0':
        row=col.row(align=True);row.scale_y=1.5;ui_image_hints.prop(row,p,'rebar_side',text='Face',expand=True)
    if p.rebar_layout in {'0','1'}:
        if bars:
            col.prop(p,'rebar_angle',text='Grid Rotation')
            row=col.row(align=True);row.scale_y=1.5
            row.prop_enum(p,'rebar_outer','0',text='Horizontal Bars in Front')
            row.prop_enum(p,'rebar_outer','1',text='Vertical Bars in Front')
            row=col.row(align=True);row.scale_y=1.5
            row.prop(p,'rebar_bars_u',text='Horizontal Bars',toggle=True)
            row.prop(p,'rebar_bars_v',text='Vertical Bars',toggle=True)
    return box


def draw_boolean_central_rebar(layout,p):
    from . import effect_layout
    box=effect_layout.section(layout,'Central Rebar')
    material_controls.inline(box,p,'rebar_material',text='Steel')
    box.separator()
    row=box.row(align=True);row.scale_y=1.5
    row.prop(p,'boolean_central_axis',text='Direction',expand=True)
    col=box.column(align=True)
    col.prop(p,'boolean_central_diameter',text='Diameter (cm)')
    col.prop(p,'boolean_central_embed',text='Anchoring Length (cm)')
    for profiles in ((('0','Ribbed'),('1','Helical Ribs'),('2','Smooth')),(('3','Twisted Square'),('4','Strand'))):
        row=box.row(align=True);row.scale_y=1.3
        for key,label in profiles:row.prop_enum(p,'rebar_type',key,text=label)
    col=box.column(align=True)
    if p.rebar_type in {'0','1'}:
        col.prop(p,'rebar_rib_height',text='Rib Height')
        col.prop(p,'rebar_rib_pitch',text='Rib Spacing')
    if p.rebar_type in {'3','4'}:
        col.prop(p,'rebar_twist_pitch',text='Twist Spacing')
    quality_ui.draw(col, p,'rebar_resolution',slider=True)
    col.prop(p,'rebar_sides',text='Cross-section Sides')
    box.label(text='Follows the centres of structural members.',icon='INFO')
    box.label(text='Damage must reach these internal paths.')


def draw_boolean_rebars(layout,p):
    from . import effect_layout
    _boolean_layout_choices(layout,p,bars=True)
    box=effect_layout.section(layout,'Bars')
    col=box.column(align=True)
    material_controls.inline(box, p, 'rebar_material', text='Rebar')
    col.prop(p,'rebar_amount',text='Amount')
    col.prop(p,'rebar_scale',text='Scale')
    ui_image_hints.prop(col,p,'rebar_cover',text='Deepness')
    col.prop(p,'rebar_seed',text='Seed')

    if p.rebar_mode=='1':col.prop(p,'rebar_deform',text='Deform')
    row=box.row(align=True);row.scale_y=1.5;row.prop(p,'rebar_type',text='Bar Profile',expand=True)
    col=box.column(align=True)
    col.prop(p,'rebar_radius',text='Bar Radius')
    if p.rebar_layout in {'2','3'}:
        col.prop(p,'rebar_spacing_u',text='Longitudinal Bar Spacing')
        col.prop(p,'rebar_tie_spacing',text='Stirrup / Hoop Spacing')
        col.prop(p,'rebar_tie_radius',text='Stirrup / Hoop Radius')
        if p.rebar_layout=='3':col.prop(p,'rebar_spiral',text='Spiral Hoops',toggle=True)
    else:
        if p.rebar_bars_u:col.prop(p,'rebar_spacing_u',text='Horizontal Bar Spacing')
        if p.rebar_bars_v:col.prop(p,'rebar_spacing_v',text='Vertical Bar Spacing')
    if p.rebar_type in {'0','1'}:
        col.prop(p,'rebar_rib_height',text='Rib Height')
        col.prop(p,'rebar_rib_pitch',text='Rib Spacing')
    if p.rebar_type in {'3','4'}:col.prop(p,'rebar_twist_pitch',text='Twist Spacing')
    quality_ui.draw(col, p,'rebar_resolution',slider=True)
    col.prop(p,'rebar_sides',text='Cross-section Sides')

    col.prop(p,'rebar_jitter',text='Placement Variation')
    box=effect_layout.section(layout,'Exposure');col=box.column(align=True)
    col.prop(p,'rebar_embed',text='Anchoring Length')
    col.prop(p,'rebar_min_exposed',text='Minimum Visible Length')
    col.prop(p,'rebar_drop_loose',text='Remove Unanchored Bars',toggle=True)
    box=effect_layout.section(layout,'Rebars Damage');col=box.column(align=True)
    col.prop(p,'rebar_wobble',text='Small Bends')
    col.prop(p,'rebar_bow',text='Sag')
    col.prop(p,'rebar_bend',text='Broken-end Bend')
    col.prop(p,'rebar_broken',text='Break Probability')
    col.prop(p,'rebar_trim',text='Broken-end Shortening')
    col.prop(p,'rebar_neck',text='Broken-end Taper')
    draw_hand_drawn_rebars(layout,p)


def draw_hand_drawn_rebars(layout,p):
    from . import boolean_ui_controls, effect_layout
    box=effect_layout.section(layout,'Hand-Drawn Bars — Curve')
    box.prop(p,'rebar_drawing',text='Curve')
    step = boolean_ui_controls.curve_creation_step(p.id_data)
    if step != 'ADD':
        box.label(text='Place the 3D Cursor at the end point' if step == 'END' else 'Place the 3D Cursor at the start point',
                  icon='PIVOT_CURSOR')
    row=box.row(align=True);row.scale_y=1.5
    row.operator_context='EXEC_DEFAULT'
    label, icon = {'ADD': ('Add', 'ADD'), 'START': ('Set Start Point', 'PIVOT_CURSOR'),
                   'END': ('Set End Point', 'CHECKMARK')}[step]
    row.operator('project_damage.rebar_curve',text=label,icon=icon).action=step
    if step != 'ADD':
        row.operator('project_damage.rebar_curve',text='Cancel',icon='X').action='CANCEL'
    elif p.rebar_drawing is not None:
        row.operator('project_damage.rebar_curve',text='Edit Curve',icon='EDITMODE_HLT').action='EDIT'
        row.operator('project_damage.paint_delete',text='',icon='TRASH').target='REBAR'
    if p.rebar_drawing is not None:
        col=box.column(align=True)
        col.prop(p,'rebar_stroke_radius',text='Curve Bar Width')
        material_controls.inline(box, p, 'rebar_drawn_material', text='Hand-Drawn Rebar Material')


def draw_boolean_volume_placement(layout,p):
    from . import effect_layout
    box=effect_layout.section(layout,'Organic Placement — Experimental')
    row=box.row(align=True);row.scale_y=1.5
    row.prop(p,'boolean_stable_volume',text='Stable Volume Fill',toggle=True,icon='MESH_GRID')
    if not p.boolean_stable_volume:return False
    col=box.column(align=True)
    row=col.row(align=True);row.scale_y=1.5
    row.prop(p,'boolean_volume_axis',text='Grid Plane',expand=True)
    col.prop(p,'boolean_volume_clearance',text='Part Clearance (cm)')
    col.prop(p,'boolean_volume_min_length',text='Minimum Piece Length (cm)')
    col.prop(p,'boolean_volume_min_section',text='Minimum Cross Section',slider=True)
    col.prop(p,'boolean_volume_budget',text='Candidate Budget')
    origin=box.box();material_controls.centered_heading(origin,'Grid Origin',icon='PIVOT_CURSOR')
    col=origin.column(align=True)
    col.prop(p,'boolean_volume_origin_x',text='X (cm)')
    col.prop(p,'boolean_volume_origin_y',text='Y (cm)')
    col.prop(p,'boolean_volume_origin_z',text='Z (cm)')
    box.label(text='Uses the prepared source volume and thickness.',icon='INFO')
    box.label(text='One fixed grid; cut from any side.',icon='MESH_CUBE')
    return True


def draw_boolean_planks(layout,p):
    from . import effect_layout
    box=effect_layout.section(layout,'Wood Laths' if p.rebar_fill=='5' else 'Wood Planks');col=box.column(align=True)
    col.prop(p,'rebar_scale',text='Wood Master Scale')
    material_controls.inline(box, p, 'rebar_plank_material', text='Wood Planks')
    col.prop(p,'boolean_plank_amount',text='Amount')
    col.prop(p,'rebar_scale',text='Scale')
    ui_image_hints.prop(col,p,'boolean_wood_deepness',text='Deepness (cm)')
    _boolean_layout_choices(box,p)
    col=box.column(align=True)
    col.prop(p,'rebar_seed',text='Seed')

    if p.rebar_mode=='1':col.prop(p,'rebar_deform',text='Deform')
    row=col.row(align=True);row.scale_y=1.5;row.prop(p,'rebar_plank_dir',text='Orientation',expand=True)
    col.prop(p,'rebar_angle',text='Board Rotation')
    col.prop(p,'rebar_plank_width',text='Board Width')
    col.prop(p,'rebar_plank_thickness',text='Board Thickness')
    col.prop(p,'rebar_plank_gap',text='Gap Between Boards')
    col.prop(p,'rebar_splinter',text='Splinters')
    col.prop(p,'rebar_broken',text='Break Probability')
    col.prop(p,'rebar_bow',text='Bowing')
    col.prop(p,'rebar_trim',text='Broken-end Shortening')
    col.prop(p,'rebar_embed',text='Anchoring Length')

    col.prop(p,'rebar_drop_loose',text='Remove Loose Boards',toggle=True)


def draw_boolean_chips(layout,p):
    from . import chips,effect_layout
    box=effect_layout.section(layout,'Wooden Chips')
    material_controls.inline(box, p, 'rebar_chips_material', text='Splinters')
    chips.draw_settings(box.column(align=True),p)


def draw_boolean_framing(layout,p):
    from . import effect_layout
    box=effect_layout.section(layout,'Frame Settings');col=box.column(align=True)
    material_controls.inline(box, p, 'rebar_framing_material', text='Timber Frame')
    col.prop(p,'boolean_frame_amount',text='Amount')
    col.prop(p,'rebar_scale',text='Scale')
    col.prop(p,'rebar_seed',text='Seed')
    col.prop(p,'rebar_stud_width',text='Stud Width')
    col.prop(p,'rebar_stud_depth',text='Stud Depth')
    col.prop(p,'rebar_stud_spacing',text='Stud Spacing')
    col.prop(p,'rebar_joist_depth',text='Joist Depth')
    col.prop(p,'rebar_noggin_spacing',text='Blocking Spacing')
    col.prop(p,'rebar_double_plate',text='Double Top Plate',toggle=True)
    col.prop(p,'rebar_facet_angle',text='Panel Angle')
    box=effect_layout.section(layout,'Drywall Skin');col=box.column(align=True)
    col.prop(p,'rebar_skin',text='Skin Thickness')
    col.prop(p,'rebar_cavity_reach',text='Cavity Reach')
    col.prop(p,'rebar_paper_tear',text='Paper Tear Width')
    if p.rebar_paper_tear>0:col.prop(p,'rebar_paper_depth',text='Paper Thickness')
    box=effect_layout.section(layout,'Frame Damage');col=box.column(align=True)
    material_controls.inline(box, p, 'rebar_nail_material', text='Nails')
    col.prop(p,'rebar_wood_broken',text='Broken Timber')
    col.prop(p,'rebar_nails',text='Remaining Nails')
    if p.rebar_nails>0:col.prop(p,'rebar_nail_length',text='Nail Length')


def draw_boolean_tubes(layout,p,variant):
    from . import effect_layout,pipes,cables
    pipe=variant=='PIPES'
    box=effect_layout.section(layout,'Pipes' if pipe else 'Cables');col=box.column(align=True)
    material_controls.inline(box, p, 'pipe_material' if pipe else 'cable_material', text='Pipe Material' if pipe else 'Cable Material')
    if pipe:
        col.prop(p,'pipe_count',text='Amount')
        col.prop(p,'boolean_pipe_scale',text='Scale')
        col.prop(p,'pipe_diameter',text='Diameter')
        col.prop(p,'pipe_wall',text='Wall Thickness')
        col.prop(p,'pipe_cover',text='Deepness')
    else:
        col.prop(p,'cable_count',text='Amount')
        col.prop(p,'cable_run_gap')
        col.prop(p,'boolean_cable_scale',text='Scale')
        col.prop(p,'cable_diameter',text='Diameter')
        col.prop(p,'cable_cover',text='Deepness')
    placement=effect_layout.section(box,'Placement Logic');col=placement.column(align=True)
    if pipe:pipes.draw_axes(col,p)
    else:cables.draw_axes(col,p)
                                                                       
                                                                             
    if pipe:
        if p.pipe_side=='MID':col.prop(p,'pipe_side',text='Side',expand=True)
    else:col.prop(p,'cable_side',text='Side',expand=True)
    row=col.row(align=True);row.scale_y=1.5
    row.prop(p,'pipe_layout' if pipe else 'cable_layout',text='Orientation Axis',expand=True)
    col=box.column(align=True)
    if pipe:
        row=col.row(align=True);row.scale_y=1.5;ui_image_hints.prop(row,p,'pipe_type',text='Type',expand=True)
        col.prop(p,'pipe_bundle',text='Pipes per Bundle')
        if p.pipe_bundle>1:col.prop(p,'pipe_bundle_gap',text='Bundle Gap')
        col.prop(p,'pipe_elbows',text='Elbows')
        col.prop(p,'pipe_joint_spacing',text='Joint Spacing')
        col.prop(p,'pipe_seed',text='Seed')
        col.prop(p,'pipe_wires',text='Internal Wires',toggle=True,icon='CURVE_DATA')
        if p.pipe_wires:col.prop(p,'pipe_wire_count',text='Wire Count')
        box=effect_layout.section(layout,'Pipe Damage');col=box.column(align=True)
        col.prop(p,'pipe_broken',text='Broken Pipes');col.prop(p,'pipe_bend',text='Bent Ends')
        col.prop(p,'pipe_jagged',text='Jagged Ends');col.prop(p,'pipe_dent',text='Dents')
        col.prop(p,'pipe_bow',text='Bowing');col.prop(p,'pipe_embed',text='Anchoring Length')
    else:
        row=col.row(align=True);row.scale_y=1.5;ui_image_hints.prop(row,p,'cable_type',text='Type',expand=True)
        col.prop(p,'cable_bundle',text='Cables per Bundle')
        if p.cable_bundle>1:col.prop(p,'cable_bundle_gap',text='Bundle Gap')
        col.prop(p,'cable_turns',text='Turns');col.prop(p,'cable_seed',text='Seed')
        box=effect_layout.section(layout,'Cable Damage');col=box.column(align=True)
        col.prop(p,'cable_cut',text='Cut Cables');ui_image_hints.prop(col,p,'cable_slack',text='Slack')
        col.prop(p,'cable_stiffness',text='Stiffness');col.prop(p,'cable_embed',text='Anchoring Length')
        col.prop(p,'cable_strip',text='Strip Length');col.prop(p,'cable_splay',text='Exposed-core Spread')
        col.prop(p,'cable_fray',text='Frayed Sheath')
        box=effect_layout.section(layout,'Hanging Cables');row=box.row(align=True);row.scale_y=1.5
        ui_image_hints.prop(row,p,'cable_hang',text='Hanging',expand=True)
        if p.cable_hang!='OFF':
            col=box.column(align=True);col.prop(p,'cable_hang_length',text='Hang Length')
            col.prop(p,'cable_hang_ends',text='Loose Ends');col.prop(p,'cable_hang_sway',text='Sway')
            if p.cable_hang=='HORIZONTAL':col.prop(p,'cable_hang_tilt',text='Ceiling Angle')


def draw_break_texture(layout, br):
    from . import effect_layout
    box=effect_layout.section(layout,'Break Surface')
    if br is None:
        box.label(text='Add or select a Breaker to edit its surface.',icon='INFO')
        return
    bp=br.project_damage_breaker
    row=box.row(align=True)
    row.prop(bp,'smooth_slope',toggle=True,icon='MOD_SMOOTH')
    if bp.smooth_slope:
        row.prop(bp,'smooth_width')
    row=box.row(align=True);row.scale_y=1.5
    row.prop(bp,'surf_mode',expand=True)
    from . import break_surface
    if bp.surf_mode!='1':
        break_surface.draw_procedural(box,br,show_copy=True)
        return
    for field in ('tex_path','strength','tex_scale','tex_strength','tex_exit','tex_macro'):
        box.prop(bp,field)
    offset=effect_layout.section(box,'Texture Offset')
    for axis in ('x','y','z'):offset.prop(bp,'tex_offset_'+axis)
    box.label(text='Empty texture: bundled gravel displacement.',icon='INFO')
    if bp.tex_macro>0:
        base=effect_layout.section(box,'Procedural Base (Macro Shape)')
        break_surface.draw_procedural(base,br,show_copy=True,show_strength=False)


class OBJECT_PT_project_damage_breakers(_ProjectDamagePanel):
                                                                                                       
                                                  
                                                                       
                                                                            
    bl_parent_id = "OBJECT_PT_project_damage"
    bl_label = "Boolean Break"
    bl_order = 10000

    def draw(self, context):
        p = ownership.context_source(context).project_damage
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False
        from . import boolean_break
        boolean_break.draw_breaker_list(layout, p, context)
        draw_linked_break_surface(layout, p, context, boolean_options=False)

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


class OBJECT_PT_project_damage_rebar(_ProjectDamagePanel):
    """Child of the Breaker panel (register after OBJECT_PT_project_damage_breakers)."""
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_context = "object"
    bl_parent_id = "OBJECT_PT_project_damage_breakers"
    bl_label = "Rebar"

    def draw_header(self, context):
        self.layout.prop(ownership.context_source(context).project_damage, "rebar_enable", text="")

    def draw(self, context):
        p = ownership.context_source(context).project_damage
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False
        layout.active = p.rebar_enable
        layout.prop(p, "rebar_preset")
        from . import rebar
        if not getattr(self,'variant_ui',False):rebar.draw_fill_type(layout,p)
        if p.rebar_fill!='0':
            rebar.draw_fill(layout,p)
            return
        layout.prop(p,"rebar_material")
        ui_image_hints.prop(layout,p,'rebar_mode')
        ui_image_hints.prop(layout,p,'rebar_fit')
        if p.rebar_mode=='1':layout.prop(p,'rebar_deform')

        layout.prop(p,'rebar_core')
        layout.prop(p,'rebar_show_all')
        from . import paint
        ink=layout.box();ink.label(text='Hand-Drawn Bars',icon='GREASEPENCIL')
        paint.drawing_row(ink,p,'REBAR')
        ink.prop(p,'rebar_stroke_radius');ink.prop(p,'rebar_stroke_embed')
        layout.prop(p, "rebar_type")
        ui_image_hints.prop(layout,p, "rebar_layout")
        layout.prop(p, "rebar_seed")

        col = layout.column(heading="Layout")
        from . import break_surface
        break_surface.draw_rebar_axes(col, p)
        cage = p.rebar_layout in ('2', '3')
        grid = not cage
        sub = col.column()
        sub.active = p.rebar_layout == '0'
        ui_image_hints.prop(sub,p, "rebar_side")
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
        ui_image_hints.prop(col,p, "rebar_cover")
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
        quality_ui.draw(col, p, "rebar_resolution", slider=True)
        col.prop(p, "rebar_sides")


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

class OBJECT_PT_project_damage_breaker(_ProjectDamagePanel):
    """The breaker Empty's OWN parameters - shown when the Empty itself is the
    active object, mirroring the separate ProjectDamage Breaker object's Attribute
    Manager tab in C4D (cpp/source/project_damage_breaker.cpp)."""
    bl_idname = "OBJECT_PT_project_damage_breaker"
    bl_label = "Boolean Break"
    bl_options = set()                                                              

    @classmethod
    def poll(cls, context):
        return False                                                             

    def draw(self, context):
        br = ownership.context_source(context)
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
        from . import break_surface
        break_surface.draw_noise_extra(noise, bp)
        layout.separator()

        layout.label(text="Edge Break-up")
        edge = layout.column()
        edgefields = edge.column()
        edgefields.active = sheet
        edgefields.prop(bp, "rim_amp")
        edgefields.prop(bp, "rim_scale")


                                                                        
                                                                        
                                                                                                 
                                                                          
_CLASSES = (OBJECT_PT_project_damage, OBJECT_PT_project_damage_global, OBJECT_PT_project_damage_chips,
            OBJECT_PT_project_damage_chips_noise, OBJECT_PT_project_damage_surface,
            OBJECT_PT_project_damage_surface_noise, OBJECT_PT_project_damage_buildup,
            OBJECT_PT_project_damage_piles, OBJECT_PT_project_damage_deposits,
            OBJECT_PT_project_damage_coating, OBJECT_PT_project_damage_heat, OBJECT_PT_project_damage_rust,
            OBJECT_PT_project_damage_fracture, OBJECT_PT_project_damage_fracture_realism,
            OBJECT_PT_project_damage_fracture_proximity, OBJECT_PT_project_damage_fracture_noise,
         OBJECT_PT_project_damage_corners)
from . import damaged_bevel as _damaged_bevel
_EFFECT_PANELS += (_damaged_bevel.make_panel(),)
_CLASSES += tuple(_EFFECT_PANELS) + (OBJECT_PT_project_damage_cleanup,
                                   OBJECT_PT_project_damage_breakers,
                                   OBJECT_PT_project_damage_rebar,
                                   OBJECT_PT_project_damage_breaker)


                                                                             
                                                                              
                                                  
from . import impact_deform_tools, recording_tools, scatter_ui
_NON_PANELS = (PROJECT_DAMAGE_OT_wallpaper_finder_add, PROJECT_DAMAGE_OT_toggle_hit_core, PROJECT_DAMAGE_UL_guides, PROJECT_DAMAGE_OT_guide_add, PROJECT_DAMAGE_OT_guide_remove, PROJECT_DAMAGE_OT_fracture_point_add, *avoid_tools.CLASSES, *impact_deform_tools.CLASSES, *recording_tools.CLASSES, *scatter_ui.CLASSES)

                                                                         
from . import material_controls
for _panel, _module in (
        (OBJECT_PT_project_damage_chips, 'edge'),
        (OBJECT_PT_project_damage_surface, 'surface'),
        (OBJECT_PT_project_damage_corners, 'corner'),
        (OBJECT_PT_project_damage_fracture, 'fracture'),
        (OBJECT_PT_project_damage_buildup, 'buildup'),
        (OBJECT_PT_project_damage_piles, 'piles'),
        (OBJECT_PT_project_damage_deposits, 'deposits'),
        (OBJECT_PT_project_damage_coating, 'coating'),
        (OBJECT_PT_project_damage_rust, 'rust'),
        (OBJECT_PT_project_damage_heat, 'heat'),
        (OBJECT_PT_project_damage_breakers, 'boolean')):
    _panel.draw = material_controls.with_materials(_panel.draw, _module)


# подключаем свойства и действия этого модуля к Blender
def register():
    for cls in _NON_PANELS:
        bpy.utils.register_class(cls)
    impact_deform_tools.register_handlers()
    from . import sidebar
    sidebar.register()


# убираем подключения модуля при выключении аддона
def unregister():
    from . import sidebar
    sidebar.unregister()
    impact_deform_tools.unregister_handlers()
    scatter_ui.clear_previews()
    for cls in reversed(_NON_PANELS):
        bpy.utils.unregister_class(cls)


def draw_stud_framing_settings(layout,p):
    box=layout.box()
    material_controls.centered_heading(box,'Stud Frame',icon='SNAP_GRID')
    col=box.column(align=True)
    material_controls.inline(col, p, 'lath_material', text='Timber')
    col.prop(p,'lath_stud_spacing',text='Stud Spacing')
    col.prop(p,'lath_stud_width',text='Stud Width')
    col.prop(p,'lath_stud_depth',text='Stud Depth')
    col.prop(p,'lath_noggin_spacing',text='Noggin Spacing')
    col.prop(p,'lath_frame_plates',text='Top and Bottom Plates',toggle=True)
    if p.lath_frame_plates:
        col.prop(p,'lath_double_plate',text='Double Top Plate',toggle=True)
    extra=_ui_disclosure(box,'framing_extra_open','Extra Settings')
    if extra is not None:
        extra.prop(p,'lath_lath_bend',text='Lath Bend')
        extra.prop(p,'lath_lath_splinter',text='Lath Splinter')
        axes=extra.row(align=True)
        axes.prop(p,'lath_up_x',text='Up X')
        axes.prop(p,'lath_up_y',text='Up Y')
        axes.prop(p,'lath_up_z',text='Up Z')
    col=_ui_disclosure(box,'framing_nails_open','Nails')
    if col is None:return
    col.prop(p,'lath_nail_amount',text='Nails')
    if p.lath_nail_amount>0:
        col.prop(p,'lath_nail_length',text='Nail Length')


def draw_roots_settings(layout, context):
    from .roots import target_for
    layout.use_property_split = False
    layout.use_property_decorate = False
    s = target_for(context).project_damage_roots
    box = layout.box()
    material_controls.centered_heading(box, 'Seeds & Guides', icon='EMPTY_AXIS')
    column = box.column(align=True)
    row=column.row(align=True)
    row.prop(s,'seeds',text='Seeds')
    row.operator('project_damage.add_growth_seed',text='',icon='ADD').kind='roots'
    column.prop(s, 'guide')
    row = box.row(align=True);row.scale_y = 1.5
    row.operator('project_damage.roots_draw', text='New Drawing', icon='ADD').action='NEW'
    edit = row.row(align=True);edit.enabled=s.guide is not None
    edit.operator('project_damage.roots_draw', text='Draw / Edit', icon='GREASEPENCIL').action='EDIT'
    edit.operator('project_damage.roots_draw', text='', icon='TRASH').action='DELETE'
    if context.object is not None and context.object == s.guide:
        box.operator('project_damage.roots_draw', text='Finish Drawing', icon='CHECKMARK').action='FINISH'
    simple = not s.advanced_controls
    if simple and s.guide is not None:
        column = box.column(align=True)
        column.prop(s, 'follow_guides')
        sub = column.column(align=True);sub.enabled = s.follow_guides
        sub.prop(s, 'guide_strength')
    column = box.column(align=True)
    if simple:
        box = layout.box()
        material_controls.centered_heading(box, 'Roots', icon='FORCE_CURVE')
        column = box.column(align=True)
        column.prop(s, 'master_scale')
        quality_ui.draw(column, s, 'resolution',text='Resolution')
        column.prop(s, 'seed',text='Random Seed')
        column.separator(factor=.5)
        column.prop(s, 'amount')
        column.prop(s, 'length_amount')
        column.prop(s, 'branching_amount')
        column.prop(s, 'randomness')
        column.prop(s, 'downhill')
        column.prop(s, 'spread_amount')
        box = layout.box()
        material_controls.centered_heading(box, 'Shape', icon='CURVE_DATA')
        column = box.column(align=True)
        material_controls.inline(box, s, 'root_material', text='Roots')
        column.prop(s, 'thickness')
        column.prop(s, 'embed')
        column.prop(s, 'flare')
        draw_roots_merge(box, s)
        layout.prop(s, 'advanced_controls', icon='PREFERENCES')
        return
    column.prop(s, 'roots_per_seed')
    layout.prop(s, 'advanced_controls', icon='PREFERENCES')
    box=layout.box();material_controls.centered_heading(box,'General',icon='SETTINGS')
    column=box.column(align=True)
    column.prop(s,'master_scale')
    quality_ui.draw(column, s,'resolution',text='Resolution')
    column.prop(s,'seed',text='Random Seed')
    column.separator(factor=.5)
    box = layout.box()
    material_controls.centered_heading(box, 'Growth', icon='FORCE_CURVE')
    column = box.column(align=True)
    column.prop(s, 'root_length')
    column.prop(s, 'length_random')
    column.prop(s, 'size')
    column.prop(s, 'primary_weight')
    column.prop(s, 'random_weight')
    column.prop(s, 'gravity_weight')
    column.prop(s, 'spread_weight')
    if s.guide is not None:
        column.prop(s, 'guide_weight')
        column.prop(s, 'guide_radius')
    column.prop(s, 'max_nodes')
    box = layout.box()
    material_controls.centered_heading(box, 'Branching', icon='OUTLINER_OB_CURVES')
    column = box.column(align=True)
    column.prop(s, 'branching_probability')
    if s.branching_probability > 0:
        column.prop(s, 'max_levels')
        column.prop(s, 'branch_angle')
        column.prop(s, 'child_length')
        column.prop(s, 'child_radius')
    box = layout.box()
    material_controls.centered_heading(box, 'Shape', icon='CURVE_DATA')
    column = box.column(align=True)
    material_controls.inline(box, s, 'root_material', text='Roots')
    column.prop(s, 'root_size')
    column.prop(s, 'taper')
    column.prop(s, 'tip_radius')
    column.prop(s, 'embed')
    column.prop(s, 'flare')
    column.prop(s, 'flatten')
    column.prop(s, 'sides')
    column.prop(s, 'uv_scale')
    draw_roots_merge(box, s)


def draw_roots_merge(box, s):
    column = box.column(align=True)
    column.prop(s, 'merge_by_distance')
    if s.merge_by_distance:
        column.prop(s, 'merge_distance')


def draw_wallhole_settings(layout,context):
    source=ownership.context_source(context)
    p=source.project_damage_wallhole
    if material_controls.ui_section(context)=='MATERIAL':
        material_controls.draw(layout,source.project_damage,'wallhole')
        return
    layout.use_property_split=False
    layout.use_property_decorate=False
    box=layout.box()
    material_controls.centered_heading(box,'Sites',icon='SETTINGS')
    col=box.column(align=True)
    col.prop(p,'sites',text='Sites')
    col.prop(p,'seed',text='Seed')
    box=layout.box()
    material_controls.centered_heading(box,'Opening',icon='SETTINGS')
    col=box.column(align=True)
    material_controls.inline(box, source.project_damage, 'brick_behind_material', text='Opening Walls / Backing')
    col.prop(p,'hole_width',text='Hole Width')
    col.prop(p,'hole_height',text='Hole Height')
    col.prop(p,'hole_lift',text='Hole Lift')
    col.prop(p,'jagged',text='Jagged')
    col.prop(p,'edge_ragged',text='Edge Ragged')
    col.prop(p,'depth_variation',text='Depth Variation')
    box=layout.box()
    material_controls.centered_heading(box,'Masonry',icon='SETTINGS')
    col=box.column(align=True)
    material_controls.inline(box, source.project_damage, 'brick_front_material', text='Bricks')
    material_controls.inline(box, source.project_damage, 'brick_seam_material', text='Mortar Joints')
    material_controls.inline(box, source.project_damage, 'brick_break_material', text='Snapped Brick Faces')
    col.prop(p,'brick_length',text='Brick Length')
    col.prop(p,'brick_height',text='Brick Height')
    col.prop(p,'brick_width',text='Brick Width')
    col.prop(p,'mortar',text='Mortar')
    col.prop(p,'bond',text='Bond',expand=True)
    col.prop(p,'joint_depth',text='Joint Depth')
    col.prop(p,'broken_chance',text='Broken Chance')
    box=layout.box()
    material_controls.centered_heading(box,'Plaster',icon='SETTINGS')
    col=box.column(align=True)
    material_controls.inline(box, source.project_damage, 'brick_render_material', text='Broken Plaster Edge')
    col.prop(p,'plaster_thickness',text='Plaster Thickness')
    col.prop(p,'plaster_band',text='Plaster Band')
    box=layout.box()
    material_controls.centered_heading(box,'Bricks',icon='SETTINGS')
    col=box.column(align=True)
    material_controls.inline(box, source.project_damage, 'buildup_material', text='Rubble')
    col.prop(p,'brick_collection',text='Brick Collection')
    col.prop(p,'rubble_enable',text='Rubble Enable')
    if p.rubble_enable:
        col.prop(p,'rubble_share',text='Rubble Share')
        col.prop(p,'rubble_spread',text='Rubble Spread')
        col.prop(p,'rubble_max',text='Rubble Max')


def draw_riven_studs(layout,p):
    from .recording_tools import toggle_box
    col=toggle_box(layout,p,'lath_stud_enable','Enable Studs')
    if col is not None:
        col.prop(p,'lath_stud_spacing',text='Stud Spacing')
        col.prop(p,'lath_stud_width',text='Stud Width')
        col.prop(p,'lath_stud_depth',text='Stud Depth')


def draw_riven_settings(layout,p):
    box=layout.box()
    material_controls.centered_heading(box,'Riven Boards',icon='SETTINGS')
    col=box.column(align=True)
    col.prop(p,'lath_riven_wave',text='Riven Wave')
    col.prop(p,'lath_riven_wave_length',text='Riven Wave Length')
    col.prop(p,'lath_riven_width_var',text='Riven Width Var')
    col.prop(p,'lath_riven_twist',text='Riven Twist')
    col.prop(p,'lath_riven_rough',text='Riven Rough')
    col.prop(p,'lath_lath_hang',text='Lath Hang')
    col=_ui_disclosure(box,'lath_joints_open','Board Joints')
    if col is None:return
    col.prop(p,'lath_board_studs',text='Board Studs')
    col.prop(p,'lath_joint_bundle',text='Joint Bundle')


def draw_framing_settings(layout,p):
    box=layout.box()
    material_controls.centered_heading(box,'Frame',icon='SETTINGS')
    col=box.column(align=True)
    col.prop(p,'rebar_seed',text='Seed')
    col.prop(p,'rebar_show_all',text='Show All')
    col.prop(p,'rebar_stud_width',text='Stud Width')
    col.prop(p,'rebar_stud_depth',text='Stud Depth')
    col.prop(p,'rebar_stud_spacing',text='Stud Spacing')
    col.prop(p,'rebar_joist_depth',text='Joist Depth')
    col.prop(p,'rebar_noggin_spacing',text='Noggin Spacing')
    col.prop(p,'rebar_double_plate',text='Double Plate')
    col.prop(p,'rebar_facet_angle',text='Facet Angle')
    box=layout.box()
    material_controls.centered_heading(box,'Drywall Skin',icon='SETTINGS')
    col=box.column(align=True)
    col.prop(p,'rebar_skin',text='Skin')
    col.prop(p,'rebar_cavity_reach',text='Cavity Reach')
    col.prop(p,'rebar_paper_tear',text='Paper Tear')
    col.prop(p,'rebar_paper_depth',text='Paper Depth')
    box=layout.box()
    material_controls.centered_heading(box,'Damage',icon='SETTINGS')
    col=box.column(align=True)
    col.prop(p,'rebar_wood_broken',text='Wood Broken')
    col.prop(p,'rebar_nails',text='Nails')
    col.prop(p,'rebar_nail_length',text='Nail Length')
    col.prop(p,'rebar_scale',text='Scale')



