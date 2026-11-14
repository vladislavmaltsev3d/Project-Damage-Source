# каталог встроенных наборов настроек
# SPDX-License-Identifier: GPL-3.0-or-later
"""Built-in preset library: ready looks grouped by category, on top of the
three generated core presets (Mild/Strong/Heavy in presets_data).

Each entry names the effects it turns on (sidebar effect keys, enabled the
same way the Effects Lib does, so variant switches still run), the root
project_damage values it sets, and values for the standalone generators that
keep their own settings group (weld, grass, gravel). Everything else on the
object is left as it is. Applying a library preset first turns off the
effects that are currently on, so the result is that look and nothing else.

The category of a preset is the previews/presets/<category> folder that holds
its <key>.png; CATEGORY_TITLES gives that folder its name in the dropdown.
"""

from .riven_lath import LOOK as RIVEN_LOOK
from .stud_framing import LOOK as FRAMING_LOOK

CATEGORY_TITLES = {
    'general': 'General',
    'edges_corners': 'Edges & Corners',
    'surface_erosion': 'Surface & Erosion',
    'cracks_fracture': 'Cracks & Fracture',
    'walls_interiors': 'Walls & Interiors',
    'stone_masonry': 'Stone & Masonry',
    'metal_industrial': 'Metal & Industrial',
    'wood_timber': 'Wood & Timber',
    'fabric_cloth': 'Fabric & Cloth',
    'ground_nature': 'Ground & Nature',
    'combined': 'Combined Looks',
    'ivy_roots': 'Ivy & Roots',
    'bb_walls': 'Boolean: Walls',
    'bb_slabs': 'Boolean: Floors & Slabs',
    'bb_columns': 'Boolean: Columns & Beams',
    'bb_statues': 'Boolean: Statues & Sculptures',
    'bb_blocks': 'Boolean: Blocks & Props',
    'bb_combined': 'Boolean: Combined',
    'asset_concrete': 'Assets: Concrete',
    'asset_hang': 'Assets: Hanging Holes',
    'asset_brick': 'Assets: Brick',
    'asset_lath': 'Assets: Lath & Framing',
    'asset_combined': 'Assets: Combined',
    'fracture_damage': 'Fracture & Damage',
    'peeling': 'Peeling',
}

_CORE = ('chips_enable', 'corner_enable', 'surf_enable', 'build_enable', 'fract_enable')


def _preset(category, description, effects, props=None, settings=None, seeds=None, breakers=None):
                                                                               
                                                                               
    values = dict(props or {})
    if 'brick' in effects or 'brick_v2' in effects:
        values['brick_remesh'] = False
    return dict(category=category, description=description, effects=tuple(effects),
                props=values, settings={k: dict(v) for k, v in (settings or {}).items()},
                seeds={k: tuple(v) for k, v in (seeds or {}).items()},
                                                                                            
                breakers=breakers)


# Variant first-use defaults (variant_defaults.switch_*, riven_lath.LOOK,
# stud_framing.LOOK) run only when the variant changes, and apply() unsets every
# value another look sets. A look on a variant therefore carries those defaults
# itself, so it reads the same from any previous state.
_BRICK_CLASSIC = dict(brick_style='0', brick_brick_mode='1', brick_surf_mode='0', brick_behind='1',
                      brick_damage_depth=0.0, brick_damage_noise=0.0, brick_damage_scale=0.0, brick_detail=0.5,
                      brick_noise_strength=2.0, brick_noise_scale=10.0, brick_noise_detail=4.0,
                      brick_remesh=False, brick_back_resolution=0.5)
_LATH_V2 = dict(lath_keys=0.0, lath_key_size=5.0)


def _brick_classic(description, **props):
    return _preset('asset_brick', description, ('brick',), {**_BRICK_CLASSIC, **props})


def _lath_v2(description, **props):
    return _preset('asset_lath', description, ('lath_v2',), {**_LATH_V2, **props})


def _lath_riven(description, **props):
    return _preset('asset_lath', description, ('lath_riven',), {**RIVEN_LOOK, **props})


def _lath_framing(description, **props):
    return _preset('asset_lath', description, ('lath_framing',), {**FRAMING_LOOK, **props})


