# переводим настройки Blender, размеры и вспомогательные объекты в данные расчёта
from . import preview_quality
# ProjectDamage for Blender - worn edge damage cut into mesh objects.
# Copyright (C) 2026 Cornelius Dammrich
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
"""PropertyGroup -> core Params and BreakerSpecs, mirroring ReadParams and
the adapter's breaker resolution (cpp/source/project_damage_plugin.cpp) step for step,
plus the Blender-units -> centimeters conversion every mapping runs through.
"""
import math

import bpy
import numpy as np
from .fracture_assets import effective_type, proximity_mode


def unit_factor(context):
    """Blender scene units -> C4D centimeters, the unit every core parameter
    (Size, Depth, ...) is written in. `scale_length` is meters per Blender
    unit (default 1.0), so 100 * scale_length turns a Blender-unit coordinate
    into centimeters: Blender's default 2 m cube (+-1 unit) becomes the same
    200 cm cube C4D defaults to, and the C4D-scaled parameter numbers (chips
    2-8) work on it unchanged. The NONE unit system carries no scale_length
    override of its own - Blender still treats "1 unit" as 1 meter there - so
    it needs no special case, just the same formula."""
    return 100.0 * context.scene.unit_settings.scale_length


def _clamp_scale(v):
    """ReadParams' own clampScale (project_damage_plugin.cpp): every per-module Scale and
    Detail is clamped to 0.1..4.0 before the global multiplier folds in."""
    return 0.1 if v < 0.1 else (4.0 if v > 4.0 else v)


def _fissure_scale(props):
    """Ground Fissures' own Master Scale (1..400 %), on top of the global one:
    every fissure size follows it (2026-10-26)."""
    return min(4.0, max(0.01, getattr(props, 'fissure_master_scale', 100.0) / 100.0))


def _min_extent(v):
    """ReadParams' minExtent: a zero-extent cutter is undefined input for the
    Boolean, so every size/depth lands on a 1e-4 floor after scaling."""
    return v if v > 1e-4 else 1e-4


