# общий вид разделов и правила показа зависимых настроек
# SPDX-License-Identifier: GPL-3.0-or-later
"""Shared effect UI conventions and presentation-only dependency rules.

Edit this file for common styling; individual field order stays in panels.py
and the explicit Panel.draw methods in panels.py. No parameter values are changed here.
"""

SELECTOR_HEIGHT = 1.5


def heading_icon(title):
    title = title.lower()
    for words, icon in (
        (('material',), 'MATERIAL'), (('noise', 'noisify', 'rough'), 'MOD_NOISE'),
        (('paint', 'draw', 'stroke', 'guide'), 'GREASEPENCIL'),
        (('crack', 'fracture', 'split'), 'MOD_EXPLODE'),
        (('shape', 'rim', 'edge', 'corner'), 'MESH_DATA'),
        (('placement', 'proximity', 'layout'), 'PIVOT_CURSOR'),
        (('resolution', 'topology', 'quality', 'smooth'), 'MOD_SUBSURF'),
        (('layer', 'coat', 'sheet'), 'MOD_SOLIDIFY'),
        (('thread', 'rebar', 'reinforce'), 'MOD_WIREFRAME'),
    ):
        if any(word in title for word in words):
            return icon
    return 'SETTINGS'


def heading(layout, title, icon=None):
    row = layout.row(align=True)
    row.use_property_split = False
    row.alignment = 'CENTER'
    row.label(text=title, icon=icon or heading_icon(title))
    from . import editorial
    editorial.button(row,title)


def section(layout, title):
    box = layout.box()
    box.use_property_split = False
    box.use_property_decorate = False
    heading(box, title)
    return box


def standalone_update(layout, settings, operator, *, action=None, label='Manual Update'):
    row = layout.row(align=True)
    row.prop(settings, 'live_update', text='Live Update', toggle=True, icon='AUTO')
    button = row.operator(operator, text=label, icon='FILE_REFRESH')
    if action is not None:
        button.action = action