LIBRARY = {
                                                                            
    'WORN_FURNITURE': _preset(
        'edges_corners', 'Fine, shallow nicks along every edge, like handled furniture or trim',
        ('edge',),
        dict(chips_mode='AUTO', amount=320, size_min=1.0, size_max=3.0, depth_min=0.3, depth_max=1.2,
             sec_amount=1, random_offset=60.0, noise_strength=0.0)),
    'CHIPPED_STEPS': _preset(
        'edges_corners', 'Stone steps: chipped edges and broken corners from years of traffic',
        ('edge', 'corner'),
        dict(chips_mode='AUTO', amount=160, size_min=3.0, size_max=10.0, depth_min=1.0, depth_max=4.0,
             sec_amount=2, corner_mode='AUTO', corner_prob=100.0, corner_size_min=12.0, corner_size_max=20.0,
             corner_depth_min=6.0, corner_depth_max=10.0)),
    'BROKEN_CORNERS': _preset(
        'edges_corners', 'Large chunks knocked off the corners only; edges stay clean',
        ('corner',),
        dict(corner_mode='AUTO', corner_prob=100.0, corner_size_min=8.0, corner_size_max=12.0,
             corner_depth_min=4.0, corner_depth_max=7.0, corner_sec_amount=4)),
    'ANCIENT_RUIN': _preset(
        'edges_corners', 'Heavy, irregular wear on edges and corners with a pitted surface',
        ('edge', 'corner', 'surface'),
        dict(chips_mode='AUTO', amount=220, size_min=3.0, size_max=10.0, depth_min=1.5, depth_max=4.0,
             sec_amount=3, noise_strength=0.0, corner_mode='AUTO', corner_prob=100.0,
             corner_size_min=12.0, corner_size_max=20.0, corner_depth_min=6.0, corner_depth_max=10.0,
             surf_type='0', surf_mode='PROCEDURAL', surf_amount=60, surf_sec_amount=3)),
    'WEATHERED_BEVEL': _preset(
        'edges_corners', 'Soft, rounded erosion on a wide worn chamfer (Damaged Bevel, Weathered)',
        ('dbevel',), dict(dbevel_preset='WEATHERED')),

                                                                            
    'POCKMARKED_STONE': _preset(
        'surface_erosion', 'Many small craters and dents spread over the faces',
        ('surface',),
        dict(surf_type='0', surf_mode='PROCEDURAL', surf_amount=160, surf_size_min=1.0, surf_size_max=3.0,
             surf_depth_min=0.3, surf_depth_max=1.5, surf_sec_amount=2)),
    'GOUGED_WOOD': _preset(
        'surface_erosion', 'Knife gouges and dents that follow the wood grain',
        ('surface',),
        dict(surf_type='1', surf_mode='PROCEDURAL', surf_amount=40,
             surf_grain_x=1.0, surf_grain_y=0.0, surf_grain_z=0.0)),
    'SANDSTONE_EROSION': _preset(
        'surface_erosion', 'Rocky voxel erosion that eats into the whole shape',
        ('erosion',),
        dict(erosion_damage=30.0, erosion_noise_type='ROCKY', erosion_noise_scale=3.0,
             erosion_noise_contrast=0.0)),
    'WORN_LIMESTONE': _preset(
        'surface_erosion', 'Soft, rounded weathering that wears the whole block down',
        ('erosion',),
        dict(erosion_damage=60.0, erosion_noise_type='CRACKED', erosion_noise_scale=0.5,
             erosion_noise_contrast=60.0)),
    'ROUGH_PLASTER': _preset(
        'surface_erosion', 'Hand-applied plaster strokes with ridges and fine shrinkage cracks',
        ('plaster',), dict(plaster_crack_enable=True)),

                                                                            
    'HAIRLINE_CRACKS': _preset(
        'cracks_fracture', 'A light network of thin cracks running from the edges',
        ('fracture',),
        dict(fract_l1_amount=4, fract_l1_width=0.3, fract_l2_amount=30, fract_l2_width=0.15, fract_l3_amount=0)),
    'CRACKED_CONCRETE': _preset(
        'cracks_fracture', 'Concrete fracture: main cracks, branches and a fine tiled third level',
        ('fracture_concrete',),
        dict(fract_l1_amount=8, fract_l2_amount=25, fract_l3_amount=80, fract_l3_bias=85.0, fract_tile_offset=40.0)),
    'SPLIT_TIMBER': _preset(
        'cracks_fracture', 'Long splits along the grain of a wooden beam',
        ('fracture_wood',),
        dict(fract_l1_amount=15, fract_l2_amount=40, fract_grain_x=1.0, fract_grain_y=0.0, fract_grain_z=0.0)),
    'CRACKED_GLASS': _preset(
        'cracks_fracture', 'Clean glass cracks without chipped rims',
        ('fracture_glass',),
        dict(fract_l1_amount=2, fract_l2_amount=19, fract_l1_chips=False, fract_l2_chips=False)),
    'RADIAL_IMPACT': _preset(
        'cracks_fracture', 'Cracks radiating from impact points with rings between them',
        ('fracture_radial',), dict(fract_l1_amount=3, fract_l2_amount=10)),

                                                                            
    'PEELING_WALLPAPER': _preset(
        'walls_interiors', 'Torn strips and lifted flaps of wallpaper over plaster',
        ('wallpaper',), dict(wallpaper_tear_amount=4, wallpaper_lift_amount=8)),
    'BULLET_RIDDLED_WALL': _preset(
        'walls_interiors', 'Clusters of bullet holes with spalled concrete around them',
        ('bullet',), dict(bullet_mode='0', bullet_material='2', bullet_amount=60, bullet_clusters=4)),
    'SEALANT_BEADS': _preset(
        'walls_interiors', 'Sealant or mortar beads along every inside corner',
        ('buildup',), dict(build_mode='AUTO', build_coverage=100.0, build_scale=250.0, build_detail=400.0)),
    'DUSTY_CORNERS': _preset(
        'walls_interiors', 'Dust piles in the inside corners and grit settled on the floors',
        ('piles', 'deposits'),
        dict(build_corner_prob=100.0, pile_scale=150.0, pile_lumps=100.0, deposit_amount=250)),
    'SETTLED_RUBBLE': _preset(
        'walls_interiors', 'Layered, porous rubble deposits on every upward face',
        ('deposits',), dict(deposit_amount=250, deposit_style='2', deposit_layers=2, deposit_porosity=60.0)),

                                                                            
    'RUSTED_METAL': _preset(
        'metal_industrial', 'Rust holes eaten through sheet metal, with pits and cracks around them',
        ('rusty',), dict(rusty_mode='0', rusty_amount=8, rusty_size_max=7.0)),
    'SHOT_METAL_PLATE': _preset(
        'metal_industrial', 'Bullet holes punched through a steel plate with petalled rims',
        ('bullet',), dict(bullet_mode='0', bullet_material='0', bullet_metal_dent_chance=0.0)),
    'WELDED_SEAMS': _preset(
        'metal_industrial', 'Weld beads along the inside corners of joined parts',
        ('weld',), settings={'weld': dict(edges_enable=True, edge_type='0', width=0.018, height=0.006)}),
    'SHREDDED_RAG': _preset(
        'fabric_cloth', 'Bias tears, ragged rims and torn edges all mixed (Shredded Rag)',
        ('fabric',), dict(fabric_tear_preset='5')),
    'RIPPED_CANVAS': _preset(
        'fabric_cloth', 'Long rips along the grain on thick fabric (Fabric Solidify)',
        ('fabric_solid',), dict(fabric_tear_preset='3')),

                                                                            
    'CRACKED_ASPHALT': _preset(
        'ground_nature', 'Crack networks and broken areas over a road or slab',
        ('ground',), dict(ground_mode='0', ground_crack_width=1.0, ground_crack_depth=1.0,
                          ground_pothole_chance=0.5, ground_heave_chance=0.4)),
    'POTHOLED_ROAD': _preset(
        'ground_nature', 'Every damaged area a pothole with loose rubble',
        ('ground',), dict(ground_mode='0', ground_pothole_chance=1.0, ground_rubble_size_max=4.0)),
    'HEAVED_PAVEMENT': _preset(
        'ground_nature', 'Slabs lifted and tilted along cracks, as if by roots or frost',
        ('ground',), dict(ground_mode='0', ground_crack_width=1.0, ground_crack_depth=1.0, ground_heave_chance=1.0)),
    'DRY_GRASS': _preset(
        'ground_nature', 'Dry grass in cavities and along inside edges',
        ('grass',), settings={'grass': dict(kind='DRY')}),
    'GRAVEL_SCATTER': _preset(
        'ground_nature', 'Loose gravel collected in grooves, pits and along kerbs',
        ('gravel',), settings={'gravel': dict(rock_subdivision=1)}),

                                                                          
    'WAR_TORN_CONCRETE': _preset(
        'combined', 'Bullet clusters, concrete cracks, chipped edges and broken corners together',
        ('edge', 'corner', 'fracture_concrete', 'bullet'),
        dict(chips_mode='AUTO', amount=180, size_min=3.0, size_max=10.0, depth_min=1.0, depth_max=4.0,
             sec_amount=2, corner_mode='AUTO', corner_prob=100.0, corner_size_min=10.0, corner_size_max=16.0,
             corner_depth_min=5.0, corner_depth_max=9.0,
             fract_l1_amount=6, fract_l2_amount=20, fract_l3_amount=40, fract_l3_bias=85.0, fract_tile_offset=40.0,
             bullet_mode='0', bullet_material='2', bullet_amount=45, bullet_clusters=3)),
    'OVERGROWN_YARD': _preset(
        'combined', 'Cracked, potholed ground with gravel in the cracks and dry grass on top',
        ('ground', 'gravel', 'grass'),
        dict(ground_mode='0', ground_amount=3, ground_size_min=40.0, ground_size_max=90.0,
             ground_crack_width=0.8, ground_crack_depth=0.8, ground_pothole_chance=0.6),
        settings={'grass': dict(kind='DRY'), 'gravel': dict(rock_subdivision=1)}),

                                                                      
    'LIGHT_HANDLING_WEAR': _preset(
        'edges_corners', 'A few tiny nicks far apart, like lightly handled objects',
        ('edge',), dict(chips_mode='AUTO', amount=60, size_min=1.0, size_max=2.5, depth_min=0.3, depth_max=0.8)),
    'KITCHEN_COUNTER': _preset(
        'edges_corners', 'A thin worn chamfer with small chips along the front edges',
        ('edge', 'dbevel'),
        dict(dbevel_preset='LIGHT', chips_mode='AUTO', amount=80, size_min=1.0, size_max=3.0, depth_min=0.3, depth_max=1.0)),
    'CLEAN_CHAMFER': _preset(
        'edges_corners', 'A wide, crisp chamfer on every edge with no damage (Damaged Bevel, Clean)',
        ('dbevel',), dict(dbevel_preset='CLEAN')),
    'NARROW_CHAMFER': _preset(
        'edges_corners', 'A thin clean chamfer on every edge (Damaged Bevel, Narrow)',
        ('dbevel',), dict(dbevel_preset='NARROW')),
    'HEAVY_BEVEL_DAMAGE': _preset(
        'edges_corners', 'Deep chunks broken out of three quarters of each edge (Damaged Bevel, Heavy)',
        ('dbevel',), dict(dbevel_preset='HEAVY')),
    'CHIPPED_BEVEL': _preset(
        'edges_corners', 'Many small chips and nicks along a thin chamfer (Damaged Bevel, Chipped)',
        ('dbevel',), dict(dbevel_preset='CHIPPED')),
    'CONSTRUCTION_KERB': _preset(
        'edges_corners', 'Big chips along the edges and broken-off corners, like a site kerb',
        ('edge', 'corner'),
        dict(chips_mode='AUTO', amount=200, size_min=4.0, size_max=12.0, depth_min=1.5, depth_max=5.0, sec_amount=2,
             corner_mode='AUTO', corner_style='1', corner_prob=100.0, corner_size_min=12.0, corner_size_max=20.0,
             corner_depth_min=6.0, corner_depth_max=10.0)),
    'MUSEUM_PEDESTAL': _preset(
        'edges_corners', 'A neat worn chamfer and a couple of small knocked corners',
        ('dbevel', 'corner'),
        dict(dbevel_preset='LIGHT', corner_mode='AUTO', corner_prob=60.0, corner_size_min=6.0, corner_size_max=10.0,
             corner_depth_min=3.0, corner_depth_max=5.0)),
    'DROPPED_CRATE': _preset(
        'edges_corners', 'Crushed round craters on the corners and a few edge chips',
        ('corner', 'edge'),
        dict(corner_mode='AUTO', corner_style='0', corner_prob=100.0, corner_size_min=10.0, corner_size_max=16.0,
             corner_depth_min=4.0, corner_depth_max=8.0,
             chips_mode='AUTO', amount=40, size_min=2.0, size_max=5.0, depth_min=0.5, depth_max=2.0)),
    'FRESH_SHARP_BREAKS': _preset(
        'edges_corners', 'Freshly broken, sharp-rimmed chips with fine detail',
        ('edge',),
        dict(chips_mode='AUTO', amount=140, size_min=3.0, size_max=9.0, depth_min=1.0, depth_max=3.0,
             rim_soft=0.0, chip_fine=120.0, chip_profile=90.0)),
    'SOFT_ROUNDED_WEAR': _preset(
        'edges_corners', 'Old chips whose rims have been worn smooth',
        ('edge',),
        dict(chips_mode='AUTO', amount=140, size_min=3.0, size_max=9.0, depth_min=1.0, depth_max=3.0, rim_soft=80.0)),
    'STRETCHED_EDGE_CHIPS': _preset(
        'edges_corners', 'Long chips drawn out along the edges',
        ('edge',),
        dict(chips_mode='AUTO', amount=120, size_min=2.0, size_max=6.0, depth_min=0.8, depth_max=2.5, stretch=350.0)),
    'CLUSTERED_EDGE_DAMAGE': _preset(
        'edges_corners', 'Chips gathered in groups with clean stretches of edge between them',
        ('edge',),
        dict(chips_mode='AUTO', amount=260, size_min=2.0, size_max=7.0, depth_min=0.8, depth_max=3.0, chip_cluster=95.0)),
    'WORN_DUSTY_EDGES': _preset(
        'edges_corners', 'Chipped edges with fine powder settled on the upper faces',
        ('edge', 'deposits'),
        dict(chips_mode='AUTO', amount=160, size_min=2.0, size_max=6.0, depth_min=0.8, depth_max=2.5,
             deposit_amount=60, deposit_style='1', deposit_size_min=1.0, deposit_size_max=3.0)),
    'BATTERED_BLOCK': _preset(
        'edges_corners', 'Everything at once: broken chamfers, big chips and smashed corners',
        ('dbevel', 'edge', 'corner'),
        dict(dbevel_preset='DAMAGED', chips_mode='AUTO', amount=200, size_min=3.0, size_max=10.0, depth_min=1.0,
             depth_max=4.0, sec_amount=2, corner_mode='AUTO', corner_style='0', corner_prob=100.0,
             corner_size_min=8.0, corner_size_max=12.0, corner_depth_min=4.0, corner_depth_max=7.0)),

                                                                      
    'FINE_SURFACE_DENTS': _preset(
        'surface_erosion', 'A few small, shallow dents scattered over the faces',
        ('surface',), dict(surf_type='0', surf_mode='PROCEDURAL', surf_amount=60, surf_size_min=1.5,
                           surf_size_max=4.0, surf_depth_min=0.3, surf_depth_max=1.0)),
    'HAIL_DAMAGE': _preset(
        'surface_erosion', 'Even, shallow dents all over, like hail on a soft surface',
        ('surface',), dict(surf_type='0', surf_mode='PROCEDURAL', surf_amount=250, surf_size_min=1.0,
                           surf_size_max=2.0, surf_depth_min=0.2, surf_depth_max=0.5, surf_cluster=0.0)),
    'SANDBLASTED': _preset(
        'surface_erosion', 'A rough, finely pitted skin from countless tiny hits',
        ('surface',), dict(surf_type='0', surf_mode='PROCEDURAL', surf_amount=900, surf_size_min=0.3,
                           surf_size_max=0.8, surf_depth_min=0.1, surf_depth_max=0.3, surf_cluster=10.0)),
    'DEEP_CRATERS': _preset(
        'surface_erosion', 'A handful of big, deep craters',
        ('surface',), dict(surf_type='0', surf_mode='PROCEDURAL', surf_amount=14, surf_size_min=8.0,
                           surf_size_max=16.0, surf_depth_min=3.0, surf_depth_max=6.0, surf_sec_amount=3)),
    'CRATERED_BLOCK': _preset(
        'surface_erosion', 'Craters over the faces and broken corners',
        ('surface', 'corner'),
        dict(surf_type='0', surf_mode='PROCEDURAL', surf_amount=80, surf_sec_amount=2,
             corner_mode='AUTO', corner_prob=100.0, corner_size_min=10.0, corner_size_max=16.0,
             corner_depth_min=5.0, corner_depth_max=8.0)),
    'KNIFE_SCARRED_TABLE': _preset(
        'surface_erosion', 'Dense knife cuts and gouges along the grain of a table top',
        ('surface',), dict(surf_type='1', surf_mode='PROCEDURAL', surf_amount=120, surf_size_min=2.0,
                           surf_size_max=6.0, surf_grain_x=1.0, surf_grain_y=0.0, surf_grain_z=0.0)),
    'CELLULAR_EROSION': _preset(
        'surface_erosion', 'Cell-like hollows eaten into the stone, like tuff',
        ('erosion',), dict(erosion_damage=45.0, erosion_noise_type='CELLS', erosion_noise_scale=2.0,
                           erosion_noise_contrast=50.0, erosion_round_first=False)),
    'SOFT_EROSION': _preset(
        'surface_erosion', 'Gently slumped, melted-looking forms',
        ('erosion',), dict(erosion_damage=35.0, erosion_noise_type='SOFT', erosion_noise_scale=0.6)),
    'HEAVY_ROCKY_EROSION': _preset(
        'surface_erosion', 'Rocky erosion that eats deep into the whole shape',
        ('erosion',), dict(erosion_damage=50.0, erosion_noise_type='ROCKY', erosion_noise_scale=3.0)),
    'ERODED_BASE_DEBRIS': _preset(
        'surface_erosion', 'Rocky erosion with crumbs piled in the corners and grit on the floor',
        ('erosion', 'piles', 'deposits'),
        dict(erosion_damage=25.0, erosion_noise_type='ROCKY', erosion_noise_scale=3.0,
             build_corner_prob=100.0, pile_scale=150.0, pile_lumps=100.0, deposit_amount=200)),
    'EROSION_AND_GRIT': _preset(
        'surface_erosion', 'Rocky erosion with sand settled on the upper faces',
        ('erosion', 'deposits'),
        dict(erosion_damage=25.0, erosion_noise_type='ROCKY', erosion_noise_scale=3.0, deposit_amount=80,
             deposit_style='1', deposit_size_min=1.0, deposit_size_max=3.0)),
    'SKIM_COAT_PLASTER': _preset(
        'surface_erosion', 'A thin, even plaster coat with soft strokes',
        ('plaster',), dict(plaster_strength=0.25, plaster_ridge_height=0.2, plaster_crack_amount=0.2)),
    'HEAVY_TROWEL_PLASTER': _preset(
        'surface_erosion', 'Thick, rough trowel strokes with high ridges and drag lines',
        ('plaster',), dict(plaster_strength=1.2, plaster_ridge_height=1.2, plaster_streaks=0.8)),
    'PITTED_STUCCO': _preset(
        'surface_erosion', 'Stucco full of small pores and a coarse grain',
        ('plaster',), dict(plaster_strength=0.5, plaster_pits=1.0, plaster_grain=0.3, plaster_grain_size=0.6)),
    'WEATHERED_CONCRETE': _preset(
        'surface_erosion', 'Old concrete: craters on the faces and worn, rounded chamfers',
        ('surface', 'dbevel'),
        dict(surf_type='0', surf_mode='PROCEDURAL', surf_amount=80, dbevel_preset='WEATHERED')),

                                                                      
    'SINGLE_DEEP_CRACK': _preset(
        'cracks_fracture', 'One wide, deep crack across the block',
        ('fracture',), dict(fract_l1_amount=2, fract_l1_width=3.0, fract_l1_depth=10.0, fract_l2_amount=4, fract_l2_width=0.5,
                           fract_l3_amount=0)),
    'CRAZED_SURFACE': _preset(
        'cracks_fracture', 'A dense, fine craquelure over the faces',
        ('fracture_concrete',),
        dict(fract_l1_amount=3, fract_l1_width=0.3, fract_l2_amount=40, fract_l2_width=0.15,
             fract_l3_amount=160, fract_l3_bias=30.0, fract_tile_offset=0.0)),
    'SHRINKAGE_NETWORK': _preset(
        'cracks_fracture', 'A network of shrinkage cracks between the main ones',
        ('fracture_concrete',),
        dict(fract_l1_amount=6, fract_l2_amount=30, fract_l3_amount=160, fract_l3_bias=85.0, fract_tile_offset=0.0)),
    'STRUCTURAL_CRACK': _preset(
        'cracks_fracture', 'A few wide, deep structural cracks with short branches',
        ('fracture_concrete',),
        dict(fract_l1_amount=3, fract_l1_width=5.0, fract_l1_depth=15.0, fract_l2_amount=8, fract_l3_amount=0)),
    'CRACKED_TILES': _preset(
        'cracks_fracture', 'Cracks broken into offset tiles',
        ('fracture_concrete',),
        dict(fract_l1_amount=8, fract_l2_amount=25, fract_l3_amount=80, fract_l3_bias=85.0, fract_tile_offset=100.0)),
    'SPIDER_WEB_GLASS': _preset(
        'cracks_fracture', 'A spider web of rings and spokes in glass',
        ('fracture_glass',),
        dict(fract_l1_amount=2, fract_l2_amount=40, fract_l1_chips=False, fract_l2_chips=False,
             fract_glass_spokes=24, fract_glass_rings=12)),
    'SHATTERED_GLASS': _preset(
        'cracks_fracture', 'Heavily shattered glass with crushed centres',
        ('fracture_glass',),
        dict(fract_l1_amount=6, fract_l2_amount=40, fract_l1_chips=False, fract_l2_chips=False,
             fract_glass_spokes=20, fract_glass_crush=25.0)),
    'STARBURST_IMPACT': _preset(
        'cracks_fracture', 'Many long rays and rings around strong impacts',
        ('fracture_radial',), dict(fract_l1_amount=2, fract_l2_amount=6, fract_radial_spokes=16, fract_radial_rings=5)),
    'CRUSHED_IMPACT': _preset(
        'cracks_fracture', 'Impacts with a crushed, broken-in centre',
        ('fracture_radial',), dict(fract_l1_amount=3, fract_l2_amount=10, fract_radial_spokes=10, fract_radial_crush=45.0)),
    'WEATHERED_WOOD_CHECKS': _preset(
        'cracks_fracture', 'Many short drying checks along the grain',
        ('fracture_wood',),
        dict(fract_l1_amount=25, fract_l1_length=15.0, fract_l1_width=0.5, fract_l2_amount=30,
             fract_grain_x=1.0, fract_grain_y=0.0, fract_grain_z=0.0)),
    'DEEP_WOOD_SPLITS': _preset(
        'cracks_fracture', 'A few wide, deep splits through the beam',
        ('fracture_wood',),
        dict(fract_l1_amount=6, fract_l1_width=4.0, fract_l1_depth=12.0, fract_l2_amount=15,
             fract_grain_x=1.0, fract_grain_y=0.0, fract_grain_z=0.0)),
    'CRACKED_STATUE': _preset(
        'cracks_fracture', 'Long cracks that wrap around rounded, sculpted forms (Rounded mode)',
        ('fracture',), dict(fract_surface='ROUNDED', fract_l1_amount=6, fract_l2_amount=20)),
    'ROUNDED_HAIRLINES': _preset(
        'cracks_fracture', 'Thin hairline cracks over a sculpture (Rounded mode)',
        ('fracture',),
        dict(fract_surface='ROUNDED', fract_l1_amount=4, fract_l1_width=0.3, fract_l2_amount=30, fract_l2_width=0.15)),
    'CRACKS_AND_CHIPS': _preset(
        'cracks_fracture', 'Cracks across the faces and chips along the edges',
        ('fracture', 'edge'),
        dict(fract_l1_amount=6, fract_l2_amount=20, chips_mode='AUTO', amount=150, size_min=2.0, size_max=7.0,
             depth_min=0.8, depth_max=3.0)),
    'CRACKED_AND_ERODED': _preset(
        'cracks_fracture', 'Old cracked stone, worn down by erosion',
        ('fracture', 'erosion'),
        dict(fract_l1_amount=6, fract_l2_amount=20, erosion_damage=20.0, erosion_noise_type='ROCKY',
             erosion_noise_scale=3.0)),

                                                                      
    'GRANITE_BLOCK': _preset(
        'stone_masonry', 'Hard stone: crisp, sharp chips and cleanly broken corners',
        ('edge', 'corner'),
        dict(chips_mode='AUTO', amount=120, size_min=2.0, size_max=6.0, depth_min=1.0, depth_max=3.0, rim_soft=0.0,
             chip_fine=120.0, corner_mode='AUTO', corner_style='0', corner_prob=100.0, corner_size_min=8.0,
             corner_size_max=12.0, corner_depth_min=4.0, corner_depth_max=7.0)),
    'MARBLE_STATUE_WEAR': _preset(
        'stone_masonry', 'Soft, worn chamfers and fine cracks, like an old marble piece',
        ('dbevel', 'fracture'),
        dict(dbevel_preset='WEATHERED', fract_surface='ROUNDED', fract_l1_amount=3, fract_l1_width=0.3,
             fract_l2_amount=15, fract_l2_width=0.15)),
    'WEATHERED_GRAVESTONE': _preset(
        'stone_masonry', 'A gravestone worn soft by weather, with a pitted face',
        ('erosion', 'surface'),
        dict(erosion_damage=40.0, erosion_noise_type='CRACKED', erosion_noise_scale=0.5, erosion_noise_contrast=60.0,
             surf_type='0', surf_mode='PROCEDURAL', surf_amount=60)),
    'CRACKED_GRAVESTONE': _preset(
        'stone_masonry', 'A cracked slab with chipped edges',
        ('fracture_concrete', 'edge'),
        dict(fract_l1_amount=4, fract_l2_amount=15, fract_l3_amount=0, chips_mode='AUTO', amount=120,
             size_min=2.0, size_max=6.0, depth_min=0.8, depth_max=2.5)),
    'FALLEN_COLUMN_DRUM': _preset(
        'stone_masonry', 'An eroded column drum with broken rims',
        ('erosion', 'edge'),
        dict(erosion_damage=25.0, erosion_noise_type='ROCKY', erosion_noise_scale=3.0,
             chips_mode='AUTO', amount=160, size_min=4.0, size_max=10.0, depth_min=1.5, depth_max=4.0)),

                                                                      
    'ROMAN_RUINS': _preset(
        'stone_masonry', 'Worn, chipped ruins with crumbs piled in the corners',
        ('edge', 'corner', 'erosion', 'piles'),
        dict(chips_mode='AUTO', amount=180, size_min=3.0, size_max=9.0, depth_min=1.0, depth_max=3.5,
             corner_mode='AUTO', corner_prob=100.0, corner_size_min=8.0, corner_size_max=14.0,
             corner_depth_min=4.0, corner_depth_max=7.0, erosion_damage=15.0, erosion_noise_type='ROCKY',
             erosion_noise_scale=3.0, build_corner_prob=100.0, pile_scale=120.0, pile_lumps=100.0)),
    'WORN_COBBLESTONE': _preset(
        'stone_masonry', 'Rounded, smoothly worn chips on paving stones',
        ('edge', 'surface'),
        dict(chips_mode='AUTO', amount=200, size_min=2.0, size_max=6.0, depth_min=0.8, depth_max=2.5, rim_soft=90.0,
             surf_type='0', surf_mode='PROCEDURAL', surf_amount=40, surf_size_min=1.0, surf_size_max=3.0)),
    'CRACKED_PAVERS': _preset(
        'stone_masonry', 'Paving slabs cracked into offset tiles',
        ('fracture_concrete',),
        dict(fract_l1_amount=5, fract_l2_amount=15, fract_l3_amount=60, fract_l3_bias=85.0, fract_tile_offset=100.0)),
    'SIDEWALK_KERB': _preset(
        'stone_masonry', 'A chipped kerb stone with dirt settled on top',
        ('edge', 'corner', 'deposits'),
        dict(chips_mode='AUTO', amount=160, size_min=2.0, size_max=7.0, depth_min=1.0, depth_max=3.0,
             corner_mode='AUTO', corner_prob=100.0, corner_size_min=8.0, corner_size_max=12.0,
             corner_depth_min=4.0, corner_depth_max=6.0, deposit_amount=80, deposit_style='0',
             deposit_size_min=1.0, deposit_size_max=2.5)),
    'CONCRETE_BARRIER': _preset(
        'stone_masonry', 'A road barrier with chipped edges, knocked corners and cracks',
        ('edge', 'corner', 'fracture_concrete'),
        dict(chips_mode='AUTO', amount=200, size_min=3.0, size_max=9.0, depth_min=1.0, depth_max=4.0,
             corner_mode='AUTO', corner_prob=100.0, corner_size_min=10.0, corner_size_max=16.0,
             corner_depth_min=5.0, corner_depth_max=8.0, fract_l1_amount=5, fract_l2_amount=15, fract_l3_amount=0)),
    'BRIDGE_PILLAR': _preset(
        'stone_masonry', 'A pillar eroded by water with grime on its ledges',
        ('erosion', 'deposits'),
        dict(erosion_damage=30.0, erosion_noise_type='ROCKY', erosion_noise_scale=3.0, deposit_amount=120,
             deposit_style='2', deposit_size_min=1.5, deposit_size_max=4.0)),
    'FOUNTAIN_BASIN': _preset(
        'stone_masonry', 'A basin with mineral crust on the floor, sludge in the corners and worn rims',
        ('piles', 'deposits', 'erosion'),
        dict(build_corner_prob=100.0, pile_scale=120.0, pile_lumps=80.0, deposit_amount=200, deposit_style='2',
             erosion_damage=15.0, erosion_noise_type='SOFT', erosion_noise_scale=0.6)),
    'GARGOYLE_WEAR': _preset(
        'stone_masonry', 'Fine rocky erosion over carved, rounded forms',
        ('erosion',), dict(erosion_damage=25.0, erosion_noise_type='ROCKY', erosion_noise_scale=5.0)),
    'QUARRY_BLOCK': _preset(
        'stone_masonry', 'A freshly quarried block: big, sharp chips and broken corners',
        ('edge', 'corner'),
        dict(chips_mode='AUTO', amount=90, size_min=5.0, size_max=14.0, depth_min=2.0, depth_max=5.0, rim_soft=0.0,
             corner_mode='AUTO', corner_prob=100.0, corner_size_min=10.0, corner_size_max=16.0,
             corner_depth_min=5.0, corner_depth_max=9.0)),
    'SANDSTONE_STEPS': _preset(
        'stone_masonry', 'Eroded sandstone with sand piled in the inside corners',
        ('erosion', 'piles'),
        dict(erosion_damage=25.0, erosion_noise_type='ROCKY', erosion_noise_scale=3.0,
             build_corner_prob=100.0, pile_scale=150.0, pile_lumps=60.0)),
    'MOSSY_STEPS': _preset(
        'stone_masonry', 'Chipped steps with grass along the inside edges',
        ('edge', 'corner', 'grass'),
        dict(chips_mode='AUTO', amount=160, size_min=3.0, size_max=9.0, depth_min=1.0, depth_max=3.5,
             corner_mode='AUTO', corner_prob=100.0, corner_size_min=8.0, corner_size_max=14.0,
             corner_depth_min=4.0, corner_depth_max=7.0),
        settings={'grass': dict(kind='FRESH', base_density=60.0)}),
    'WALL_COPING_STONE': _preset(
        'stone_masonry', 'Broken, worn chamfers on a coping stone with grime on top',
        ('dbevel', 'deposits'), dict(dbevel_preset='HEAVY', deposit_amount=100, deposit_style='0',
                                     deposit_size_min=1.0, deposit_size_max=2.5)),
    'SHATTERED_SLAB': _preset(
        'stone_masonry', 'A slab split by an impact, with knocked corners',
        ('fracture_radial', 'corner'),
        dict(fract_l1_amount=2, fract_l2_amount=8, fract_radial_spokes=9, corner_mode='AUTO', corner_prob=100.0,
             corner_size_min=8.0, corner_size_max=12.0, corner_depth_min=3.0, corner_depth_max=5.0)),
    'FROST_CRACKED_STONE': _preset(
        'stone_masonry', 'Stone split by frost, with cracked erosion on the surface',
        ('fracture', 'erosion'),
        dict(fract_l1_amount=8, fract_l1_width=1.5, fract_l2_amount=25, erosion_damage=30.0,
             erosion_noise_type='CRACKED', erosion_noise_scale=3.0, erosion_noise_contrast=30.0,
             erosion_round_first=False)),
    'ANCIENT_ALTAR': _preset(
        'stone_masonry', 'An old altar block: chips, craters and long cracks',
        ('edge', 'surface', 'fracture'),
        dict(chips_mode='AUTO', amount=160, size_min=3.0, size_max=9.0, depth_min=1.0, depth_max=3.5,
             surf_type='0', surf_mode='PROCEDURAL', surf_amount=50, fract_surface='ROUNDED',
             fract_l1_amount=4, fract_l2_amount=12)),

                                                                      
    'OPEN_WALLPAPER_SEAMS': _preset(
        'walls_interiors', 'Wallpaper seams opening up, no tears yet',
        ('wallpaper',),
        dict(wallpaper_tears_enable=False, wallpaper_flaps_enable=False, wallpaper_lifts_enable=False,
             wallpaper_seam_open=1.4, wallpaper_seam_crack=1.5,
             wallpaper_seam_width=0.6, wallpaper_seam_depth=2.0)),
    'LIFTED_WALLPAPER_FLAPS': _preset(
        'walls_interiors', 'Big curled flaps of wallpaper peeling off the wall',
        ('wallpaper',), dict(wallpaper_flap_amount=2.5, wallpaper_lift_amount=10, wallpaper_tear_amount=2)),
    'TORN_WALLPAPER_STRIPS': _preset(
        'walls_interiors', 'Long strips torn away down to the plaster',
        ('wallpaper',), dict(wallpaper_tear_amount=9, wallpaper_tear_width_max=2.5, wallpaper_lift_amount=4)),
    'WALLPAPER_OVER_PLASTER': _preset(
        'walls_interiors', 'Torn wallpaper with rough plaster showing under it',
        ('wallpaper', 'plaster'), dict(wallpaper_tear_amount=5, wallpaper_lift_amount=6)),
    'LIGHT_GUNFIRE': _preset(
        'walls_interiors', 'A few scattered bullet hits',
        ('bullet',), dict(bullet_mode='0', bullet_material='2', bullet_amount=15, bullet_clusters=0, bullet_cluster_share=0.0)),
    'HEAVY_GUNFIRE': _preset(
        'walls_interiors', 'A wall after a firefight: many dense clusters of hits',
        ('bullet',), dict(bullet_mode='0', bullet_material='2', bullet_amount=100, bullet_clusters=7,
                          bullet_cluster_share=0.85)),
    'SINGLE_BURST': _preset(
        'walls_interiors', 'One tight burst of hits',
        ('bullet',), dict(bullet_mode='0', bullet_material='2', bullet_amount=12, bullet_clusters=1,
                          bullet_cluster_share=1.0, bullet_cluster_spread=8.0)),
    'THICK_MORTAR_BEADS': _preset(
        'walls_interiors', 'Thick beads of squeezed-out mortar in the inside corners',
        ('buildup',), dict(build_mode='AUTO', build_width=2.0, build_height=1.2, build_lumps=60.0)),
    'THIN_CAULK_LINE': _preset(
        'walls_interiors', 'A neat, thin caulk line in the inside corners',
        ('buildup',), dict(build_mode='AUTO', build_width=0.4, build_height=0.25, build_irreg=20.0, build_lumps=0.0)),
    'LUMPY_BEAD': _preset(
        'walls_interiors', 'An uneven, lumpy bead along the inside corners',
        ('buildup',), dict(build_mode='AUTO', build_width=1.2, build_height=0.7, build_irreg=250.0, build_lumps=100.0)),
    'DUST_PILES_ONLY': _preset(
        'walls_interiors', 'Only dust piled into the inside corners',
        ('piles',), dict(build_corner_prob=100.0, pile_scale=150.0, pile_height=90.0, pile_lumps=60.0)),
    'LIGHT_DUST': _preset(
        'walls_interiors', 'A light film of powder on the upward faces',
        ('deposits',), dict(deposit_amount=60, deposit_style='1', deposit_size_min=1.0, deposit_size_max=3.0)),
    'HEAVY_GRIME_LAYERS': _preset(
        'walls_interiors', 'Thick, layered grime and dirt on every upward face',
        ('deposits',), dict(deposit_amount=300, deposit_style='0', deposit_size_min=2.0, deposit_size_max=4.0,
                           deposit_height=0.6)),
    'ABANDONED_ROOM': _preset(
        'walls_interiors', 'Peeling wallpaper with dust in the corners and dirt on the floor',
        ('wallpaper', 'piles', 'deposits'),
        dict(wallpaper_tear_amount=5, wallpaper_lift_amount=8, build_corner_prob=100.0, pile_scale=150.0,
             deposit_amount=150)),
    'DAMAGED_PLASTER_WALL': _preset(
        'walls_interiors', 'Rough plaster with chipped edges and broken corners',
        ('plaster', 'edge', 'corner'),
        dict(chips_mode='AUTO', amount=160, size_min=3.0, size_max=9.0, depth_min=1.0, depth_max=3.0,
             corner_mode='AUTO', corner_prob=100.0, corner_size_min=10.0, corner_size_max=16.0,
             corner_depth_min=4.0, corner_depth_max=7.0)),

                                                                      
    'LIGHT_RUST_HOLES': _preset(
        'metal_industrial', 'The first small rust holes breaking through',
        ('rusty',), dict(rusty_mode='0', rusty_amount=3, rusty_size_min=1.0, rusty_size_max=2.5)),
    'RUST_THROUGH_PANEL': _preset(
        'metal_industrial', 'A panel rotted through with many big holes',
        ('rusty',), dict(rusty_mode='0', rusty_amount=16, rusty_size_min=4.0, rusty_size_max=10.0)),
    'LACE_RUST': _preset(
        'metal_industrial', 'Lace-like rust: thin bridges of metal between the holes',
        ('rusty',), dict(rusty_mode='0', rusty_amount=8, rusty_size_max=7.0, rusty_lace=1.4, rusty_pit_amount=80)),
    'RUST_RUN_HOLES': _preset(
        'metal_industrial', 'Rust holes drawn downwards, as if water ran over the sheet',
        ('rusty',), dict(rusty_mode='0', rusty_amount=8, rusty_size_max=7.0, rusty_flow=1.0, rusty_stretch=0.9)),
    'SHOTGUN_SPREAD': _preset(
        'metal_industrial', 'Dense spreads of small holes, like shotgun pellets',
        ('bullet',), dict(bullet_mode='0', bullet_material='0', bullet_amount=90, bullet_clusters=3,
                          bullet_cluster_share=1.0, bullet_cluster_spread=6.0, bullet_size_min=0.3,
                          bullet_size_max=0.5, bullet_metal_dent_chance=0.0)),
    'SCATTERED_HITS_STEEL': _preset(
        'metal_industrial', 'Single bullet holes spread over a steel plate',
        ('bullet',), dict(bullet_mode='0', bullet_material='0', bullet_amount=20, bullet_clusters=0,
                          bullet_cluster_share=0.0, bullet_metal_dent_chance=0.0)),
    'DENTED_BULLET_HITS': _preset(
        'metal_industrial', 'Most hits only dent the steel, a few go through',
        ('bullet',), dict(bullet_mode='0', bullet_material='0', bullet_amount=30, bullet_metal_dent_chance=0.8)),
    'PETALLED_EXIT_HOLES': _preset(
        'metal_industrial', 'Holes with many torn, curled petals',
        ('bullet',), dict(bullet_mode='0', bullet_material='0', bullet_amount=25, bullet_metal_petals=10,
                          bullet_metal_petal_tear=1.0, bullet_metal_curl=1.0, bullet_metal_dent_chance=0.0)),
    'HEAVY_WELD_BEADS': _preset(
        'metal_industrial', 'Wide, rough weld beads along the inside corners',
        ('weld',), settings={'weld': dict(edges_enable=True, edge_type='0', width=0.024, height=0.008, wobble=1.2)}),
    'FINE_TIG_WELDS': _preset(
        'metal_industrial', 'Narrow, neat welds with fine, even ripples',
        ('weld',), settings={'weld': dict(edges_enable=True, edge_type='0', width=0.008, height=0.0025, pitch=0.0015,
                                          wobble=0.1)}),
    'OUTSIDE_CORNER_WELDS': _preset(
        'metal_industrial', 'Weld beads on the outside corners',
        ('weld',), settings={'weld': dict(edges_enable=True, edge_type='1', width=0.014, height=0.005)}),
    'ALL_CORNER_WELDS': _preset(
        'metal_industrial', 'Weld beads on every corner, inside and outside',
        ('weld',), settings={'weld': dict(edges_enable=True, edge_type='2', width=0.014, height=0.005)}),
    'WELDED_AND_RUSTED': _preset(
        'metal_industrial', 'An old welded frame with rust holes in the plates',
        ('weld', 'rusty'), dict(rusty_mode='0', rusty_amount=6, rusty_size_max=5.0),
        settings={'weld': dict(edges_enable=True, edge_type='0', width=0.018, height=0.006)}),
    'SHOT_AND_RUSTED': _preset(
        'metal_industrial', 'A rusted sheet that has also been shot',
        ('bullet', 'rusty'),
        dict(bullet_mode='0', bullet_material='0', bullet_amount=25, bullet_metal_dent_chance=0.0,
             rusty_mode='0', rusty_amount=6, rusty_size_max=6.0)),
    'CHIPPED_MACHINE_PART': _preset(
        'metal_industrial', 'Small knocks and nicks on the edges and corners of a part',
        ('edge', 'corner'),
        dict(chips_mode='AUTO', amount=90, size_min=1.0, size_max=3.0, depth_min=0.3, depth_max=1.0, rim_soft=40.0,
             corner_mode='AUTO', corner_prob=70.0, corner_size_min=4.0, corner_size_max=7.0,
             corner_depth_min=1.5, corner_depth_max=3.0)),
    'WORN_STEEL_BEVEL': _preset(
        'metal_industrial', 'A very thin, crisp chamfer with short, jagged nicks, like handled steel stock',
        ('dbevel',), dict(dbevel_preset='CUSTOM', dbevel_width=0.35, dbevel_smoothness=0.08, dbevel_uneven=0.05,
                          dbevel_damage=0.3, dbevel_damage_amount=0.65, dbevel_damage_factor=0.4, dbevel_damage_depth=2.5,
                          dbevel_patch_length=3.5, dbevel_chip_scatter=1.0, dbevel_jagged=0.95,
                          dbevel_placement_random=0.9, dbevel_smooth_damage=False)),
    'INDUSTRIAL_GRIME': _preset(
        'metal_industrial', 'Welded steel with grime settled on the plates',
        ('weld', 'deposits'), dict(deposit_amount=50, deposit_style='1', deposit_size_min=0.3, deposit_size_max=0.8),
        settings={'weld': dict(edges_enable=True, edge_type='0', width=0.018, height=0.006)}),

                                                                      
    'DAMAGED_WOOD': _preset(
        'wood_timber', 'Wood Damage all over: breaks, checks, grooves and slivers',
        ('wood',), dict(wood_mode='0', wood_size_min=10.0, wood_size_max=24.0, wood_amount=12)),
    'SPLINTERED_BEAM': _preset(
        'wood_timber', 'Long, sharp splinters torn out of the fibres',
        ('wood',), dict(wood_mode='0', wood_size_min=10.0, wood_size_max=24.0, wood_amount=8, wood_splinter_length=14.0, wood_splinter_sharp=2.0,
                        wood_splinter_chunk=1.2)),
    'CHECKED_TIMBER': _preset(
        'wood_timber', 'Many drying checks running along the timber',
        ('wood',), dict(wood_mode='0', wood_size_min=10.0, wood_size_max=24.0, wood_amount=1, wood_check_amount=10, wood_check_global=30,
                        wood_check_width=0.8, wood_check_depth=1.5)),
    'KNOTTY_OLD_WOOD': _preset(
        'wood_timber', 'Knots that have fallen out, with checks around them',
        ('wood',), dict(wood_mode='0', wood_size_min=10.0, wood_size_max=24.0, wood_amount=2, wood_knot_amount=8, wood_knot_size=2.5, wood_knot_depth=2.0,
                        wood_knot_checks=4)),
    'WEATHERED_BOARDS': _preset(
        'wood_timber', 'Deep weathering grooves between the hard grain lines',
        ('wood',), dict(wood_mode='0', wood_amount=20, wood_size_min=8.0, wood_size_max=18.0, wood_groove_depth=1.0,
                        wood_weather_halo=2.0, wood_detail=1.5)),
    'SLIVERED_PLANKS': _preset(
        'wood_timber', 'Long slivers lifting off the surface',
        ('wood',), dict(wood_mode='0', wood_size_min=10.0, wood_size_max=24.0, wood_amount=12, wood_sliver_amount=1.2, wood_sliver_length=12.0,
                        wood_sliver_lift=1.2)),
    'BROKEN_BOARD_ENDS': _preset(
        'wood_timber', 'Ends of the boards broken off and splintered',
        ('wood',), dict(wood_mode='0', wood_size_min=10.0, wood_size_max=24.0, wood_amount=6, wood_break_chance=1.2, wood_break_size=1.5, wood_end_bias=2.0)),
    'BULLET_HIT_PLANK': _preset(
        'wood_timber', 'Bullet holes in a plank with crushed fibres',
        ('bullet',), dict(bullet_mode='0', bullet_material='1', bullet_amount=25)),
    'SPLINTERED_EXIT_HOLES': _preset(
        'wood_timber', 'Big, splintered exit holes on the back of a plank',
        ('bullet',), dict(bullet_mode='0', bullet_material='1', bullet_amount=25, bullet_wood_exit_scale=12.0,
                          bullet_wood_splinter=3.0, bullet_wood_slivers=10)),
    'DEEP_GOUGES': _preset(
        'wood_timber', 'A few big, deep gouges along the grain',
        ('surface',), dict(surf_type='1', surf_mode='PROCEDURAL', surf_amount=25, surf_size_min=5.0,
                           surf_size_max=12.0, surf_depth_min=1.0, surf_depth_max=3.0,
                           surf_grain_x=1.0, surf_grain_y=0.0, surf_grain_z=0.0)),
    'OLD_TABLE_TOP': _preset(
        'wood_timber', 'Knife marks on the top and worn, nicked edges',
        ('surface', 'edge'),
        dict(surf_type='1', surf_mode='PROCEDURAL', surf_amount=60, surf_grain_x=1.0, surf_grain_y=0.0,
             surf_grain_z=0.0, chips_mode='AUTO', amount=120, size_min=1.0, size_max=4.0, depth_min=0.3,
             depth_max=1.5, rim_soft=50.0)),
    'BARN_WOOD': _preset(
        'wood_timber', 'Grooved, weathered boards with chipped edges',
        ('wood', 'edge'),
        dict(wood_mode='0', wood_size_min=10.0, wood_size_max=24.0, wood_amount=3, wood_groove_depth=0.4, chips_mode='AUTO', amount=100, size_min=2.0,
             size_max=6.0, depth_min=0.5, depth_max=2.0)),
    'SHIPWRECK_TIMBER': _preset(
        'wood_timber', 'Timber worn soft by water',
        ('wood', 'erosion'),
        dict(wood_mode='0', wood_size_min=10.0, wood_size_max=24.0, erosion_damage=25.0, erosion_noise_type='SOFT', erosion_noise_scale=0.6)),
    'WORMWOOD': _preset(
        'wood_timber', 'Countless small, deep holes, like woodworm',
        ('surface',), dict(surf_type='0', surf_mode='PROCEDURAL', surf_amount=600, surf_size_min=0.3,
                           surf_size_max=0.6, surf_depth_min=1.0, surf_depth_max=2.0, surf_cluster=60.0)),
    'SPLIT_AND_CHECKED_BEAM': _preset(
        'wood_timber', 'Wide splits through the beam with drying checks around them',
        ('fracture_wood', 'wood'),
        dict(fract_l1_amount=6, fract_l1_width=2.0, fract_l2_amount=15, fract_grain_x=1.0, fract_grain_y=0.0,
             fract_grain_z=0.0, wood_mode='0', wood_size_min=10.0, wood_size_max=24.0, wood_amount=1, wood_check_amount=6)),
    'WORN_STAIR_TREADS': _preset(
        'wood_timber', 'Wooden steps with smoothly worn edges and scuffed treads',
        ('edge', 'surface'),
        dict(chips_mode='AUTO', amount=180, size_min=3.0, size_max=9.0, depth_min=1.0, depth_max=3.0, rim_soft=90.0,
             surf_type='1', surf_mode='PROCEDURAL', surf_amount=30)),
    'ROUNDED_OLD_FURNITURE': _preset(
        'wood_timber', 'Corners and edges rubbed round by years of use: a broad, soft, slightly uneven chamfer with no chips',
        ('dbevel',), dict(dbevel_preset='CUSTOM', dbevel_width=2.0, dbevel_smoothness=0.9, dbevel_uneven=0.35, dbevel_damage_factor=0.0)),
    'CHEWED_CORNERS': _preset(
        'wood_timber', 'Small bites out of the corners and nicks along the edges',
        ('corner', 'edge'),
        dict(corner_mode='AUTO', corner_prob=100.0, corner_size_min=5.0, corner_size_max=8.0,
             corner_depth_min=2.0, corner_depth_max=4.0, chips_mode='AUTO', amount=80, size_min=1.0,
             size_max=3.0, depth_min=0.3, depth_max=1.0)),
    'DUSTY_ATTIC_BEAM': _preset(
        'wood_timber', 'A damaged beam with dust lying on top',
        ('wood', 'deposits'),
        dict(wood_mode='0', wood_size_min=10.0, wood_size_max=24.0, wood_amount=6, deposit_amount=80, deposit_style='1', deposit_size_min=0.5,
             deposit_size_max=1.5)),
    'RAIL_SLEEPER': _preset(
        'wood_timber', 'A damaged sleeper bedded in gravel',
        ('wood', 'gravel'), dict(wood_mode='0', wood_size_min=10.0, wood_size_max=24.0, wood_amount=6), settings={'gravel': dict(rock_subdivision=1)}),

                                                                      
    'OLD_JEANS': _preset(
        'fabric_cloth', 'Worn holes with the weft still spanning them, a few punctures (Old Jeans)',
        ('fabric',), dict(fabric_tear_preset='1')),
    'SNAGGED_SHIRT': _preset(
        'fabric_cloth', 'Corner tears from a nail with folded flaps, short rips (Snagged Shirt)',
        ('fabric',), dict(fabric_tear_preset='2')),
    'MOTH_EATEN': _preset(
        'fabric_cloth', 'Many small holes and punctures with worn spots around (Moth-Eaten)',
        ('fabric',), dict(fabric_tear_preset='4')),
    'CLASSIC_LENS_TEARS': _preset(
        'fabric_cloth', 'Ragged lens-shaped holes split into smaller ones (Classic Lenses)',
        ('fabric',), dict(fabric_tear_preset='6')),
    'THICK_JEANS': _preset(
        'fabric_cloth', 'Worn denim holes on thick, solid fabric',
        ('fabric_solid',), dict(fabric_tear_preset='1')),
    'SNAGGED_JACKET': _preset(
        'fabric_cloth', 'Corner tears with folded flaps on a thick jacket',
        ('fabric_solid',), dict(fabric_tear_preset='2')),
    'MOTH_EATEN_BLANKET': _preset(
        'fabric_cloth', 'A thick blanket full of small moth holes',
        ('fabric_solid',), dict(fabric_tear_preset='4')),
    'SHREDDED_TARP': _preset(
        'fabric_cloth', 'A heavy tarp torn and shredded all over',
        ('fabric_solid',), dict(fabric_tear_preset='5')),
    'SOLID_LENS_TEARS': _preset(
        'fabric_cloth', 'Lens-shaped tears on thick fabric',
        ('fabric_solid',), dict(fabric_tear_preset='6')),
    'FRAYED_EDGES': _preset(
        'fabric_cloth', 'Only the borders are torn and frayed, no holes',
        ('fabric',), dict(fabric_tear_preset='5', fabric_holes_enable=False, fabric_hang_enable=False,
                          fabric_edge_enable=True, fabric_edge_amount=30, fabric_fray_enable=True)),
    'HANGING_THREADS': _preset(
        'fabric_cloth', 'Holes with long loose threads hanging down',
        ('fabric',), dict(fabric_tear_preset='1', fabric_hang_enable=True, fabric_hang_amount=80,
                          fabric_hang_length_min=6.0, fabric_hang_length_max=25.0)),
    'TORN_HEM': _preset(
        'fabric_cloth', 'A torn, ragged bottom edge with strands hanging off',
        ('fabric',), dict(fabric_tear_preset='5', fabric_holes_enable=False, fabric_edge_enable=True,
                          fabric_edge_amount=24, fabric_hang_enable=True, fabric_hang_amount=60)),
    'ONE_BIG_TEAR': _preset(
        'fabric_cloth', 'A single big rip through the cloth',
        ('fabric',), dict(fabric_tear_preset='3', fabric_hole_amount=1, fabric_hole_length_min=40.0,
                          fabric_hole_length_max=60.0, fabric_hole_width_min=15.0, fabric_hole_width_max=25.0, fabric_edge_enable=False,
                          fabric_tear_rip=1.0, fabric_tear_worn=0.0, fabric_tear_puncture=0.0, fabric_tear_corner=0.0)),
    'WORN_THIN_SPOTS': _preset(
        'fabric_cloth', 'Worn-through spots where only broken threads are left',
        ('fabric',), dict(fabric_tear_preset='1', fabric_broken_share=1.5, fabric_bridge_share=1.0,
                          fabric_hole_amount=8)),
    'TENT_FLAP': _preset(
        'fabric_cloth', 'Long rips along the grain of a heavy canvas',
        ('fabric_solid',), dict(fabric_tear_preset='3', fabric_hole_amount=4, fabric_hole_length_max=50.0)),
    'TATTERED_FLAG': _preset(
        'fabric_cloth', 'Edges torn into long tatters',
        ('fabric',), dict(fabric_tear_preset='5', fabric_edge_amount=60, fabric_edge_length_max=60.0,
                          fabric_edge_width_max=6.0)),
    'BURLAP_SACK': _preset(
        'fabric_cloth', 'Coarse sacking with frayed holes and thick threads',
        ('fabric_solid',), dict(fabric_tear_preset='4', fabric_yarn_size=0.8, fabric_thread_radius=0.15)),
    'CURTAIN_RIPS': _preset(
        'fabric_cloth', 'Long vertical rips through a hanging curtain',
        ('fabric',), dict(fabric_tear_preset='3', fabric_hole_dir_mode='1', fabric_hole_dir_x=0.0,
                          fabric_hole_dir_z=1.0, fabric_hole_amount=6, fabric_hole_length_max=45.0)),

                                                                      
    'HAIRLINE_ASPHALT_CRACKS': _preset(
        'ground_nature', 'Only thin cracks running over the surface',
        ('ground',), dict(ground_mode='0', ground_area_chance=0.0, ground_crack_width=0.6, ground_crack_depth=0.5,
                          ground_crack_amount=10)),
    'ALLIGATOR_CRACKING': _preset(
        'ground_nature', 'Areas broken into a tight network of small plates',
        ('ground',), dict(ground_mode='0', ground_area_chance=1.0, ground_pothole_chance=0.0, ground_heave_chance=0.0,
                          ground_plate_size=8.0, ground_gap_width=1.5)),
    'WIDE_GROUND_GAPS': _preset(
        'ground_nature', 'Wide, deep cracks opening in the ground',
        ('ground',), dict(ground_mode='0', ground_area_chance=0.0, ground_crack_width=3.0, ground_crack_depth=4.0)),
    'SUNKEN_PLATES': _preset(
        'ground_nature', 'Broken plates that have sunk and tilted',
        ('ground',), dict(ground_mode='0', ground_area_chance=1.0, ground_pothole_chance=0.0, ground_plate_sink=3.0,
                          ground_plate_tilt=2.0)),
    'RUBBLE_STREWN_GROUND': _preset(
        'ground_nature', 'Broken ground with lots of loose rubble',
        ('ground',), dict(ground_mode='0', ground_pothole_chance=0.5, ground_rubble_amount=100,
                          ground_rubble_size_min=4.0, ground_rubble_size_max=9.0)),
    'CRUMBLING_ROAD_EDGE': _preset(
        'ground_nature', 'A cracked road whose edges are breaking away',
        ('ground', 'edge'),
        dict(ground_mode='0', ground_crack_width=1.0, ground_crack_depth=1.0, chips_mode='AUTO', amount=200,
             size_min=8.0, size_max=20.0, depth_min=3.0, depth_max=8.0)),
    'FRESH_GREEN_GRASS': _preset(
        'ground_nature', 'Fresh green grass in cavities and along inside edges',
        ('grass',), settings={'grass': dict(kind='FRESH')}),
    'GRASSY_EDGES': _preset(
        'ground_nature', 'Thick grass along every inside edge and joint',
        ('grass',), settings={'grass': dict(kind='FRESH', base_density=50.0, edge_density=8000.0)}),
    'SPARSE_WEEDS': _preset(
        'ground_nature', 'A few sparse tufts of dry weeds',
        ('grass',), settings={'grass': dict(kind='DRY', base_density=150.0, edge_density=1500.0, cavity_density=2000.0)}),
    'FINE_GRAVEL': _preset(
        'ground_nature', 'Fine gravel and grit in the low spots',
        ('gravel',), settings={'gravel': dict(size_min=0.002, size_max=0.01, base_density=600.0)}),
    'COARSE_RUBBLE': _preset(
        'ground_nature', 'Big, angular rubble stones',
        ('gravel',), settings={'gravel': dict(size_min=0.015, size_max=0.04, angularity=0.8, base_density=60.0,
                                              edge_density=300.0, cavity_density=500.0)}),
    'GRAVEL_PATH': _preset(
        'ground_nature', 'Gravel covering the whole surface',
        ('gravel',), settings={'gravel': dict(base_density=3000.0, rock_subdivision=1)}),
    'WEEDS_IN_CRACKS': _preset(
        'ground_nature', 'Cracked ground with weeds growing out of it',
        ('ground', 'grass'), dict(ground_mode='0', ground_crack_width=1.0, ground_crack_depth=1.0),
        settings={'grass': dict(kind='FRESH', base_density=80.0)}),
    'GRAVEL_FILLED_POTHOLES': _preset(
        'ground_nature', 'Potholes with gravel collected in them',
        ('ground', 'gravel'), dict(ground_mode='0', ground_pothole_chance=1.0),
        settings={'gravel': dict(rock_subdivision=1, cavity_density=4000.0)}),
    'FOREST_FLOOR_PATH': _preset(
        'ground_nature', 'A path of gravel with fresh grass at the sides',
        ('gravel', 'grass'), settings={'gravel': dict(base_density=1200.0), 'grass': dict(kind='FRESH')}),

                                                                      
    'ABANDONED_FACTORY_FLOOR': _preset(
        'combined', 'A cracked factory floor with gravel and grime in the low spots',
        ('ground', 'gravel', 'deposits'),
        dict(ground_mode='0', ground_crack_width=1.0, ground_crack_depth=1.0, ground_pothole_chance=0.5,
             deposit_amount=200, deposit_style='0', deposit_size_min=2.0, deposit_size_max=5.0),
        settings={'gravel': dict(rock_subdivision=1)}),
    'BOMBED_BUILDING_CORNER': _preset(
        'combined', 'A cracked, shot-up building corner with rubble piled at its foot',
        ('fracture_concrete', 'bullet', 'corner', 'piles'),
        dict(fract_l1_amount=6, fract_l2_amount=20, fract_l3_amount=40, bullet_mode='0', bullet_material='2',
             bullet_amount=40, corner_mode='AUTO', corner_prob=100.0, corner_size_min=8.0, corner_size_max=14.0,
             corner_depth_min=4.0, corner_depth_max=7.0, build_corner_prob=100.0, pile_scale=120.0, pile_lumps=100.0)),
    'ANCIENT_TEMPLE_COLUMN': _preset(
        'combined', 'An eroded column with long cracks and crumbs at its base',
        ('erosion', 'fracture', 'piles'),
        dict(erosion_damage=20.0, erosion_noise_type='ROCKY', erosion_noise_scale=3.0, fract_surface='ROUNDED',
             fract_l1_amount=5, fract_l2_amount=15, build_corner_prob=100.0, pile_scale=150.0)),
    'RUINED_STONE_STAIRS': _preset(
        'combined', 'Broken stone stairs with craters and grass in the corners',
        ('edge', 'corner', 'surface', 'grass'),
        dict(chips_mode='AUTO', amount=220, size_min=3.0, size_max=10.0, depth_min=1.0, depth_max=4.0, sec_amount=2,
             corner_mode='AUTO', corner_prob=100.0, corner_size_min=10.0, corner_size_max=18.0,
             corner_depth_min=5.0, corner_depth_max=9.0, surf_type='0', surf_mode='PROCEDURAL', surf_amount=60),
        settings={'grass': dict(kind='DRY')}),
    'RUSTY_SHIPPING_CONTAINER': _preset(
        'combined', 'Rusted-through steel with bullet holes and dented, chipped edges',
        ('rusty', 'bullet', 'edge'),
        dict(rusty_mode='0', rusty_amount=8, rusty_size_max=7.0, bullet_mode='0', bullet_material='0',
             bullet_amount=20, bullet_metal_dent_chance=0.3, chips_mode='AUTO', amount=60, size_min=1.0,
             size_max=3.0, depth_min=0.2, depth_max=0.4)),
    'DERELICT_WORKSHOP': _preset(
        'combined', 'Old welded steel with rust holes and grime',
        ('weld', 'rusty', 'deposits'),
        dict(rusty_mode='0', rusty_amount=6, rusty_size_max=5.0, deposit_amount=50, deposit_style='1',
             deposit_size_min=0.3, deposit_size_max=0.8),
        settings={'weld': dict(edges_enable=True, edge_type='0', width=0.018, height=0.006)}),
    'HAUNTED_MANSION_WALL': _preset(
        'combined', 'Torn wallpaper over rough plaster, dust piled in the corners',
        ('wallpaper', 'plaster', 'piles'),
        dict(wallpaper_tear_amount=6, wallpaper_lift_amount=8, build_corner_prob=100.0, pile_scale=150.0)),
    'WILD_WEST_SALOON': _preset(
        'combined', 'Shot-up wooden boards with knife marks and worn edges',
        ('bullet', 'surface', 'edge'),
        dict(bullet_mode='0', bullet_material='1', bullet_amount=20, surf_type='1', surf_mode='PROCEDURAL',
             surf_amount=40, chips_mode='AUTO', amount=100, size_min=1.0, size_max=4.0, depth_min=0.3,
             depth_max=1.5, rim_soft=60.0)),
    'BATTLEFIELD_BUNKER': _preset(
        'combined', 'A cracked, shot-up bunker wall with dirt on its ledges',
        ('bullet', 'fracture_concrete', 'deposits'),
        dict(bullet_mode='0', bullet_material='2', bullet_amount=70, bullet_clusters=5, fract_l1_amount=6,
             fract_l2_amount=20, fract_l3_amount=40, deposit_amount=100, deposit_style='1', deposit_size_min=0.5,
             deposit_size_max=1.5)),
    'OVERGROWN_RUIN_BLOCK': _preset(
        'combined', 'An eroded block with grass and gravel around it',
        ('erosion', 'grass', 'gravel'),
        dict(erosion_damage=25.0, erosion_noise_type='ROCKY', erosion_noise_scale=3.0),
        settings={'grass': dict(kind='FRESH'), 'gravel': dict(rock_subdivision=1)}),
    'SHIPWRECK_PLANKS': _preset(
        'combined', 'Water-worn, damaged planks with silt settled on them',
        ('wood', 'erosion', 'deposits'),
        dict(wood_mode='0', wood_size_min=10.0, wood_size_max=24.0, wood_amount=10, erosion_damage=20.0, erosion_noise_type='SOFT', erosion_noise_scale=0.6,
             deposit_amount=60, deposit_style='2', deposit_size_min=0.5, deposit_size_max=1.5)),
    'EARTHQUAKE_SLAB': _preset(
        'combined', 'Heaved, broken ground with deep cracks through the slab',
        ('ground', 'fracture_concrete'),
        dict(ground_mode='0', ground_heave_chance=1.0, ground_crack_width=1.5, ground_crack_depth=2.0,
             fract_l1_amount=6, fract_l1_width=3.0, fract_l2_amount=12, fract_l3_amount=0)),
    'DIRTY_SHOP_WINDOW': _preset(
        'combined', 'Cracked glass with dust settled on the frame and sill',
        ('fracture_glass', 'deposits'),
        dict(fract_l1_amount=3, fract_l2_amount=20, fract_l1_chips=False, fract_l2_chips=False,
             deposit_amount=60, deposit_style='1', deposit_size_min=0.3, deposit_size_max=1.0)),
    'ALLEY_DUMPSTER': _preset(
        'combined', 'A rusty, welded steel bin with grime',
        ('rusty', 'weld', 'deposits'),
        dict(rusty_mode='0', rusty_amount=10, rusty_size_max=6.0, deposit_amount=100, deposit_style='0',
             deposit_size_min=0.5, deposit_size_max=1.5),
        settings={'weld': dict(edges_enable=True, edge_type='2', width=0.014, height=0.005)}),
    'MEDIEVAL_CASTLE_WALL': _preset(
        'combined', 'Eroded, chipped castle stone with grass at the foot',
        ('edge', 'corner', 'erosion', 'grass'),
        dict(chips_mode='AUTO', amount=200, size_min=4.0, size_max=12.0, depth_min=1.5, depth_max=5.0,
             corner_mode='AUTO', corner_prob=100.0, corner_size_min=12.0, corner_size_max=20.0,
             corner_depth_min=6.0, corner_depth_max=10.0, erosion_damage=15.0, erosion_noise_type='ROCKY',
             erosion_noise_scale=3.0),
        settings={'grass': dict(kind='FRESH')}),
    'FLOODED_BASEMENT': _preset(
        'combined', 'Peeling wallpaper, swollen plaster and silt on the floor',
        ('wallpaper', 'plaster', 'deposits'),
        dict(wallpaper_tear_amount=6, wallpaper_lift_amount=10, deposit_amount=250, deposit_style='2',
             deposit_size_min=1.0, deposit_size_max=3.0)),
    'MINE_TUNNEL_TIMBER': _preset(
        'combined', 'Split, damaged support timber covered in dust',
        ('wood', 'fracture_wood', 'deposits'),
        dict(wood_mode='0', wood_size_min=10.0, wood_size_max=24.0, wood_amount=8, fract_l1_amount=6, fract_l1_width=2.0, fract_l2_amount=15, fract_grain_x=1.0,
             fract_grain_y=0.0, fract_grain_z=0.0, deposit_amount=100, deposit_style='1', deposit_size_min=0.5,
             deposit_size_max=1.5)),
    'ZOMBIE_BARRICADE': _preset(
        'combined', 'Shot-up planks with splintered breaks and battered edges',
        ('bullet', 'wood', 'edge'),
        dict(bullet_mode='0', bullet_material='1', bullet_amount=30, wood_mode='0', wood_size_min=10.0, wood_size_max=24.0, wood_amount=8, wood_splinter_length=14.0,
             chips_mode='AUTO', amount=120, size_min=2.0, size_max=6.0, depth_min=0.5, depth_max=2.0)),

                                                                       
    'CLIMBING_IVY': _preset(
        'ivy_roots', 'Ivy climbing up a wall from three points at its foot',
        ('ivy',), settings={'ivy': dict(amount=0.6)},
        seeds={'ivy': [(0.2, 0.0, 0.02), (0.5, 0.0, 0.02), (0.8, 0.0, 0.02)]}),
    'DENSE_IVY_COVER': _preset(
        'ivy_roots', 'A thick blanket of ivy with many leaves',
        ('ivy',), settings={'ivy': dict(amount=1.0, density=0.9, branching_amount=0.8)},
        seeds={'ivy': [(0.1, 0.0, 0.02), (0.3, 0.0, 0.02), (0.5, 0.0, 0.02), (0.7, 0.0, 0.02), (0.9, 0.0, 0.02)]}),
    'YOUNG_IVY_SPROUTS': _preset(
        'ivy_roots', 'Young ivy just starting to climb, small sparse leaves',
        ('ivy',), settings={'ivy': dict(amount=0.25, density=0.4, simple_leaf_size=0.7, branch_thickness=0.6)},
        seeds={'ivy': [(0.25, 0.0, 0.02), (0.6, 0.0, 0.02), (0.85, 0.0, 0.02)]}),
    'TALL_CLIMBING_IVY': _preset(
        'ivy_roots', 'Long, straight ivy runs climbing high up the wall',
        ('ivy',), settings={'ivy': dict(amount=1.0, climb=0.9, randomness=0.2, branching_amount=0.3)},
        seeds={'ivy': [(0.3, 0.0, 0.02), (0.7, 0.0, 0.02)]}),
    'IVY_COVERED_RUIN': _preset(
        'ivy_roots', 'A chipped, broken wall with ivy growing over it',
        ('edge', 'corner', 'ivy'),
        dict(chips_mode='AUTO', amount=200, size_min=4.0, size_max=12.0, depth_min=1.5, depth_max=5.0,
             corner_mode='AUTO', corner_prob=100.0, corner_size_min=12.0, corner_size_max=20.0,
             corner_depth_min=6.0, corner_depth_max=10.0),
        settings={'ivy': dict(amount=0.7)}, seeds={'ivy': [(0.2, 0.0, 0.02), (0.75, 0.0, 0.02)]}),
    'TREE_ROOT_FLARE': _preset(
        'ivy_roots', 'Roots spreading out from the foot of a trunk',
        ('roots',), settings={'roots': dict(amount=0.6)}, seeds={'roots': [(0.5, 0.43, 0.12)]}),
    'ROOTS_OVER_ROCK': _preset(
        'ivy_roots', 'Roots running down over a boulder',
        ('roots',), settings={'roots': dict(downhill=1.0)}, seeds={'roots': [(0.5, 0.5, 1.0)]}),
    'SPREADING_SURFACE_ROOTS': _preset(
        'ivy_roots', 'Long, thin roots creeping far over the ground',
        ('roots',), settings={'roots': dict(length_amount=1.0, thickness=0.6, branching_amount=0.7)},
        seeds={'roots': [(0.5, 0.43, 0.12)]}),
    'THICK_OLD_ROOTS': _preset(
        'ivy_roots', 'A few thick, old roots',
        ('roots',), settings={'roots': dict(amount=0.3, thickness=2.5, branching_amount=0.3)},
        seeds={'roots': [(0.5, 0.43, 0.12)]}),
    'ROOTS_IN_CRACKED_GROUND': _preset(
        'ivy_roots', 'Roots spreading over cracked, heaved ground',
        ('ground', 'roots'),
        dict(ground_mode='0', ground_crack_width=1.0, ground_crack_depth=1.0, ground_heave_chance=0.6),
        settings={'roots': dict(amount=0.6)}, seeds={'roots': [(0.5, 0.43, 0.12)]}),

                                                                        
    'BLASTED_WALL_REBAR': _preset(
        'bb_walls', 'A hole blown through the wall with the reinforcement mesh left hanging',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'REBAR', 'cutters': [{'shape': 'ROCK', 'rule': 'auto', 'size': 0.3, 'count': 1, 'through': True}], 'rebar': 'auto'}),
    'SHELL_HOLES_REBAR': _preset(
        'bb_walls', 'Three shell bites in the face of the wall, rebar showing in each',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'REBAR', 'cutters': [{'shape': 'ROCK', 'rule': 'auto', 'size': 0.14, 'count': 3, 'depth': 0.3}], 'rebar': 'auto'}),
    'BREACHED_WALL_DOUBLE_MESH': _preset(
        'bb_walls', 'A breach through a wall with a mesh layer behind each face',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'REBAR', 'cutters': [{'shape': 'ROCK', 'rule': 'auto', 'size': 0.35, 'count': 1, 'through': True}], 'rebar': '2'}),
    'WIRE_MESH_SPALL': _preset(
        'bb_walls', 'Spalled patches exposing a fine wire mesh',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'REBAR', 'cutters': [{'shape': 'ROCK', 'rule': 'auto', 'size': 0.2, 'count': 2, 'depth': 0.5}], 'rebar': '5'}),
    'OLD_SMOOTH_BARS_WALL': _preset(
        'bb_walls', 'A breach in an old wall reinforced with plain round bars',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'REBAR', 'cutters': [{'shape': 'ROCK', 'rule': 'auto', 'size': 0.28, 'count': 1, 'through': True}], 'rebar': '6'}),
    'BRICK_BEHIND_RENDER': _preset(
        'bb_walls', 'Render knocked off, revealing the brickwork behind',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'BRICKS', 'cutters': [{'shape': 'ROCK', 'rule': 'auto', 'size': 0.22, 'count': 2, 'depth': 0.4}]}),
    'BRICK_WALL_BREACH': _preset(
        'bb_walls', 'A breach through a brick wall with loose bricks at the rim',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'BRICKS', 'cutters': [{'shape': 'ROCK', 'rule': 'auto', 'size': 0.3, 'count': 1, 'through': True}]}),
    'PLASTER_OVER_PLANKS': _preset(
        'bb_walls', 'Plaster broken away from the wooden boards behind it',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'PLANKS', 'cutters': [{'shape': 'ROCK', 'rule': 'auto', 'size': 0.2, 'count': 2, 'depth': 0.45}]}),
    'SMASHED_DRYWALL': _preset(
        'bb_walls', 'A hole smashed through drywall, the timber studs showing',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'FRAMING', 'cutters': [{'shape': 'ROCK', 'rule': 'auto', 'size': 0.25, 'count': 1, 'through': True}]}),
    'DRYWALL_PUNCH_HOLES': _preset(
        'bb_walls', 'Small punched holes through drywall',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'FRAMING', 'cutters': [{'shape': 'ROCK', 'rule': 'auto', 'size': 0.08, 'count': 3, 'through': True}]}),
    'EXPOSED_PIPES': _preset(
        'bb_walls', 'Wall broken open over the pipes running inside it',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'PIPES', 'cutters': [{'shape': 'ROCK', 'rule': 'auto', 'size': 0.3, 'count': 1, 'depth': 0.5}]}),
    'PIPE_CHASE_CUT': _preset(
        'bb_walls', 'A vertical chase cut into the wall along a pipe run',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'PIPES', 'cutters': [{'shape': 'ROCK', 'rule': 'auto', 'size': 0.12, 'count': 1, 'depth': 0.5, 'stretch': (0.5, 1.0, 3.0)}]}),
    'TORN_OUT_CABLES': _preset(
        'bb_walls', 'Wall torn open with electrical cables hanging out',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'CABLES', 'cutters': [{'shape': 'ROCK', 'rule': 'auto', 'size': 0.28, 'count': 1, 'depth': 0.5}]}),
    'CABLE_CHASE': _preset(
        'bb_walls', 'A chase cut along the cable run',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'CABLES', 'cutters': [{'shape': 'ROCK', 'rule': 'auto', 'size': 0.1, 'count': 1, 'depth': 0.5, 'stretch': (0.5, 1.0, 3.0)}]}),
    'CORNER_KNOCKOUT_REBAR': _preset(
        'bb_walls', 'A corner of the wall knocked out, bars bent at the break',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'REBAR', 'cutters': [{'shape': 'ROCK', 'rule': 'corner', 'size': 0.3, 'count': 1}], 'rebar': 'auto'}),
    'TOP_EDGE_COLLAPSE': _preset(
        'bb_walls', 'The top edge of the wall broken away in chunks',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'SIMPLE', 'cutters': [{'shape': 'ROCK', 'rule': 'upper', 'size': 0.3, 'count': 2, 'depth': 0.2}]}),
    'PUNCHED_ROUND_HOLE': _preset(
        'bb_walls', 'A small round hole punched through the wall',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'SIMPLE', 'cutters': [{'shape': 'ROCK', 'rule': 'auto', 'size': 0.08, 'count': 1, 'through': True}]}),
    'DOORWAY_BREACH': _preset(
        'bb_walls', 'A tall, rough opening broken through the lower wall',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'SIMPLE', 'cutters': [{'shape': 'ROCK', 'rule': 'lower', 'size': 0.3, 'count': 1, 'through': True, 'stretch': (0.8, 1.0, 1.6)}]}),
    'MULTIPLE_SMALL_BREACHES': _preset(
        'bb_walls', 'Several small holes punched through the wall',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'SIMPLE', 'cutters': [{'shape': 'ROCK', 'rule': 'auto', 'size': 0.1, 'count': 4, 'through': True}]}),
    'SPALLED_SURFACE_REBAR': _preset(
        'bb_walls', 'Shallow spalls all over the face with bars just showing',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'REBAR', 'cutters': [{'shape': 'ROCK', 'rule': 'auto', 'size': 0.1, 'count': 5, 'depth': 0.6}], 'rebar': '1'}),
    'COLLAPSED_FLOOR_REBAR': _preset(
        'bb_slabs', 'A hole through the floor slab with the reinforcement hanging into it',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'REBAR', 'cutters': [{'shape': 'ROCK', 'rule': 'auto', 'size': 0.3, 'count': 1, 'through': True, 'stretch': (1.35, 0.8, 1.0)}], 'rebar': 'auto'}),
    'FLOOR_POTHOLES_REBAR': _preset(
        'bb_slabs', 'Broken patches in a concrete floor with rebar showing',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'REBAR', 'cutters': [{'shape': 'ROCK', 'rule': 'auto', 'size': 0.12, 'count': 3, 'depth': 0.4}], 'rebar': 'auto'}),
    'PUNCHED_THROUGH_SLAB': _preset(
        'bb_slabs', 'A rough hole punched through the slab',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'SIMPLE', 'cutters': [{'shape': 'ROCK', 'rule': 'auto', 'size': 0.2, 'count': 1, 'through': True}]}),
    'BROKEN_SLAB_EDGE': _preset(
        'bb_slabs', 'The edge of the slab broken away, bars sticking out',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'REBAR', 'cutters': [{'shape': 'ROCK', 'rule': 'side', 'size': 0.25, 'count': 2, 'depth': 0.3}], 'rebar': 'auto'}),
    'PRESTRESSED_SLAB_BREAK': _preset(
        'bb_slabs', 'A break through a prestressed slab with the strands exposed',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'REBAR', 'cutters': [{'shape': 'ROCK', 'rule': 'auto', 'size': 0.3, 'count': 1, 'through': True}], 'rebar': '8'}),
    'FLOORBOARD_HOLE': _preset(
        'bb_slabs', 'A hole through the floor showing the boards below',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'PLANKS', 'cutters': [{'shape': 'ROCK', 'rule': 'auto', 'size': 0.25, 'count': 1, 'through': True}]}),
    'FLOOR_PIPES_EXPOSED': _preset(
        'bb_slabs', 'Floor broken open over the pipes laid in it',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'PIPES', 'cutters': [{'shape': 'ROCK', 'rule': 'auto', 'size': 0.32, 'count': 1, 'depth': 0.6, 'stretch': (1.7, 0.8, 1.0)}]}),
    'FLOOR_CABLE_TRENCH': _preset(
        'bb_slabs', 'A trench cut in the floor along the cables',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'CABLES', 'cutters': [{'shape': 'ROCK', 'rule': 'auto', 'size': 0.08, 'count': 1, 'depth': 0.2, 'stretch': (4.0, 0.6, 1.0)}]}),
    'PUNCHED_FLOOR_HOLES': _preset(
        'bb_slabs', 'Three small round holes punched through the slab',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'SIMPLE', 'cutters': [{'shape': 'ROCK', 'rule': 'auto', 'size': 0.05, 'count': 3, 'through': True}]}),
    'SLAB_CORNER_BREAK': _preset(
        'bb_slabs', 'A corner of the slab broken off with bars bent out',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'REBAR', 'cutters': [{'shape': 'ROCK', 'rule': 'corner', 'size': 0.4, 'count': 1}], 'rebar': 'auto'}),
    'SINKHOLE': _preset(
        'bb_slabs', 'A round collapse through the middle of the slab',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'SIMPLE', 'cutters': [{'shape': 'ROCK', 'rule': 'auto', 'size': 0.35, 'count': 1, 'through': True}]}),
    'WIRE_MESH_SCREED_DAMAGE': _preset(
        'bb_slabs', 'Broken screed patches over a fine wire mesh',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'REBAR', 'cutters': [{'shape': 'ROCK', 'rule': 'auto', 'size': 0.1, 'count': 4, 'depth': 0.6}], 'rebar': '5'}),
    'TRENCH_CUT': _preset(
        'bb_slabs', 'A long trench cut across the slab',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'SIMPLE', 'cutters': [{'shape': 'ROCK', 'rule': 'auto', 'size': 0.1, 'count': 1, 'depth': 0.3, 'stretch': (4.0, 0.6, 1.0)}]}),
    'BRICK_VAULT_FLOOR': _preset(
        'bb_slabs', 'A hole through the floor down to a brick layer',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'BRICKS', 'cutters': [{'shape': 'ROCK', 'rule': 'auto', 'size': 0.22, 'count': 2, 'through': True}]}),
    'LARGE_FLOOR_OPENING': _preset(
        'bb_slabs', 'A large rough opening broken through the slab',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'SIMPLE', 'cutters': [{'shape': 'ROCK', 'rule': 'auto', 'size': 0.3, 'count': 1, 'through': True, 'stretch': (1.2, 0.95, 1.0)}]}),
    'SPALLED_COLUMN_CAGE': _preset(
        'bb_columns', 'Spalled patches on a column exposing the bar cage',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'REBAR', 'cutters': [{'shape': 'ROCK', 'rule': 'side', 'size': 0.8, 'count': 2}], 'rebar': 'auto'}),
    'COLUMN_CORE_EXPOSED': _preset(
        'bb_columns', 'One big bite into the column down to its core',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'REBAR', 'cutters': [{'shape': 'ROCK', 'rule': 'side', 'size': 1.28, 'count': 1, 'depth': 0.2}], 'rebar': 'auto'}),
    'SNAPPED_COLUMN_TOP': _preset(
        'bb_columns', 'The top of the column snapped off at an angle',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'REBAR', 'cutters': [{'shape': 'ROCK', 'rule': 'slice', 'size': 0.24, 'count': 1, 'at': 0.7, 'tilt': 0.4}], 'rebar': 'auto'}),
    'ROUND_PIER_SPALL': _preset(
        'bb_columns', 'Spalls on a round pier showing the round cage',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'REBAR', 'cutters': [{'shape': 'ROCK', 'rule': 'side', 'size': 0.6, 'count': 3}], 'rebar': 'auto'}),
    'SNAPPED_ROUND_PIER': _preset(
        'bb_columns', 'A round pier snapped off near the top',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'REBAR', 'cutters': [{'shape': 'ROCK', 'rule': 'slice', 'size': 0.24, 'count': 1, 'at': 0.65, 'tilt': 0.45}], 'rebar': 'auto'}),
    'SPIRAL_CAGE_EXPOSED': _preset(
        'bb_columns', 'Several bites showing a spiral-bound round cage',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'REBAR', 'cutters': [{'shape': 'ROCK', 'rule': 'side', 'size': 0.72, 'count': 3, 'depth': 0.4}], 'rebar': '4'}),
    'BEAM_UNDERSIDE_SPALL': _preset(
        'bb_columns', 'Spalls along the face of a beam',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'REBAR', 'cutters': [{'shape': 'ROCK', 'rule': 'side', 'size': 0.55, 'count': 3, 'stretch': (1.7, 0.8, 1.0)}], 'rebar': 'auto'}),
    'BROKEN_BEAM_END': _preset(
        'bb_columns', 'The end of a beam broken off',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'REBAR', 'cutters': [{'shape': 'ROCK', 'rule': 'slice', 'size': 0.24, 'count': 1, 'at': 0.85, 'tilt': 0.5}], 'rebar': 'auto'}),
    'COLUMN_BASE_DAMAGE': _preset(
        'bb_columns', 'The foot of a column smashed in',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'REBAR', 'cutters': [{'shape': 'ROCK', 'rule': 'lower', 'size': 0.96, 'count': 2}], 'rebar': 'auto'}),
    'COLUMN_CAPITAL_DAMAGE': _preset(
        'bb_columns', 'Chunks broken off the head of a column',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'SIMPLE', 'cutters': [{'shape': 'ROCK', 'rule': 'upper', 'size': 0.8, 'count': 2}]}),
    'CHIPPED_COLUMN_CORNERS': _preset(
        'bb_columns', 'Corners of a square column knocked off',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'SIMPLE', 'cutters': [{'shape': 'ROCK', 'rule': 'corner', 'size': 0.56, 'count': 4}]}),
    'SHEARED_COLUMN': _preset(
        'bb_columns', 'A column sheared through at mid height',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'SIMPLE', 'cutters': [{'shape': 'ROCK', 'rule': 'slice', 'size': 0.24, 'count': 1, 'at': 0.5, 'tilt': 0.9}]}),
    'BRICK_PILLAR_DAMAGE': _preset(
        'bb_columns', 'Render broken off a pillar, the bricks showing',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'BRICKS', 'cutters': [{'shape': 'ROCK', 'rule': 'side', 'size': 0.96, 'count': 2}]}),
    'DEEP_BITES_PIER': _preset(
        'bb_columns', 'Deep, narrow bites taken out of a round pier',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'SIMPLE', 'cutters': [{'shape': 'ROCK', 'stretch': (1.0, 1.0, 1.6), 'rule': 'side', 'size': 0.4, 'count': 3, 'depth': 0.6}]}),
    'SHATTERED_PILLAR': _preset(
        'bb_columns', 'A pillar broken at the top and bitten along its sides',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'REBAR', 'cutters': [{'shape': 'ROCK', 'rule': 'side', 'size': 0.72, 'count': 4}, {'shape': 'ROCK', 'rule': 'slice', 'size': 0.24, 'count': 1, 'at': 0.8, 'tilt': 0.5}], 'rebar': 'auto'}),
    'STATUE_ARMATURE_EXPOSED': _preset(
        'bb_statues', 'Broken limbs revealing the armature inside a statue',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'REBAR', 'cutters': [{'shape': 'ROCK', 'rule': 'extremity', 'size': 0.2, 'count': 2, 'depth': 0.3}], 'rebar': '9'}),
    'BROKEN_STATUE_HEAD': _preset(
        'bb_statues', 'The head of a statue broken off, the armature showing',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'REBAR', 'cutters': [{'shape': 'ROCK', 'rule': 'slice', 'size': 0.15, 'count': 1, 'at': 0.8, 'tilt': 0.5}], 'rebar': 'auto'}),
    'SCULPTURE_WIRE_MESH': _preset(
        'bb_statues', 'Broken patches over the fine wire mesh of a sculpture',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'REBAR', 'cutters': [{'shape': 'ROCK', 'rule': 'extremity', 'size': 0.12, 'count': 3, 'depth': 0.4}], 'rebar': '10'}),
    'CHIPPED_STATUE': _preset(
        'bb_statues', 'Small chips knocked off the outermost parts of a statue',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'SIMPLE', 'cutters': [{'shape': 'ROCK', 'rule': 'extremity', 'size': 0.1, 'count': 4, 'depth': 0.5}]}),
    'VANDALIZED_STATUE': _preset(
        'bb_statues', 'Two big chunks smashed off a statue',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'SIMPLE', 'cutters': [{'shape': 'ROCK', 'rule': 'extremity', 'size': 0.2, 'count': 2, 'depth': 0.2}]}),
    'MISSING_DETAILS': _preset(
        'bb_statues', 'Nose, fingers and other small details broken off',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'SIMPLE', 'cutters': [{'shape': 'ROCK', 'rule': 'extremity', 'size': 0.08, 'count': 3, 'depth': 0.0}]}),
    'ERODED_BROKEN_STATUE': _preset(
        'bb_statues', 'A weathered statue with broken-off parts',
        ('boolean', 'erosion'), {'erosion_damage': 20.0, 'erosion_noise_type': 'SOFT', 'erosion_noise_scale': 0.6}, settings={}, seeds={},
        breakers={'variant': 'SIMPLE', 'cutters': [{'shape': 'ROCK', 'rule': 'extremity', 'size': 0.15, 'count': 2}]}),
    'CRACKED_BROKEN_STATUE': _preset(
        'bb_statues', 'Cracks running over a statue with broken parts and armature',
        ('boolean', 'fracture'), {'fract_surface': 'ROUNDED', 'fract_l1_amount': 4, 'fract_l2_amount': 12}, settings={}, seeds={},
        breakers={'variant': 'REBAR', 'cutters': [{'shape': 'ROCK', 'rule': 'extremity', 'size': 0.15, 'count': 2}], 'rebar': '9'}),
    'HEADLESS_STATUE': _preset(
        'bb_statues', 'A statue with its head broken clean off',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'SIMPLE', 'cutters': [{'shape': 'ROCK', 'rule': 'slice', 'size': 0.15, 'count': 1, 'at': 0.78, 'tilt': 0.2}]}),
    'PEDESTAL_DAMAGE': _preset(
        'bb_statues', 'Chunks broken out of the base of a statue',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'SIMPLE', 'cutters': [{'shape': 'ROCK', 'rule': 'lower', 'size': 0.15, 'count': 3}]}),
    'STATUE_SHOT_DAMAGE': _preset(
        'bb_statues', 'A statue hit by gunfire, one part shot away',
        ('boolean', 'bullet'), {'bullet_mode': '0', 'bullet_material': '2', 'bullet_amount': 40}, settings={}, seeds={},
        breakers={'variant': 'SIMPLE', 'cutters': [{'shape': 'ROCK', 'rule': 'extremity', 'size': 0.15, 'count': 1}]}),
    'HOLLOW_CAST_STATUE': _preset(
        'bb_statues', 'Deep breaks into a statue, as if it were hollow cast',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'SIMPLE', 'cutters': [{'shape': 'ROCK', 'rule': 'extremity', 'size': 0.25, 'count': 2, 'depth': 0.6}]}),
    'ANCIENT_TORSO': _preset(
        'bb_statues', 'Head and limbs gone, like an ancient torso',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'REBAR', 'cutters': [{'shape': 'ROCK', 'rule': 'slice', 'size': 0.15, 'count': 1, 'at': 0.82, 'tilt': 0.3}, {'shape': 'ROCK', 'rule': 'extremity', 'size': 0.2, 'count': 2}], 'rebar': '9'}),
    'WEATHERED_MONUMENT': _preset(
        'bb_statues', 'Rocky erosion with chunks missing from the top',
        ('boolean', 'erosion'), {'erosion_damage': 15.0, 'erosion_noise_type': 'ROCKY', 'erosion_noise_scale': 3.0}, settings={}, seeds={},
        breakers={'variant': 'SIMPLE', 'cutters': [{'shape': 'ROCK', 'rule': 'upper', 'size': 0.12, 'count': 3}]}),
    'SPLIT_SCULPTURE': _preset(
        'bb_statues', 'A sculpture split down its length',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'SIMPLE', 'cutters': [{'shape': 'ROCK', 'rule': 'slice', 'size': 0.15, 'count': 1, 'at': 0.5, 'tilt': 1.4}]}),
    'KNOCKED_BLOCK_CORNERS': _preset(
        'bb_blocks', 'Three corners of a block knocked off',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'SIMPLE', 'cutters': [{'shape': 'ROCK', 'rule': 'corner', 'size': 0.2, 'count': 3}]}),
    'CORNER_BREAK_REBAR': _preset(
        'bb_blocks', 'Corners broken off a concrete block with the bars bent out',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'REBAR', 'cutters': [{'shape': 'ROCK', 'rule': 'corner', 'size': 0.35, 'count': 2}], 'rebar': 'auto'}),
    'TOP_CRATER_BLOCK': _preset(
        'bb_blocks', 'A round crater smashed into the top',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'SIMPLE', 'cutters': [{'shape': 'ROCK', 'rule': 'top', 'size': 0.35, 'count': 1, 'depth': 0.2}]}),
    'BROKEN_STEP_NOSING': _preset(
        'bb_blocks', 'Step edges broken away',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'SIMPLE', 'cutters': [{'shape': 'ROCK', 'rule': 'upper', 'size': 0.15, 'count': 3}]}),
    'CRUMBLED_BARRIER': _preset(
        'bb_blocks', 'The top of a barrier crumbled, rebar showing',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'REBAR', 'cutters': [{'shape': 'ROCK', 'rule': 'upper', 'size': 0.3, 'count': 2}], 'rebar': 'auto'}),
    'CRACKED_OPEN_PLINTH': _preset(
        'bb_blocks', 'A big piece of a plinth broken away',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'SIMPLE', 'cutters': [{'shape': 'ROCK', 'rule': 'corner', 'size': 0.5, 'count': 1}]}),
    'BITTEN_EDGES': _preset(
        'bb_blocks', 'Bites taken out of the sides',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'SIMPLE', 'cutters': [{'shape': 'ROCK', 'rule': 'side', 'size': 0.15, 'count': 4}]}),
    'GOUGED_TOP': _preset(
        'bb_blocks', 'Deep round pits gouged into the top',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'SIMPLE', 'cutters': [{'shape': 'ROCK', 'rule': 'top', 'size': 0.12, 'count': 2, 'depth': 0.5}]}),
    'SHATTERED_CORNER_BLOCK': _preset(
        'bb_blocks', 'All corners shattered',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'SIMPLE', 'cutters': [{'shape': 'ROCK', 'rule': 'corner', 'size': 0.25, 'count': 4}]}),
    'HOLLOW_CORE_BLOCK': _preset(
        'bb_blocks', 'A deep hollow broken down into the block',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'SIMPLE', 'cutters': [{'shape': 'ROCK', 'rule': 'top', 'size': 0.25, 'count': 1, 'depth': 1.0, 'stretch': (1.0, 1.0, 2.0)}]}),
    'BRICK_FILLED_BLOCK': _preset(
        'bb_blocks', 'Corners broken off a rendered block, bricks inside',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'BRICKS', 'cutters': [{'shape': 'ROCK', 'rule': 'corner', 'size': 0.35, 'count': 2}]}),
    'PLANKED_CRATE_BREAK': _preset(
        'bb_blocks', 'A box broken open on its sides, boards inside',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'PLANKS', 'cutters': [{'shape': 'ROCK', 'rule': 'side', 'size': 0.3, 'count': 2}]}),
    'DRYWALL_BOX_BREAK': _preset(
        'bb_blocks', 'A drywall box broken open, studs inside',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'FRAMING', 'cutters': [{'shape': 'ROCK', 'rule': 'side', 'size': 0.35, 'count': 1}]}),
    'PIPES_IN_PLINTH': _preset(
        'bb_blocks', 'A plinth broken open over the pipes inside it',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'PIPES', 'cutters': [{'shape': 'ROCK', 'rule': 'side', 'size': 0.4, 'count': 1}]}),
    'CABLES_IN_PLINTH': _preset(
        'bb_blocks', 'A plinth broken open with cables hanging out',
        ('boolean',), {}, settings={}, seeds={},
        breakers={'variant': 'CABLES', 'cutters': [{'shape': 'ROCK', 'rule': 'side', 'size': 0.4, 'count': 1}]}),
    'WAR_DAMAGED_WALL': _preset(
        'bb_combined', 'A breached wall with rebar, bullet holes and battered edges',
        ('boolean', 'edge', 'corner', 'bullet'), {'chips_mode': 'AUTO', 'amount': 160, 'size_min': 3.0, 'size_max': 9.0, 'depth_min': 1.0, 'depth_max': 3.5, 'corner_mode': 'AUTO', 'corner_prob': 100.0, 'corner_size_min': 10.0, 'corner_size_max': 16.0, 'corner_depth_min': 5.0, 'corner_depth_max': 8.0, 'bullet_mode': '0', 'bullet_material': '2', 'bullet_amount': 50, 'bullet_clusters': 4}, settings={}, seeds={},
        breakers={'variant': 'REBAR', 'cutters': [{'shape': 'ROCK', 'rule': 'auto', 'size': 0.28, 'count': 1, 'through': True}], 'rebar': 'auto'}),
    'RUINED_BUILDING_WALL': _preset(
        'bb_combined', 'A broken wall with chipped edges and dirt on its ledges',
        ('boolean', 'edge', 'deposits'), {'chips_mode': 'AUTO', 'amount': 160, 'size_min': 3.0, 'size_max': 9.0, 'depth_min': 1.0, 'depth_max': 3.5, 'deposit_amount': 120, 'deposit_style': '0', 'deposit_size_min': 1.0, 'deposit_size_max': 3.0}, settings={}, seeds={},
        breakers={'variant': 'REBAR', 'cutters': [{'shape': 'ROCK', 'rule': 'upper', 'size': 0.3, 'count': 2}], 'rebar': 'auto'}),
    'OVERGROWN_BREACH': _preset(
        'bb_combined', 'A hole through a wall with ivy climbing around it',
        ('boolean', 'ivy'), {}, settings={'ivy': {'amount': 0.6}}, seeds={'ivy': [(0.2, 0.0, 0.02), (0.5, 0.0, 0.02), (0.8, 0.0, 0.02)]},
        breakers={'variant': 'SIMPLE', 'cutters': [{'shape': 'ROCK', 'rule': 'auto', 'size': 0.25, 'count': 1, 'through': True}]}),
    'EARTHQUAKE_COLUMN': _preset(
        'bb_combined', 'A cracked column with spalls exposing the cage',
        ('boolean', 'fracture_concrete'), {'fract_l1_amount': 6, 'fract_l2_amount': 20, 'fract_l3_amount': 0}, settings={}, seeds={},
        breakers={'variant': 'REBAR', 'cutters': [{'shape': 'ROCK', 'rule': 'side', 'size': 0.8, 'count': 2}], 'rebar': 'auto'}),
    'ABANDONED_ROOM_BREACH': _preset(
        'bb_combined', 'Peeling wallpaper over a wall broken through to the bricks',
        ('boolean', 'wallpaper'), {'wallpaper_tear_amount': 5, 'wallpaper_lift_amount': 8}, settings={}, seeds={},
        breakers={'variant': 'BRICKS', 'cutters': [{'shape': 'ROCK', 'rule': 'auto', 'size': 0.25, 'count': 2, 'depth': 0.4}]}),
    'BOMBED_FLOOR': _preset(
        'bb_combined', 'A floor with a hole blown through it, cracks and debris around',
        ('boolean', 'ground', 'deposits'), {'ground_mode': '0', 'ground_crack_width': 1.0, 'ground_crack_depth': 1.0, 'deposit_amount': 120, 'deposit_style': '0', 'deposit_size_min': 1.0, 'deposit_size_max': 3.0}, settings={}, seeds={},
        breakers={'variant': 'REBAR', 'cutters': [{'shape': 'ROCK', 'rule': 'auto', 'size': 0.3, 'count': 1, 'through': True}], 'rebar': 'auto'}),
    'RUINED_TEMPLE_COLUMN': _preset(
        'bb_combined', 'A snapped, eroded column with ivy',
        ('boolean', 'erosion', 'ivy'), {'erosion_damage': 15.0, 'erosion_noise_type': 'ROCKY', 'erosion_noise_scale': 3.0}, settings={'ivy': {'amount': 0.5}}, seeds={'ivy': [(0.5, 0.0, 0.02)]},
        breakers={'variant': 'SIMPLE', 'cutters': [{'shape': 'ROCK', 'rule': 'slice', 'size': 0.15, 'count': 1, 'at': 0.7, 'tilt': 0.45}]}),
    'OVERGROWN_STATUE': _preset(
        'bb_combined', 'A broken statue with ivy growing up its base',
        ('boolean', 'ivy'), {}, settings={'ivy': {'amount': 0.5}}, seeds={'ivy': [(0.3, 0.0, 0.02), (0.7, 0.0, 0.02)]},
        breakers={'variant': 'SIMPLE', 'cutters': [{'shape': 'ROCK', 'rule': 'extremity', 'size': 0.15, 'count': 2}]}),
    'ANCIENT_STATUE_RUIN': _preset(
        'bb_combined', 'An eroded, cracked statue with broken parts and armature',
        ('boolean', 'erosion', 'fracture'), {'erosion_damage': 12.0, 'erosion_noise_type': 'SOFT', 'erosion_noise_scale': 0.6, 'fract_surface': 'ROUNDED', 'fract_l1_amount': 3, 'fract_l2_amount': 10}, settings={}, seeds={},
        breakers={'variant': 'REBAR', 'cutters': [{'shape': 'ROCK', 'rule': 'extremity', 'size': 0.15, 'count': 2}], 'rebar': '9'}),
    'FIREFIGHT_DRYWALL': _preset(
        'bb_combined', 'Drywall smashed through and peppered with bullet holes',
        ('boolean', 'bullet'), {'bullet_mode': '0', 'bullet_material': '2', 'bullet_amount': 50, 'bullet_clusters': 4}, settings={}, seeds={},
        breakers={'variant': 'FRAMING', 'cutters': [{'shape': 'ROCK', 'rule': 'auto', 'size': 0.22, 'count': 1, 'through': True}]}),
    'FLOODED_BASEMENT_BREACH': _preset(
        'bb_combined', 'A wall broken open over the pipes, silt on every ledge',
        ('boolean', 'plaster', 'deposits'), {'deposit_amount': 150, 'deposit_style': '2', 'deposit_size_min': 1.0, 'deposit_size_max': 3.0}, settings={}, seeds={},
        breakers={'variant': 'PIPES', 'cutters': [{'shape': 'ROCK', 'rule': 'auto', 'size': 0.3, 'count': 1, 'depth': 0.5}]}),
    'DERELICT_SERVICE_WALL': _preset(
        'bb_combined', 'Cables torn out of a dirty wall',
        ('boolean', 'deposits'), {'chips_mode': 'AUTO', 'amount': 160, 'size_min': 3.0, 'size_max': 9.0, 'depth_min': 1.0, 'depth_max': 3.5, 'deposit_amount': 120, 'deposit_style': '0', 'deposit_size_min': 1.0, 'deposit_size_max': 3.0}, settings={}, seeds={},
        breakers={'variant': 'CABLES', 'cutters': [{'shape': 'ROCK', 'rule': 'auto', 'size': 0.28, 'count': 1, 'depth': 0.5}]}),
    'SHELLED_BUNKER': _preset(
        'bb_combined', 'Shell bites with rebar, bullet holes and cracks',
        ('boolean', 'bullet', 'fracture_concrete'), {'bullet_mode': '0', 'bullet_material': '2', 'bullet_amount': 50, 'bullet_clusters': 4, 'fract_l1_amount': 5, 'fract_l2_amount': 15, 'fract_l3_amount': 0}, settings={}, seeds={},
        breakers={'variant': 'REBAR', 'cutters': [{'shape': 'ROCK', 'rule': 'auto', 'size': 0.15, 'count': 3, 'depth': 0.3}], 'rebar': 'auto'}),
    'COLLAPSED_STAIRS': _preset(
        'bb_combined', 'Stairs broken at the top with rebar, chips and rubble in the corners',
        ('boolean', 'edge', 'corner', 'piles'), {'chips_mode': 'AUTO', 'amount': 160, 'size_min': 3.0, 'size_max': 9.0, 'depth_min': 1.0, 'depth_max': 3.5, 'corner_mode': 'AUTO', 'corner_prob': 100.0, 'corner_size_min': 10.0, 'corner_size_max': 16.0, 'corner_depth_min': 5.0, 'corner_depth_max': 8.0, 'build_corner_prob': 100.0, 'pile_scale': 150.0}, settings={}, seeds={},
        breakers={'variant': 'REBAR', 'cutters': [{'shape': 'ROCK', 'rule': 'upper', 'size': 0.2, 'count': 2}], 'rebar': 'auto'}),
    'OVERGROWN_SLAB_HOLE': _preset(
        'bb_combined', 'A hole through a slab with grass and gravel around it',
        ('boolean', 'grass', 'gravel'), {}, settings={'grass': {'kind': 'FRESH'}, 'gravel': {'rock_subdivision': 1}}, seeds={},
        breakers={'variant': 'SIMPLE', 'cutters': [{'shape': 'ROCK', 'rule': 'auto', 'size': 0.25, 'count': 1, 'through': True}]}),
    'RUSTED_REBAR_RUIN': _preset(
        'bb_combined', 'An eroded old wall with plain bars in the breach',
        ('boolean', 'erosion'), {'erosion_damage': 12.0, 'erosion_noise_type': 'ROCKY', 'erosion_noise_scale': 3.0}, settings={}, seeds={},
        breakers={'variant': 'REBAR', 'cutters': [{'shape': 'ROCK', 'rule': 'auto', 'size': 0.28, 'count': 1, 'through': True}], 'rebar': '6'}),
    'CRUMBLING_PIER': _preset(
        'bb_combined', 'An eroded round pier with spalls and grime',
        ('boolean', 'erosion', 'deposits'), {'erosion_damage': 12.0, 'erosion_noise_type': 'ROCKY', 'erosion_noise_scale': 3.0, 'deposit_amount': 120, 'deposit_style': '0', 'deposit_size_min': 1.0, 'deposit_size_max': 3.0}, settings={}, seeds={},
        breakers={'variant': 'REBAR', 'cutters': [{'shape': 'ROCK', 'rule': 'side', 'size': 0.8, 'count': 2}], 'rebar': 'auto'}),
    'MOSSY_BROKEN_STATUE': _preset(
        'bb_combined', 'A broken statue with grass growing at its foot',
        ('boolean', 'grass'), {}, settings={'grass': {'kind': 'FRESH'}}, seeds={},
        breakers={'variant': 'SIMPLE', 'cutters': [{'shape': 'ROCK', 'rule': 'extremity', 'size': 0.15, 'count': 2}]}),
    'BROKEN_BRICK_WALL_DUST': _preset(
        'bb_combined', 'A brick wall breach with chipped edges and dust on top',
        ('boolean', 'edge', 'deposits'), {'chips_mode': 'AUTO', 'amount': 160, 'size_min': 3.0, 'size_max': 9.0, 'depth_min': 1.0, 'depth_max': 3.5, 'deposit_amount': 80, 'deposit_style': '1', 'deposit_size_min': 0.5, 'deposit_size_max': 1.5}, settings={}, seeds={},
        breakers={'variant': 'BRICKS', 'cutters': [{'shape': 'ROCK', 'rule': 'auto', 'size': 0.3, 'count': 1, 'through': True}]}),
    'BATTLE_SCARRED_PILLAR': _preset(
        'bb_combined', 'A pillar with spalls, rebar, bullet holes and battered edges',
        ('boolean', 'bullet', 'edge'), {'bullet_mode': '0', 'bullet_material': '2', 'bullet_amount': 50, 'bullet_clusters': 4, 'chips_mode': 'AUTO', 'amount': 160, 'size_min': 3.0, 'size_max': 9.0, 'depth_min': 1.0, 'depth_max': 3.5}, settings={}, seeds={},
        breakers={'variant': 'REBAR', 'cutters': [{'shape': 'ROCK', 'rule': 'side', 'size': 0.72, 'count': 2}], 'rebar': 'auto'}),

    'LIGHT_CONCRETE_SPALLS': _preset(
        'asset_concrete', 'Small shallow spalls in the concrete skin; no steel showing',
        ('concrete',),
        dict(concrete_amount=9, concrete_size_min=12.0, concrete_size_max=28.0, concrete_depth_min=0.8,
             concrete_depth_max=2.0, concrete_terraces=1, concrete_rebar_enable=False, concrete_crack_amount=2)),
    'REBAR_EXPOSED_SPALLS': _preset(
        'asset_concrete', 'Deep spalls with a full reinforcement mesh showing',
        ('concrete',),
        dict(concrete_amount=4, concrete_size_min=45.0, concrete_size_max=85.0, concrete_depth_min=3.0,
             concrete_depth_max=6.0, concrete_rebar_expose=1.0, concrete_rebar_radius=0.8,
             concrete_rebar_spacing_x=15.0, concrete_rebar_spacing_y=15.0)),
    'EDGE_BROKEN_CONCRETE': _preset(
        'asset_concrete', 'Spalls gathered along the outer edges of a wall or slab',
        ('concrete',),
        dict(concrete_amount=8, concrete_size_min=25.0, concrete_size_max=55.0, concrete_edge_bias=3.0,
             concrete_stretch=2.4, concrete_depth_min=2.0, concrete_depth_max=5.0, concrete_rebar_spacing_x=18.0,
             concrete_rebar_spacing_y=18.0)),
    'DEEP_CONCRETE_POCKETS': _preset(
        'asset_concrete', 'Deep, stepped pockets knocked into thick concrete, with torn bars inside',
        ('concrete',),
        dict(concrete_amount=3, concrete_size_min=40.0, concrete_size_max=60.0, concrete_depth_min=6.0,
             concrete_depth_max=10.0, concrete_terraces=3, concrete_thickness_cap=0.8, concrete_rebar_broken=0.7,
             concrete_rebar_radius=0.7, concrete_rebar_expose=0.9)),
    'CONCRETE_CRACK_WEB': _preset(
        'asset_concrete', 'A few spalls feeding a long, branching web of cracks',
        ('concrete',),
        dict(concrete_amount=4, concrete_size_min=20.0, concrete_size_max=40.0, concrete_crack_amount=24,
             concrete_crack_length_min=40.0, concrete_crack_length_max=100.0, concrete_crack_width=0.7, concrete_crack_depth=1.5,
             concrete_crack_branch=0.8, concrete_crack_interior=0.6, concrete_rebar_enable=False)),
    'COARSE_AGGREGATE_SPALLS': _preset(
        'asset_concrete', 'Rough breaks with big stones in the fracture face',
        ('concrete',),
        dict(concrete_amount=6, concrete_size_min=25.0, concrete_size_max=55.0, concrete_roughness=1.5,
             concrete_aggregate_size=2.2, concrete_surface_strength=220.0, concrete_surface_detail=5.0,
             concrete_rebar_enable=False, concrete_jagged=1.0)),
    'STEPPED_CONCRETE_LAYERS': _preset(
        'asset_concrete', 'Spalls that step down in clear layers, cover then core',
        ('concrete',),
        dict(concrete_amount=4, concrete_size_min=40.0, concrete_size_max=75.0, concrete_terraces=4,
             concrete_terrace_skin=0.5, concrete_terrace_inset=0.3, concrete_depth_min=4.0, concrete_depth_max=8.0,
             concrete_rebar_expose=0.5)),
    'TWISTED_BROKEN_REBAR': _preset(
        'asset_concrete', 'Blast-bent, snapped bars sticking out of the breaks',
        ('concrete',),
        dict(concrete_amount=3, concrete_size_min=55.0, concrete_size_max=90.0, concrete_depth_min=4.0,
             concrete_depth_max=7.0, concrete_rebar_bend=1.6, concrete_rebar_broken=1.0, concrete_rebar_wobble=0.9,
             concrete_rebar_jitter=0.8, concrete_rebar_radius=0.9, concrete_rebar_spacing_x=20.0,
             concrete_rebar_spacing_y=20.0, concrete_rebar_expose=1.0)),
    'SINGLE_MESH_DIAGONAL': _preset(
        'asset_concrete', 'One large spall over thin, dense wire reinforcement set at an angle',
        ('concrete',),
        dict(concrete_amount=1, concrete_size_min=85.0, concrete_size_max=100.0, concrete_depth_min=2.0,
             concrete_depth_max=3.5, concrete_rebar_radius=0.3, concrete_rebar_spacing_x=6.0,
             concrete_rebar_spacing_y=6.0, concrete_rebar_angle=45.0, concrete_rebar_cover=1.2,
             concrete_rebar_expose=1.0, concrete_rebar_ribs=0.0)),
    'V2_SOFT_SPALL_POCKETS': _preset(
        'asset_concrete', 'Concrete V2: smooth-rimmed, weathered spall pockets',
        ('concrete_v2',),
        dict(concrete_amount=6, concrete_size_min=25.0, concrete_size_max=50.0, concrete_wall_soft=1.2,
             concrete_jagged=0.25, concrete_smooth_width=1.0, concrete_rebar_enable=False, concrete_crack_amount=1)),
    'V2_JAGGED_SPALLS': _preset(
        'asset_concrete', 'Concrete V2: sharp, jagged outlines around fresh breaks',
        ('concrete_v2',),
        dict(concrete_amount=5, concrete_size_min=35.0, concrete_size_max=70.0, concrete_jagged=1.8,
             concrete_outline_scale=1.8, concrete_lobes=7, concrete_wall_soft=0.1, concrete_depth_min=3.0,
             concrete_depth_max=6.0)),
    'V2_DEEP_RC_DAMAGE': _preset(
        'asset_concrete', 'Concrete V2: deep structural damage with a double row of heavy bars',
        ('concrete_v2',),
        dict(concrete_amount=3, concrete_size_min=60.0, concrete_size_max=90.0, concrete_depth_min=6.0,
             concrete_depth_max=11.0, concrete_terraces=3, concrete_rebar_radius=1.1, concrete_rebar_spacing_x=18.0,
             concrete_rebar_spacing_y=22.0, concrete_rebar_expose=1.0, concrete_rebar_bend=0.7, concrete_rebar_sides=12)),
    'V2_VERTICAL_BARS_ONLY': _preset(
        'asset_concrete', 'Concrete V2: tall spalls exposing vertical bars only',
        ('concrete_v2',),
        dict(concrete_amount=4, concrete_size_min=40.0, concrete_size_max=70.0, concrete_stretch=3.0,
             concrete_rebar_bars_x=False, concrete_rebar_spacing_x=14.0, concrete_rebar_spacing_y=14.0,
             concrete_rebar_radius=0.8, concrete_rebar_expose=1.0, concrete_depth_min=3.0, concrete_depth_max=6.0)),
    'V2_MANY_SMALL_SPALLS': _preset(
        'asset_concrete', 'Concrete V2: dozens of small scattered chips and pockets',
        ('concrete_v2',),
        dict(concrete_amount=36, concrete_size_min=8.0, concrete_size_max=20.0, concrete_depth_min=0.8,
             concrete_depth_max=2.2, concrete_terraces=1, concrete_rebar_enable=False, concrete_crack_amount=0)),
    'V2_ROUGH_ERODED_FACE': _preset(
        'asset_concrete', 'Concrete V2: wide areas eaten away, with a strongly noised face',
        ('concrete_v2',),
        dict(concrete_amount=3, concrete_size_min=60.0, concrete_size_max=95.0, concrete_depth_min=3.0,
             concrete_depth_max=5.5, concrete_surface_strength=400.0, concrete_surface_distortion=1.4,
             concrete_surface_roughness=90.0, concrete_surface_detail=6.0, concrete_rebar_enable=False)),
    'FORMWORK_PINHOLES': _preset(
        'asset_concrete', 'Concrete Pitting: fine pinhole pores of cast formwork concrete, no flaking',
        ('pits',),
        dict(pits_strength=1.4, pits_pore_density=20.0, pits_pore_size_min=0.4, pits_pore_size_max=1.6, pits_pore_depth=0.7,
             pits_flake_enable=False, pits_dent_density=0.0, pits_clustering=0.3)),
    'BUGHOLE_CONCRETE': _preset(
        'asset_concrete', 'Concrete Pitting: larger clustered blowholes and sharp air voids',
        ('pits',),
        dict(pits_pore_density=5.0, pits_pore_size_min=0.6, pits_pore_size_max=2.8, pits_pore_depth=0.75,
             pits_clustering=0.85, pits_cluster_scale=18.0, pits_flake_enable=False)),
    'FLAKING_CONCRETE_SKIN': _preset(
        'asset_concrete', 'Concrete Pitting: patches of the surface skin peeled off in layers',
        ('pits',),
        dict(pits_flake_coverage=0.45, pits_flake_scale=30.0, pits_flake_depth=0.55, pits_flake_layers=3,
             pits_flake_pores=9.0, pits_pore_density=3.0, pits_strength=1.3)),
    'ACID_ETCHED_CONCRETE': _preset(
        'asset_concrete', 'Concrete Pitting: rough grainy surface with a wavy, etched feel',
        ('pits',),
        dict(pits_strength=1.6, pits_grain=0.1, pits_grain_size=0.6, pits_undulation=0.3, pits_undulation_scale=18.0,
             pits_pore_density=30.0, pits_pore_size_min=0.3, pits_pore_size_max=1.0, pits_flake_enable=False)),
    'SEA_WALL_CONCRETE': _preset(
        'asset_concrete', 'Concrete Pitting: storm-beaten sea wall with dents, flakes and pits',
        ('pits',),
        dict(pits_strength=1.8, pits_dent_density=2.6, pits_dent_size_min=1.0, pits_dent_size_max=6.0,
             pits_dent_depth=0.2, pits_flake_coverage=0.3, pits_pore_density=7.0, pits_pore_size_max=1.8)),

    'ONE_LARGE_REBAR_HOLE': _preset(
        'asset_hang', 'A single big hole, with bars drooping down out of the upper rim',
        ('hang',),
        dict(hang_amount=1, hang_size_min=130.0, hang_size_max=160.0, hang_droop=1.1, hang_bar_broken=0.8)),
    'WIDE_GRID_HOLES': _preset(
        'asset_hang', 'Two holes crossed by a coarse grid of thick, gently sagging bars',
        ('hang',),
        dict(hang_amount=2, hang_size_min=80.0, hang_size_max=120.0, hang_bar_spacing=28.0, hang_bar_radius=1.0,
             hang_droop=0.35, hang_bar_broken=0.35, hang_cable_amount=2)),
    'SMALL_PUNCH_HOLES': _preset(
        'asset_hang', 'Many small punched holes with fine wire and no cables',
        ('hang',),
        dict(hang_amount=6, hang_size_min=28.0, hang_size_max=48.0, hang_bar_spacing=9.0, hang_bar_radius=0.35,
             hang_cables_enable=False)),
    'HANGING_BARS_ONLY': _preset(
        'asset_hang', 'Long snapped reinforcement hanging down; no cables',
        ('hang',),
        dict(hang_amount=2, hang_size_min=90.0, hang_size_max=130.0, hang_cables_enable=False, hang_droop=1.8,
             hang_bar_broken=1.2, hang_bar_bow=1.2)),
    'CABLE_TANGLE_HOLE': _preset(
        'asset_hang', 'A hole full of looping, sagging cables with no steel',
        ('hang',),
        dict(hang_amount=1, hang_size_min=100.0, hang_size_max=130.0, hang_bars_enable=False, hang_cable_amount=14,
             hang_cable_radius=0.3, hang_cable_length=1.6, hang_cable_sag=1.2, hang_cable_loops=1.0)),
    'HEAVY_BRIDGE_REBAR': _preset(
        'asset_hang', 'Thick bridge-grade bars bowing across a deep breach',
        ('hang',),
        dict(hang_amount=1, hang_size_min=110.0, hang_size_max=150.0, hang_bar_radius=1.6, hang_bar_spacing=22.0,
             hang_bar_depth=1.0, hang_bar_bow=1.4, hang_droop=0.5, hang_sides=12, hang_cables_enable=False)),
    'BLASTED_ROUGH_HOLES': _preset(
        'asset_hang', 'Ragged, flared blast holes with very rough walls',
        ('hang',),
        dict(hang_amount=2, hang_size_min=70.0, hang_size_max=105.0, hang_jagged=1.25, hang_flare=0.8, hang_roughness=2.4,
             hang_noise_strength=240.0, hang_noise_detail=5.0, hang_bar_broken=1.2)),
    'CORED_CLEAN_OPENING': _preset(
        'asset_hang', 'Near-round, tidily cut openings with short cropped bars',
        ('hang',),
        dict(hang_amount=2, hang_size_min=60.0, hang_size_max=85.0, hang_jagged=0.12, hang_flare=0.05, hang_roughness=0.3,
             hang_noise_strength=30.0, hang_droop=0.0, hang_bar_bow=0.0, hang_bar_broken=1.4, hang_cables_enable=False)),

    'RENDER_PATCHES_BRICK': _brick_classic(
        'Small render patches fallen off, a little brick showing',
        brick_amount=6, brick_size_min=40.0, brick_size_max=75.0, brick_missing=0.0, brick_loose=0.02, brick_crack_amount=2),
    'LARGE_BRICK_EXPOSURE': _brick_classic(
        'Most of the render gone over a large area of clean running-bond bricks',
        brick_amount=2, brick_size_min=150.0, brick_size_max=230.0, brick_jagged=0.9, brick_crack_amount=5),
    'ENGLISH_BOND_RUIN': _brick_classic(
        'English bond with broken, chipped bricks and some missing',
        brick_amount=3, brick_size_min=95.0, brick_size_max=160.0, brick_bond='2', brick_broken=0.8, brick_missing=0.1, brick_chip=1.2),
    'FLEMISH_BOND_WALL': _brick_classic(
        'Tidy Flemish bond with deep raked joints',
        brick_amount=3, brick_size_min=90.0, brick_size_max=150.0, brick_bond='3', brick_mortar=1.2, brick_mortar_depth=1.4),
    'LOOSE_SLIPPED_BRICKS': _brick_classic(
        'Bricks working loose and sliding out, gaps where others have fallen',
        brick_amount=3, brick_size_min=100.0, brick_size_max=170.0, brick_loose=0.4, brick_loose_depth=4.0, brick_loose_tilt=11.0,
        brick_loose_out=0.8, brick_loose_out_dist=5.0, brick_missing=0.22),
    'CRUMBLING_OLD_BRICK': _brick_classic(
        'Soft, old, crumbling bricks with worn corners and rough joints',
        brick_amount=3, brick_size_min=100.0, brick_size_max=170.0, brick_crumble=1.6, brick_chip=2.4, brick_chip_size=2.2,
        brick_brick_rough=1.0, brick_joint_rough=1.5, brick_broken=0.8, brick_broken_rough=1.2),
    'ROMAN_THIN_BRICKS': _brick_classic(
        'Long, flat Roman bricks with wide mortar joints',
        brick_amount=3, brick_size_min=100.0, brick_size_max=165.0, brick_brick_length=36.0, brick_brick_height=4.0,
        brick_mortar=2.0, brick_mortar_depth=1.0),
    'LOW_POLY_BRICK_PATCHES': _preset(
        'asset_brick', 'Brick V2: light low-poly bricks for games and background walls',
        ('brick_v2',),
        dict(brick_style='1', brick_amount=3, brick_size_min=90.0, brick_size_max=150.0, brick_remesh=False,
             brick_damage_depth=5.0)),
    'SWEPT_LOW_POLY_BRICKS': _preset(
        'asset_brick', 'Brick V2: swept low-poly style with a stretched, wide patch',
        ('brick_v2',),
        dict(brick_style='2', brick_amount=2, brick_size_min=120.0, brick_size_max=190.0, brick_stretch=2.6,
             brick_remesh=False)),
    'THICK_RENDER_BRICK_WALL': _preset(
        'asset_brick', 'Brick V2: heavy old render breaking away in crumbling steps',
        ('brick_v2',),
        dict(brick_amount=3, brick_size_min=90.0, brick_size_max=150.0, brick_render_min=4.0, brick_render_max=7.0, brick_terraces=3,
             brick_terrace_skin=0.6, brick_terrace_inset=0.14, brick_crumble=1.3, brick_crumble_size=2.5, brick_remesh=False,
             brick_back_resolution=0.5)),
    'HEADER_BOND_BREACH': _preset(
        'asset_brick', 'Brick V2: header bond, showing the short ends of the bricks',
        ('brick_v2',),
        dict(brick_bond='4', brick_amount=3, brick_size_min=95.0, brick_size_max=160.0, brick_layers=3, brick_broken=0.65, brick_remesh=False,
             brick_back_resolution=0.5)),
    'STACK_BOND_HOLES': _preset(
        'asset_brick', 'Brick V2: stack bond with holes opening through the brickwork',
        ('brick_v2',),
        dict(brick_bond='1', brick_amount=3, brick_size_min=100.0, brick_size_max=170.0, brick_hole_enable=True,
             brick_hole_chance=1.0, brick_hole_size=0.55, brick_missing=0.15, brick_remesh=False,
             brick_back_resolution=0.5)),

    'FALLEN_PLASTER_LATHS': _preset(
        'asset_lath', 'Plaster fallen off in small patches over regular sawn laths',
        ('lath',),
        dict(lath_amount=6, lath_size_min=35.0, lath_size_max=65.0, lath_lath_missing=0.02, lath_lath_broken=0.05, lath_crack_amount=2)),
    'BROKEN_LATH_CEILING': _preset(
        'asset_lath', 'A large opening through sagging, snapped laths',
        ('lath',),
        dict(lath_amount=2, lath_size_min=100.0, lath_size_max=150.0, lath_hole_enable=True, lath_hole_chance=1.0, lath_hole_size=0.55,
             lath_hole_laths=0.65, lath_lath_broken=0.45, lath_lath_missing=0.2, lath_lath_bend=3.0, lath_lath_warp=0.6)),
    'WIDE_GAP_WARPED_LATHS': _preset(
        'asset_lath', 'Thin laths with wide gaps, warped and tilted by damp',
        ('lath',),
        dict(lath_amount=3, lath_size_min=70.0, lath_size_max=110.0, lath_lath_gap=2.4, lath_lath_width=2.6, lath_lath_warp=0.8,
             lath_lath_tilt=1.8, lath_lath_jitter=1.0, lath_plaster_min=2.0, lath_plaster_max=3.2)),
    'V2_SOFT_EDGED_LATHS': _lath_v2(
        'Laths V2: soft, rounded plaster rim over a shallow cavity',
        lath_amount=4, lath_size_min=55.0, lath_size_max=95.0, lath_rim_smooth=0.9, lath_cavity_smooth=0.9, lath_wall_soft=0.35),
    'V2_THICK_PLASTER_BREAK': _lath_v2(
        'Laths V2: thick old plaster breaking in steps, with plaster keys between laths',
        lath_amount=3, lath_size_min=70.0, lath_size_max=115.0, lath_plaster_min=3.0, lath_plaster_max=5.0, lath_terraces=3,
        lath_terrace_skin=0.6, lath_crumble=1.2, lath_keys=0.8, lath_key_size=4.0),
    'HAND_RIVEN_LATHS': _lath_riven(
        'Riven Lath: wavy, uneven hand-split laths, with wide patches showing',
        lath_amount=3, lath_size_min=80.0, lath_size_max=125.0, lath_riven_wave=1.2, lath_riven_twist=9.0, lath_riven_width_var=0.7),
    'OLD_HOUSE_RIVEN_RUIN': _lath_riven(
        'Riven Lath: rough, broken laths hanging from a big hole',
        lath_amount=2, lath_size_min=110.0, lath_size_max=155.0, lath_hole_enable=True, lath_hole_chance=1.0, lath_hole_size=0.5,
        lath_lath_broken=0.35, lath_lath_missing=0.15, lath_riven_rough=1.0, lath_lath_bend=2.6),
    'OPEN_STUD_BAYS': _lath_framing(
        'Stud Framing: drywall torn away, exposing studs with nails',
        lath_amount=2, lath_size_min=110.0, lath_size_max=160.0, lath_nail_amount=1.0, lath_nail_length=1.6),
    'WIDE_SPACED_POST_FRAME': _lath_framing(
        'Stud Framing: heavier posts at wide spacing, with noggins between them',
        lath_amount=2, lath_size_min=120.0, lath_size_max=170.0, lath_stud_spacing=60.0, lath_stud_width=7.0, lath_stud_depth=12.0,
        lath_noggin_spacing=70.0),
    'SMALL_DRYWALL_HOLES_STUDS': _lath_framing(
        'Stud Framing: several small holes in the board with a stud glimpsed behind',
        lath_amount=7, lath_size_min=30.0, lath_size_max=60.0, lath_crack_amount=5, lath_plaster_min=1.0, lath_plaster_max=1.3),

    'BURST_OPEN_CRACKS': _preset(
        'fracture_damage', 'Very wide cracks with chunks chipped out along both lips',
        ('fracture_concrete',),
        dict(fract_l1_amount=6, fract_l1_width=5.0, fract_l1_depth=14.0, fract_l2_amount=14, fract_l2_width=2.5, fract_l3_amount=0,
             fract_flare=70.0, fract_l1_chip_amount=14.0, fract_l1_chip_size=5.0, fract_l1_chip_depth=2.5, fract_l2_chip_amount=8.0)),
    'CRACKED_INTO_PLATES': _preset(
        'fracture_damage', 'A wall split into separate plates by a dense, even crack net',
        ('fracture_concrete',),
        dict(fract_l1_amount=12, fract_l1_width=2.2, fract_l2_amount=34, fract_l2_width=1.1, fract_l3_amount=0, fract_tile_offset=60.0)),
    'BROKEN_FLOOR_PLATES': _preset(
        'fracture_damage', 'A floor slab broken into chunky plates with crumbled joints',
        ('fracture_concrete',),
        dict(fract_l1_amount=12, fract_l1_width=3.2, fract_l1_depth=9.0, fract_l2_amount=24, fract_l2_width=1.5, fract_l3_amount=0,
             fract_l1_chip_amount=10.0, fract_l1_chip_size=4.0, fract_rim_jag=70.0)),
    'BRANCHING_CRACK_TREE': _preset(
        'fracture_damage', 'A few long main cracks forking like a tree',
        ('fracture',),
        dict(fract_l1_amount=3, fract_l1_branches=3.0, fract_l1_branch_length=0.6, fract_l2_amount=10, fract_l2_branches=2.0,
             fract_l2_branch_length=0.5, fract_l3_amount=0)),
    'FEATHERED_HAIRLINES': _preset(
        'fracture_damage', 'Delicate fine cracks feathering out in three levels',
        ('fracture',),
        dict(fract_l1_amount=5, fract_l1_width=0.5, fract_l1_depth=3.0, fract_l1_chips=False, fract_l2_amount=20, fract_l2_width=0.25,
             fract_l2_branches=3.0, fract_l2_chips=False, fract_l3_amount=60, fract_l3_width=0.12, fract_l3_bias=60.0)),
    'FLARED_OPEN_CRACKS': _preset(
        'fracture_damage', 'Wide-open cracks with broad, jagged and flared lips',
        ('fracture_concrete',),
        dict(fract_l1_amount=5, fract_l1_width=4.0, fract_l1_depth=11.0, fract_l2_amount=10, fract_l2_width=1.8, fract_l3_amount=0,
             fract_flare=90.0, fract_rim_jag=85.0, fract_l1_chip_amount=9.0, fract_l1_chip_size=4.0)),
    'WEATHERED_SOFT_CRACKS': _preset(
        'fracture_damage', 'Old cracks with lips rounded and softened by weather',
        ('fracture_concrete',),
        dict(fract_l1_amount=6, fract_l1_width=2.2, fract_l2_amount=16, fract_l3_amount=0, fract_rim_soft=75.0, fract_flare=65.0,
             fract_rim_jag=15.0, fract_l1_chips=False, fract_l2_chips=False)),
    'SHORT_STRESS_CRACKS': _preset(
        'fracture_damage', 'Many short separate stress cracks scattered over the faces',
        ('fracture',),
        dict(fract_l1_amount=70, fract_l1_length=18.0, fract_l1_width=1.3, fract_l1_depth=5.0, fract_l1_chips=False,
             fract_l2_amount=0, fract_l3_amount=0)),
    'DEEP_CUT_FISSURES': _preset(
        'fracture_damage', 'A few very deep, dark clefts running through the block',
        ('fracture_concrete',),
        dict(fract_l1_amount=4, fract_l1_width=3.0, fract_l2_amount=6, fract_l3_amount=0, fract_depth_mult=320.0, fract_disp_strength=180.0)),
    'MULTI_IMPACT_STARS': _preset(
        'fracture_damage', 'Several separate impact stars across the surface',
        ('fracture_radial',),
        dict(fract_impacts=5, fract_l1_amount=5, fract_l1_length=45.0, fract_l1_width=1.6, fract_l2_amount=10, fract_radial_spokes=9,
             fract_radial_rings=2, fract_radial_crush=40.0)),
    'SLEDGEHAMMER_BLOW': _preset(
        'fracture_damage', 'One brutal blow: crushed core, dense spokes and full rings',
        ('fracture_radial',),
        dict(fract_impacts=1, fract_l1_amount=2, fract_l2_amount=8, fract_radial_spokes=13, fract_radial_rings=4, fract_radial_ring_fill=100.0,
             fract_radial_crush=60.0)),
    'CONCENTRIC_RING_BREAK': _preset(
        'fracture_damage', 'Ring-shaped fractures dominate, with only a few spokes',
        ('fracture_radial',),
        dict(fract_impacts=1, fract_l1_amount=1, fract_l1_width=1.6, fract_l2_amount=14, fract_l2_width=1.0, fract_radial_spokes=5,
             fract_radial_rings=10, fract_radial_ring_fill=100.0, fract_radial_crush=8.0)),
    'BULLET_STARRED_WINDOW': _preset(
        'fracture_damage', 'Five separate gunshot stars in a pane of glass',
        ('fracture_glass',),
        dict(fract_impacts=5, fract_l1_amount=5, fract_l2_amount=12, fract_l1_chips=False, fract_l2_chips=False, fract_glass_spokes=10,
             fract_glass_rings=3, fract_glass_crush=18.0)),
    'CRUSHED_WINDSHIELD': _preset(
        'fracture_damage', 'Very dense crazing around two crushed points, like laminated glass',
        ('fracture_glass',),
        dict(fract_impacts=2, fract_l1_amount=2, fract_l2_amount=40, fract_l1_chips=False, fract_l2_chips=False, fract_glass_spokes=38,
             fract_glass_rings=20, fract_glass_branch=90.0, fract_glass_crush=35.0, fract_glass_width=0.15)),
    'SPLINTERED_BOARD': _preset(
        'fracture_damage', 'A board torn along the grain with chipped, splintered lips',
        ('fracture_wood',),
        dict(fract_l1_amount=14, fract_l1_width=3.0, fract_l1_depth=8.0, fract_l2_amount=28, fract_l2_width=1.2, fract_grain_x=1.0,
             fract_grain_y=0.0, fract_grain_z=0.0, fract_grain_pull=0.85, fract_l1_chip_amount=10.0, fract_l1_chip_size=3.0)),
    'DEEP_ROUNDED_FRACTURES': _preset(
        'fracture_damage', 'Rounded mode on a sculpted form: wide, deep breaks that follow the curves',
        ('fracture',),
        dict(fract_surface='ROUNDED', fract_l1_amount=8, fract_l1_width=2.8, fract_l1_depth=9.0, fract_l2_amount=18, fract_l2_width=1.2,
             fract_l3_amount=0)),
    'CRACKED_WORN_PILLAR': _preset(
        'fracture_damage', 'Cracks, chipped edges, broken corners and pitted faces together',
        ('fracture_concrete', 'edge', 'corner', 'surface'),
        dict(fract_l1_amount=7, fract_l1_width=2.4, fract_l2_amount=18, fract_l3_amount=0, chips_mode='AUTO', amount=120, size_min=3.0,
             size_max=9.0, depth_min=1.0, depth_max=3.5, corner_mode='AUTO', corner_prob=100.0, corner_size_min=9.0, corner_size_max=15.0,
             corner_depth_min=4.0, corner_depth_max=7.0, surf_type='0', surf_mode='PROCEDURAL', surf_amount=40)),
    'CELL_SHATTERED_BLOCK': _preset(
        'fracture_damage', 'Cell Fracture: the block split into separate concrete chunks, ready for rigid bodies',
        ('cellf',),
        dict(cellf_material='2', cellf_pieces=45, cellf_gap=1.5)),
    'CELL_ROCK_BLOCKS': _preset(
        'fracture_damage', 'Cell Fracture: big angular rock blocks with hardly any small fines',
        ('cellf',),
        dict(cellf_material='3', cellf_pieces=14, cellf_gap=2.0)),
    'CELL_SPLINTERED_BEAM': _preset(
        'fracture_damage', 'Cell Fracture: a beam split into long wood splinters along the grain',
        ('cellf',),
        dict(cellf_material='0', cellf_pieces=30, cellf_gap=0.8, cellf_grain_x=1.0, cellf_grain_y=0.0, cellf_grain_z=0.0, cellf_grain_stretch=8.0)),

    'FLAKING_WALL_PAINT': _preset(
        'peeling', 'Old wall paint breaking into small, angular plates that barely lift',
        ('peeling',),
        dict(peeling_size=14.0, peeling_amount=30.0, peeling_small_damage_amount=45.0, peeling_ragged=75.0,
             peeling_lift=4.0, peeling_bend=14.0, peeling_curl=30.0, peeling_plates=90.0,
             peeling_tongues=15.0, peeling_tears=40.0, peeling_resolution=80.0)),
    'LARGE_ROLLED_PAINT_CURLS': _preset(
        'peeling', 'Big sheets of paint rolled back from the wall in deep, smooth curls',
        ('peeling',),
        dict(peeling_size=60.0, peeling_amount=38.0, peeling_ragged=35.0, peeling_lift=17.0,
             peeling_bend=34.0, peeling_variation=60.0, peeling_curl=95.0, peeling_plates=15.0,
             peeling_tongues=50.0, peeling_tears=25.0)),
    'DAMP_SAGGING_PAINT': _preset(
        'peeling', 'Damp paint skin that lets go at the top and hangs forward under its own weight',
        ('peeling',),
        dict(peeling_size=42.0, peeling_amount=36.0, peeling_lift=17.0, peeling_bend=32.0,
             peeling_curl=60.0, peeling_plates=25.0, peeling_tongues=35.0, peeling_droop=90.0,
             peeling_ragged=40.0)),
    'ALLIGATOR_PAINT_SCALES': _preset(
        'peeling', 'Thick old paint crazed into a dense pattern of small, stiff scales',
        ('peeling',),
        dict(peeling_size=9.0, peeling_amount=42.0, peeling_small_damage_amount=30.0, peeling_ragged=60.0,
             peeling_lift=3.0, peeling_bend=10.0, peeling_curl=12.0, peeling_plates=100.0,
             peeling_tongues=0.0, peeling_tears=15.0, peeling_thickness=0.5, peeling_depth=2.0,
             peeling_resolution=100.0)),
    'RIBBON_TORN_COATING': _preset(
        'peeling', 'Coating torn into long narrow tongues and slit flaps around each patch',
        ('peeling',),
        dict(peeling_size=30.0, peeling_amount=34.0, peeling_ragged=80.0, peeling_lift=14.0,
             peeling_bend=22.0, peeling_variation=100.0, peeling_curl=70.0, peeling_plates=35.0,
             peeling_tongues=100.0, peeling_tears=100.0, peeling_resolution=80.0)),
    'SPARSE_PAINT_LOSS': _preset(
        'peeling', 'A mostly intact wall with a few small patches of lost paint and pinhole flaking',
        ('peeling',),
        dict(peeling_size=24.0, peeling_amount=11.0, peeling_small_damage_amount=70.0, peeling_lift=5.0,
             peeling_curl=45.0, peeling_plates=70.0, peeling_tongues=25.0, peeling_tears=30.0,
             peeling_resolution=70.0)),
    'STRIPPED_TO_SUBSTRATE': _preset(
        'peeling', 'Most of the coating gone: islands of paint left on a deeply recessed base',
        ('peeling',),
        dict(peeling_size=48.0, peeling_amount=72.0, peeling_ragged=70.0, peeling_lift=6.0,
             peeling_bend=16.0, peeling_curl=55.0, peeling_plates=60.0, peeling_tongues=30.0,
             peeling_tears=45.0, peeling_depth=3.0, peeling_thickness=0.3)),
    'LIFTED_FILM_COATING': _preset(
        'peeling', 'A thin polymer film lifting in broad, gentle arcs with clean outlines',
        ('peeling',),
        dict(peeling_size=70.0, peeling_amount=28.0, peeling_ragged=12.0, peeling_lift=7.0,
             peeling_bend=40.0, peeling_variation=25.0, peeling_curl=5.0, peeling_plates=0.0,
             peeling_tongues=0.0, peeling_tears=8.0, peeling_thickness=0.05, peeling_depth=0.6)),
    'PAINT_OVER_WALLPAPER': _preset(
        'peeling', 'Paint peeling off old wallpaper, with only small tears down to the plaster',
        ('peeling',),
        dict(peeling_size=46.0, peeling_amount=40.0, peeling_lift=8.0, peeling_curl=65.0,
             peeling_plates=45.0, peeling_wallpaper=True, peeling_paper_torn=18.0, peeling_plaster_depth=2.0)),
    'SHREDDED_PAINTED_PAPER': _preset(
        'peeling', 'Painted wallpaper in shreds: most of the paper torn away, tall flaps left standing',
        ('peeling',),
        dict(peeling_size=34.0, peeling_amount=55.0, peeling_ragged=85.0, peeling_lift=15.0,
             peeling_bend=24.0, peeling_curl=80.0, peeling_plates=30.0, peeling_tongues=70.0,
             peeling_tears=80.0, peeling_wallpaper=True, peeling_paper_torn=72.0, peeling_plaster_depth=5.0,
             peeling_resolution=80.0)),
    'PAPER_OVER_BROKEN_PLASTER': _preset(
        'peeling', 'Large patches of paper torn away over deep, broken plaster',
        ('peeling',),
        dict(peeling_size=56.0, peeling_amount=46.0, peeling_lift=6.0, peeling_curl=40.0,
             peeling_plates=55.0, peeling_droop=40.0, peeling_wallpaper=True, peeling_paper_torn=50.0,
             peeling_plaster_depth=8.0)),
    'SUN_BLISTERED_BOARD': _preset(
        'peeling', 'Sun-baked paint on a board: narrow flakes curling up off the wood',
        ('peeling',),
        dict(peeling_size=12.0, peeling_amount=38.0, peeling_small_damage_amount=25.0, peeling_ragged=65.0,
             peeling_lift=12.0, peeling_bend=16.0, peeling_curl=85.0, peeling_plates=50.0,
             peeling_tongues=55.0, peeling_tears=45.0, peeling_resolution=100.0)),
    'SHIP_HULL_PAINT_SCALE': _preset(
        'peeling', 'Thick marine paint scaling off steel in hard, heavy plates',
        ('peeling',),
        dict(peeling_size=26.0, peeling_amount=44.0, peeling_ragged=55.0, peeling_lift=6.0,
             peeling_bend=12.0, peeling_curl=20.0, peeling_plates=95.0, peeling_tongues=10.0,
             peeling_tears=25.0, peeling_thickness=0.8, peeling_depth=3.5, peeling_resolution=80.0)),
    'COLUMN_COATING_SPIRAL': _preset(
        'peeling', 'Coating coming away around a round column in soft, wrapping flaps',
        ('peeling',),
        dict(peeling_size=32.0, peeling_amount=40.0, peeling_ragged=45.0, peeling_lift=9.0,
             peeling_bend=26.0, peeling_curl=75.0, peeling_plates=30.0, peeling_tongues=60.0,
             peeling_tears=30.0, peeling_droop=30.0, peeling_resolution=80.0)),
    'STATUE_LACQUER_FLAKES': _preset(
        'peeling', 'Aged lacquer flaking from a rounded sculpture in fine, curled fragments',
        ('peeling',),
        dict(peeling_size=11.0, peeling_amount=32.0, peeling_small_damage_amount=55.0, peeling_ragged=60.0,
             peeling_lift=5.0, peeling_bend=14.0, peeling_curl=70.0, peeling_plates=65.0,
             peeling_tongues=30.0, peeling_tears=30.0, peeling_thickness=0.1, peeling_resolution=100.0)),
    # ---- 100 asset looks (2026-11-05): 60 single generator, 40 combined
    'HONEYCOMB_POUR_DEFECT': _preset(
        'asset_concrete', 'Badly vibrated pour: a rough honeycomb of stones in shallow pockets',
        ('concrete',),
        {'concrete_amount': 5, 'concrete_size_min': 22.0, 'concrete_size_max': 48.0, 'concrete_depth_min':
         1.5, 'concrete_depth_max': 3.0, 'concrete_terraces': 1, 'concrete_roughness': 2.0,
         'concrete_aggregate_size': 2.8, 'concrete_rebar_enable': False, 'concrete_crack_amount': 0}),
    'CORRODED_REBAR_DELAMINATION': _preset(
        'asset_concrete', 'Rust-burst cover lifted off in wide, thin sheets over a dense bar grid',
        ('concrete',),
        {'concrete_amount': 3, 'concrete_size_min': 70.0, 'concrete_size_max': 110.0, 'concrete_depth_min':
         2.0, 'concrete_depth_max': 3.0, 'concrete_terraces': 1, 'concrete_rebar_cover': 1.5,
         'concrete_rebar_expose': 1.0, 'concrete_rebar_spacing_x': 12.0, 'concrete_rebar_spacing_y': 12.0,
         'concrete_rebar_broken': 0.1, 'concrete_rebar_bend': 0.1}),
    'COLUMN_CORNER_SPALLS': _preset(
        'asset_concrete', 'Square column with its arrises spalled along the height, bar cage showing',
        ('concrete',),
        {'concrete_amount': 5, 'concrete_size_min': 18.0, 'concrete_size_max': 36.0, 'concrete_stretch':
         2.8, 'concrete_edge_bias': 3.5, 'concrete_depth_min': 2.0, 'concrete_depth_max': 4.0,
         'concrete_rebar_spacing_x': 12.0, 'concrete_rebar_spacing_y': 16.0, 'concrete_rebar_expose': 1.0}),
    'BEAM_SOFFIT_SPALLS': _preset(
        'asset_concrete', 'A concrete beam with long spalls running along it and bars left bare',
        ('concrete',),
        {'concrete_amount': 3, 'concrete_size_min': 30.0, 'concrete_size_max': 60.0, 'concrete_stretch':
         3.2, 'concrete_depth_min': 2.5, 'concrete_depth_max': 5.0, 'concrete_rebar_spacing_x': 10.0,
         'concrete_rebar_spacing_y': 18.0, 'concrete_rebar_radius': 0.9, 'concrete_rebar_expose': 1.0}),
    'BLOCK_EDGE_BITES': _preset(
        'asset_concrete', 'A cast block with bites knocked out along its edges, no steel inside',
        ('concrete',),
        {'concrete_amount': 7, 'concrete_size_min': 16.0, 'concrete_size_max': 34.0, 'concrete_edge_bias':
         4.0, 'concrete_depth_min': 3.0, 'concrete_depth_max': 7.0, 'concrete_terraces': 3,
         'concrete_rebar_enable': False}),
    'WORN_SLAB_POTHOLES': _preset(
        'asset_concrete', 'A floor slab with worn potholes and soft sloping sides',
        ('concrete',),
        {'concrete_amount': 6, 'concrete_size_min': 25.0, 'concrete_size_max': 55.0, 'concrete_depth_min':
         2.0, 'concrete_depth_max': 4.5, 'concrete_wall_soft': 1.4, 'concrete_jagged': 0.3,
         'concrete_rebar_enable': False, 'concrete_crack_amount': 6}),
    'PUNCHED_THROUGH_SPALLS': _preset(
        'asset_concrete', 'Spalls with holes broken right through the wall behind the bars',
        ('concrete',),
        {'concrete_amount': 3, 'concrete_size_min': 55.0, 'concrete_size_max': 85.0, 'concrete_depth_min':
         5.0, 'concrete_depth_max': 9.0, 'concrete_hole_enable': True, 'concrete_hole_chance': 1.0,
         'concrete_hole_size': 0.55, 'concrete_rebar_expose': 1.0, 'concrete_rebar_broken': 0.6}),
    'THIN_BARS_DENSE_MESH': _preset(
        'asset_concrete', 'Tight welded-wire mesh over wide, very shallow scaling',
        ('concrete',),
        {'concrete_amount': 4, 'concrete_size_min': 45.0, 'concrete_size_max': 80.0, 'concrete_depth_min':
         1.2, 'concrete_depth_max': 2.2, 'concrete_terraces': 1, 'concrete_rebar_radius': 0.25,
         'concrete_rebar_spacing_x': 5.0, 'concrete_rebar_spacing_y': 5.0, 'concrete_rebar_cover': 0.9,
         'concrete_rebar_expose': 1.0, 'concrete_rebar_ribs': 0.0}),
    'WOOD_CORE_CONCRETE_SHELL': _preset(
        'asset_concrete', 'A thin cement render broken off a timber wall, boards showing behind',
        ('concrete',),
        {'concrete_wall_type': 'WOOD', 'concrete_wood_thickness': 3.0, 'concrete_wood_width': 14.0,
         'concrete_wood_splinter': 9.0, 'concrete_amount': 4, 'concrete_size_min': 45.0,
         'concrete_size_max': 80.0, 'concrete_rebar_enable': False}),
    'ANGLED_MESH_BIG_BREAK': _preset(
        'asset_concrete', 'Massive breaks showing heavy bars laid at thirty degrees',
        ('concrete',),
        {'concrete_amount': 3, 'concrete_size_min': 60.0, 'concrete_size_max': 90.0, 'concrete_depth_min':
         5.0, 'concrete_depth_max': 9.0, 'concrete_terraces': 3, 'concrete_rebar_radius': 1.1,
         'concrete_rebar_angle': 30.0, 'concrete_rebar_spacing_x': 22.0, 'concrete_rebar_spacing_y': 22.0,
         'concrete_rebar_expose': 1.0, 'concrete_rebar_bend': 0.8}),
    'V2_SMOOTH_WATER_EROSION': _preset(
        'asset_concrete', 'Concrete V2: water-smoothed hollows with no sharp rims',
        ('concrete_v2',),
        {'concrete_amount': 5, 'concrete_size_min': 30.0, 'concrete_size_max': 70.0, 'concrete_wall_soft':
         2.0, 'concrete_jagged': 0.1, 'concrete_smooth_width': 1.0, 'concrete_depth_min': 2.0,
         'concrete_depth_max': 4.0, 'concrete_terraces': 1, 'concrete_rebar_enable': False,
         'concrete_crack_amount': 0}),
    'V2_COLUMN_SKIN_LOSS': _preset(
        'asset_concrete', 'Concrete V2: shallow skin loss wrapping a column',
        ('concrete_v2',),
        {'concrete_amount': 10, 'concrete_size_min': 20.0, 'concrete_size_max': 42.0, 'concrete_depth_min': 1.5, 'concrete_depth_max': 3.0, 'concrete_jagged': 0.9, 'concrete_rebar_enable': False}),
    'V2_HORIZONTAL_BARS_BAND': _preset(
        'asset_concrete', 'Concrete V2: a wide band of spalling showing horizontal bars only',
        ('concrete_v2',),
        {'concrete_amount': 3, 'concrete_size_min': 50.0, 'concrete_size_max': 90.0, 'concrete_stretch':
         3.4, 'concrete_rebar_bars_y': False, 'concrete_rebar_spacing_x': 14.0, 'concrete_rebar_spacing_y':
         14.0, 'concrete_rebar_radius': 0.8, 'concrete_rebar_expose': 1.0, 'concrete_depth_min': 3.0,
         'concrete_depth_max': 6.0}),
    'V2_BENT_OUT_BARS': _preset(
        'asset_concrete', 'Concrete V2: bars pulled and bent out of a single torn crater',
        ('concrete_v2',),
        {'concrete_amount': 3, 'concrete_size_min': 50.0, 'concrete_size_max': 80.0, 'concrete_depth_min':
         5.0, 'concrete_depth_max': 8.0, 'concrete_rebar_bend': 1.8, 'concrete_rebar_broken': 0.8,
         'concrete_rebar_wobble': 1.0, 'concrete_rebar_radius': 0.9, 'concrete_rebar_spacing_x': 16.0,
         'concrete_rebar_spacing_y': 16.0, 'concrete_rebar_expose': 1.0}),
    'V2_TERRACED_QUARRY_FACE': _preset(
        'asset_concrete', 'Concrete V2: many crisp terraces stepping down like a quarry face',
        ('concrete_v2',),
        {'concrete_amount': 3, 'concrete_size_min': 55.0, 'concrete_size_max': 90.0, 'concrete_terraces': 4,
         'concrete_terrace_skin': 0.65, 'concrete_terrace_inset': 0.3, 'concrete_depth_min': 6.0,
         'concrete_depth_max': 10.0, 'concrete_wall_soft': 0.1, 'concrete_rebar_enable': False}),
    'V2_SLAB_TRAFFIC_WEAR': _preset(
        'asset_concrete', 'Concrete V2: a floor scuffed into low, wide, soft-edged patches',
        ('concrete_v2',),
        {'concrete_amount': 7, 'concrete_size_min': 30.0, 'concrete_size_max': 70.0, 'concrete_depth_min':
         0.8, 'concrete_depth_max': 1.8, 'concrete_terraces': 1, 'concrete_wall_soft': 1.0,
         'concrete_rebar_enable': False, 'concrete_crack_amount': 3}),
    'V2_LOBED_BLAST_SPALLS': _preset(
        'asset_concrete', 'Concrete V2: many-lobed blast marks with a noisy, broken floor',
        ('concrete_v2',),
        {'concrete_amount': 4, 'concrete_size_min': 40.0, 'concrete_size_max': 75.0, 'concrete_lobes': 8,
         'concrete_jagged': 1.4, 'concrete_surface_strength': 300.0, 'concrete_surface_detail': 5.0,
         'concrete_rebar_expose': 0.8, 'concrete_rebar_broken': 0.8}),
    'V2_CRACKED_RIM_POCKETS': _preset(
        'asset_concrete', 'Concrete V2: compact pockets, each throwing long cracks across the wall',
        ('concrete_v2',),
        {'concrete_amount': 3, 'concrete_size_min': 25.0, 'concrete_size_max': 45.0,
         'concrete_crack_amount': 18, 'concrete_crack_length_min': 30.0, 'concrete_crack_length_max': 90.0,
         'concrete_crack_width': 0.6, 'concrete_crack_branch': 0.7, 'concrete_rebar_enable': False}),
    'SANDBLASTED_CONCRETE': _preset(
        'asset_concrete', 'Concrete Pitting: an even, grit-blasted surface with exposed fine grain',
        ('pits',),
        {'pits_strength': 1.8, 'pits_grain': 0.22, 'pits_grain_size': 0.4, 'pits_pore_density': 70.0,
         'pits_pore_size_min': 0.2, 'pits_pore_size_max': 1.5, 'pits_pore_depth': 0.9, 'pits_clustering':
         0.1, 'pits_flake_enable': False, 'pits_dent_density': 0.0}),
    'FROST_SCALED_SURFACE': _preset(
        'asset_concrete', 'Concrete Pitting: frost damage, the skin scaling away in overlapping layers',
        ('pits',),
        {'pits_strength': 1.5, 'pits_flake_coverage': 0.7, 'pits_flake_scale': 16.0, 'pits_flake_depth':
         0.5, 'pits_flake_layers': 4, 'pits_flake_jagged': 0.9, 'pits_pore_density': 2.0,
         'pits_dent_density': 0.2}),
    'BIG_AIR_VOID_CASTING': _preset(
        'asset_concrete', 'Concrete Pitting: few, wide round voids from a dry casting',
        ('pits',),
        {'pits_pore_density': 1.2, 'pits_pore_size_min': 1.5, 'pits_pore_size_max': 5.0,
         'pits_pore_small_bias': 0.2, 'pits_pore_depth': 0.9, 'pits_pore_sharp': 0.9, 'pits_pore_jagged':
         0.25, 'pits_clustering': 0.2, 'pits_flake_enable': False, 'pits_dent_density': 0.0}),
    'HAMMER_DENTED_FACE': _preset(
        'asset_concrete', 'Concrete Pitting: a face peppered with chipped hammer dents',
        ('pits',),
        {'pits_dent_density': 6.0, 'pits_dent_size_min': 1.5, 'pits_dent_size_max': 7.0, 'pits_dent_depth':
         0.35, 'pits_dent_chip': 1.0, 'pits_pore_density': 1.0, 'pits_flake_enable': False}),
    'WAVY_TROWEL_SLAB': _preset(
        'asset_concrete', 'Concrete Pitting: uneven hand-troweled slab with soft waves and few pores',
        ('pits',),
        {'pits_undulation': 1.2, 'pits_undulation_scale': 26.0, 'pits_grain': 0.1, 'pits_pore_density':
         3.0, 'pits_pore_size_max': 0.9, 'pits_flake_coverage': 0.06, 'pits_dent_density': 0.2}),
    'STRETCHED_DRIP_PORES': _preset(
        'asset_concrete', 'Concrete Pitting: pores drawn into long streaks by slumping wet concrete',
        ('pits',),
        {'pits_pore_density': 10.0, 'pits_pore_size_min': 0.5, 'pits_pore_size_max': 2.4,
         'pits_pore_stretch': 1.0, 'pits_pore_jagged': 0.9, 'pits_clustering': 0.75, 'pits_cluster_scale':
         14.0, 'pits_flake_enable': False}),
    'AGGREGATE_FLAKE_PATCHES': _preset(
        'asset_concrete', 'Concrete Pitting: large flakes exposing a coarse, pore-riddled aggregate',
        ('pits',),
        {'pits_flake_coverage': 0.35, 'pits_flake_scale': 45.0, 'pits_flake_depth': 0.8,
         'pits_flake_layers': 2, 'pits_aggregate_size': 1.6, 'pits_flake_pores': 14.0, 'pits_pore_density':
         1.0}),
    'DOUBLE_LAYER_BIG_BREACH': _preset(
        'asset_hang', 'One giant breach with thick bars set deep and few survivors',
        ('hang',),
        {'hang_amount': 1, 'hang_size_min': 150.0, 'hang_size_max': 190.0, 'hang_bar_spacing': 24.0,
         'hang_bar_radius': 1.2, 'hang_bar_depth': 0.9, 'hang_bar_broken': 1.5, 'hang_droop': 1.0,
         'hang_cables_enable': False}),
    'FINE_WIRE_CONDUIT_HOLE': _preset(
        'asset_hang', 'A neat small hole hung with fine wire and thin service cables',
        ('hang',),
        {'hang_amount': 1, 'hang_size_min': 55.0, 'hang_size_max': 75.0, 'hang_bar_spacing': 7.0,
         'hang_bar_radius': 0.25, 'hang_bar_broken': 0.8, 'hang_cable_amount': 6, 'hang_cable_radius': 0.12,
         'hang_cable_length': 1.3, 'hang_jagged': 0.4}),
    'SHELLFIRE_THREE_HOLES': _preset(
        'asset_hang', 'Three jagged shell holes across a wall, with bars torn downward',
        ('hang',),
        {'hang_amount': 3, 'hang_size_min': 55.0, 'hang_size_max': 95.0, 'hang_jagged': 1.1, 'hang_flare':
         0.7, 'hang_bar_broken': 1.0, 'hang_droop': 1.3, 'hang_cable_amount': 1}),
    'STRAIGHT_UNBENT_CAGE': _preset(
        'asset_hang', 'A clean hole with rigid bars crossing it, none bent or sagging',
        ('hang',),
        {'hang_amount': 1, 'hang_size_min': 95.0, 'hang_size_max': 125.0, 'hang_droop': 0.0, 'hang_bar_bow':
         0.0, 'hang_bar_broken': 0.2, 'hang_bar_spacing': 18.0, 'hang_bar_radius': 0.8,
         'hang_cables_enable': False, 'hang_jagged': 0.35}),
    'LONG_DANGLING_CABLES': _preset(
        'asset_hang', 'Long cable loops falling from a high hole, with a few snapped bars',
        ('hang',),
        {'hang_amount': 1, 'hang_size_min': 90.0, 'hang_size_max': 120.0, 'hang_cable_amount': 9,
         'hang_cable_radius': 0.25, 'hang_cable_length': 2.2, 'hang_cable_sag': 1.6, 'hang_cable_loops':
         0.7, 'hang_bar_spacing': 25.0, 'hang_bar_broken': 1.2}),
    'SHALLOW_FLARED_CRATERS': _preset(
        'asset_hang', 'Wide flaring craters with very rough rims and light bars',
        ('hang',),
        {'hang_amount': 2, 'hang_size_min': 85.0, 'hang_size_max': 125.0, 'hang_flare': 1.0, 'hang_jagged':
         0.9, 'hang_roughness': 1.8, 'hang_bar_radius': 0.45, 'hang_bar_spacing': 20.0,
         'hang_cables_enable': False}),
    'SERVICE_RISER_BREACH': _preset(
        'asset_hang', 'A service riser broken open: thick cable bundle, no reinforcement',
        ('hang',),
        {'hang_amount': 1, 'hang_size_min': 80.0, 'hang_size_max': 110.0, 'hang_bars_enable': False,
         'hang_cable_amount': 7, 'hang_cable_radius': 0.6, 'hang_cable_length': 1.2, 'hang_cable_sag': 0.7,
         'hang_cable_loops': 0.2}),
    'MANY_MEDIUM_BROKEN_HOLES': _preset(
        'asset_hang', 'Four medium holes with bars bowed outward and heavy droop',
        ('hang',),
        {'hang_amount': 4, 'hang_size_min': 45.0, 'hang_size_max': 70.0, 'hang_bar_bow': 1.6, 'hang_droop':
         1.5, 'hang_bar_spacing': 12.0, 'hang_cable_amount': 1}),
    'SMOOTH_WORN_OLD_HOLES': _preset(
        'asset_hang', 'Old holes with weather-softened rims and rusted stubs',
        ('hang',),
        {'hang_amount': 2, 'hang_size_min': 70.0, 'hang_size_max': 105.0, 'hang_jagged': 0.25,
         'hang_roughness': 0.5, 'hang_noise_strength': 45.0, 'hang_bar_broken': 1.6, 'hang_droop': 0.2,
         'hang_cables_enable': False}),
    'MONK_BOND_RUIN': _preset(
        'asset_brick', 'Tall, irregular old bricks with heavily rounded faces and open joints',
        ('brick',),
        {**_BRICK_CLASSIC, 'brick_amount': 3, 'brick_size_min': 95.0, 'brick_size_max': 160.0,
         'brick_brick_length': 30.0, 'brick_brick_height': 9.0, 'brick_face_jitter': 0.8, 'brick_face_tilt':
         1.4, 'brick_mortar': 1.6, 'brick_mortar_depth': 1.2}),
    'CLEAN_NEW_BRICK_WORK': _preset(
        'asset_brick', 'Neat, crisp modern bricks behind crumbled thin render, no breakage',
        ('brick',),
        {**_BRICK_CLASSIC, 'brick_amount': 4, 'brick_size_min': 70.0, 'brick_size_max': 120.0,
         'brick_brick_rough': 0.05, 'brick_chip': 0.05, 'brick_broken': 0.0, 'brick_missing': 0.0,
         'brick_loose': 0.0, 'brick_face_jitter': 0.0, 'brick_render_min': 0.8, 'brick_render_max': 1.2}),
    'SPLINTERED_CHIMNEY_BRICK': _preset(
        'asset_brick', 'Brick column faces shattered, with many cracked units',
        ('brick',),
        {**_BRICK_CLASSIC, 'brick_amount': 4, 'brick_size_min': 35.0, 'brick_size_max': 65.0,
         'brick_broken': 1.0, 'brick_broken_rough': 1.5, 'brick_chip': 2.0, 'brick_missing': 0.15}),
    'STRETCHED_BAND_BREAK': _preset(
        'asset_brick', 'A long horizontal band of render lost above sound brickwork',
        ('brick',),
        {**_BRICK_CLASSIC, 'brick_amount': 2, 'brick_size_min': 80.0, 'brick_size_max': 120.0,
         'brick_stretch': 3.5, 'brick_jagged': 1.0, 'brick_crack_amount': 8}),
    'WIDE_RAKED_MORTAR_JOINTS': _preset(
        'asset_brick', 'Soft mortar scoured back, leaving deep wide joints around every brick',
        ('brick',),
        {**_BRICK_CLASSIC, 'brick_amount': 3, 'brick_size_min': 100.0, 'brick_size_max': 160.0,
         'brick_mortar': 2.2, 'brick_mortar_depth': 2.4, 'brick_joint_rough': 1.6, 'brick_crumble': 1.0}),
    'COLLAPSED_LOWER_COURSES': _preset(
        'asset_brick', 'The lower courses dropping out: many bricks missing, the rest tilted',
        ('brick',),
        {**_BRICK_CLASSIC, 'brick_amount': 3, 'brick_size_min': 110.0, 'brick_size_max': 170.0,
         'brick_missing': 0.4, 'brick_loose': 0.5, 'brick_loose_tilt': 14.0, 'brick_loose_depth': 5.0,
         'brick_edge_bias': 2.0}),
    'THIN_LIMEWASH_BRICK': _preset(
        'asset_brick', 'Thin limewash worn off in small scattered scabs',
        ('brick',),
        {**_BRICK_CLASSIC, 'brick_amount': 11, 'brick_size_min': 22.0, 'brick_size_max': 45.0,
         'brick_render_min': 0.4, 'brick_render_max': 0.8, 'brick_terraces': 1, 'brick_crack_amount': 1,
         'brick_crumble': 1.2}),
    'JITTERED_RUSTIC_COURSES': _preset(
        'asset_brick', 'Rustic hand-laid bricks, wildly uneven in face and position',
        ('brick',),
        {**_BRICK_CLASSIC, 'brick_amount': 3, 'brick_size_min': 100.0, 'brick_size_max': 170.0,
         'brick_face_jitter': 1.2, 'brick_face_tilt': 2.2, 'brick_bond_offset': 0.35, 'brick_brick_rough':
         1.2}),
    'V2_THICK_THREE_LAYER_WALL': _preset(
        'asset_brick', 'Brick V2: deep breach showing three brick layers behind each other',
        ('brick_v2',),
        {'brick_amount': 2, 'brick_size_min': 130.0, 'brick_size_max': 190.0, 'brick_layers': 3,
         'brick_break_size': 0.8, 'brick_layer_inset': 0.3, 'brick_broken': 0.7, 'brick_remesh': False,
         'brick_back_resolution': 0.5}),
    'V2_ENGLISH_CROSS_PATCHES': _preset(
        'asset_brick', 'Brick V2: small English-bond patches through thin plaster',
        ('brick_v2',),
        {'brick_bond': '2', 'brick_amount': 6, 'brick_size_min': 45.0, 'brick_size_max': 80.0,
         'brick_render_min': 0.8, 'brick_render_max': 1.4, 'brick_remesh': False, 'brick_back_resolution':
         0.5}),
    'V2_WIDE_LOW_POLY_RUIN': _preset(
        'asset_brick', 'Brick V2: low-poly game ruin with broad damage and missing units',
        ('brick_v2',),
        {'brick_style': '1', 'brick_amount': 2, 'brick_size_min': 150.0, 'brick_size_max': 210.0,
         'brick_missing': 0.25, 'brick_damage_depth': 9.0, 'brick_damage_scale': 12.0, 'brick_remesh': False}),
    'V2_FLEMISH_LOOSE_FACE': _preset(
        'asset_brick', 'Brick V2: Flemish face with loose, slipping stretchers',
        ('brick_v2',),
        {'brick_bond': '3', 'brick_amount': 3, 'brick_size_min': 100.0, 'brick_size_max': 160.0,
         'brick_loose': 0.5, 'brick_loose_out': 1.0, 'brick_loose_out_dist': 6.0, 'brick_remesh': False,
         'brick_back_resolution': 0.5}),
    'V2_BLOCK_PLINTH_BRICKS': _preset(
        'asset_brick', 'Brick V2: a rendered plinth with its corners stripped to brick',
        ('brick_v2',),
        {'brick_amount': 5, 'brick_size_min': 40.0, 'brick_size_max': 75.0, 'brick_edge_bias': 4.0,
         'brick_broken': 0.5, 'brick_remesh': False, 'brick_back_resolution': 0.5}),
    'V2_SOFT_RENDER_MANY_PATCHES': _preset(
        'asset_brick', 'Brick V2: soft-edged render patches scattered across the wall',
        ('brick_v2',),
        {'brick_amount': 8, 'brick_size_min': 35.0, 'brick_size_max': 70.0, 'brick_wall_soft': 0.6,
         'brick_jagged': 0.3, 'brick_terraces': 1, 'brick_crack_amount': 0, 'brick_remesh': False,
         'brick_back_resolution': 0.5}),
    'DIAGONAL_LATH_PLASTER': _preset(
        'asset_lath', 'Laths nailed on the diagonal under broken plaster',
        ('lath',),
        {'lath_amount': 3, 'lath_size_min': 70.0, 'lath_size_max': 110.0, 'lath_lath_angle': 35.0,
         'lath_lath_gap': 1.2, 'lath_crack_amount': 4}),
    'THICK_OLD_PLASTER_KEYS': _preset(
        'asset_lath', 'Very thick plaster breaking in steps, heavy keys bulging between the laths',
        ('lath',),
        {'lath_amount': 3, 'lath_size_min': 75.0, 'lath_size_max': 115.0, 'lath_plaster_min': 3.5,
         'lath_plaster_max': 5.5, 'lath_terraces': 3, 'lath_keys': 1.0, 'lath_key_size': 4.5,
         'lath_crumble': 1.3}),
    'NARROW_DENSE_LATHS': _preset(
        'asset_lath', 'Slim, tight laths with almost no gap, crisp and undamaged',
        ('lath',),
        {'lath_amount': 4, 'lath_size_min': 55.0, 'lath_size_max': 90.0, 'lath_lath_width': 2.0,
         'lath_lath_gap': 0.35, 'lath_lath_jitter': 0.1, 'lath_lath_warp': 0.05, 'lath_lath_broken': 0.0,
         'lath_lath_missing': 0.0}),
    'SPLINTERED_MISSING_LATHS': _preset(
        'asset_lath', 'Laths snapped and splintered, a good third of them gone',
        ('lath',),
        {'lath_amount': 3, 'lath_size_min': 75.0, 'lath_size_max': 110.0, 'lath_lath_missing': 0.35,
         'lath_lath_broken': 0.55, 'lath_lath_splinter': 3.0, 'lath_lath_bend': 2.4}),
    'V2_LONG_CRACKED_PLASTER': _preset(
        'asset_lath', 'Laths V2: small openings feeding long plaster cracks',
        ('lath_v2',),
        {**_LATH_V2, 'lath_amount': 3, 'lath_size_min': 45.0, 'lath_size_max': 70.0, 'lath_crack_amount':
         14, 'lath_crack_length_min': 30.0, 'lath_crack_length_max': 70.0, 'lath_crack_width': 0.45,
         'lath_crack_interior': 0.5}),
    'V2_DEEP_CAVITY_HOLES': _preset(
        'asset_lath', 'Laths V2: a deep wall cavity seen through broken laths',
        ('lath_v2',),
        {**_LATH_V2, 'lath_amount': 2, 'lath_size_min': 90.0, 'lath_size_max': 130.0, 'lath_cavity_depth':
         7.0, 'lath_hole_enable': True, 'lath_hole_chance': 1.0, 'lath_hole_size': 0.5, 'lath_hole_laths':
         0.6, 'lath_lath_broken': 0.3}),
    'RIVEN_WAVY_ATTIC_LATHS': _preset(
        'asset_lath', 'Riven Lath: strongly waved, thin attic laths with wide gaps',
        ('lath_riven',),
        {**RIVEN_LOOK, 'lath_amount': 3, 'lath_size_min': 85.0, 'lath_size_max': 120.0, 'lath_riven_wave':
         1.8, 'lath_riven_wave_length': 22.0, 'lath_riven_width_var': 0.8, 'lath_lath_gap': 2.0}),
    'RIVEN_TWISTED_STRIPS': _preset(
        'asset_lath', 'Riven Lath: strongly twisted strips splitting away from the wall',
        ('lath_riven',),
        {**RIVEN_LOOK, 'lath_amount': 3, 'lath_size_min': 75.0, 'lath_size_max': 115.0, 'lath_riven_twist':
         18.0, 'lath_riven_rough': 1.0, 'lath_lath_tilt': 2.2, 'lath_lath_broken': 0.3}),
    'RIVEN_SMALL_PATCHES': _preset(
        'asset_lath', 'Riven Lath: many small plaster losses over rough split wood',
        ('lath_riven',),
        {**RIVEN_LOOK, 'lath_amount': 8, 'lath_size_min': 30.0, 'lath_size_max': 55.0, 'lath_riven_rough':
         0.9, 'lath_plaster_min': 1.0, 'lath_plaster_max': 1.8}),
    'NARROW_STUD_SPACING': _preset(
        'asset_lath', 'Stud Framing: tightly spaced light studs behind a torn board',
        ('lath_framing',),
        {**FRAMING_LOOK, 'lath_amount': 3, 'lath_size_min': 70.0, 'lath_size_max': 100.0,
         'lath_stud_spacing': 24.0, 'lath_stud_width': 4.0, 'lath_stud_depth': 7.0, 'lath_noggin_spacing':
         45.0}),
    'HEAVY_TIMBER_FRAME': _preset(
        'asset_lath', 'Stud Framing: massive square posts and thick plates behind the break',
        ('lath_framing',),
        {**FRAMING_LOOK, 'lath_amount': 3, 'lath_size_min': 70.0, 'lath_size_max': 100.0,
         'lath_stud_spacing': 70.0, 'lath_stud_width': 14.0, 'lath_stud_depth': 14.0, 'lath_nail_amount':
         0.2}),
    'LONG_NAILS_LOOSE_BOARD': _preset(
        'asset_lath', 'Stud Framing: thin boards ripped away, long nails left bristling',
        ('lath_framing',),
        {**FRAMING_LOOK, 'lath_amount': 3, 'lath_size_min': 80.0, 'lath_size_max': 125.0,
         'lath_nail_amount': 1.0, 'lath_nail_length': 2.6, 'lath_crack_amount': 8, 'lath_plaster_min': 0.8,
         'lath_plaster_max': 1.1}),
    'PITTED_REBAR_SPALLS': _preset(
        'asset_combined', 'Concrete Damage + Pitting: rebar spalls in a pore-riddled old surface',
        ('concrete', 'pits'),
        {'pits_pore_density': 6.0, 'pits_pore_size_max': 1.8, 'pits_flake_coverage': 0.2,
         'pits_dent_density': 1.0, 'pits_strength': 1.2, 'concrete_amount': 4, 'concrete_size_min': 40.0,
         'concrete_size_max': 75.0, 'concrete_rebar_expose': 1.0}),
    'STORM_BEATEN_RC_WALL': _preset(
        'asset_combined', 'Concrete Damage + Pitting: deep breaks in a heavily flaked sea wall',
        ('concrete', 'pits'),
        {'pits_pore_density': 6.0, 'pits_pore_size_max': 1.8, 'pits_flake_coverage': 0.5,
         'pits_dent_density': 1.0, 'pits_strength': 1.2, 'concrete_amount': 3, 'concrete_size_min': 55.0,
         'concrete_size_max': 90.0, 'concrete_depth_min': 4.0, 'concrete_depth_max': 8.0,
         'concrete_rebar_broken': 0.8, 'pits_flake_depth': 0.5}),
    'CHIPPED_BLOCK_SPALLS': _preset(
        'asset_combined', 'Concrete Damage + Edges + Corners: a chipped block with deep spall pockets',
        ('concrete', 'edge', 'corner'),
        {'chips_mode': 'AUTO', 'amount': 160, 'size_min': 2.0, 'size_max': 7.0, 'depth_min': 0.8,
         'depth_max': 3.0, 'sec_amount': 2, 'corner_mode': 'AUTO', 'corner_prob': 100.0, 'corner_size_min':
         8.0, 'corner_size_max': 14.0, 'corner_depth_min': 4.0, 'corner_depth_max': 7.0, 'concrete_amount':
         4, 'concrete_size_min': 20.0, 'concrete_size_max': 38.0, 'concrete_rebar_enable': False}),
    'BATTERED_COLUMN_CAGE': _preset(
        'asset_combined', 'Concrete Damage + Edges: battered column arrises with the cage showing',
        ('concrete', 'edge'),
        {'chips_mode': 'AUTO', 'amount': 160, 'size_min': 2.0, 'size_max': 7.0, 'depth_min': 0.8,
         'depth_max': 3.0, 'sec_amount': 2, 'concrete_amount': 4, 'concrete_size_min': 18.0,
         'concrete_size_max': 34.0, 'concrete_depth_min': 2.0, 'concrete_depth_max': 4.0,
         'concrete_rebar_expose': 1.0, 'concrete_rebar_spacing_x': 12.0, 'concrete_rebar_spacing_y': 15.0}),
    'FIREFIGHT_SPALLED_WALL': _preset(
        'asset_combined', 'Concrete Damage + Bullets: spalled wall sprayed with bullet impacts',
        ('concrete', 'bullet'),
        {'bullet_mode': '0', 'bullet_material': '2', 'bullet_amount': 40, 'bullet_clusters': 3,
         'concrete_amount': 3, 'concrete_size_min': 40.0, 'concrete_size_max': 70.0}),
    'SNIPER_NEST_COLUMN': _preset(
        'asset_combined', 'Concrete V2 + Bullets: a column chewed up around tight bullet clusters',
        ('concrete_v2', 'bullet'),
        {'bullet_mode': '0', 'bullet_material': '2', 'bullet_amount': 50, 'bullet_clusters': 2,
         'concrete_amount': 3, 'concrete_size_min': 20.0, 'concrete_size_max': 38.0}),
    'QUAKE_CRACKED_SPALLS': _preset(
        'asset_combined', 'Concrete Damage + Fracture: structural cracks running through spalled concrete',
        ('concrete', 'fracture_concrete'),
        {'fract_l1_amount': 5, 'fract_l2_amount': 18, 'fract_l3_amount': 0, 'concrete_amount': 3,
         'concrete_size_min': 35.0, 'concrete_size_max': 60.0, 'concrete_crack_amount': 2}),
    'RADIAL_HIT_CONCRETE': _preset(
        'asset_combined', 'Concrete V2 + Radial Fracture: impact stars around deep spall pockets',
        ('concrete_v2', 'fracture_radial'),
        {'fract_l1_amount': 3, 'fract_l2_amount': 10, 'concrete_amount': 5, 'concrete_size_min': 30.0,
         'concrete_size_max': 55.0, 'concrete_depth_min': 3.0, 'concrete_depth_max': 6.0}),
    'RUBBLE_STREWN_SLAB': _preset(
        'asset_combined', 'Concrete Damage + Deposits: broken slab with rubble settled over it',
        ('concrete', 'deposits'),
        {'deposit_amount': 220, 'deposit_style': '2', 'deposit_size_min': 0.5, 'deposit_size_max': 3.0,
         'deposit_layers': 2, 'concrete_amount': 4, 'concrete_size_min': 35.0, 'concrete_size_max': 65.0,
         'concrete_rebar_expose': 0.9}),
    'DUSTY_WORN_FLOOR': _preset(
        'asset_combined', 'Concrete V2 + Deposits: soft worn floor pockets filling with grit',
        ('concrete_v2', 'deposits'),
        {'deposit_amount': 320, 'deposit_style': '0', 'deposit_size_min': 0.5, 'deposit_size_max': 3.0,
         'deposit_layers': 2, 'concrete_amount': 6, 'concrete_size_min': 25.0, 'concrete_size_max': 55.0,
         'concrete_wall_soft': 1.0, 'concrete_rebar_enable': False}),
    'GRASSY_BROKEN_SLAB': _preset(
        'asset_combined', 'Concrete V2 + Grass: an abandoned slab with grass taking over the damage',
        ('concrete_v2', 'grass'),
        {'concrete_amount': 4, 'concrete_size_min': 40.0, 'concrete_size_max': 70.0,
         'concrete_rebar_enable': False},
        settings={'grass': {'kind': 'FRESH', 'base_density': 90.0}}),
    'STONE_STREWN_POTHOLE_YARD': _preset(
        'asset_combined', 'Concrete Damage + Gravel: potholes in a yard strewn with loose stones',
        ('concrete', 'gravel'),
        {'concrete_amount': 5, 'concrete_size_min': 30.0, 'concrete_size_max': 55.0,
         'concrete_rebar_enable': False, 'concrete_wall_soft': 0.8},
        settings={'gravel': {'rock_subdivision': 1, 'base_density': 900.0}}),
    'IVY_ON_REBAR_WALL': _preset(
        'asset_combined', 'Concrete Damage + Ivy: ivy climbing a spalled reinforced wall',
        ('concrete', 'ivy'),
        {'concrete_amount': 5, 'concrete_size_min': 50.0, 'concrete_size_max': 95.0,
         'concrete_rebar_expose': 1.0},
        settings={'ivy': {'amount': 0.5}},
        seeds={'ivy': [(0.2, 0.0, 0.02), (0.5, 0.0, 0.02), (0.8, 0.0, 0.02)]}),
    'PAINTED_CONCRETE_SPALLS': _preset(
        'asset_combined', 'Concrete V2 + Peeling: painted concrete with peeling paint and spalls',
        ('concrete_v2', 'peeling'),
        {'peeling_size': 40.0, 'peeling_amount': 30.0, 'peeling_lift': 9.0, 'peeling_curl': 60.0,
         'peeling_plates': 50.0, 'concrete_amount': 3, 'concrete_size_min': 35.0, 'concrete_size_max': 65.0,
         'concrete_rebar_expose': 0.9}),
    'BREACHED_RC_SPALL_WALL': _preset(
        'asset_combined', 'Concrete Damage + Boolean Break: a through hole beside spalled, rebar-exposed patches',
        ('concrete', 'boolean'),
        {'concrete_amount': 3, 'concrete_size_min': 35.0, 'concrete_size_max': 60.0},
        breakers={'variant': 'SIMPLE', 'cutters': [{'shape': 'ROCK', 'rule': 'auto', 'size': 0.22, 'count': 1, 'through': True}]}),
    'SURFACE_POCKED_SPALLS': _preset(
        'asset_combined', 'Concrete V2 + Surface: spalls in a wall peppered with small craters',
        ('concrete_v2', 'surface'),
        {'surf_type': '0', 'surf_mode': 'PROCEDURAL', 'surf_amount': 220, 'surf_size_min': 3.0,
         'surf_size_max': 8.0, 'surf_depth_min': 1.0, 'surf_depth_max': 3.0, 'surf_sec_amount': 2,
         'concrete_amount': 3, 'concrete_size_min': 40.0, 'concrete_size_max': 70.0,
         'concrete_rebar_enable': False}),
    'WEATHERED_PITTED_BLOCK': _preset(
        'asset_combined', 'Pitting + Edges + Corners: an old cast block, worn at every edge',
        ('pits', 'edge', 'corner'),
        {'pits_pore_density': 6.0, 'pits_pore_size_max': 1.8, 'pits_flake_coverage': 0.2,
         'pits_dent_density': 1.0, 'pits_strength': 1.2, 'chips_mode': 'AUTO', 'amount': 160, 'size_min':
         2.0, 'size_max': 7.0, 'depth_min': 0.8, 'depth_max': 3.0, 'sec_amount': 2, 'corner_mode': 'AUTO',
         'corner_prob': 100.0, 'corner_size_min': 8.0, 'corner_size_max': 14.0, 'corner_depth_min': 4.0,
         'corner_depth_max': 7.0}),
    'CRACKED_PITTED_PAVING': _preset(
        'asset_combined', 'Pitting + Fracture: pore-riddled paving with long cracks',
        ('pits', 'fracture_concrete'),
        {'pits_pore_density': 6.0, 'pits_pore_size_max': 1.8, 'pits_flake_coverage': 0.2,
         'pits_dent_density': 1.0, 'pits_strength': 1.2, 'fract_l1_amount': 5, 'fract_l2_amount': 18,
         'fract_l3_amount': 0}),
    'PEELING_PITTED_RENDER': _preset(
        'asset_combined', 'Pitting + Peeling: painted render lifting off a pitted base',
        ('pits', 'peeling'),
        {'pits_pore_density': 6.0, 'pits_pore_size_max': 1.8, 'pits_flake_coverage': 0.2,
         'pits_dent_density': 1.0, 'pits_strength': 1.2, 'peeling_size': 55.0, 'peeling_amount': 45.0,
         'peeling_lift': 9.0, 'peeling_curl': 60.0, 'peeling_plates': 50.0}),
    'GRIME_FILLED_PORES': _preset(
        'asset_combined', 'Pitting + Deposits: an upward slab with grit settled in its pores',
        ('pits', 'deposits'),
        {'pits_pore_density': 6.0, 'pits_pore_size_max': 1.8, 'pits_flake_coverage': 0.2,
         'pits_dent_density': 1.0, 'pits_strength': 1.2, 'deposit_amount': 320, 'deposit_style': '0',
         'deposit_size_min': 0.5, 'deposit_size_max': 3.0, 'deposit_layers': 2}),
    'HOLE_IN_PITTED_WALL': _preset(
        'asset_combined', 'Hanging Holes + Pitting: a breach with bars through a weathered wall',
        ('hang', 'pits'),
        {'pits_pore_density': 6.0, 'pits_pore_size_max': 1.8, 'pits_flake_coverage': 0.2,
         'pits_dent_density': 1.0, 'pits_strength': 1.2, 'hang_amount': 1, 'hang_size_min': 100.0,
         'hang_size_max': 135.0}),
    'BULLET_RIDDLED_HOLES': _preset(
        'asset_combined', 'Hanging Holes + Bullets: shell holes in a wall sprayed by gunfire',
        ('hang', 'bullet'),
        {'bullet_mode': '0', 'bullet_material': '2', 'bullet_amount': 40, 'bullet_clusters': 3,
         'hang_amount': 2, 'hang_size_min': 70.0, 'hang_size_max': 105.0}),
    'CRACKED_AROUND_BREACH': _preset(
        'asset_combined', 'Hanging Holes + Fracture: long cracks radiating from a rebar breach',
        ('hang', 'fracture_concrete'),
        {'fract_l1_amount': 5, 'fract_l2_amount': 18, 'fract_l3_amount': 0, 'hang_amount': 1,
         'hang_size_min': 90.0, 'hang_size_max': 125.0}),
    'CHIPPED_WALL_HANGING_HOLE': _preset(
        'asset_combined', 'Hanging Holes + Edges + Corners: a breach in a wall with battered edges',
        ('hang', 'edge', 'corner'),
        {'chips_mode': 'AUTO', 'amount': 160, 'size_min': 2.0, 'size_max': 7.0, 'depth_min': 0.8,
         'depth_max': 3.0, 'sec_amount': 2, 'corner_mode': 'AUTO', 'corner_prob': 100.0, 'corner_size_min':
         8.0, 'corner_size_max': 14.0, 'corner_depth_min': 4.0, 'corner_depth_max': 7.0, 'hang_amount': 1,
         'hang_size_min': 90.0, 'hang_size_max': 120.0}),
    'IVY_THROUGH_HANGING_HOLE': _preset(
        'asset_combined', 'Hanging Holes + Ivy: ivy climbing past a breach with hanging bars',
        ('hang', 'ivy'),
        {'hang_amount': 2, 'hang_size_min': 100.0, 'hang_size_max': 150.0},
        settings={'ivy': {'amount': 0.5}},
        seeds={'ivy': [(0.2, 0.0, 0.02), (0.5, 0.0, 0.02), (0.8, 0.0, 0.02)]}),
    'SHOT_UP_BRICK_RENDER': _preset(
        'asset_combined', 'Brick + Bullets: rendered brick with bullet scars around the breaks',
        ('brick', 'bullet'),
        {**_BRICK_CLASSIC, 'bullet_mode': '0', 'bullet_material': '2', 'bullet_amount': 45,
         'bullet_clusters': 3, 'brick_amount': 3, 'brick_size_min': 80.0, 'brick_size_max': 130.0}),
    'IVY_CLAD_BRICK_RUIN': _preset(
        'asset_combined', 'Brick + Ivy: ivy spreading over exposed brickwork',
        ('brick', 'ivy'),
        {**_BRICK_CLASSIC, 'brick_amount': 3, 'brick_size_min': 130.0, 'brick_size_max': 200.0,
         'brick_missing': 0.12},
        settings={'ivy': {'amount': 0.5}},
        seeds={'ivy': [(0.2, 0.0, 0.02), (0.5, 0.0, 0.02), (0.8, 0.0, 0.02)]}),
    'PAINTED_BRICK_RENDER': _preset(
        'asset_combined', 'Brick + Peeling: painted render peeling beside exposed brick',
        ('brick', 'peeling'),
        {**_BRICK_CLASSIC, 'peeling_size': 40.0, 'peeling_amount': 30.0, 'peeling_lift': 9.0,
         'peeling_curl': 60.0, 'peeling_plates': 50.0, 'brick_amount': 2, 'brick_size_min': 80.0,
         'brick_size_max': 130.0}),
    'CRACKED_RENDER_BRICK': _preset(
        'asset_combined', 'Brick V2 + Fracture: render cracked through around brick patches',
        ('brick_v2', 'fracture_concrete'),
        {'fract_l1_amount': 5, 'fract_l2_amount': 18, 'fract_l3_amount': 0, 'brick_amount': 3,
         'brick_size_min': 70.0, 'brick_size_max': 120.0, 'brick_remesh': False, 'brick_back_resolution':
         0.5}),
    'BATTERED_BRICK_CORNERS': _preset(
        'asset_combined', 'Brick V2 + Edges + Corners: a brick-faced plinth with broken corners',
        ('brick_v2', 'edge', 'corner'),
        {'chips_mode': 'AUTO', 'amount': 160, 'size_min': 2.0, 'size_max': 7.0, 'depth_min': 0.8,
         'depth_max': 3.0, 'sec_amount': 2, 'corner_mode': 'AUTO', 'corner_prob': 100.0, 'corner_size_min':
         8.0, 'corner_size_max': 14.0, 'corner_depth_min': 4.0, 'corner_depth_max': 7.0, 'brick_amount': 4,
         'brick_size_min': 40.0, 'brick_size_max': 70.0, 'brick_remesh': False, 'brick_back_resolution': 0.5}),
    'DUST_ON_BRICK_WALL_TOP': _preset(
        'asset_combined', 'Brick + Deposits: a rendered block stripped to brick, grit and stones settled on top',
        ('brick', 'deposits'),
        {**_BRICK_CLASSIC, 'deposit_amount': 320, 'deposit_style': '2', 'deposit_size_min': 0.3,
         'deposit_size_max': 1.2, 'deposit_layers': 2, 'brick_amount': 3, 'brick_size_min': 80.0,
         'brick_size_max': 130.0}),
    'WALLPAPER_OVER_LATH': _preset(
        'asset_combined', 'Lath + Wallpaper: torn wallpaper around plaster broken down to the laths',
        ('lath', 'wallpaper'),
        {'wallpaper_tear_amount': 3, 'wallpaper_lift_amount': 6, 'lath_amount': 2, 'lath_size_min': 70.0,
         'lath_size_max': 100.0}),
    'PEELING_PAINT_LATH_WALL': _preset(
        'asset_combined', 'Laths V2 + Peeling: flaking paint around breaks exposing the laths',
        ('lath_v2', 'peeling'),
        {**_LATH_V2, 'peeling_size': 40.0, 'peeling_amount': 30.0, 'peeling_lift': 9.0, 'peeling_curl':
         60.0, 'peeling_plates': 50.0, 'lath_amount': 3, 'lath_size_min': 60.0, 'lath_size_max': 95.0}),
    'BULLETS_THROUGH_LATH': _preset(
        'asset_combined', 'Lath + Bullets: bullet holes through plaster with laths behind',
        ('lath', 'bullet'),
        {'bullet_mode': '0', 'bullet_material': '1', 'bullet_amount': 40, 'bullet_clusters': 3,
         'lath_amount': 3, 'lath_size_min': 60.0, 'lath_size_max': 90.0}),
    'CRACKED_RIVEN_PLASTER': _preset(
        'asset_combined', 'Riven Lath + Fracture: cracked old plaster around riven laths',
        ('lath_riven', 'fracture_concrete'),
        {**RIVEN_LOOK, 'fract_l1_amount': 5, 'fract_l2_amount': 18, 'fract_l3_amount': 0, 'lath_amount': 3,
         'lath_size_min': 70.0, 'lath_size_max': 105.0}),
    'ROUGH_PLASTER_RIVEN': _preset(
        'asset_combined', 'Riven Lath + Plaster: hand-trowelled plaster breaking over riven laths',
        ('lath_riven', 'plaster'),
        {**RIVEN_LOOK, 'plaster_crack_enable': True, 'lath_amount': 3, 'lath_size_min': 70.0,
         'lath_size_max': 105.0}),
    'WALLPAPERED_STUD_WALL': _preset(
        'asset_combined', 'Stud Framing + Wallpaper: papered drywall torn open onto studs',
        ('lath_framing', 'wallpaper'),
        {**FRAMING_LOOK, 'wallpaper_tear_amount': 3, 'wallpaper_lift_amount': 6, 'lath_amount': 2,
         'lath_size_min': 75.0, 'lath_size_max': 105.0}),
    'PAINTED_DRYWALL_STUDS': _preset(
        'asset_combined', 'Stud Framing + Peeling: peeling paint on drywall torn open onto studs',
        ('lath_framing', 'peeling'),
        {**FRAMING_LOOK, 'peeling_size': 40.0, 'peeling_amount': 22.0, 'peeling_lift': 9.0, 'peeling_curl':
         60.0, 'peeling_plates': 50.0, 'lath_amount': 2, 'lath_size_min': 75.0, 'lath_size_max': 105.0}),
    'SHOT_DRYWALL_STUDS': _preset(
        'asset_combined', 'Stud Framing + Bullets: gunfire through drywall, studs behind',
        ('lath_framing', 'bullet'),
        {**FRAMING_LOOK, 'bullet_mode': '0', 'bullet_material': '1', 'bullet_amount': 40, 'bullet_clusters':
         3, 'lath_amount': 2, 'lath_size_min': 70.0, 'lath_size_max': 100.0}),
    'GROUND_ROOTS_CRACKED_SLAB': _preset(
        'asset_combined', 'Concrete V2 + Roots: tree roots creeping across a spalled slab',
        ('concrete_v2', 'roots'),
        {'concrete_amount': 7, 'concrete_size_min': 35.0, 'concrete_size_max': 65.0,
         'concrete_rebar_enable': False},
        seeds={'roots': [(0.25, 0.25, 1.0), (0.75, 0.7, 1.0)]}),
}