def fill_params(props, core, obj=None, geometry=None):
    """PropertyGroup -> core.Params, mirroring ReadParams (project_damage_plugin.cpp)
    step for step: the Enable switches zero their module's PRIMARY field only,
    Global Scale/Detail fold into the per-module ones, sizes are scaled before
    the minExtent floor. Everything not exposed keeps the core's own default.

    `obj` is optional so every existing 2-arg call site (goldens, fingerprint
    probes in check_blender_e2e.py) keeps working unchanged - it is only used
    below to resolve whether `mask_group` names a REAL vertex group, and its
    absence falls back to a name-only approximation (see field_linked)."""
    p = core.Params()
    gscale = _clamp_scale(props.global_scale / 100.0)
    gdetail = _clamp_scale(preview_quality.percent(props) / 100.0)
    all_off = props.all_off

            
    p.seed = props.seed
    p.damage_shading = props.damage_shading
    p.damage_smooth_angle = props.damage_smooth_angle
    p.base_smooth_angle = props.base_smooth_angle
    p.threshold_deg = math.degrees(props.edge_threshold)
    p.topology = int(props.topology)
                                                                    
                                            
                                                                              
                                                                       
                                                                                            
                            
    p.base_map = bool(props.keep_selections or getattr(props, 'preserve_data', False))
                                                                             
                                                                         
    p.axis_x = props.axis_x == '1'
    p.axis_y = props.axis_y == '1'
    p.axis_z = props.axis_z == '1'
                                                                             
                                                                                

           
    chips_on = not all_off and props.chips_enable
    p.amount = props.amount if chips_on else 0
                                                                       
                                                                                      
    chip_scale = _clamp_scale(props.chip_scale / 100.0) * gscale
    p.size_min = _min_extent(props.size_min * chip_scale)
    p.size_max = _min_extent(props.size_max * chip_scale)
    p.depth_min = _min_extent(props.depth_min * chip_scale)
    p.depth_max = _min_extent(props.depth_max * chip_scale)
    p.detail = (props.detail / 100.0) * gdetail                                         
                                                                             
                                                                            
                                                                            
    stretch = props.stretch / 100.0
    p.stretch = stretch if stretch > 0.01 else 0.01                          
    p.chip_rim_soft = props.rim_soft / 100.0
    p.norm_enable = props.normalize
    norm_max = props.norm_max_size / 100.0
    p.norm_max_size = norm_max if norm_max > 0.01 else 0.01                          
                                                            
    p.offset = props.offset / 100.0
    p.random_offset = props.random_offset / 100.0
                                                                      
    p.noise_strength = props.noise_strength
    p.noise_scale = props.noise_scale
    if hasattr(p, 'noise_roughness'):
        p.noise_roughness = props.noise_roughness / 100.0
        p.noise_distortion = props.noise_distortion / 100.0
                                                                            
                                                                           
    p.sec_amount = 0
    p.sec_size = props.sec_size / 100.0
    p.sec_depth = props.sec_depth / 100.0
    if hasattr(p,'chip_seed'):p.chip_seed=props.chip_seed
    worn_v2 = hasattr(p, 'chip_cluster')                                               
    if worn_v2:
        p.chip_cluster = props.chip_cluster / 100.0
        p.chip_fine = props.chip_fine / 100.0
        p.chip_profile = props.chip_profile / 100.0
        p.chip_asym = props.chip_asym / 100.0
        p.chip_guard = props.chip_guard / 100.0

             
    surf_on = not all_off and props.surf_enable
    p.surf_amount = props.surf_amount if surf_on else 0
    p.surf_seed = props.surf_seed
    p.surf_type = int(props.surf_type)
    p.surf_grain_x = props.surf_grain_x
    p.surf_grain_y = props.surf_grain_y
    p.surf_grain_z = props.surf_grain_z
    p.surf_knife_jitter = props.surf_knife_jitter
    p.surf_knife_back = props.surf_knife_back / 100.
    p.surf_paint_only = surf_on and props.surf_mode == 'PAINT'
    p.surf_paint_snap = props.surf_paint_snap
    surf_scale = _clamp_scale(props.surf_scale / 100.0) * gscale
    p.surf_size_min = _min_extent(props.surf_size_min * surf_scale)
    p.surf_size_max = _min_extent(props.surf_size_max * surf_scale)
    p.surf_depth_min = _min_extent(props.surf_depth_min * surf_scale)
    p.surf_depth_max = _min_extent(props.surf_depth_max * surf_scale)
    sdetail = (props.surf_detail / 100.0) * gdetail
    p.surf_detail = sdetail if sdetail > 0.05 else 0.05
                                                                              
                                                                 
    snoise = props.surf_noise / 100.0
    p.surf_noise = snoise if snoise > 0.0 else 0.0
    snoise_in = props.surf_noise_in / 100.0
    p.surf_noise_in = snoise_in if snoise_in > 0.0 else 0.0
                                                                              
                                                                      
    p.surf_sec_amount = 0
    p.surf_sec_size = props.surf_sec_size / 100.0
    p.surf_sec_depth = props.surf_sec_depth / 100.0
    if worn_v2:
        p.surf_cluster = props.surf_cluster / 100.0
        p.surf_size_bias = props.surf_size_bias / 100.0
        p.surf_depth_follow = props.surf_depth_follow / 100.0
        p.surf_guard = props.surf_guard / 100.0

                                                                         
                                                                             
    build_on = not all_off and props.build_enable
    pile_on = not all_off and (props.pile_enable if worn_v2 else props.build_enable)
    p.build_coverage = (props.build_coverage / 100.0) if build_on else 0.0
    if hasattr(p, 'build_paint_only'):
                                                                    
                                                 
        p.build_paint_only = build_on and props.build_mode == 'PAINT'
        p.build_paint_radius = max(0.01, props.build_paint_radius) * gscale
        if p.build_paint_only:
            p.build_coverage = 1.0
    p.build_corner_prob = (props.build_corner_prob / 100.0) if pile_on else 0.0
    build_scale = _clamp_scale(props.build_scale / 100.0) * gscale
    pile_scale = _clamp_scale(props.pile_scale / 100.0) * gscale if worn_v2 else build_scale
    p.build_detail = _clamp_scale(props.build_detail / 100.0) * gdetail
                                                                          
                                        
    p.build_width = _min_extent(props.build_width * build_scale)
    p.build_height = _min_extent(props.build_height * build_scale)
    p.build_corner_size = _min_extent(props.build_corner_size * pile_scale)
    if worn_v2:
        p.build_shape = int(props.build_shape)
        p.build_lumps = props.build_lumps / 100.0
        p.pile_height = props.pile_height / 100.0
        p.pile_lumps = props.pile_lumps / 100.0
        p.pile_detail = _clamp_scale(props.pile_detail / 100.0) * gdetail
                                                                             
    irreg = props.build_irreg / 100.0
    p.build_irreg = irreg if irreg > 0.0 else 0.0
                                                                      
    blend = props.build_blend / 100.0
    p.build_blend = 0.0 if blend < 0.0 else (2.0 if blend > 2.0 else blend)

    deposit_on = not all_off and props.deposit_enable and (worn_v2 or props.build_enable)
    deposit_scale = _clamp_scale(props.deposit_scale / 100.0) * gscale if worn_v2 else build_scale
    p.deposit_amount = props.deposit_amount if deposit_on else 0
    p.deposit_style = int(props.deposit_style)
    p.deposit_size_min = props.deposit_size_min * deposit_scale
    p.deposit_size_max = max(props.deposit_size_min, props.deposit_size_max) * deposit_scale
    p.deposit_height = props.deposit_height * deposit_scale
    p.deposit_gravity = props.deposit_gravity / 100.0
    p.deposit_gravity_x, p.deposit_gravity_y, p.deposit_gravity_z = props.deposit_gravity_direction
    p.deposit_shelter = props.deposit_shelter / 100.0
    p.deposit_shelter_radius = props.deposit_shelter_radius * deposit_scale
    if worn_v2:
        p.deposit_detail = _clamp_scale(props.deposit_detail / 100.0) * gdetail
    p.deposit_embed = props.deposit_embed / 100.0
    p.deposit_layers = props.deposit_layers
    p.deposit_porosity = props.deposit_porosity / 100.0
    p.deposit_separate = True
                                                                            
    p.rust_use_paint = props.rust_mode == 'PAINT'
    p.rust_enable = False                                                      
    p.rust_seed = props.rust_seed
    p.rust_intensity = props.rust_intensity / 100.0
    p.rust_edge_bias = props.rust_edge_bias / 100.0
    p.rust_bottom_bias = props.rust_bottom_bias / 100.0
    p.rust_noise = props.rust_noise / 100.0
    p.rust_noise_scale = props.rust_noise_scale * gscale
    p.rust_detail = props.rust_detail * gscale / gdetail
    p.rust_crust_start = props.rust_crust_start / 100.0
    p.rust_crust_height = props.rust_crust_height * gscale
    p.rust_crust_rough = props.rust_crust_rough / 100.0
    p.rust_pits = props.rust_pits
    p.rust_pit_start = props.rust_pit_start / 100.0
    p.rust_pit_size = props.rust_pit_size * gscale
    p.rust_pit_depth = props.rust_pit_depth * gscale
    p.rust_hole_start = props.rust_hole_start / 100.0
    p.rust_hole_depth = props.rust_hole_depth * gscale
    p.rust_flakes = props.rust_flakes / 100.0
    p.rust_flake_size = props.rust_flake_size * gscale
    p.rust_paint = props.rust_paint
    p.rust_paint_start = props.rust_paint_start / 100.0
    p.rust_band = props.rust_band / 100.0
    p.rust_paint_flakes = props.rust_paint_flakes / 100.0
    p.rust_craze = props.rust_craze / 100.0
    p.rust_craze_cell = props.rust_craze_cell * gscale
    p.rust_blisters = props.rust_blisters
    p.rust_blister_size = props.rust_blister_size * gscale
    p.rust_streak = props.rust_streak / 100.0
    p.rust_streak_length = props.rust_streak_length * gscale
    if obj is not None and props.rust_world_gravity:
        from mathutils import Vector
        g = obj.matrix_world.to_3x3().inverted_safe() @ Vector((0.0, 0.0, -1.0))
        g = g.normalized() if g.length > 1e-12 else Vector((0.0, 0.0, -1.0))
        p.rust_gravity_x, p.rust_gravity_y, p.rust_gravity_z = g.x, g.y, g.z
    else:
        p.rust_gravity_x, p.rust_gravity_y, p.rust_gravity_z = props.rust_gravity
    p.heat_enable = False                                                      
    p.heat_seed = props.heat_seed
    p.heat_intensity = props.heat_intensity
    p.heat_diffuse = props.heat_diffuse
    p.heat_spread = props.heat_spread * gscale
    p.heat_duration = props.heat_duration
    p.heat_steps = props.heat_steps
    p.heat_char_start = props.heat_char_start / 100.0
    p.heat_shrink_depth = props.heat_shrink_depth * gscale
    p.heat_detail = props.heat_detail * gscale / gdetail
    p.heat_block_round = props.heat_block_round / 100.0
    p.heat_cell_min = props.heat_cell_min * gscale
    p.heat_cell_max = props.heat_cell_max * gscale
    p.heat_crack_cells = props.heat_crack_cells
    p.heat_crack_width = props.heat_crack_width * gscale
    p.heat_crack_depth = props.heat_crack_depth * gscale
    p.heat_blisters = props.heat_blisters
    p.heat_blister_min = props.heat_blister_min * gscale
    p.heat_blister_max = props.heat_blister_max * gscale
    p.heat_blister_low = props.heat_blister_low / 100.0
    p.heat_blister_high = props.heat_blister_high / 100.0
    p.heat_blister_burst = props.heat_blister_burst / 100.0
    p.heat_char_layer = props.heat_char_layer * gscale
    p.heat_cell_max = max(p.heat_cell_min, p.heat_cell_max)
    p.heat_blister_max = max(p.heat_blister_min, p.heat_blister_max)
    p.coating_zone_enable = props.coating_zone_enable and not all_off
    p.coating_zone_threshold = props.coating_zone_threshold / 100.0
    p.coating_zone_smooth = props.coating_zone_smooth * gscale
    p.coating_zone_inset = props.coating_zone_inset * gscale
    p.coating_zone_detail = props.coating_zone_detail * gscale / gdetail
    p.coating_flake_side = int(props.coating_flake_side)
    p.coating_flake_side_x = props.coating_flake_side_x
    p.coating_flake_side_y = props.coating_flake_side_y
    p.coating_flake_side_z = props.coating_flake_side_z
    p.coating_enable = props.coating_enable and not all_off
    p.coating_amount = props.coating_amount
    p.coating_thickness = props.coating_thickness * gscale
    p.coating_patch_min = props.coating_patch_min * gscale
    p.coating_patch_max = max(props.coating_patch_min, props.coating_patch_max) * gscale
    p.coating_jagged = props.coating_jagged / 100.0
    p.coating_flakes = props.coating_flakes / 100.0
    p.coating_curl = props.coating_curl / 100.0
    p.coating_cracks = props.coating_cracks / 100.0
    p.coating_seed = props.coating_seed
    p.coating_satellites = props.coating_satellites / 100.0
    p.coating_stretch = props.coating_stretch / 100.0
    p.coating_dir_x = props.coating_dir_x
    p.coating_dir_y = props.coating_dir_y
    p.coating_dir_z = props.coating_dir_z
    p.coating_edge_wrap = props.coating_edge_wrap / 100.0
    p.coating_facing_only = props.coating_facing_only
    p.coating_facing_x = props.coating_facing_x
    p.coating_facing_y = props.coating_facing_y
    p.coating_facing_z = props.coating_facing_z
    p.coating_facing_angle = props.coating_facing_angle
    p.coating_flake_size = props.coating_flake_size
    p.coating_crack_length = props.coating_crack_length

                                                                           
                                                               
    from .effect_schema import EFFECT_FIELDS, WALLPAPER_RANDOMNESS_FIELDS
    for module, fields in EFFECT_FIELDS.items():
        block = getattr(p, module, None)
        if block is None:
            continue                                                    
        for name, is_length in fields:
            value = getattr(props, module + "_" + name)
            if module=='hang' and name in {'size_min','size_max'}:
                value *= props.hang_master_scale/100.
            if module == 'wallpaper':
                if is_length:
                    value *= props.wallpaper_master_scale / 100.0
                elif name in WALLPAPER_RANDOMNESS_FIELDS:
                    value /= 100.0
            if module=='lath' and name in {'lath_width','lath_thickness','lath_gap','lath_length','lath_embed','riven_wave_length','stud_spacing','stud_width','stud_depth','noggin_spacing'}:
                value *= props.lath_wood_master_scale/100.
            if module=='glass' and is_length:
                value *= props.glass_master_scale/100.
            if module=='plaster' and is_length:
                value *= _clamp_scale(props.plaster_master_scale/100.)
            if module=='fissure' and is_length:
                value *= _fissure_scale(props)
            if name == "enable":
                value = bool(value and not all_off and module not in {'bevel','metal'})
                if module in {'hit','glass'}:
                    value = bool((value or getattr(props,module+'_through_enable') or (module == 'glass' and (props.glass_hit_enable or props.glass_drawn_enable))) and not all_off)
                    if module=='glass':value=bool((props.glass_hit_enable or props.glass_drawn_enable) and not all_off)
                elif module == 'fabric':
                    value = bool((props.fabric_enable or props.fabric_solid_enable) and not all_off)
                elif module == 'ground':
                    value = value and (props.ground_axis_x or props.ground_axis_y or props.ground_axis_z)
                elif module == 'concrete':
                    value = bool((props.concrete_enable or props.concrete_v2_enable) and not all_off)
                elif module in ('lath', 'brick'):
                    value = bool((getattr(props, module + '_enable') or getattr(props, module + '_v2_enable')
                                  or (module == 'lath' and (props.lath_riven_enable or props.lath_framing_enable))) and not all_off)
            elif module == 'concrete' and name == 'profile':
                value = 2 if props.concrete_wall_type=="WOOD" else (1 if props.concrete_v2_enable and not all_off else 0)
            elif module in ('lath', 'brick') and name == 'profile':
                value = 1 if getattr(props, module + '_v2_enable') and not all_off else 0
            elif module == 'lath' and name == 'style':
                value = 0 if all_off else 2 if props.lath_framing_enable else 1 if props.lath_riven_enable else 0
            elif module == 'fabric' and name == 'solid':
                value = bool(props.fabric_solid_enable and not all_off)
            elif module=='glass' and name=='chip_enable' and (props.glass_hit_enable or props.glass_drawn_enable):
                value=False
            elif module=='brick' and props.brick_enable and not props.brick_v2_enable and name in {'style','brick_mode','surf_mode','behind','damage_depth','damage_noise','damage_scale'}:
                value={'style':0,'brick_mode':1,'surf_mode':0,'behind':1,'damage_depth':0.,'damage_noise':0.,'damage_scale':0.}[name]
            elif module == 'glass' and name == 'paint_only':
                                                                                  
                value = bool(value or getattr(props, 'glass_hit_enable') or getattr(props, 'glass_drawn_enable'))
            elif module == 'glass' and name == 'impact_enable' and (getattr(props, 'glass_hit_enable') or getattr(props, 'glass_drawn_enable')):
                value = False                                                             
            elif module == 'glass' and name == 'paint_hole_cracks' and getattr(props, 'glass_drawn_enable'):
                value = True
            elif module == 'glass' and name == 'crack_reach' and getattr(props, 'glass_drawn_enable'):
                value = float(getattr(props, 'glass_drawn_reach'))
            elif module == 'glass' and name == 'through' and (props.glass_hit_enable or props.glass_drawn_enable):
                                                            
                value = bool(getattr(props, 'glass_pass_through', False) and not all_off)
            elif module == 'glass' and name.startswith('fill_') and not hasattr(block, name):
                continue                                    
            elif module == 'glass' and name == 'fill_hole':
                value = bool(value and (props.glass_hit_enable or props.glass_drawn_enable) and not all_off)
            elif module=='hit' and name=='through' and props.hit_enable and not props.hit_through_enable:
                                                                                     
                value=bool(getattr(getattr(props.id_data,'project_damage_physics',None),'hit_separate',False) and not all_off)
            elif module in {'hit','glass'} and name=='through':
                value = bool(value and getattr(props,module+'_through_enable') and not all_off)
            elif module in {'hit','glass'} and name=='through_min_piece':
                value /= 100.0
            elif module=='debris' and name=='min_volume':
                value *= gscale**3
            elif module in ('concrete','lath','brick','hang') and name=='tex_path':
                value=surface_texture_path(props,module)
            elif module=='hang' and name=='surf_mode':
                value=0                                                  
            elif module=='wallpaper' and name=='paint_enable':
                value=props.wallpaper_drawing is not None
            elif isinstance(value, str) and name in {'mode', 'grain_mode', 'aim', 'rebar_outer', 'hole_dir_mode', 'material', 'shot_mode', 'bond', 'profile', 'size_mode', 'style', 'crush_aim', 'behind', 'brick_mode', 'thickness_logic', 'surf_mode', 'planar', 'stroke_mode'}:
                value = int(value)
            elif is_length and not (module == "bevel" and name in {"size_top", "size_side"} and int(props.bevel_size_mode) == 1):
                value *= gscale
            if name == 'mode' and module in {'wood','bullet','ground','rusty'} and value == 2:
                value = 1
            if name == 'detail':value *= gdetail
            if (module == 'lath' or (module == 'hang' and name == 'thickness')) and not hasattr(block, name):
                continue                                                                 
            setattr(block, name, value)
        if module == "scratch":
            block.density /= max(gscale, 1e-9)
            if block.style != 3: block.size_scale *= gscale
        if module == "bevel": block.wear_chips /= max(gscale, 1e-9)
        if module == "fissure":
                                                                           
                                                            
            block.branch_density /= _fissure_scale(props)
            block.rubble_density /= _fissure_scale(props)
        if module == "fabric" and hasattr(block, "solid_bevel"):
            block.solid_bevel = float(getattr(props, "fabric_solid_bevel", 0.0))
            block.solid_bevel_segments = int(getattr(props, "fabric_solid_bevel_segments", 3))
        if module == "fabric" and hasattr(block, "tear_style"):
                                                                      
            if hasattr(block,'woven_threads'):
                block.woven_threads = getattr(props,'fabric_fibre_model','LOOSE') == 'WOVEN'
            block.tear_style = 2 if getattr(props,"fabric_tear_style","1")=="1" else 0
            if hasattr(block,"distribution_scale"):
                block.distribution_scale=props.fabric_distribution_scale*gscale
                block.distribution_strength=props.fabric_distribution_strength
                block.shape_noise_scale=props.fabric_shape_noise_scale*gscale
                block.shape_noise_strength=props.fabric_shape_noise_strength
            block.tear_rip = float(getattr(props, "fabric_tear_rip", 0.4))
            block.tear_corner = float(getattr(props, "fabric_tear_corner", 0.25))
            block.tear_worn = float(getattr(props, "fabric_tear_worn", 0.25))
            block.tear_puncture = float(getattr(props, "fabric_tear_puncture", 0.1))
            block.grain_snap = float(getattr(props, "fabric_grain_snap", 0.85))
            block.yarn_size = max(1e-4, float(getattr(props, "fabric_yarn_size", 0.25)) * gscale)
            block.flap_lift = float(getattr(props, "fabric_flap_lift", 0.6))

    for module in ('concrete','lath','brick'):
        block=getattr(p,module)
        if hasattr(block,'organic'):
            block.organic=getattr(props,module+'_model_type')=='ORGANIC'

    from . import damage_proximity
    damage_proximity.fill(p,props,core,obj,geometry)

    from . import ground_depth
    ground_depth.fill(p, props)

    from . import damaged_bevel
    damaged_bevel.fill_params(p, props, gscale, gdetail, all_off)

    if p.rbf.enable and props.rbf_impact_collection is not None:
        deps=bpy.context.evaluated_depsgraph_get()
        source=props.id_data
        inverse=source.evaluated_get(deps).matrix_world.inverted_safe()
        factor=unit_factor(bpy.context)
        impacts=[]
        for obj in sorted(props.rbf_impact_collection.all_objects,key=lambda o:o.name):
            if obj.type!='EMPTY' or obj==source:continue
            pos=inverse @ obj.evaluated_get(deps).matrix_world.translation
            impact=core.RBImpact();impact.x,impact.y,impact.z=[float(x)*factor for x in pos]
            impact.radius=p.rbf.impact_radius
            impact.strength=max(0.,float(obj.get('project_damage_rbf_strength',1.)))
            impacts.append(impact)
        p.rbf.impacts=impacts

    if getattr(p, 'cellf', None) is not None and p.cellf.enable:
        p.cellf.proximity = fill_cellf_proximity(props.id_data, props, core, unit_factor(bpy.context))
        p.cellf.stroke_mode = 4  # Modifier guidance; filled damage uses the original fracture path.
        if props.cellf_guidance_drawing is None: p.cellf.guidance_amount = 0

    if obj is not None and props.rusty_enable:
                                                             
        down = obj.matrix_world.to_3x3().inverted_safe() @ __import__('mathutils').Vector((0.0, 0.0, -1.0))
        if down.length > 1e-9:
            down.normalize()
            p.rusty.down_x, p.rusty.down_y, p.rusty.down_z = down.x, down.y, down.z
              
    fract_on = not all_off and (props.fract_enable or props.fract_through_enable)
    if obj is not None and props.deform_enable and not all_off:
        fill_deform(props, core, obj, p)
    if obj is not None and props.hang_enable:
                                                                          
        down = obj.matrix_world.to_3x3().inverted_safe() @ __import__('mathutils').Vector((0.0, 0.0, -1.0))
        if down.length > 1e-9:
            down.normalize()
            p.hang.gravity_x, p.hang.gravity_y, p.hang.gravity_z = down.x, down.y, down.z
    if obj is not None:
        from . import wall_hole as _wall_hole
        _wall_hole.fill(bpy.context, obj, core, p, unit_factor(bpy.context))
    p.fract_through = fract_on and props.fract_through_enable and props.fract_through
    volume = False                                                
    if hasattr(p, 'fract_through_fill'):
        p.fract_through_fill = bool(volume and p.fract_through)
        p.fract_through_fill_size = props.fract_volume_size * gscale
        p.fract_through_rough = props.fract_volume_rough / 100.0
    elif volume:
        raise RuntimeError('Fracture Volume needs the updated native core. Restart Blender after installing the update.')
    p.fract_physics = fract_on and not props.fract_through_enable and props.fract_physics
    p.fract_physics_depth = props.fract_physics_depth * gscale
    p.fract_physics_close = props.fract_physics_close
    p.fract_helper_snap = props.fract_helper_snap * gscale
    p.fract_through_min_piece = props.fract_through_min_piece / 100.0
    p.fract_paint_only = fract_on and props.fract_mode == 'PAINT'
    p.fract_type = {'FRACTURE':0,'RADIAL':1,'GLASS':2,'WOOD':3}[effective_type(props)] if fract_on and not p.fract_paint_only else 0
    p.fract_impacts = props.fract_impacts
    p.fract_grain_x,p.fract_grain_y,p.fract_grain_z = props.fract_grain_x,props.fract_grain_y,props.fract_grain_z
    p.fract_grain_pull = props.fract_grain_pull
    scale = props.fract_paint_scale if p.fract_paint_only else props.fract_scale
    fract_scale = _clamp_scale(scale / 100.0) * gscale
    p.fract_detail = _clamp_scale(props.fract_detail / 100.0) * gdetail
    for fl, n in ((p.fract1, 1), (p.fract2, 2), (p.fract3, 3)):
        amount = getattr(props, "fract_l%d_amount" % n)
        fl.amount = amount if fract_on and (not p.fract_paint_only or n > 1) else 0
        fl.paint_radius = getattr(props, 'fract_l%d_paint_radius' % n)
                                                                              
                                                                           
                                                                            
                                                                   
        fl.width = getattr(props, "fract_l%d_width" % n) * fract_scale
        fl.depth = getattr(props, "fract_l%d_depth" % n) * fract_scale
                                                                          
                                      
        bias = getattr(props, "fract_l%d_bias" % n) / 100.0
        fl.bias = 0.0 if bias < 0.0 else (1.0 if bias > 1.0 else bias)
        fl.chips = getattr(props, "fract_l%d_chips" % n)
                                                                           
                                                                            
                                                            
        fl.chip_amount = getattr(props, "fract_l%d_chip_amount" % n)
        fl.chip_size = getattr(props, "fract_l%d_chip_size" % n) * fract_scale
        fl.chip_depth = getattr(props, "fract_l%d_chip_depth" % n) * fract_scale
        fl.subdiv = getattr(props, "fract_l%d_subdiv" % n)
        fl.length = getattr(props, 'fract_l%d_length' % n) * fract_scale
        if p.fract_paint_only and n>1:
            if not hasattr(fl,'paint_length_min'):
                raise RuntimeError('Paint fracture Length Min and Max need the updated native core. Restart Blender after installing the update.')
            fl.amount = getattr(props, 'fract_l%d_paint_amount' % n)
            length = 'fract_l%d_paint_length' % n
            upper = getattr(props, length)
            if props.get(length + '_min') is not None:
                lower, upper = sorted((getattr(props, length + '_min'), upper))
                fl.paint_length_min = lower * fract_scale
            fl.length = upper * fract_scale
            fl.paint_distortion=getattr(props,'fract_l%d_paint_distortion'%n)/100.
            fl.paint_distortion_scale=getattr(props,'fract_l%d_paint_distortion_scale'%n)*fract_scale
        fl.branches = getattr(props,'fract_l%d_branches'%n)
        fl.branch_length = getattr(props,'fract_l%d_branch_length'%n)
                                                                           
                                 
                                                                             
    if hasattr(p,'fract_rounded'):p.fract_rounded=fract_on and props.fract_surface=='ROUNDED'
    from . import surface_guidance
    if hasattr(p, 'surface_guide'):
        p.surface_guide = surface_guidance.enabled()
    if hasattr(p, 'fract_surface_proxy'):
        entry = bpy.context.preferences.addons.get(__package__)
        p.fract_surface_proxy = bool(fract_on and entry is not None
                                    and getattr(entry.preferences, 'fracture_surface_proxy', False))
    p.fract_recipe = 1 if props.fract_recipe == 'LEGACY' else 2
    p.fract_thickness_cap = props.fract_thickness_cap / 100.0
    p.fract_prox_mode = {'OFF': 0, 'DEFAULT': 1, 'RADIAL': 2, 'GLASS': 3}[proximity_mode(props)] if fract_on else 0
    if p.fract_paint_only:
        p.fract_prox_mode = 0
    p.fract_prox_radius = props.fract_prox_radius * _clamp_scale(props.global_scale / 100.0)
    p.fract_prox_strength = props.fract_prox_strength / 100.0
    from .fracture_assets import active_key,PROXIMITY_ASSETS
    if active_key(props)=='fracture_proximity_default' and fract_on:
        p.fract1.amount=props.fract_proximity_amount
        p.fract2.amount=p.fract3.amount=0
        p.fract_prox_strength=1.0
    if p.fract_prox_mode==2:
        for level in (p.fract1,p.fract2,p.fract3):
            level.width=props.fract_l1_width*fract_scale
            level.depth=props.fract_l1_depth*fract_scale
    p.fract_radial_spokes = props.fract_radial_spokes
    p.fract_radial_rings = props.fract_radial_rings
    p.fract_radial_ring_fill = props.fract_radial_ring_fill / 100.0
    p.fract_radial_crush = props.fract_radial_crush / 100.0
                                                                            
    p.fract_glass_spokes = props.fract_glass_spokes
    p.fract_glass_rings = props.fract_glass_rings
    p.fract_glass_branch = props.fract_glass_branch / 100.0
    p.fract_glass_ring_fill = props.fract_glass_ring_fill / 100.0
    p.fract_glass_width = props.fract_glass_width * fract_scale
    p.fract_glass_depth = props.fract_glass_depth * fract_scale
    p.fract_glass_crush = props.fract_glass_crush / 100.0
    disp_strength = props.fract_disp_strength / 100.0
    p.fract_disp_strength = disp_strength if disp_strength > 0.0 else 0.0
    disp_scale = props.fract_disp_scale / 100.0
    p.fract_disp_scale = disp_scale if disp_scale > 0.05 else 0.05
    if p.fract_paint_only:
        p.fract_disp_strength = 0.0
                                                                   
                                                                           
                                                                           
                                
    depth_mult = props.fract_depth_mult / 100.0
    p.fract_depth_mult = depth_mult if depth_mult > 0.01 else 0.01
                                                                          
                                                                              
                                                               
    tile = props.fract_tile_offset / 100.0
    p.fract_tile_offset = 0.0 if tile <= 0.0 else (1.0 if tile > 1.0 else tile)
    if p.fract_paint_only:
        p.fract_tile_offset = 0.0
    p.fract_tile_seed = props.fract_tile_seed
    if worn_v2:
        p.fract_rim_jag = props.fract_rim_jag / 100.0
        p.fract_flare = props.fract_flare / 100.0
                                                                            
        p.fract_shard_enable = False                          
        p.fract_shard_dir = int(props.fract_shard_dir)
        p.fract_shard_amount = max(0.0, props.fract_shard_amount) * gscale
        p.fract_shard_radius = max(0.1, props.fract_shard_radius) * gscale
        p.fract_shard_falloff = props.fract_shard_falloff
        p.fract_shard_tilt = props.fract_shard_tilt / 100.0
        p.fract_shard_random = props.fract_shard_random / 100.0
        p.fract_shard_seed = props.fract_shard_seed
                                                                   
                                                                       
    p.fract_rim_soft = props.fract_rim_soft / 100.0
                                                                              
                                                                              
                                                                           
                                                                           
                                                   
    p.avoid_chips = props.avoid_chips
    p.avoid_surf = props.avoid_surf
    p.avoid_build = props.avoid_build
    p.avoid_fract = props.avoid_fract
    p.avoid_corner = props.avoid_corner

             
    corner_on = not all_off and props.corner_enable
    p.corner_prob = (1.0 if props.corner_mode == 'PAINT' else props.corner_prob / 100.0) if corner_on else 0.0
    p.corner_min_edges = props.corner_min_edges
    corner_scale = _clamp_scale(props.corner_scale / 100.0) * gscale
    p.corner_size_min = _min_extent(props.corner_size_min * corner_scale)
    p.corner_size_max = _min_extent(props.corner_size_max * corner_scale)
    p.corner_depth_min = _min_extent(props.corner_depth_min * corner_scale)
    p.corner_depth_max = _min_extent(props.corner_depth_max * corner_scale)
    p.corner_detail = _clamp_scale(props.corner_detail / 100.0) * gdetail
                                                                      
                                          
    p.corner_rim_soft = props.corner_rim_soft / 100.0
                                                                             
                                                                          
    p.corner_sec_amount = 0
    p.corner_sec_size = props.corner_sec_size / 100.0
    p.corner_sec_depth = props.corner_sec_depth / 100.0
    if worn_v2:
        p.corner_style = int(props.corner_style)
        p.corner_rough = props.corner_rough / 100.0
        p.corner_tilt = props.corner_tilt / 100.0
        p.corner_flakes = props.corner_flakes / 100.0
                                                                        
        p.corner_paint_only = corner_on and props.corner_mode == 'PAINT'
        p.corner_paint_radius = max(0.01, props.corner_paint_radius) * gscale

                                                                                   
                                                                          
                                                                  
                                                                           
                                                  
    p.break_surf_dmg = props.break_surf_dmg
    p.break_rim_amount = props.break_rim_amount
    p.break_rim_size = props.break_rim_size / 100.0
    p.break_rim_depth = props.break_rim_depth / 100.0
                                                                           
                                                                 
    if all_off or not props.break_rim_enable:
        p.break_rim_amount = 0
        p.break_surf_dmg = False
    from .rebar import FIELDS
    p.break_rebar.enable = bool(props.rebar_enable and props.ui_boolean_enable and not all_off)
    if hasattr(p.break_rebar,'experimental_riven'):
        p.break_rebar.experimental_riven=False
    if hasattr(p.break_rebar, 'existing_damage'):
        p.break_rebar.existing_damage = bool(props.rebar_existing_damage)
    for name,kind,default,is_length in FIELDS:
        if name=='enable':continue
        if name.startswith('chip_') and not hasattr(p.break_rebar,name):continue
        value=getattr(props,'rebar_'+('fill' if name=='material' else name))
        if isinstance(value,str):value=int(value)
        if is_length:value*=gscale
        if name=='detail' and props.rebar_fill!='0':value*=gdetail
        setattr(p.break_rebar,name,value)
    if hasattr(p.break_rebar,'stable_volume'):
        p.break_rebar.stable_volume=props.boolean_stable_volume and props.rebar_fill=='1'
        p.break_rebar.volume_axis=int(props.boolean_volume_axis)
        p.break_rebar.volume_budget=props.boolean_volume_budget
        p.break_rebar.volume_clearance=props.boolean_volume_clearance*gscale
        p.break_rebar.volume_min_length=props.boolean_volume_min_length*gscale
        p.break_rebar.volume_min_section=props.boolean_volume_min_section
        for axis in ('x','y','z'):
            setattr(p.break_rebar,'volume_origin_'+axis,getattr(props,'boolean_volume_origin_'+axis)*gscale)
        if p.break_rebar.stable_volume:p.break_rebar.brick_3d=False
    elif props.boolean_stable_volume and props.rebar_fill=='1':
        raise RuntimeError('Organic Placement requires the updated native core. Restart Blender after updating Project Damage.')
    if props.rebar_fill in {'2','5'}:
        p.break_rebar.cover=props.boolean_wood_deepness*gscale
    from . import break_surface as _break_surface
    if not getattr(p.break_rebar,'stable_volume',False):_break_surface.fill_rebar_axes(p.break_rebar, props)
    else:p.break_rebar.axis_mask=0
                                                                        
                                                                         
    if props.rebar_fill!='0':
        quality=max(.05,props.boolean_quality/100.)
        p.break_rebar.detail *= quality
        p.break_rebar.sides=max(4,min(64,round(p.break_rebar.sides*quality)))
    p.break_rebar.rebar_resolution=preview_quality.rebar_resolution(props,'rebar_resolution')
    p.concrete.rebar_resolution=preview_quality.rebar_resolution(props,'concrete_rebar_resolution')
    p.hang.rebar_resolution=preview_quality.rebar_resolution(props,'hang_rebar_resolution')
    if props.rebar_fill=='0' and props.rebar_amount>1:
        density=props.rebar_amount**.5
        p.break_rebar.spacing_u/=density
        p.break_rebar.spacing_v/=density
        p.break_rebar.tie_spacing/=density
        p.break_rebar.amount=1.
    if props.rebar_fill=='3':
        density=props.boolean_frame_amount
        if density<=0:p.break_rebar.enable=False
        else:
            p.break_rebar.stud_spacing/=density
            p.break_rebar.noggin_spacing/=density
    if props.boolean_central_rebar and props.rebar_fill == '0':
        if not getattr(core, 'central_rebar_layout', False):
            raise RuntimeError('Central Rebar requires the updated native core. Restart Blender after updating Project Damage.')
        central = p.break_rebar
        central.layout = 4
        central.axis = int(props.boolean_central_axis)
        central.axis_mask = 0
        central.radius = props.boolean_central_diameter * .5 * gscale
        central.embed = props.boolean_central_embed * gscale
        central.scale = 1.
        central.mode = 0
        central.cover = central.reach = central.min_exposed = 0.
        central.amount = 1.
        central.wobble = central.bow = central.bend = central.broken = 0.
        central.jitter = central.deform = central.trim = central.neck = 0.
        central.core = central.show_all = False
    if obj is not None:
        from . import input_prep
        p.glass.thickness_logic=0 if input_prep.settings(obj).thickness_logic=='LEGACY' else 1
    if glass_straight_through(props):
        p.glass.thickness_logic=2
    if props.glass_hit_enable:
        p.glass.paint_impacts_enable = True
        p.glass.paint_point_hole = True
    if props.glass_drawn_enable:
        p.glass.paint_holes_enable = True
    if obj is not None and p.break_rebar.material != 0:
        from mathutils import Vector
        up=obj.matrix_world.to_3x3().inverted_safe() @ Vector((0.,0.,1.))
        if up.length_squared>1e-20:
            up.normalize()
            p.break_rebar.up_x,p.break_rebar.up_y,p.break_rebar.up_z=up
    if p.concrete.enable:
        p.concrete.surf_mode=0
        if hasattr(p.concrete,'surface_distortion'):
            p.concrete.surface_distortion=props.concrete_surface_distortion
            p.concrete.surface_roughness=props.concrete_surface_roughness/100.
            p.concrete.surface_strength=props.concrete_surface_strength/100.
    if p.lath.enable:p.lath.surf_mode=0
    from . import asset_noise as _asset_noise
    _asset_noise.fill(p, props, gscale)
    if p.rusty.enable:
        from .effect_schema import _RUSTY_LENGTHS
        for name in _RUSTY_LENGTHS:setattr(p.rusty,name,getattr(p.rusty,name)*props.rusty_master_scale/100.)
    from . import fissure_realism
    fissure_realism.fill(p, props)
    from . import generated_geometry
    generated_geometry.fill(p, props)
    return p