# скрываем настройки, которые сейчас не влияют на выбранный режим
def visible(owner, name):
    if 'avoid' in name.lower():return False
    """Hide dependent inputs, never their own zero/off controller."""
    if name.endswith(('_tex_edge','_edge_length','_fine_edge','_mesh_edge_size')) and not name.startswith('pits_'):return False
    for module in ('concrete','brick','lath','rusty','ground','wallpaper','surf'):
        if name in {module+'_'+suffix for suffix in ('size_min','size_max','lobes','stretch')} and getattr(owner,module+'_mode','0') in {'1','2','PAINT'}:return False
    for module in ('concrete','wood','lath','bullet','ground','brick','rusty'):
        if name.startswith(module+'_paint_') and getattr(owner,module+'_mode','0')=='2':return False
    for module in ('concrete','lath','brick'):
        if getattr(owner,module+'_mode','0')=='2':
            if name.startswith(module+'_crack_') or name.startswith(module+'_hole_') or name in {module+'_depth_min',module+'_depth_max',module+'_thickness_cap'}:return False
    if not hasattr(owner, name):
        return True
    if name.startswith('fract_l') and name.endswith('_branch_length'):
        return getattr(owner,name.replace('_branch_length','_branches'),0)>0
    if getattr(owner, 'fabric_solid_enable', False) and (name == 'fabric_layer_depth' or name.startswith('fabric_edge_')):
        return False
    if name.startswith('fabric_'):
        solid = getattr(owner, 'fabric_solid_enable', False)
        if getattr(owner, 'fabric_paint_only', False):
            if name == 'fabric_hole_dir_mode':
                return not solid and owner.fabric_fibre_model == 'WOVEN'
            if name in {'fabric_hole_dir_x', 'fabric_hole_dir_y', 'fabric_hole_dir_z'}:
                return owner.fabric_threads_enable if solid else owner.fabric_fibre_model == 'WOVEN' and owner.fabric_hole_dir_mode == '1'
            if name.startswith(('fabric_hole_', 'fabric_edge_', 'fabric_tear_', 'fabric_distribution_', 'fabric_shape_noise_')) or name in {'fabric_holes_enable', 'fabric_edge_enable'}:
                return False
            if name == 'fabric_detail' and not solid:
                return False
            if name in {'fabric_paint_jagged', 'fabric_paint_rag_size'} and not (solid or owner.fabric_fibre_model == 'WOVEN'):
                return False
            if name.startswith('fabric_paint_edge_') and not (solid and owner.fabric_paint_edges_enable):
                return False
            if name == 'fabric_yarn_size' and owner.fabric_fibre_model != 'WOVEN':
                return False
            if name == 'fabric_hang_amount':
                return False
        if name in {'fabric_thickness', 'fabric_skin_depth', 'fabric_cut_margin', 'fabric_group'}:
            return False
        if name == 'fabric_fray_spacing' and not solid:
            return False
        if name.startswith('fabric_hang_') and not solid:
            return False
        if name == 'fabric_paint_hang_spacing':
            return solid and owner.fabric_threads_enable and owner.fabric_paint_only and owner.fabric_paint_edges_enable and owner.fabric_hang_enable
        if name in {'fabric_gravity_x', 'fabric_gravity_y', 'fabric_gravity_z', 'fabric_max_threads'}:
            return owner.fabric_threads_enable
    if getattr(owner, 'concrete_v2_enable', False) and name in {'concrete_terraces','concrete_terrace_skin','concrete_terrace_inset','concrete_wall_soft'}:
        return False
    if getattr(owner, 'lath_v2_enable', False) and name in {'lath_terraces','lath_terrace_skin','lath_terrace_inset','lath_wall_soft'}:
        return False
    if getattr(owner, 'lath_riven_enable', False) and name == 'lath_lath_length':
        return False
    if getattr(owner, 'lath_framing_enable', False):
        from .stud_framing import HIDDEN
        if name in HIDDEN:
            return False
    if getattr(owner, 'brick_v2_enable', False) and name in {'brick_terraces','brick_terrace_skin','brick_terrace_inset','brick_wall_soft','brick_sharp_edge'}:
        return False
    if (getattr(owner,'glass_hit_enable',False) or getattr(owner,'glass_drawn_enable',False)) and name in {'glass_impact_enable','glass_impact_amount','glass_paint_only'}:
        return False
                                                                     
    if name.endswith(('_material','_drawing')):
        return True
    # Native, hand-authored Surface / Edge / Corner / Deposits controls.
    gates = {
        'norm_max_size': ('normalize',),
        'sec_size': ('sec_amount',), 'sec_depth': ('sec_amount',),
        'surf_sec_size': ('surf_sec_amount',), 'surf_sec_depth': ('surf_sec_amount',),
        'corner_sec_size': ('corner_prob', 'corner_sec_amount'),
        'corner_sec_depth': ('corner_prob', 'corner_sec_amount'),
        'build_width': ('build_coverage',), 'build_height': ('build_coverage',),
        'build_irreg': ('build_coverage',), 'build_corner_size': ('build_corner_prob',),
        'build_blend': ('build_corner_prob',),
        'build_shape': ('build_coverage',), 'build_lumps': ('build_coverage',),
        'pile_height': ('build_corner_prob',), 'pile_lumps': ('build_corner_prob',),
        'fract_shard_collection': ('fract_shard_enable',), 'fract_shard_dir': ('fract_shard_enable',),
        'fract_shard_amount': ('fract_shard_enable',), 'fract_shard_use_size': ('fract_shard_enable',),
        'fract_shard_radius': ('fract_shard_enable',), 'fract_shard_falloff': ('fract_shard_enable',),
        'fract_shard_tilt': ('fract_shard_enable',), 'fract_shard_random': ('fract_shard_enable',),
        'fract_shard_seed': ('fract_shard_enable',),
        'break_rim_amount': ('break_rim_enable',),
        'break_rim_size': ('break_rim_enable', 'break_rim_amount'),
        'break_rim_depth': ('break_rim_enable', 'break_rim_amount'),
        'rebar_mortar_recess': ('rebar_mortar_joints',),
        'rebar_mortar_material': ('rebar_mortar_joints',),
        'deposit_shelter_radius': ('deposit_enable', 'deposit_amount', 'deposit_shelter'),
        'deposit_gravity_direction': ('deposit_enable', 'deposit_amount', 'deposit_gravity'),
    }
    drawn_bead = getattr(owner, 'build_mode', 'AUTO') == 'PAINT'                                    
    if any(not getattr(owner, gate, True) for gate in gates.get(name, ())
           if not (drawn_bead and gate == 'build_coverage')):
        return False
    if name in {'corner_rough', 'corner_tilt', 'corner_flakes'} and getattr(owner, 'corner_style', '1') != '1':
        return False
    if name == 'corner_paint_radius' and getattr(owner, 'corner_mode', 'AUTO') != 'PAINT':
        return False
    if name == 'build_paint_radius' and getattr(owner, 'build_mode', 'AUTO') != 'PAINT':
        return False
    if name == 'build_coverage' and getattr(owner, 'build_mode', 'AUTO') == 'PAINT':
        return False
    if name.startswith('deposit_') and name not in {'deposit_enable', 'deposit_style', 'deposit_amount', 'deposit_material'}:
        if not owner.deposit_enable or owner.deposit_amount <= 0:
            return False
    if name.startswith('fract_l1_'):
        from .fracture_assets import active_key
        if active_key(owner).startswith('fracture_proximity_') and name in {'fract_l1_width','fract_l1_depth','fract_l1_length','fract_l1_branches','fract_l1_branch_length'}:return True
    for level in (1, 2, 3):
        prefix = 'fract_l%d_' % level
        if name.startswith(prefix):
            field = name[len(prefix):]
            if field not in {'amount', 'chips'} and getattr(owner, prefix+'amount') <= 0:
                return False
            if field in {'chip_amount', 'chip_size', 'chip_depth'} and not getattr(owner, prefix+'chips'):
                return False
                                                                             
    if name in ("fabric_hole_amount", "fabric_hole_length_min", "fabric_hole_length_max", "fabric_hole_width_min", "fabric_hole_width_max", "fabric_hole_jagged", "fabric_hole_lobes", "fabric_hole_meander", "fabric_hole_split_max", "fabric_hole_bridge", "fabric_hole_dir_mode", "fabric_hole_dir_x", "fabric_hole_dir_y", "fabric_hole_dir_z", "fabric_hole_dir_jitter",) and not owner.fabric_holes_enable:
        return False
    if name in ("fabric_edge_amount", "fabric_edge_spacing_jitter", "fabric_edge_length_min", "fabric_edge_length_max", "fabric_edge_width_min", "fabric_edge_width_max", "fabric_edge_taper", "fabric_edge_jagged", "fabric_edge_meander", "fabric_edge_angle_jitter",) and not owner.fabric_edge_enable:
        return False
    if name in ("fabric_thread_radius", "fabric_thread_sides", "fabric_thread_spacing", "fabric_bridge_enable", "fabric_bridge_share", "fabric_thread_sag", "fabric_thread_skew", "fabric_thread_wobble", "fabric_broken_enable", "fabric_broken_share", "fabric_thread_droop", "fabric_thread_curl", "fabric_thread_taper", "fabric_fray_enable", "fabric_fray_spacing", "fabric_fray_length_min", "fabric_fray_length_max", "fabric_hang_enable", "fabric_hang_amount", "fabric_hang_length_min", "fabric_hang_length_max",) and not owner.fabric_threads_enable:
        return False
    if name in ("fabric_bridge_share",) and not owner.fabric_bridge_enable:
        return False
    if name in ("fabric_broken_share",) and not owner.fabric_broken_enable:
        return False
    if name in ("fabric_fray_spacing", "fabric_fray_length_min", "fabric_fray_length_max",) and not owner.fabric_fray_enable:
        return False
    if name in ("fabric_hang_amount", "fabric_hang_length_min", "fabric_hang_length_max",) and not owner.fabric_hang_enable:
        return False
    if name in ("fabric_paint_jagged", "fabric_paint_rag_size") and not (owner.fabric_paint_holes_enable or owner.fabric_paint_edges_enable):
        return False
    if name in ("concrete_size_min", "concrete_size_max", "concrete_lobes", "concrete_stretch", "concrete_edge_bias",) and not owner.concrete_amount:
        return False
    if name in ("concrete_hole_chance", "concrete_hole_size", "concrete_hole_jagged",) and not owner.concrete_hole_enable:
        return False
    if name in ("concrete_crack_amount", "concrete_crack_length_min", "concrete_crack_length_max", "concrete_crack_width", "concrete_crack_depth", "concrete_crack_meander", "concrete_crack_branch", "concrete_crack_interior",) and not owner.concrete_crack_enable:
        return False
    if name in ("concrete_rebar_amount", "concrete_rebar_density", "concrete_rebar_radius", "concrete_rebar_scale", "concrete_rebar_spacing_x", "concrete_rebar_spacing_y", "concrete_rebar_bars_x", "concrete_rebar_bars_y", "concrete_rebar_outer", "concrete_rebar_angle", "concrete_rebar_cover", "concrete_rebar_expose", "concrete_rebar_embed", "concrete_rebar_ribs", "concrete_rebar_sides", "concrete_rebar_jitter", "concrete_rebar_wobble", "concrete_rebar_bend", "concrete_rebar_broken", "concrete_up_x", "concrete_up_y", "concrete_up_z",) and not owner.concrete_rebar_enable:
        return False
    if name in ("hit_hole_size",) and not owner.hit_hole_enable:
        return False
    if name in ("hit_crack_width", "hit_crack_depth", "hit_radial_count", "hit_radial_length_min", "hit_radial_length_max", "hit_radial_meander", "hit_radial_branch", "hit_ring_count", "hit_ring_fill",) and not owner.hit_crack_enable:
        return False
    if name in ("plaster_crack_amount", "plaster_crack_scale", "plaster_crack_width", "plaster_crack_depth", "plaster_crack_meander", "plaster_crack_breakup", "plaster_crack_step", "plaster_crack_lip", "plaster_up_x", "plaster_up_y", "plaster_up_z",) and not owner.plaster_crack_enable:
        return False
    if name in ("wood_size_min", "wood_size_max", "wood_lobes", "wood_stretch", "wood_end_bias", "wood_edge_bias",) and not owner.wood_amount:
        return False
    if name in ("wood_groove_spacing", "wood_groove_depth", "wood_groove_wave", "wood_groove_breakup", "wood_weather_halo",) and not owner.wood_groove_enable:
        return False
    if name in ("wood_break_chance", "wood_break_size",) and not owner.wood_break_enable:
        return False
    if name in ("wood_check_amount", "wood_check_global", "wood_check_length_min", "wood_check_length_max", "wood_check_width", "wood_check_depth", "wood_check_meander",) and not owner.wood_check_enable:
        return False
    if name in ("wood_knot_size", "wood_knot_stretch", "wood_knot_depth", "wood_knot_checks",) and not owner.wood_knot_amount:
        return False
    if name in ("wood_sliver_amount", "wood_sliver_length", "wood_sliver_width", "wood_sliver_thick", "wood_sliver_lift", "wood_sliver_embed",) and not owner.wood_sliver_enable:
        return False
    if name in ("lath_size_min", "lath_size_max", "lath_lobes", "lath_stretch", "lath_edge_bias",) and not owner.lath_amount:
        return False
    if name in ("lath_hole_chance", "lath_hole_size", "lath_hole_jagged", "lath_hole_laths",) and not owner.lath_hole_enable:
        return False
    if name in ("lath_crack_amount", "lath_crack_length_min", "lath_crack_length_max", "lath_crack_width", "lath_crack_depth", "lath_crack_meander", "lath_crack_branch", "lath_crack_interior",) and not owner.lath_crack_enable:
        return False
    if name in ("lath_lath_amount", "lath_lath_width", "lath_lath_thickness", "lath_lath_gap", "lath_lath_scale", "lath_lath_angle", "lath_lath_length", "lath_lath_embed", "lath_lath_jitter", "lath_lath_tilt", "lath_lath_warp", "lath_lath_edge", "lath_lath_missing", "lath_lath_broken", "lath_lath_bend", "lath_lath_splinter", "lath_up_x", "lath_up_y", "lath_up_z",) and not owner.lath_lath_enable:
        return False
    if name in ("wallpaper_seam_width", "wallpaper_seam_depth", "wallpaper_seam_open",) and not owner.wallpaper_seams_enable:
        return False
    if name in ("wallpaper_tear_amount", "wallpaper_tear_length_min", "wallpaper_tear_length_max", "wallpaper_tear_width_min", "wallpaper_tear_width_max", "wallpaper_tear_diagonal", "wallpaper_tear_from_top", "wallpaper_tear_side", "wallpaper_tear_jagged", "wallpaper_seam_crack", "wallpaper_seam_crack_length",) and not owner.wallpaper_tears_enable:
        return False
    if name in ("wallpaper_flap_amount", "wallpaper_flap_fold", "wallpaper_flap_fold_radius", "wallpaper_flap_curl", "wallpaper_flap_curl_focus", "wallpaper_flap_cone", "wallpaper_flap_wave", "wallpaper_flap_ragged", "wallpaper_flap_group", "wallpaper_flap_wall_falloff", "wallpaper_flap_height_influence", "wallpaper_flap_smooth",) and not owner.wallpaper_flaps_enable:
        return False
    if name in ("wallpaper_lift_amount", "wallpaper_lift_length_min", "wallpaper_lift_length_max", "wallpaper_lift_width_min", "wallpaper_lift_width_max", "wallpaper_lift_curl",) and not owner.wallpaper_lifts_enable:
        return False
    if name in ("wallpaper_paint_reach", "wallpaper_paint_jagged", "wallpaper_seam_snap",) and not owner.wallpaper_paint_enable:
        return False
    if name in ("bullet_clusters", "bullet_cluster_share", "bullet_cluster_spread", "bullet_min_spacing",) and not owner.bullet_amount:
        return False
    if name in ("bullet_halo_scale", "bullet_halo_depth", "bullet_halo_jagged",) and not owner.bullet_halo_enable:
        return False
    if name in ("bullet_pit_size", "bullet_pit_spread",) and not owner.bullet_pit_amount:
        return False
    if name in ("bullet_conc_spall_scale", "bullet_conc_spall_depth", "bullet_conc_skin_depth", "bullet_conc_bore_depth", "bullet_conc_jagged", "bullet_conc_roughness", "bullet_conc_aggregate", "bullet_conc_exit_scale", "bullet_conc_cracks", "bullet_conc_crack_length", "bullet_crack_width", "bullet_crack_depth",) and not owner.bullet_conc_spall_chance:
        return False
    if name in ("blast_crack_count", "blast_crack_length", "blast_crack_width", "blast_crack_depth",) and not owner.blast_crack_enable:
        return False
    if name in ("ground_size_min", "ground_size_max", "ground_area_chance", "ground_area_size", "ground_area_stretch",) and not owner.ground_amount:
        return False
    if name in ("ground_crack_amount", "ground_crack_generations", "ground_crack_length_min", "ground_crack_length_max", "ground_crack_width", "ground_crack_width_var", "ground_crack_taper", "ground_crack_depth", "ground_crack_meander", "ground_crack_kink", "ground_crack_branch", "ground_crack_join", "ground_crack_chip",) and not owner.ground_crack_enable:
        return False
    if name in ("ground_pothole_size", "ground_pothole_loose", "ground_pothole_depth", "ground_roughness", "ground_aggregate_size", "ground_thickness_cap",) and not owner.ground_pothole_chance:
        return False
    if name in ("ground_rubble_amount", "ground_rubble_size_min", "ground_rubble_size_max",) and not owner.ground_rubble_enable:
        return False
    if name in ("ground_heave_height", "ground_heave_width", "ground_heave_gap",) and not owner.ground_heave_chance:
        return False
    if name in ("brick_size_min", "brick_size_max", "brick_lobes", "brick_stretch", "brick_edge_bias",) and not owner.brick_amount:
        return False
    if name in ("brick_hole_chance", "brick_hole_size",) and not owner.brick_hole_enable:
        return False
    if name in ("brick_crack_amount", "brick_crack_length_min", "brick_crack_length_max", "brick_crack_width", "brick_crack_depth", "brick_crack_meander", "brick_crack_branch", "brick_crack_interior", "brick_up_x", "brick_up_y", "brick_up_z",) and not owner.brick_crack_enable:
        return False
    if name in ("glass_impact_amount", "glass_hole_chance", "glass_hole_size_min", "glass_hole_size_max", "glass_hole_lobes", "glass_impact_spacing",) and not owner.glass_impact_enable:
        return False
    if name in ("glass_crack_width", "glass_crack_depth", "glass_crack_both_sides", "glass_crack_reach", "glass_crack_reach_jitter", "glass_crack_meander", "glass_crack_branch", "glass_crack_taper", "glass_crush_size", "glass_ring_count", "glass_ring_first", "glass_ring_growth", "glass_ring_fill", "glass_ring_jitter", "glass_ring_bow",) and not owner.glass_crack_enable:
        return False
    if name in ("glass_chip_chance", "glass_chip_spacing", "glass_chip_size", "glass_chip_depth",) and not owner.glass_chip_enable:
        return False
    if name in ("glass_paint_impacts_enable", "glass_paint_only", "glass_paint_shard_width", "glass_paint_hole_cracks", "glass_paint_point_size", "glass_paint_impact_size", "glass_paint_point_hole", "glass_max_cracks",) and not owner.glass_paint_holes_enable:
        return False
    if name in ("fissure_slab_rows", "fissure_slab_length_min", "fissure_slab_length_max", "fissure_slab_width_min", "fissure_slab_width_max", "fissure_slab_row_falloff", "fissure_slab_thick_min", "fissure_slab_thick_max", "fissure_tilt_min", "fissure_tilt_max", "fissure_sink_chance", "fissure_roll", "fissure_flat_chance", "fissure_side_bias", "fissure_slab_gap", "fissure_slab_jagged", "fissure_slab_skew",) and not owner.fissure_slab_enable:
        return False
    if name in ("fissure_branch_density", "fissure_branch_length_min", "fissure_branch_length_max", "fissure_branch_width", "fissure_branch_depth", "fissure_branch_meander", "fissure_branch_angle", "fissure_branch_fork",) and not owner.fissure_branch_enable:
        return False
    if name in ("fissure_rubble_density", "fissure_rubble_size_min", "fissure_rubble_size_max", "fissure_rubble_wedged",) and not owner.fissure_rubble_enable:
        return False
    if name in ("bevel_wear_uneven", "bevel_wear_uneven_scale", "bevel_wear_rough", "bevel_wear_chips", "bevel_wear_chip_min", "bevel_wear_chip_max", "bevel_wear_chip_depth", "bevel_wear_chip_stretch", "bevel_wear_big", "bevel_wear_mask_width", "bevel_wear_mask_noise", "bevel_wear_mask_scale", "bevel_up_x", "bevel_up_y", "bevel_up_z",) and not owner.bevel_wear_enable:
        return False
    if name in ("scratch_bundle_min", "scratch_bundle_max", "scratch_bundle_gap", "scratch_bundle_thin",) and not owner.scratch_bundle_chance:
        return False
    if name in ("scratch_scuff_width", "scratch_scuff_depth",) and not owner.scratch_scuff_enable:
        return False
    module = name.split('_', 1)[0]
    field = name[len(module)+1:]
    if module == 'cellf':
        material = int(owner.cellf_material)
        if (field.startswith('grain_') or field == 'splinter') and material != 0:return False
        if field.startswith('glass_') and material != 1:return False
        if field == 'planar_ratio' and owner.cellf_planar != '0':return False
        if field == 'remainder_reach' and not owner.cellf_keep_remainder:return False
    if module == 'rbf':
        material = int(owner.rbf_material)
        if field.startswith('grain_') and material not in {1, 2}:return False
        if field == 'grain_stretch' and material != 1:return False
        if field == 'weave_stretch' and material != 2:return False
        if field.startswith('glass_') and material != 3:return False
    if module == 'scratch':
        if owner.scratch_mode == '1' and field in {'density','max_scratches','spread','falloff','angle_jitter','cross_share','along'}:return False
        if owner.scratch_style != '3' and field not in {'seed','style','avoid','detail','density','max_scratches','spread','size_scale','paint_snap','wrap_angle','mode'}:return False
    if module == 'metal':
        shared={'enable','seed','avoid','style','edge_length','detail','max_tris','sheet_thickness','roughness'}
        crush={'crush_aim','crush_proximity','crush_depth','crush_length','crush_hinge','crush_skew','fold_size','fold','fold_sharp','fold_wander','fold_lift','crush_crumple','crush_facet'}
        if field not in shared and ((owner.metal_style == '1') != (field in crush)):return False
                                                                         
    for effect in ('concrete', 'wood', 'lath', 'bullet', 'ground', 'brick'):
        if name.startswith(effect+'_paint_') and getattr(owner,effect+'_mode') != '1':
            return False
        if name in {effect+'_amount', effect+'_edge_bias'} and getattr(owner,effect+'_mode') in {'1','2'}:
            return False
    if name.startswith('bullet_'):
        field=name[7:]
        material=int(owner.bullet_material)
        if field.startswith('metal_') and material!=0:return False
        if (field.startswith('wood_') or field.startswith('grain_')) and material!=1:return False
        if (field.startswith('conc_') or field in {'crack_width','crack_depth'}) and material!=2:return False
    return True