from . import combined_presets
CATEGORY_TITLES.update(combined_presets.CATEGORY_TITLES)
LIBRARY.update(combined_presets.create(LIBRARY, _preset))

from . import presets_new_damage
CATEGORY_TITLES.update(presets_new_damage.CATEGORY_TITLES)
LIBRARY.update(presets_new_damage.create(_preset))

from .removed_controls import clean as _clean_removed
for _entry in LIBRARY.values():
    _entry['props'] = _clean_removed(_entry['props'])
    if 'edge' in _entry['effects']:
        _entry['props'].setdefault('noise_roughness', 0.0)
        _entry['props'].setdefault('noise_distortion', 0.0)


def unavailable_effects(entry):
    from . import sidebar, boolean_break
    # solidify теперь вариант Fabric Tears, остальные скрытые эффекты не предлагаем
    supported = (set(sidebar.CATALOG) - sidebar.HIDDEN_EFFECTS - sidebar.REMOVED_EFFECTS) | {'fabric_solid'}
    missing = {effect for effect in entry['effects'] if effect not in supported}
    active_flags = {sidebar.CATALOG[effect][2] for effect in supported if effect in sidebar.CATALOG}
    for effect, row in sidebar.CATALOG.items():
        if effect not in supported and row[2] not in active_flags and entry['props'].get(row[2]):
            missing.add(effect)
    variant = (entry.get('breakers') or {}).get('variant')
    if variant and variant not in boolean_break.IMPORTABLE_VARIANTS:
        missing.add('boolean_' + variant.lower())
    if 'boolean' in entry['effects'] and entry['props'].get('rebar_enable'):
        fill = str(entry['props'].get('rebar_fill', '0'))
        variant = next((v[0] for v in boolean_break._SAVED_VARIANTS if v[5] == fill), None)
        if variant and variant not in boolean_break.IMPORTABLE_VARIANTS:
            missing.add('boolean_' + variant.lower())
    return missing