def _nothing_asked_for(params):
    """True when no module can produce a single cutter - the core returns
    passthrough for that just as it does for an open mesh, and the two need
    different words. Reads the PRIMARY field of each module, the same field
    each module's Enable zeroes (project_damage_plugin.cpp ReadParams), plus the
    breaker list - a breaker-only build (all damage off, one breaker linked)
    must NOT report "no damage is enabled"."""
    return (not params.wall_hole.enable and params.amount == 0 and params.surf_amount == 0
            and (params.corner_prob == 0.0
                 or (getattr(params, 'corner_paint_only', False) and len(params.corner_paint_count) == 0))
            and (params.build_coverage == 0.0
                 or (getattr(params, 'build_paint_only', False) and len(params.build_paint_count) == 0))
            and params.build_corner_prob == 0.0
            and params.deposit_amount == 0
            and not params.coating_enable
            and not params.heat_enable
            and not params.rust_enable
            and not params.hang.enable and not params.fabric.enable and not params.concrete.enable
            and not params.wood.enable and not params.lath.enable and not params.wallpaper.enable
            and not any(getattr(params, m).enable for m in ("bullet", "ground", "brick", "glass", "fissure", "rusty"))
            and not (params.blast.enable and len(params.blast.empties) > 0)
            and not params.bevel.enable and not params.scratch.enable
            and not getattr(getattr(params, "damaged_bevel", None), "enable", False)
            and not (params.metal.enable and (len(params.metal.impactors) if params.metal.style == 0 else len(params.metal.empties)))
            and not (params.break_rebar.enable and (params.break_rebar.show_all or len(params.break_rebar.stroke_count)))
            and not getattr(getattr(params, 'metal_tear', None), 'enable', False)
            and not getattr(getattr(params, 'clay', None), 'enable', False)
            and not getattr(getattr(params, 'pits', None), 'enable', False)
            and not getattr(getattr(params, 'erosion', None), 'enable', False)
            and not params.plaster.enable and not (params.hit.enable and len(params.hit.empties) > 0)
            and not getattr(params,"fracture_layers",())
            and not params.fract_paint_only
            and not params.rbf.enable
            and not (getattr(params, 'cellf', None) is not None and params.cellf.enable)
            and params.fract1.amount == 0 and params.fract2.amount == 0
            and params.fract3.amount == 0
            and not (params.fract_prox_mode in (2, 3) and len(params.fract_proximity) > 0)
            and len(params.breakers) == 0)