def group_fields(p, module, fields):
    """Mirror existing type filters before creating a box, avoiding empty boxes."""
    result = []
    for field in fields:
        if field == 'enable' or (field == 'mode' and module != 'scratch'):
            continue
        if module == 'cellf':
            material = int(p.cellf_material)
            if (field.startswith('grain_') or field == 'splinter') and material != 0: continue
            if field.startswith('glass_') and material != 1: continue
        if module == 'rbf':
            material = int(p.rbf_material)
            if field.startswith('grain_') and material not in {1, 2}: continue
            if field == 'grain_stretch' and material != 1: continue
            if field == 'weave_stretch' and material != 2: continue
            if field.startswith('glass_') and material != 3: continue
        if module == 'scratch':
            if p.scratch_mode == '1' and field in {'density','max_scratches','spread','falloff','angle_jitter','cross_share','along'}: continue
            if p.scratch_style != '3' and field not in {'seed','style','avoid','detail','density','max_scratches','spread','size_scale','paint_snap','wrap_angle','mode'}: continue
        if module == 'metal':
            shared={'enable','seed','avoid','style','edge_length','detail','max_tris','sheet_thickness','roughness'}
            crush={'crush_aim','crush_proximity','crush_depth','crush_length','crush_hinge','crush_skew','fold_size','fold','fold_sharp','fold_wander','fold_lift','crush_crumple','crush_facet'}
            if field not in shared and ((p.metal_style == '1') != (field in crush)): continue
        if visible(p, module+'_'+field):
            result.append(field)
    return tuple(result)