UNAVAILABLE_PRESETS = {key: tuple(sorted(missing)) for key, entry in LIBRARY.items()
                       if (missing := unavailable_effects(entry))}
for _key in UNAVAILABLE_PRESETS:
    del LIBRARY[_key]

_LIBRARY_PROPS = {name for entry in LIBRARY.values() for name in entry['props']}
_LIBRARY_SETTINGS = {}
for _entry in LIBRARY.values():
    for _group, _values in _entry['settings'].items():
        _LIBRARY_SETTINGS.setdefault(_group, set()).update(_values)


def preset_title(key):
    return (key[3:] if key.startswith('CD_') else key).replace('_', ' ').title()


def props_table():
    """{key: root values} for presets_data.PRESETS (so the apply operator and
    the preview catalog see library keys exactly like the core three)."""
    return {key: dict(entry['props']) for key, entry in LIBRARY.items()}


def apply(src, key, *, additive=False, context=None):
    """Apply the preset, keeping unrelated effects and their settings in Add mode."""
    from . import sidebar, preset_breakers, boolean_auto, boolean_break
    entry = LIBRARY[key]
    p = src.project_damage
    from .damage_generator_assets import RETIRED_KEYS
    effects = [effect for effect in entry['effects'] if effect not in RETIRED_KEYS]
    excluded = ()
    breakers = entry['breakers']
    keep_peeling = additive and p.peeling_enable
    if additive:
        from .presets import additive_effects
        effects, excluded = additive_effects(src, effects)
        if breakers and sidebar.enabled(src, 'boolean'):
            if boolean_break.variant(p) != breakers.get('variant', 'SIMPLE'):
                effects = [effect for effect in effects if effect != 'boolean']
                excluded += ('break_', 'boolean_', 'rebar_', 'ui_boolean_', 'pipe_', 'cable_')
                breakers = None
        if not effects:
            return False
    else:
        preset_breakers.clear(src)
        for effect, row in sidebar.CATALOG.items():
            if effect in sidebar.REMOVED_EFFECTS or not hasattr(p, row[2]):
                continue
            if sidebar.enabled(src,effect) and effect not in effects:
                sidebar.set_enabled(src, effect, False)
        for prop in _CORE:
            setattr(p, prop, False)
    for effect in effects:
        if not additive or not sidebar.enabled(src, effect):
            sidebar.set_enabled(src, effect, True)

    from . import fracture_stack
    for effect in effects:
        if effect in fracture_stack.FRACTURE_KEYS:fracture_stack.select(src,effect)
    if not additive:
        for name in _LIBRARY_PROPS - entry['props'].keys() - {'dbevel_preset'}:
            p.property_unset(name)
    for name, value in entry['props'].items():
        if keep_peeling and name in {'peeling_wrap_edges', 'peeling_damage_factor'}:
            continue
        if additive and (name.startswith(excluded) or
                         (name in sidebar.ENABLE_PROPS and not value)):
            continue
        setattr(p, name, value)
    if any(effect in {'fabric','fabric_solid'} for effect in effects):
        p.fabric_geometry='SOLIDIFY' if 'fabric_solid' in effects else 'FLAT'
    if 'peeling' in effects and not keep_peeling:
        if 'peeling_wrap_edges' not in entry['props']:
            p.peeling_wrap_edges = True
        if 'peeling_damage_factor' not in entry['props']:
            p.peeling_damage_factor = 1.
    for group, values in entry['settings'].items():
        settings = getattr(src, 'project_damage_' + group)
        if not additive:
            for name in _LIBRARY_SETTINGS.get(group, set()) - values.keys():
                settings.property_unset(name)
        for name, value in values.items():
            setattr(settings, name, value)
        if group in {'metal_gen','wood_gen','tile_gen','erosion_gen','crater_gen'}:
            settings['_in_hierarchy']=True
            settings['_resolution_reference']=settings.resolution
    if key in presets_new_damage.create(_preset):
        presets_new_damage.inputs(src,key,context or __import__('bpy').context,additive)
    for group, points in entry['seeds'].items():
        getattr(src, 'project_damage_' + group).seeds = _seed_collection(src, group, points)
    if 'boolean' in effects and breakers:
        p.break_variant = breakers.get('variant', 'SIMPLE')
    if 'boolean' in effects and boolean_break.variant(p) in boolean_auto.SUPPORTED and additive:
        if context is None:
            import bpy
            context = bpy.context
        boolean_auto.setup_existing(context, src)
    if breakers and 'boolean' in effects:
        preset_breakers.place(src, breakers, seed=p.seed, replace_existing=not additive)
    if 'boolean' in effects and boolean_break.variant(p) in boolean_auto.SUPPORTED and not additive:
        if context is None:
            import bpy
            context = bpy.context
        boolean_auto.setup_existing(context, src)
    fracture_stack.flags(src)
    return True