def fill_fract_proximity(source, props, core, factor):
    """Objects of the Proximity collection -> FractProximity points in the
    SOURCE's local space, core units (like the mesh points). With Radius from
    Empty Size, an empty's display size x its largest world scale is its
    reach, carried into the source's space and units."""
    pts = []
    coll = props.fract_prox_collection
    if coll is None or proximity_mode(props) == 'OFF':
        return pts
    deps = bpy.context.evaluated_depsgraph_get()
    src_world = source.evaluated_get(deps).matrix_world
    src_inv = src_world.inverted()
    src_scale = sum(abs(v) for v in src_world.to_scale()) / 3.0 or 1.0
    for ob in coll.all_objects:
        if ob is source:
            continue
        mw = ob.evaluated_get(deps).matrix_world
        loc = src_inv @ mw.translation
        fp = core.FractProximity()
        fp.x, fp.y, fp.z = loc.x * factor, loc.y * factor, loc.z * factor
        if props.fract_prox_use_size and ob.type == 'EMPTY':
            world_r = ob.empty_display_size * max(abs(v) for v in mw.to_scale())
            fp.radius = world_r / src_scale * factor
        else:
            fp.radius = 0.0                                                   
        fp.strength = 1.0
        pts.append(fp)
    return pts


def fill_shard_points(source, props, core, factor):
    """Shard Displace points: the Shard collection, or the Proximity points
    when none is linked - FractProximity in the source's local space and core
    units, radius 0 = the Radius slider."""
    coll = props.fract_shard_collection or props.fract_prox_collection
    if coll is None or not props.fract_shard_enable:
        return []
    use_size = props.fract_shard_use_size if props.fract_shard_collection else props.fract_prox_use_size
    deps = bpy.context.evaluated_depsgraph_get()
    src_world = source.evaluated_get(deps).matrix_world
    src_inv = src_world.inverted_safe()
    src_scale = sum(abs(v) for v in src_world.to_scale()) / 3.0 or 1.0
    pts = []
    for ob in sorted(coll.all_objects, key=lambda o: o.name):
        if ob is source or ob.get('project_damage_result_of'):
            continue
        mw = ob.evaluated_get(deps).matrix_world
        loc = src_inv @ mw.translation
        fp = core.FractProximity()
        fp.x, fp.y, fp.z = loc.x * factor, loc.y * factor, loc.z * factor
        fp.radius = 0.0
        if use_size and ob.type == 'EMPTY':
            fp.radius = ob.empty_display_size * max(abs(v) for v in mw.to_scale()) / src_scale * factor
        fp.strength = 1.0
        pts.append(fp)
    return pts


