# категории и поисковые слова для библиотеки эффектов
MATERIALS = (
    ('concrete', 'Concrete', ('surface','edge','corner','fracture','fracture_concrete','fracture_radial','fracture_proximity_default','fracture_proximity_radial','concrete','concrete_v2','pits','hang','hit','plaster','brick','brick_v2','lath','lath_v2','lath_riven','lath_framing','pipes','cables','ground','fissure','erosion','cellf','dbevel','deform')),
    ('wood', 'Wood', ('surface','edge','corner','fracture_wood','wood','lath','lath_v2','lath_riven','lath_framing','plank_surface','deform','cellf')),
    ('metal', 'Metal', ('metaltear','rusty','weld','dbevel','scratch','bullet','deform','pipes','cables','cable_curve','cable_proximity','hang','erosion','cellf')),
    ('brick', 'Brick', ('brick','brick_v2','edge','corner','surface','erosion','cellf')),
    ('glass', 'Glass', ('glass_hit','glass_drawn','fracture_glass','fracture_proximity_glass','cellf')),
    ('fabric', 'Fabric', ('fabric','cloth_hanging','cloth_curve','cloth_empties')),
    ('paper', 'Paper & Coatings', ('paper_generator','wallpaper','peeling')),
    ('stone', 'Stone & Soil', ('surface','edge','corner','erosion','clay','ground','fissure','gravel','deposits','piles','cellf')),
    ('vegetation', 'Vegetation', ('ivy','roots','grass')),
    ('sealant', 'Sealants', ('foam','buildup')),
)
TYPES = (
    ('wall', 'Wall', ('concrete','concrete_v2','brick','brick_v2','pits','hang','hit','plaster','lath','lath_v2','lath_riven','lath_framing','wallpaper','peeling','pipes','cables','foam','plank_surface')),
    ('scratch', 'Scratch', ('scratch',)),
    ('crack', 'Cracks & Fracture', ('fracture','fracture_concrete','fracture_wood','fracture_glass','fracture_radial','fracture_proximity_default','fracture_proximity_radial','fracture_proximity_glass','cellf','ground','fissure')),
    ('surface', 'Surface Wear', ('surface','erosion','pits','plaster','peeling','clay','rusty','deposits','dbevel')),
    ('edges', 'Edges & Corners', ('edge','corner','dbevel','buildup','piles','weld')),
    ('tear', 'Tears & Holes', ('metaltear','fabric','glass_hit','glass_drawn','bullet','hit','hang','rusty','wallpaper','paper_generator')),
    ('structure', 'Structural Inserts', ('concrete','concrete_v2','hang','lath','lath_v2','lath_riven','lath_framing','brick','brick_v2','pipes','cables','plank_surface')),
    ('scatter', 'Scatter & Growth', ('grass','gravel','ivy','roots','foam','deposits','piles')),
    ('deformation', 'Deformation', ('deform','clay','paper_generator','cloth_hanging','cloth_curve','cloth_empties')),
    ('cables', 'Cables & Tubes', ('pipes','cables','cable_curve','cable_proximity')),
)
CATEGORIES = (
    ('surface', 'Surface & Edge Wear', ('surface','edge','corner','scratch','dbevel','buildup','piles','deposits','erosion','gen_erosion')),
    ('fracture', 'Cracks & Fracture', ('fracture','fracture_concrete','fracture_wood','fracture_radial','fracture_proximity_default','fracture_proximity_radial')),
    ('concrete', 'Concrete & Brick', ('concrete','concrete_v2','pits','hang','hit')),
    ('wood', 'Wood & Lath', ('lath','lath_v2','lath_riven','lath_framing','wood','gen_wood')),
    ('boolean_damage', 'Boolean Damage', ('boolean',)),
    ('metal', 'Metal & Impacts', ('metaltear','rusty','weld','bullet','metal','blast','gen_metal')),
    ('glass', 'Glass', ('glass_hit','glass_drawn','fracture_glass','fracture_proximity_glass','glass')),
    ('fabric', 'Fabric', ('fabric','fabric_solid')),
    ('coatings', 'Coatings & Deform', ('peeling','wallpaper','paper_generator','plaster','foam','clay','deform','coating')),
    ('nature', 'Ground & Nature', ('ground','fissure','ivy','roots','grass','gravel','gen_crater')),
    ('services', 'Pipes & Cables', ('pipes','cables')),
    ('standalone', 'Standalone', ('structure_rebars','structure_pipes','structure_laths','structure_studs','structure_bricks','plank_surface','cable_curve','cable_proximity','cloth_hanging','cloth_curve','cloth_empties')),
    ('physical', 'Physics Support', ('cellf','rbf','debris','fracture_through','fracture_volume')),
)


def search_text(key):
    return ' '.join(title for _,title,keys in MATERIALS+TYPES if key in keys)


def preset_category(key, entry, legacy=''):
    effects=set(entry.get('effects',()))
    lookup={name:set(keys) for name,_,keys in CATEGORIES}
    for name in ('glass','fabric','physical','boolean_damage','services','wood','concrete','coatings','metal','nature','fracture','surface','standalone'):
        if effects & lookup[name]:return name
    aliases={'edges_corners':'surface','surface_erosion':'surface','cracks_fracture':'fracture',
             'fracture_damage':'fracture','fabric_cloth':'fabric','ground_nature':'nature','ivy_roots':'nature',
             'metal_industrial':'metal','wood_timber':'wood','walls_interiors':'concrete',
             'stone_masonry':'concrete','combined_concrete':'concrete','combined_wood':'wood',
             'combined_metal':'metal','peeling':'coatings','asset_brick':'concrete','asset_lath':'wood',
             'asset_concrete':'concrete','asset_hang':'concrete'}
    return 'boolean_damage' if legacy.startswith('bb_') else aliases.get(legacy,'surface')

BOOLEAN_MATERIALS = {
    'SIMPLE': ('concrete','wood','metal','brick','stone'),
    'REBAR': ('concrete','metal'), 'CENTRAL': ('concrete','metal'),
    'BRICKS': ('brick','concrete'), 'PLANKS': ('wood',), 'LATHS': ('wood',),
    'FRAMING': ('wood',), 'PIPES': ('metal','concrete'), 'CABLES': ('metal','concrete'),
}


def boolean_tags(variant):
    return set() if variant=='BRICKS' else {'boolean_damage','physical'}


def boolean_matches(variant, category='ALL', query=''):
    from .boolean_break import VARIANTS
    tags = boolean_tags(variant)
    if category != 'ALL' and category not in tags:
        return False
    row = next(row for row in VARIANTS if row[0] == variant)
    names = {key:title for key,title,_ in CATEGORIES}
    text = ' '.join((*row[:4], *BOOLEAN_MATERIALS.get(variant,()), *(names.get(tag,tag) for tag in tags))).casefold()
    return all(word in text for word in query.casefold().split())

GENERATOR_TAGS = {key:{category} for category,_,keys in CATEGORIES for key in keys if key.startswith('gen_')}