def _seed_collection(src, group, points):
    """Empties at bounding-box fractions of src, in one collection per object
    and generator (reused, so applying again replaces the old seeds)."""
    import bpy
    from mathutils import Vector
    name = '%s %s Seeds' % (src.name, group.title())
    col = bpy.data.collections.get(name)
    if col is None:
        col = bpy.data.collections.new(name)
        (src.users_collection[0] if src.users_collection else bpy.context.scene.collection).children.link(col)
    for old in list(col.objects):
        bpy.data.objects.remove(old)
    corners = [src.matrix_world @ Vector(c) for c in src.bound_box]
    lo = Vector([min(c[i] for c in corners) for i in range(3)])
    hi = Vector([max(c[i] for c in corners) for i in range(3)])
    for i, (x, y, z) in enumerate(points):
        empty = bpy.data.objects.new('%s Seed %d' % (group.title(), i + 1), None)
        empty.empty_display_type = 'SPHERE'
        empty.empty_display_size = max((hi - lo).length * 0.02, 0.01)
        empty.location = lo + Vector((x * (hi.x - lo.x), y * (hi.y - lo.y), z * (hi.z - lo.z)))
        col.objects.link(empty)
    return col


def grow(src, context):
    """Grow ivy / roots for a look that seeds them; returns the generators grown."""
    import bpy
    grown = []
    for group in ('ivy', 'roots'):
        if getattr(src.project_damage, group + '_enable', False) and getattr(src, 'project_damage_' + group).seeds:
            with context.temp_override(active_object=src, object=src, selected_objects=[src]):
                if getattr(bpy.ops.project_damage, 'grow_' + group)() == {'FINISHED'}:
                    grown.append(group)
    return grown