def fill_corner_paint(context, source, props, params, factor):
    """Corner Draw strokes -> Params.set_corner_paint, source-local core units.
    Returns the stroke arrays (for the fingerprint)."""
    if not getattr(params, 'corner_paint_only', False):
        return []
    from . import paint
    paths = [np.ascontiguousarray(path * factor, dtype=np.float64)
             for path in paint.read_paths(context, source, props.corner_drawing, min_points=1)]
    xyz = [float(v) for path in paths for v in path.reshape(-1)]
    params.set_corner_paint(xyz, [len(path) for path in paths])
    return paths


def fill_build_paint(context, source, props, params, factor):
    """Bead Draw Zones strokes -> Params.set_build_paint, source-local core units.
    Returns the stroke arrays (for the fingerprint)."""
    if not getattr(params, 'build_paint_only', False):
        return []
    from . import paint
    paths = [np.ascontiguousarray(path * factor, dtype=np.float64)
             for path in paint.read_paths(context, source, props.build_drawing, min_points=1)]
    xyz = [float(v) for path in paths for v in path.reshape(-1)]
    params.set_build_paint(xyz, [len(path) for path in paths])
    return paths


def fill_cellf_proximity(source, props, core, factor):
    """Cell Fracture Proximity collection -> CellProximity points in the
    SOURCE's local space and core units. An empty is one point whose reach is
    its display size x largest world scale (Use Empty Size) or Proximity
    Radius; a mesh / curve object gives up to 64 of its evaluated surface
    points, each reaching Proximity Radius (radius 0 = the core default)."""
    coll = props.cellf_prox_collection
    if coll is None:
        return []
    deps = bpy.context.evaluated_depsgraph_get()
    src_world = source.evaluated_get(deps).matrix_world
    src_inv = src_world.inverted_safe()
    src_scale = sum(abs(v) for v in src_world.to_scale()) / 3.0 or 1.0
    result = []
    def add(local, radius, strength):
        q = core.CellProximity()
        q.x, q.y, q.z = (float(local[i]) * factor for i in range(3))
        q.radius, q.strength = radius, strength
        result.append(q)
    for ob in sorted(coll.all_objects, key=lambda o: o.name):
        if ob is source or ob.get('project_damage_result_of') or ob.get('project_damage_cellf_of'):
            continue
        ev = ob.evaluated_get(deps)
        strength = max(0.0, float(ob.get('project_damage_cellf_strength', 1.0)))
        if ob.type == 'EMPTY':
            radius = 0.0
            if props.cellf_prox_use_size:
                world_r = ob.empty_display_size * max(abs(v) for v in ev.matrix_world.to_scale())
                radius = world_r / src_scale * factor
            add(src_inv @ ev.matrix_world.translation, radius, strength)
            continue
        if ob.type not in {'MESH', 'CURVE', 'SURFACE', 'META', 'FONT'}:
            add(src_inv @ ev.matrix_world.translation, 0.0, strength)
            continue
        mesh = ev.to_mesh()
        try:
            if mesh is None or not len(mesh.vertices):
                add(src_inv @ ev.matrix_world.translation, 0.0, strength)
                continue
            co = np.empty(len(mesh.vertices) * 3, dtype=np.float64)
            mesh.vertices.foreach_get('co', co)
            co = co.reshape(-1, 3)
            if len(co) > 64:
                co = co[np.linspace(0, len(co) - 1, 64).astype(np.int64)]
            matrix = np.array(src_inv @ ev.matrix_world)
            for local in co @ matrix[:3, :3].T + matrix[:3, 3]:
                add(local, 0.0, strength)
        finally:
            ev.to_mesh_clear()
    return result


def fill_breakers(source, props, core, factor):
    if not (props.ui_boolean_enable or (props.hang_enable and props.hang_mode=='1')):return []
    """Linked breakers -> BreakerSpecs, mirroring the adapter's resolution
    loop (GetVirtualObjects, project_damage_plugin.cpp): disabled rows are skipped THERE, not at the
    list (the C4D OM-tick semantics), mode is DERIVED, and Breaker Scale folds
    into strength - the one fold, nothing else is scaled."""
    from . import breaker as breaker_mod
    specs = []
                                                                            
                                                                      
                                                                         
                                                                          
                                                                    
    deps = bpy.context.evaluated_depsgraph_get()
    src_inv = source.evaluated_get(deps).matrix_world.inverted()
                                                                             
    bscale = _clamp_scale(props.break_scale / 100.0) * _clamp_scale(props.global_scale / 100.0)
    for row in props.breakers:
        br = row.obj
        if br is None or br == source or not br.project_damage_breaker.enabled or (br.type == "EMPTY" and not br.project_damage_breaker.is_breaker):
            continue
        bp = br.project_damage_breaker
        s = core.BreakerSpec()
        s.mode = breaker_mod.effective_mode(br)
        s.flip = bp.flip
        s.custom_sheet=int(bp.custom_sheet)
        s.rebar=bp.rebar
        s.seed = bp.seed
                                                                        
                                                                          
                                                      
        s.length = bp.length
        s.width = bp.width
        s.radius = bp.radius
        s.thickness = bp.thickness
        s.resolution = bp.resolution / 100.0 * _clamp_scale(preview_quality.percent(props) / 100.0) * max(.05,props.boolean_quality/100.)
        s.strength = (bp.strength / 100.0) * bscale
        s.macro_h = bp.macro_h / 100.0
        s.turb_h = bp.turb_h / 100.0
        s.fine_h = bp.fine_h / 100.0
        s.noise_scale = bp.noise_scale / 100.0
        s.rim_amp = bp.rim_amp / 100.0
        s.rim_scale = bp.rim_scale / 100.0
        s.surf_mode=int(bp.surf_mode)
        s.tex_path=breaker_texture_path(bp)
        for name in ('tex_scale','tex_strength','tex_edge','tex_exit','tex_macro','tex_offset_x','tex_offset_y','tex_offset_z'):
            setattr(s,name,getattr(bp,name))
        s.smooth_slope = bool(bp.smooth_slope)
        s.smooth_width = float(bp.smooth_width)
                                                                               
                                                                              
        if props.ui_boolean_enable:
            s.surf_mode = 0
            s.smooth_slope = False
        from . import break_surface
        break_surface.fill_spec(s, bp)
        rel = src_inv @ br.evaluated_get(deps).matrix_world
                                                                        
                                                                                                   
                                                                           
        s.rel = ([rel.translation[i] * factor for i in range(3)]
                 + [rel.col[c][r] for c in range(3) for r in range(3)])
        if s.mode == 3:
            shape = bake_custom(br, core, factor, deps)
            if shape is not None:
                s.shape = shape
                s.custom_hash = shape.hash
        specs.append(s)
    return specs


def fill_deform(props, core, obj, p):
    """Impact Deform inputs in object space (cm): see impact_deform_tools.fill."""
    from . import impact_deform_tools
    impact_deform_tools.fill(props, core, obj, p, unit_factor(bpy.context))


def surface_texture_path(props, module):
    """Concrete spall / lath back-surface texture: the picked file, or the bundled gravel."""
    from pathlib import Path
    picked = getattr(props, module + '_tex_path', '')
    if picked:
        return bpy.path.abspath(picked, library=props.id_data.library)
    return str(Path(__file__).parent/'data/gravel_displacement/Concrete025_2K-JPG_Displacement.jpg')


def concrete_texture_path(props):
    return surface_texture_path(props, 'concrete')


def breaker_texture_path(bp):
    from pathlib import Path
    if bp.tex_path:
        return bpy.path.abspath(bp.tex_path,library=bp.id_data.library)
    return str(Path(__file__).parent/'data/gravel_displacement/Concrete025_2K-JPG_Displacement.jpg')


def bake_custom(br, core, factor, deps):
    """The breaker's children -> one CustomShape in breaker-local core
    centimeters, mirroring BakeFromClone: quads split (a,b,c)+(a,c,d) like
    CToManifold, size = largest bbox extent, the same quantised FNV-1a.
    Ngons go through Blender's loop triangulation - C4D pre-tessellates ngons
    with its own kernel there, so no cross-host parity is claimed for ngon
    children (tri/quad children are exact). Returns None on an empty bake -
    the same graceful no-shape path the adapter takes.

    `deps` is the caller's own evaluated depsgraph (fill_breakers already
    fetched one for `rel`) - same staleness risk as there, no need for a
    second fetch."""
                                                                         
                                                          
    inv = br.evaluated_get(deps).matrix_world.inverted()
    pts, tris = [], []
    stack = list(br.children)
    if br.project_damage_breaker.cutter is not None:stack.insert(0,br.project_damage_breaker.cutter)
    if br.type != 'EMPTY':stack.insert(0,br)
    visited=set()
    while stack:
        child = stack.pop(0)
        if child in visited:continue
        visited.add(child)
        if child != br:stack.extend(child.children)
        ev = child.evaluated_get(deps)
        try:
            me = ev.to_mesh()
        except RuntimeError:
            continue
        if me is None or len(me.polygons) == 0:
            ev.to_mesh_clear()
            continue
        local = inv @ ev.matrix_world
        base = len(pts)
        for v in me.vertices:
            p = local @ v.co
            pts.append((p.x * factor, p.y * factor, p.z * factor))
        for poly in me.polygons:
            vs = [me.loops[li].vertex_index + base for li in poly.loop_indices]
            if len(vs) == 3:
                tris.extend(vs)
            elif len(vs) == 4:
                tris.extend((vs[0], vs[1], vs[2], vs[0], vs[2], vs[3]))
            else:
                me.calc_loop_triangles()
                for lt in me.loop_triangles:
                    if lt.polygon_index == poly.index:
                        tris.extend(v + base for v in lt.vertices)
        ev.to_mesh_clear()
    if not tris:
        return None
    arr = np.array(pts, dtype=np.float64)
    size = float((arr.max(axis=0) - arr.min(axis=0)).max())
    if size <= 0.0:
        return None
    mask = 0xFFFFFFFFFFFFFFFF
    h = 1469598103934665603
    for p in pts:
        for c in p:
            h = ((h ^ (int(c * 1024.0) & mask)) * 1099511628211) & mask
    for t in tris:
        h = ((h ^ t) * 1099511628211) & mask
    cs = core.CustomShape()
    cs.pts = arr
    cs.tris = tris
    cs.size = size
    cs.hash = h
    return cs


def glass_straight_through(props):
    """Hit Glass, Glass Drawn and Broken Glass Through cut straight through the mesh."""
    return bool(getattr(props, 'glass_through_enable', False) or getattr(props, 'glass_hit_enable', False)
                or getattr(props, 'glass_drawn_enable', False))
