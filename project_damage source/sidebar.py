# библиотека эффектов, список включённых генераторов и основные вкладки
# SPDX-License-Identifier: GPL-3.0-or-later
"""Categorized Add and Tools panels; effect management is not a modifier stack."""
from types import SimpleNamespace
from functools import lru_cache
import time
from pathlib import Path
import bpy
import bpy.utils.previews
from bpy.app.handlers import persistent
from . import ownership, dev_tools, material_controls, ui_image_hints
from . import effect_layout, generator_assets, baked_materials
from .apply_selected import PROJECT_DAMAGE_OT_apply_selected
from .fracture_assets import TYPE_BY_ASSET, FRACTURE_KEYS, active_key

SOCIAL_LINKS = {
    'superhive': ('Superhive', 'https://superhivemarket.com/creators/lazy3d'),
    'gumroad': ('Gumroad', 'https://lazy3d.gumroad.com/'),
    'patreon': ('Patreon', 'https://www.patreon.com/Lazy3d'),
    'x': ('X', 'https://x.com'),
    'youtube': ('YouTube', 'https://www.youtube.com/playlist?list=PLbD5I1cRdfaU'),
}
TUTORIAL_URL = 'https://www.youtube.com/playlist?list=PLbD5I1cRdfaU'

                                                         
EFFECTS = (
    ('edge','Edge Damage','chips_enable','MOD_BEVEL'),
    ('surface','Surface Damage','surf_enable','MOD_DISPLACE'),
    ('corner','Corner Damage','corner_enable','VERTEXSEL'),
    ('fracture','Fracture','fract_enable','MOD_EXPLODE'),
    ('fracture_through','Fracture Through','fract_through_enable','MOD_EXPLODE'),
    ('fracture_volume','Fracture Volume','fract_through_enable','MOD_EXPLODE'),
    ('buildup','Bead','build_enable','MOD_BUILD'),
                                                                                       
    ('piles','Corner Piles','pile_enable','MESH_ICOSPHERE'),
    ('deposits','Deposits','deposit_enable','MOD_PARTICLES'),
    ('scratch','Scratches Draw','scratch_enable','GREASEPENCIL'),
    ('deform','Impact Deform','deform_enable','MOD_MESHDEFORM'),
    ('ivy','Ivy Generator','ivy_enable','OUTLINER_OB_CURVES'),
    ('roots','Roots Generator','roots_enable','FORCE_CURVE'),
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
    ('rusty','Rusty Holes','rusty_enable','MOD_DECIM'),
    ('wallhole','Wall Hole (Bricks)','wallhole_enable','MOD_BUILD'),
    ('boolean','Boolean Break','ui_boolean_enable','MOD_BOOLEAN'),
)
EFFECTS += (('fabric_solid','Fabric Tears','fabric_solid_enable','MOD_SOLIDIFY'),)
EFFECTS += (('erosion','Voxel Erosion','erosion_enable','MOD_REMESH'),)
EFFECTS += (('pits','Concrete Pitting','pits_enable','MOD_NOISE'),)
EFFECTS += (('weld','Welded Edges','weld_enable','MOD_SCREW'), ('grass','Grass Scatter','grass_enable','CURVES'), ('gravel','Gravel Scatter','gravel_enable','MESH_ICOSPHERE'))
EFFECTS += (('foam','Expanding Foam','foam_enable','META_BALL'),)
EFFECTS += (('pipes','Pipe Damage','pipe_damage_enable','MESH_CYLINDER'),)
EFFECTS += (('cables','Cable Damage','cable_damage_enable','PLUGIN'),)
EFFECTS += (('lath_riven','Riven Lath','lath_riven_enable','MOD_LATTICE'),)
EFFECTS += (('lath_framing','Stud Framing','lath_framing_enable','SNAP_GRID'),)
EFFECTS += (('hit_through','Concrete Hit Through','hit_through_enable','FORCE_FORCE'),
            ('glass_through','Broken Glass Through','glass_through_enable','MESH_UVSPHERE'),
            ('debris','Damage Debris','debris_enable','PARTICLES'))
EFFECTS += (('concrete_v2','Concrete Damage V2','concrete_v2_enable','MESH_ICOSPHERE'), ('lath_v2','Lath and Plaster V2','lath_v2_enable','MESH_GRID'), ('brick_v2','Brick Damage V2','brick_v2_enable','MOD_BUILD'), ('hang','Hanging Rebar Holes','hang_enable','MOD_WIREFRAME'), ('glass_hit','Hit Glass','glass_hit_enable','EMPTY_AXIS'), ('glass_drawn','Glass Drawn','glass_drawn_enable','GREASEPENCIL'))
CATALOG={r[0]:r for r in EFFECTS}
TILE_LABELS = {'edge':'Edges','surface':'Surface','corner':'Corners','fracture':'Fracture',
    'fracture_through':'Fracture Through','fracture_volume':'Fracture Volume','buildup':'Bead','piles':'Corner Piles','deposits':'Deposits','scratch':'Scratches',
    'deform':'Impact Deform','ivy':'Ivy','roots':'Roots','coating':'Peeling','fabric':'Fabric',
    'concrete':'Concrete','hit':'Hit','plaster':'Plaster','metal':'Metal','wood':'Wood',
    'lath':'Lath','wallpaper':'Wallpaper','bullet':'Bullets','blast':'Destruction',
    'ground':'Ground','brick':'Bricks','glass':'Glass','fissure':'Fissures','boolean':'Boolean'}
TILE_LABELS.update(concrete_v2='Concrete V2', lath_v2='Lath V2', brick_v2='Brick V2', hang='Rebar Holes', glass_hit='Hit Glass', glass_drawn='Glass Drawn', blast='Buildings', fabric='Fabric Tears', fabric_solid='Fabric Tears')
TILE_LABELS.update(rusty='Rusty Holes',hit_through='Hit Through',glass_through='Glass Through',debris='Damage Debris',lath_riven='Riven Lath')
EFFECTS += (
    ('fracture_concrete','Fracture Concrete','fract_enable','MOD_EXPLODE'),
    ('fracture_wood','Fracture Wood','fract_enable','MOD_EXPLODE'),
    ('fracture_glass','Fracture Glass','fract_enable','MOD_EXPLODE'),
    ('fracture_radial','Fracture Radial','fract_enable','MOD_EXPLODE'),
    ('fracture_proximity_default','Fracture Proximity Default','fract_enable','MOD_EXPLODE'),
    ('fracture_proximity_radial','Radial Fracture Proximity','fract_enable','MOD_EXPLODE'),
    ('fracture_proximity_glass','Glass Fracture Proximity','fract_enable','MOD_EXPLODE'),
)
CATALOG={r[0]:r for r in EFFECTS}
TILE_LABELS.update({key:CATALOG[key][1] for key in TYPE_BY_ASSET})
EFFECTS += (('rbf','Rigid Body Fracture','rbf_enable','RIGID_BODY'),)
CATALOG={r[0]:r for r in EFFECTS}
TILE_LABELS['rbf']='Rigid Body Fracture'
EFFECTS += (('cellf','Cell Fracture','cellf_enable','MOD_EXPLODE'),)
CATALOG={r[0]:r for r in EFFECTS}
TILE_LABELS['cellf']='Cell Fracture'
TILE_LABELS['pits']='Concrete Pitting'
TILE_LABELS.update(weld='Welded Edges',grass='Grass Scatter',gravel='Gravel Scatter')
TILE_LABELS['foam']='Expanding Foam'
TILE_LABELS['lath_framing']='Stud Framing'
TILE_LABELS['pipes']='Pipe Damage'
TILE_LABELS['cables']='Cable Damage'
EFFECTS=tuple((key,'Laths V2 Smoother Edges' if key=='lath_v2' else label,prop,icon)
              for key,label,prop,icon in EFFECTS)
CATALOG={row[0]:row for row in EFFECTS}
TILE_LABELS['lath_v2']='Laths V2 Smoother Edges'
from . import damaged_bevel as _damaged_bevel
EFFECTS += (_damaged_bevel.TILE,)
CATALOG={r[0]:r for r in EFFECTS}
TILE_LABELS[_damaged_bevel.KEY]=_damaged_bevel.TILE_LABEL
EFFECTS += (('clay','Clay','clay_enable','SCULPTMODE_HLT'),)
CATALOG={r[0]:r for r in EFFECTS}
TILE_LABELS['clay']='Clay'
EFFECTS += (('metaltear','Metal Tear Draw','metaltear_enable','GREASEPENCIL'),)
CATALOG={r[0]:r for r in EFFECTS}
TILE_LABELS['metaltear']='Metal Tear Draw'
EFFECTS += (('peeling','Peeling','peeling_enable','MOD_SOLIDIFY'),)
CATALOG={r[0]:r for r in EFFECTS}
TILE_LABELS['peeling']='Peeling'
EFFECTS += (('paper_generator','Paper Generator','paper_generator_enable','MESH_PLANE'),)
CATALOG={row[0]:row for row in EFFECTS}
from . import damage_generator_assets
EFFECTS += damage_generator_assets.ROWS
CATALOG = {row[0]:row for row in EFFECTS}
TILE_LABELS.update({row[0]:row[1] for row in damage_generator_assets.ROWS})
REMOVED_EFFECTS={'heat','bevel','rust','glass_through','rbf','metal','fracture_volume','glass'}
HIDDEN_EFFECTS={'brick','brick_v2','fabric_solid','hit_through','coating','glass_through','rbf','debris','wallhole','blast','fracture_through'}                                              
HIDDEN_EFFECTS.update(damage_generator_assets.RETIRED_KEYS)
ENABLE_PROPS={r[2] for r in EFFECTS}
_SYNCING=False

                                                                
                                                                 
from . import effect_categories
EFFECT_CATEGORIES = effect_categories.CATEGORIES

TOOLS_CATEGORIES = (
    ('PREP', 'Mesh Preparation', 'Input repair, open surfaces and Smart Remesh'),
    ('CLEANUP', 'Cleanup / Trim', 'Remove floating parts and trim generated results'),
    ('MASKS', 'Advanced / Masks', 'Avoid regions, vertex-group masks and edge rules'),
    ('OUTPUT', 'Output', 'Source-data preservation, topology and UV output'),
    ('PERFORMANCE', 'Performance', 'Background calculation, CPU budget and caching'),
)

                                                                           
EXTRA_EFFECT_TAGS = {}


def effect_tags(key):
    if key in {'brick','brick_v2'}:return set()
    tags = {cat for cat, _, keys in EFFECT_CATEGORIES if key in keys}
    tags.update(EXTRA_EFFECT_TAGS.get(key, ()))
    tags.update(effect_categories.GENERATOR_TAGS.get(key, ()))
    if key == 'boolean':
        from .boolean_break import IMPORTABLE_VARIANTS
        for variant in effect_categories.BOOLEAN_MATERIALS:
            if variant in IMPORTABLE_VARIANTS:tags.update(effect_categories.boolean_tags(variant))
    if key in PHYSICS_KEYS and key not in {'piles', 'deposits'}:
        tags.add('physical')
    else:
        tags.discard('physical')
    return tags


def browser_entries(category='ALL', query=''):
    return _browser_entries(category, query, dev_tools.layout_revision())


@lru_cache(maxsize=32)
def _browser_entries(category, query, revision):
    """UI-only filtering: never changes enable flags or queues geometry."""
    words = query.casefold().split()
    titles = {cat:title for cat,title,_ in EFFECT_CATEGORIES}
    found = []
    for row in dev_tools.tiles(EFFECTS + generator_assets.ASSETS):
        key, label, _, _ = row
        if key in HIDDEN_EFFECTS or key in REMOVED_EFFECTS:
            continue
        tags = effect_tags(key)
        if category != 'ALL' and category not in tags:
            continue
        searchable = ' '.join((key, label, dev_tools.effect_name(key,label),
                               effect_categories.search_text(key), *(titles[tag] for tag in tags))).casefold()
        if key == 'boolean':
            from .boolean_break import VARIANTS, IMPORTABLE_VARIANTS
            if any(effect_categories.boolean_matches(row[0], category, query) for row in VARIANTS if row[0] in IMPORTABLE_VARIANTS):
                found.append(row)
            continue
        if key == 'pipes':
            searchable += ' pipes pipe plumbing conduit copper pvc steel cast iron tubes wires'
        if key == 'cables':
            searchable += ' cables cable wires wire electrical electric power data armored hanging ceiling'
        if key == 'foam':
            searchable += ' expanding spray polyurethane foam insulation seams gaps sealant'
        if key == 'erosion':searchable += ' eroded weathered stone rock concrete voxel remesh'
        if key == 'pits':
            searchable += ' pits pitting pores pockmarks bug holes air voids dents chips flaking scaling honeycomb concrete surface texture seamless'
        if all(word in searchable for word in words):
            found.append(row)
    if category == 'physical':
        found.sort(key=lambda row: row[0] != 'cellf')
    return found

                                                                             
                                                                               
                                                                             
_PREVIEWS = None
_PREVIEW_FILES = {}
_PREVIEW_CACHE = {}
_PREVIEW_ROOT = Path(__file__).resolve().parent / 'previews'


def _preview_icon(key, fallback):
    """Keep disk checks outside the per-frame path; refresh edited icons in 5 s."""
    global _PREVIEWS
    now = time.monotonic()
    cached = _PREVIEW_CACHE.get((key, fallback))
    if cached is not None and now < cached[1]:
        return cached[0]
    path = _PREVIEW_ROOT / (key + '.png')
    icon_id = 0
    try:
        stamp = path.stat().st_mtime_ns
        if _PREVIEWS is None:
            _PREVIEWS = bpy.utils.previews.new()
        if _PREVIEW_FILES.get(key) != stamp or key not in _PREVIEWS:
            if key in _PREVIEWS: del _PREVIEWS[key]
            _PREVIEWS.load(key, str(path), 'IMAGE')
            _PREVIEW_FILES[key] = stamp
        icon_id = _PREVIEWS[key].icon_id
    except (OSError, RuntimeError, KeyError):
        icons = bpy.types.UILayout.bl_rna.functions['prop'].parameters['icon'].enum_items
        item = icons.get(fallback)
        icon_id = item.value if item is not None else 0
    _PREVIEW_CACHE[(key, fallback)] = (icon_id, now + 5.0)
    return icon_id


def _draw_effect_tiles(layout, context, src):
    wm = context.window_manager
    top = layout.row(align=True)
    top.prop(wm, 'project_damage_add_search', text='', icon='VIEWZOOM')
    if wm.project_damage_add_search:
        clear = top.operator('wm.context_set_string', text='', icon='X')
        clear.data_path = 'window_manager.project_damage_add_search'
        clear.value = ''

    tabs = layout.row(align=True)
    tabs.scale_y = 1.5
    tabs.prop(wm, 'project_damage_add_category', text='Category', expand=False)
    layout.prop(wm, 'project_damage_add_view', expand=True)
    entries = browser_entries(wm.project_damage_add_category, wm.project_damage_add_search)
    if wm.project_damage_add_view == 'COMPACT':
        _draw_thumbnail_browser(layout, wm, src)
    else:
        _draw_asset_grid(layout, src, entries, full=True, category=wm.project_damage_add_category, query=wm.project_damage_add_search)


_THUMBNAIL_NUMBERS = {row[0]: i for i, row in enumerate(EFFECTS + generator_assets.ASSETS, 1)}
_THUMBNAIL_STRINGS = {}


def _thumbnail_entries(wm):
    from . import boolean_break
    rows = browser_entries(wm.project_damage_add_category, wm.project_damage_add_search)
    for key, label, _, icon in rows:
        if key == 'boolean':
            for i, (variant, title, _, description, variant_icon, _) in enumerate(boolean_break.VARIANTS, 1):
                if variant == 'BRICKS':continue
                if not effect_categories.boolean_matches(variant, wm.project_damage_add_category, wm.project_damage_add_search):
                    continue
                preview_key = key if variant == 'SIMPLE' else 'boolean_' + variant.lower()
                title = dev_tools.effect_name(key, title) if variant == 'SIMPLE' else title
                yield preview_key, title, description, variant_icon, len(_THUMBNAIL_NUMBERS) + i, key, variant
        else:
            title = dev_tools.effect_name(key, label)
            yield key, title, 'Select ' + title, icon, _THUMBNAIL_NUMBERS[key], key, None


def _thumbnail_items(wm, context):
    items = []
    for key, title, description, icon, number, _, _ in _thumbnail_entries(wm):
        strings = (key, title, description)
        # Blender retains references to strings supplied by dynamic enum callbacks.
        strings = _THUMBNAIL_STRINGS.setdefault(strings, strings)
        items.append((*strings, _preview_icon(key, icon), number))
    return items or [('NONE', 'No matching effects', '', 0, 0)]


def _thumbnail_get(wm):
    numbers = [entry[4] for entry in _thumbnail_entries(wm)]
    selected = wm.get('project_damage_effect_thumbnail', 0)
    return selected if selected in numbers else numbers[0] if numbers else 0


def _thumbnail_set(wm, value):
    wm['project_damage_effect_thumbnail'] = value


def _add_view_get(wm):
    override = wm.get('project_damage_add_view')
    if override in (0, 1):
        return override
    addon = bpy.context.preferences.addons.get(__package__)
    return int(addon is None or addon.preferences.default_view_mode == 'FULL')


def _add_view_set(wm, value):
    wm['project_damage_add_view'] = value


def _draw_thumbnail_browser(layout, wm, src):
    selected = _thumbnail_get(wm)
    entry = next((entry for entry in _thumbnail_entries(wm) if entry[4] == selected), None)
    if entry is None:
        layout.label(text='No matching effects', icon='INFO')
        return
    _, title, _, icon, _, effect, variant = entry
    box = layout.box()
    box.template_icon_view(wm, 'project_damage_effect_thumbnail', show_labels=True,
                           scale=6.0, scale_popup=5.0)
    row = box.row()
    row.alignment = 'CENTER'
    row.label(text=title, icon=icon)
    row = box.row(align=True)
    label = 'Apply ' + title
    row.operator_context = 'INVOKE_DEFAULT'
    if variant:
        row.operator('project_damage.boolean_variant_add', text=label, icon='CHECKMARK').variant = variant
    elif effect in generator_assets.KEYS:
        row.operator('project_damage.add_generator_asset', text=label, icon='CHECKMARK').asset = effect
    else:
        op = row.operator('project_damage.effect_action', text=label, icon='CHECKMARK')
        op.effect, op.action = effect, 'ADD'
    if variant:
        from . import boolean_break
        can_remove = variant == boolean_break.variant(src.project_damage)
    else:
        can_remove = True
    if can_remove and any(item.key == effect for item in src.project_damage_effects):
        op = row.operator('project_damage.effect_action', text='', icon='X')
        op.effect, op.action = effect, 'REMOVE'


def _draw_independent_generators(layout):
    box = layout.box()
    material_controls.centered_heading(box, 'Independent Generators', icon='OUTLINER_OB_MESH')
    _draw_asset_grid(box, None, dev_tools.tiles(generator_assets.ASSETS+generator_assets.DUAL_ASSETS), full=True)
    box.separator()
    note = box.column(align=True)
    for text in ('These generators work without', 'Add Project Damage.'):
        row = note.row()
        row.alignment = 'CENTER'
        row.label(text=text)


def _draw_asset_grid(layout, src, entries, *, full, category='ALL', query=''):
    if not entries:
        layout.label(text='No matching effects', icon='INFO')
        return
    grid = layout.grid_flow(row_major=True, columns=3 if full else 2,
                          even_columns=True, even_rows=True, align=True)
    for key, label, _, image in entries:
        if key == 'boolean':
            from . import boolean_break
            boolean_break.draw_tiles(grid, src, full, enabled, _preview_icon, dev_tools.effect_name,
                variant_filter=lambda variant: effect_categories.boolean_matches(variant, category, query))
            continue
        tile = grid.column(align=True)
        if full:
            card = tile.box()
            preview = card.row(align=True)
            preview.alignment = 'CENTER'
            preview.template_icon(icon_value=_preview_icon(key,image), scale=5.0)
            button_row = tile.row(align=True)
            button_row.scale_y = 1.3
            button_row.operator_context = 'INVOKE_DEFAULT'
            button = button_row.operator('project_damage.add_generator_asset' if key in generator_assets.KEYS or (src is None and key in generator_assets.DAMAGE_KINDS) else 'project_damage.effect_action',
                text=dev_tools.effect_name(key,label), depress=enabled(src,key))
        else:
            tile.scale_y = 1.15
            button_row = tile.row(align=True)
            button = button_row.operator('project_damage.add_generator_asset' if key in generator_assets.KEYS or (src is None and key in generator_assets.DAMAGE_KINDS) else 'project_damage.effect_action',
                text=dev_tools.effect_name(key,TILE_LABELS.get(key,label)),
                icon=image, depress=enabled(src,key))
        if key in generator_assets.KEYS or (src is None and key in generator_assets.DAMAGE_KINDS):button.asset=key
        else:
            button.effect=key
            button.action='ADD'
        if src is not None and any(item.key==key for item in src.project_damage_effects):
            remove=button_row.operator('project_damage.effect_action',text='',icon='X')
            remove.effect=key
            remove.action='REMOVE'

def source(context):
    if not all(hasattr(bpy.types.Object,name) for name in ('project_damage','project_damage_breaker','project_damage_og_object','project_damage_effects')):
        return None
    ob=ownership.context_source(context)
    return ob if ob is not None and ob.type=='MESH' and not ob.project_damage_breaker.is_breaker else None

def enabled(src,key):
    if src is not None and key=='fabric':return bool(src.project_damage.fabric_enable or src.project_damage.fabric_solid_enable)
    if key=='fabric_solid':return False
    if key in generator_assets.KEYS or src is None:return False
    if key in FRACTURE_KEYS:
        from . import fracture_stack
        return fracture_stack.enabled(src,key)
    return bool(getattr(src.project_damage,CATALOG[key][2]))

def initialized(context):
    from . import build
    src=source(context)
    return src is not None and build.result_of(src) is not None

def sync(src):
    """Migrate enabled effects and preserve hidden entries without touching settings."""
    from . import live
    if live._HISTORY_RESTORING:
        return
    if src is None or src.type!='MESH' or any(src.get(k) for k in ownership.STAMPS):return
    if baked_materials.is_applied(src):return
    dev_tools.apply_new_defaults(src)
                                                                             
    p=src.project_damage
    desired_paint=p.wallpaper_drawing is not None
    if p.wallpaper_paint_enable!=desired_paint:p['wallpaper_paint_enable']=desired_paint
                                                                             
                                                                         
    from . import live
    applying=live._APPLYING;live._APPLYING=True
    try:
        for key in REMOVED_EFFECTS:
            if key=='fracture_volume':
                if p.fract_asset=='fracture_volume':
                    p.fract_through_enable=False;p.fract_asset='fracture'
                continue
            if getattr(p,key+'_enable'):setattr(p,key+'_enable',False)
        if p.fract_shard_enable:p.fract_shard_enable=False
        if p.fabric_solid_enable:
            if p.fabric_enable:p.fabric_enable=False
            if not p.fabric_solid:p.fabric_solid=True
            if p.fabric_flat:p.fabric_flat=False
        elif p.fabric_enable and not p.fabric_flat:p.fabric_flat=True
        fabric_rows=[i for i,row in enumerate(src.project_damage_effects) if row.key in {'fabric','fabric_solid'}]
        if fabric_rows:
            selected_fabric=src.project_damage_effect_index in fabric_rows
            first=fabric_rows[0]
            if src.project_damage_effects[first].key!='fabric':src.project_damage_effects[first].key='fabric'
            for i in reversed(fabric_rows[1:]):src.project_damage_effects.remove(i)
            if selected_fabric and src.project_damage_effect_index!=first:src.project_damage_effect_index=first
        selected=(src.project_damage_effects[src.project_damage_effect_index].key
                  if 0<=src.project_damage_effect_index<len(src.project_damage_effects) else None)
        for i in reversed(range(len(src.project_damage_effects))):
            if src.project_damage_effects[i].key in REMOVED_EFFECTS:src.project_damage_effects.remove(i)
        if selected not in REMOVED_EFFECTS and selected is not None:
            selected_index=next((i for i,row in enumerate(src.project_damage_effects) if row.key==selected),0)
            if src.project_damage_effect_index!=selected_index:src.project_damage_effect_index=selected_index
    finally:live._APPLYING=applying
    if not src.get('project_damage_defaults_v2'):
        if src.get('project_damage_uid'):
            for name in ('chips_enable','surf_enable','build_enable','fract_enable','corner_enable','ui_boolean_enable'):
                if not p.is_property_set(name):setattr(p,name,True)
        elif not any(p.is_property_set(name) for name in ENABLE_PROPS):
            src.project_damage_effects.clear()
            src.project_damage_effect_index=0
        src['project_damage_defaults_v2']=True
    if not src.get('project_damage_worn_v2'):
                                                                        
                                                                          
                                                              
        applying=live._APPLYING;live._APPLYING=True
        try:
            if p.build_enable and p.build_corner_prob>0.0:p.pile_enable=True
                                                                        
                                                    
            keys={row.key for row in src.project_damage_effects}
            if p.deposit_enable and not p.build_enable and 'buildup' in keys and not keys&{'piles','deposits'}:
                p.deposit_enable=False
            for name in ('pile_scale','deposit_scale'):
                if not p.is_property_set(name):setattr(p,name,p.build_scale)
            for name in ('pile_detail','deposit_detail'):
                if not p.is_property_set(name):setattr(p,name,p.build_detail)
        finally:live._APPLYING=applying
        src['project_damage_worn_v2']=True
    legacy_rebar=any(row.key=='rebar' for row in src.project_damage_effects)
    if legacy_rebar:
        selected_rebar=(len(src.project_damage_effects)>src.project_damage_effect_index
                        and src.project_damage_effects[src.project_damage_effect_index].key=='rebar')
        for i in reversed(range(len(src.project_damage_effects))):
            if src.project_damage_effects[i].key=='rebar':src.project_damage_effects.remove(i)
        if not any(row.key=='boolean' for row in src.project_damage_effects):src.project_damage_effects.add().key='boolean'
        if selected_rebar:src.project_damage_effect_index=next(i for i,row in enumerate(src.project_damage_effects) if row.key=='boolean')
    from . import fracture_stack
    fracture_stack.ensure(src)
    seen={r.key for r in src.project_damage_effects}
    for key,_,_,_ in EFFECTS:
        if key not in seen and enabled(src,key) and (key!='boolean' or len(p.breakers) or p.rebar_enable or legacy_rebar):
            src.project_damage_effects.add().key=key
    index=min(src.project_damage_effect_index,max(0,len(src.project_damage_effects)-1))
    if src.project_damage_effect_index!=index:src.project_damage_effect_index=index

@persistent
def sync_scene(*unused):
    global _SYNCING
    from . import live
    if live._HISTORY_RESTORING:
        return
    if _SYNCING or not hasattr(bpy.data,'objects') or not hasattr(bpy.types.Object,'project_damage_effects'):return
    _SYNCING=True
    try:
        active=getattr(bpy.context,'active_object',None)
        current=ownership.source_of(active) if hasattr(bpy.types.Object,'project_damage_og_object') else active
        depsgraph=unused[1] if len(unused)==2 and isinstance(unused[1],bpy.types.Depsgraph) else None
        baked_materials.invalidate(depsgraph)
        if depsgraph is not None:
            # Every depsgraph update: the active source and what changed. A full
            # pass runs on file load; an object is synced again when it becomes active.
            candidates={u.id.original for u in depsgraph.updates if isinstance(u.id,bpy.types.Object)}
            if current is not None:candidates.add(current)
        else:
            candidates=bpy.data.objects
        for ob in candidates:
            if (ob.type=='MESH' and not ob.project_damage_breaker.is_breaker
                    and (ob==current or ob.get('project_damage_uid') or len(ob.project_damage_effects))):sync(ob)
    finally:_SYNCING=False

def set_enabled(src,key,on):
    from . import background_compute
    background_compute.cancel(src)
    from . import live
    previous=live._APPLYING;live._APPLYING=True
    try:
        if key in FRACTURE_KEYS:
            from . import fracture_stack
            fracture_stack.set_enabled(src,key,on)
            live._mark_dirty(src)
            return
        if on and key=='bullet':
            from . import damage_physics
            physics=damage_physics.settings(src)
            if not physics.is_property_set('material'):physics.material='CONCRETE'
        if on and key in {'brick','brick_v2'}:
            from . import variant_defaults
            variant_defaults.switch_brick(src,key)
        if on and key in {'glass','glass_through','glass_hit','glass_drawn'}:
            from . import variant_defaults
            variant_defaults.switch_glass(src,key)
        if on and key in {'hit','hit_through'}:
            from . import variant_defaults
            variant_defaults.switch_hit(src,key)
        if on and key in {'lath','lath_v2','lath_riven','lath_framing'}:
            from . import variant_defaults
            variant_defaults.switch(src,key)
        if key not in {'fabric','fabric_solid'}:setattr(src.project_damage,CATALOG[key][2],on)
        if on and key in {'concrete','concrete_v2','pipes','cables'}:
                                                                                
            src.project_damage.concrete_enable = key == 'concrete'
            src.project_damage.concrete_v2_enable = key == 'concrete_v2'
            src.project_damage.pipe_damage_enable = key == 'pipes'
            src.project_damage.cable_damage_enable = key == 'cables'
            if key in {'pipes','cables'}:src.project_damage.concrete_mode='1'
        for base in ('lath', 'brick'):
            if on and key in {base, base + '_v2'}:
                setattr(src.project_damage, base + '_enable', key == base)
                setattr(src.project_damage, base + '_v2_enable', key == base + '_v2')
        if on and key in {'lath', 'lath_v2', 'lath_riven', 'lath_framing'}:
                                                                           
            if key == 'lath_riven':
                from . import riven_lath
                riven_lath.apply_look(src)
            if key == 'lath_framing':
                from . import stud_framing
                stud_framing.apply_look(src)
            for variant in ('lath', 'lath_v2', 'lath_riven', 'lath_framing'):
                setattr(src.project_damage, variant + '_enable', variant == key)
        if on and key in {'glass','glass_through','glass_hit','glass_drawn'}:
            for variant in ('glass','glass_through','glass_hit','glass_drawn'):
                setattr(src.project_damage, variant+'_enable', variant == key)
        if on and key in {'hit','hit_through','glass','glass_through'}:
            base=key.removesuffix('_through')
            setattr(src.project_damage,base+('_enable' if key.endswith('_through') else '_through_enable'),False)
        if key in {'fabric','fabric_solid'}:
            p=src.project_damage
            solid=key=='fabric_solid' or p.fabric_geometry=='SOLIDIFY'
            p.fabric_solid=solid;p.fabric_flat=not solid
            p.fabric_enable=bool(on and not solid);p.fabric_solid_enable=bool(on and solid)
            if on and solid:src.project_damage_input.use_modifiers=True
        if on:src.project_damage.all_off=False
    finally:live._APPLYING=previous
    if key == 'ivy':
        from . import ivy
        ivy.set_visibility(src, on)
    elif key == 'roots':
        from . import roots
        roots.set_visibility(src, on)
    else:
        live._mark_dirty(src)

class ProjectDamageEffectEntry(bpy.types.PropertyGroup):
    key: bpy.props.StringProperty()

# действие «Manage Effect»
class PROJECT_DAMAGE_OT_effect_action(bpy.types.Operator):
    bl_idname='project_damage.effect_action'
    bl_label='Manage Effect'
    bl_options={'REGISTER','UNDO'}
    bl_description='Add/select, hide, remove or evaluate an effect; original mesh stays editable'
    effect: bpy.props.StringProperty()
    action: bpy.props.EnumProperty(items=tuple((k,k.title(),'') for k in ('ADD','TOGGLE','REMOVE','APPLY')))
    @classmethod
    def poll(cls,context):return source(context) is not None and context.mode=='OBJECT'
    @classmethod
    def description(cls,context,properties):
        label=CATALOG.get(properties.effect,('', 'Selected effect'))[1]
        return {'ADD':'Enable and select '+label,'TOGGLE':'Hide/show '+label+' without losing settings','REMOVE':'Remove '+label+' and its unshared Grease Pencil drawing','APPLY':'Refresh '+label+' in the live setup; use Apply All to bake and remove the full damage stack'}.get(properties.action,label)
    def invoke(self, context, event):
        from . import background_compute
        src = source(context)
        rows = src.project_damage_effects
        index = src.project_damage_effect_index
        key = self.effect or (rows[index].key if 0 <= index < len(rows) else '')
        if self.action in {'ADD', 'APPLY'} and background_compute.confirm_effect(context, src, key, self.action):
            return {'CANCELLED'}
        return self.execute(context)

    def execute(self,context):
        src=source(context);sync(src);rows=src.project_damage_effects
        key=self.effect or (rows[src.project_damage_effect_index].key if rows else '')
        if key not in CATALOG:return {'CANCELLED'}
        if self.action=='ADD' and key in damage_generator_assets.RETIRED_KEYS:return {'CANCELLED'}
        if key in {'paper_generator', *damage_generator_assets.KINDS} and not initialized(context):
            self.report({'WARNING'}, 'Use Add Project Damage on a mesh before adding this generator')
            return {'CANCELLED'}
        index=next((i for i,r in enumerate(rows) if r.key==key),None)
        fresh=index is None
        if self.action in {'ADD','APPLY'}:
            if key=='foam' and self.action=='ADD':
                from . import foam
                foam.capture_selected(src,context)
            if index is None:
                if key in FRACTURE_KEYS:
                    from . import fracture_stack
                    fracture_stack.select(src,key)
                    index=next(i for i,r in enumerate(rows) if r.key==key)
                    dev_tools.apply_effect_defaults(src,key)
                else:
                    dev_tools.apply_effect_defaults(src,key)
                if index is None:
                    rows.add().key=key;index=len(rows)-1
            src.project_damage_effect_index=index
            set_enabled(src,key,True)
            if fresh:
                from . import generator_defaults
                generator_defaults.reset(src,key)
                if self.action=='ADD':
                    from . import paint
                    paint.begin_new_effect(context,src,key)
        elif self.action=='TOGGLE':set_enabled(src,key,not enabled(src,key))
        elif self.action=='REMOVE':
            from . import paint
            drawings=paint.effect_drawings(src,key)
            set_enabled(src,key,False)
            if index is not None:rows.remove(index)
            from . import paint
            paint.remove_effect_drawing(src,key,drawings)
            src.project_damage_effect_index=min(src.project_damage_effect_index,max(0,len(rows)-1))
        if key in damage_generator_assets.KINDS:
            try:return damage_generator_assets.refresh(context,src,key,self.action)
            except Exception as exc:
                self.report({'ERROR'},str(exc));return {'CANCELLED'}
        if key == 'ivy':
            if self.action in {'ADD','APPLY'}:
                return bpy.ops.project_damage.grow_ivy()
            return {'FINISHED'}
        if key == 'roots':
            if self.action in {'ADD','APPLY'}:
                return bpy.ops.project_damage.grow_roots()
            return {'FINISHED'}
        if self.action=='APPLY':
            from .build import PROJECT_DAMAGE_OT_update
            if not PROJECT_DAMAGE_OT_update.poll(context):return {'CANCELLED'}
            return bpy.ops.project_damage.update()
                                                                                            
        from . import build
        if self.action in {'TOGGLE','REMOVE'} and build.result_of(src) is not None:
            from . import live
            previous=live._APPLYING;live._APPLYING=True
            try:result,reports=build.rebuild_object(context,src)
            finally:live._APPLYING=previous
            for kind,message in reports:self.report(kind,message)
            if result is None:return {'CANCELLED'}
        return {'FINISHED'}

class PROJECT_DAMAGE_UL_effects(bpy.types.UIList):
    def draw_item(self,context,layout,data,item,icon,active_data,active_prop,index=0,flt_flag=0):
        src=data;key=item.key
        if key not in CATALOG:return
        _,label,_,image=CATALOG[key]
        if key=='boolean':
            from . import boolean_break
            label=boolean_break.variant_label(src)
        row=layout.row(align=True)
        row.label(text=dev_tools.effect_name(key,label),icon=image)
        button=row.operator('project_damage.effect_action',text='',icon='HIDE_OFF' if enabled(src,key) else 'HIDE_ON',emboss=False);button.effect=key;button.action='TOGGLE'
                                                                   
        row.operator_context = 'INVOKE_DEFAULT'
        button=row.operator('project_damage.effect_action',text='',icon='FILE_REFRESH',emboss=False);button.effect=key;button.action='APPLY'
        row.operator('project_damage.apply_selected',text='',icon='CHECKMARK',emboss=False).effect=key
        button=row.operator('project_damage.effect_action',text='',icon='X',emboss=False);button.effect=key;button.action='REMOVE'

class _Panel(bpy.types.Panel):
    bl_space_type='VIEW_3D';bl_region_type='UI';bl_category="⚒ Project Damage";bl_options=set()
    @classmethod
    def poll(cls,context):
        return True                                                               

def draw_target_header(layout,context,source_override=...,show_pin=True):
    src=source(context) if source_override is ... else source_override
    row=layout.row(align=True)
    row.label(text=src.name if src is not None else 'Select a mesh to begin',icon='OBJECT_DATA' if src is not None else 'INFO')
    if show_pin:row.operator('project_damage.pin_settings',text='',icon='PINNED' if ownership.pinned_source(context) else 'UNPINNED',depress=ownership.pinned_source(context) is not None)


def fold(layout,key,label,closed=True):
    return effect_layout.section(layout,label)

class PROJECT_DAMAGE_OT_import_section(bpy.types.Operator):
    bl_idname='project_damage.import_section'
    bl_label='Show Import Section'
    bl_description='Open this library; click again to close it'
    bl_options={'INTERNAL'}
    section:bpy.props.EnumProperty(items=(('EFFECTS','Effects Lib','Effect browser'),('PRESETS','Presets','Built-in and user presets')))
    def execute(self,context):
        wm=context.window_manager
        wm.project_damage_import_section='NONE' if wm.project_damage_import_section==self.section else self.section
        for screen in bpy.data.screens:
            for area in screen.areas:area.tag_redraw()
        return {'FINISHED'}


def import_source(context):
    active=getattr(context,'active_object',None)
    if (active is not None and active.type=='MESH' and any(active.get(k) for k in ownership.LOOSE_STAMPS)
            and not any(active.get(k) for k in ownership.STAMPS)):return active
    src=ownership.source_of(active)
    return src if src is not None and src.type=='MESH' else None


# раздел «Import Project Damage»
class OBJECT_PT_project_damage(_Panel):
    bl_idname='OBJECT_PT_project_damage';bl_label='Import Project Damage';bl_order=0

    @classmethod
    def poll(cls,context):
        return _Panel.poll(context) and baked_materials.target(context) is None

    def draw_header(self, context):
        self.layout.label(text='', icon='IMPORT')

    def draw(self,context):
        from . import core_loader,build
        layout=self.layout;src=import_source(context)
        if src is None:
            notice = layout.box()
            row = notice.row()
            row.alignment = 'CENTER'
            row.alert = True
            row.scale_y = 1.4
            row.label(text='Select a mesh to begin', icon='INFO')
            _draw_independent_generators(layout)
            from . import preparation_tools
            preparation_tools.draw(layout, context)
            return
        draw_target_header(layout,context,source_override=src,show_pin=False)
        layout.context_pointer_set('project_damage_import_source',src)
        if core_loader.version_mismatch():layout.label(text='Restart Blender to finish update',icon='ERROR')
        result=build.result_of(src)
        if result is None:
            add_row = layout.row()
            add_row.scale_y = 2.0
            add_row.operator('project_damage.update', text='Add Project Damage', icon='FILE_REFRESH')
            _draw_independent_generators(layout)
            from . import preparation_tools
            preparation_tools.draw(layout, context)
            return
        row = layout.row(align=True)
        if context.active_object==src:
            row.operator('project_damage.select_result',text='Switch to Damage',icon='MODIFIER')
        else:
            row.operator('project_damage.select_original',text='Switch to Original',icon='MESH_DATA')
        row.operator('project_damage.delete_result',text='',icon='TRASH')
        row=layout.row(align=True);row.scale_y=1.5
        section=context.window_manager.project_damage_import_section
        for key,label,icon in (('EFFECTS','Effects Lib','ASSET_MANAGER'),('PRESETS','Presets','PRESET')):
            button=row.operator('project_damage.import_section',text=label,icon='X' if section==key else icon,depress=section==key)
            button.section=key
        if section=='EFFECTS':_draw_effect_tiles(layout,context,src)
        elif section=='PRESETS':OBJECT_PT_project_damage_presets.draw(SimpleNamespace(layout=layout),context)
        layout.separator()
        draw_update_controls(layout, src.project_damage)

# раздел «Presets»
class OBJECT_PT_project_damage_presets(_Panel):
    bl_label='Presets';bl_order=1
    bl_options={'DEFAULT_CLOSED'}
    @classmethod
    def poll(cls,context):return _Panel.poll(context) and initialized(context)
    def draw(self,context):
        layout=self.layout
        if source(context) is None:return
        row=layout.row(align=True);row.scale_y=1.5
        row.prop(context.window_manager,'project_damage_preset_library',expand=True)
        if context.window_manager.project_damage_preset_library=='BUILTIN':
            from . import presets,preset_pack
            if not preset_pack.installed():
                layout.operator('project_damage.install_builtin_presets',icon='IMPORT')
                return
            wm=context.window_manager;box=layout.box()
            search = box.row(align=True)
            search.prop(wm, 'project_damage_builtin_search', text='', icon='VIEWZOOM', placeholder='Search presets')
            if wm.project_damage_builtin_search:
                clear = search.operator('wm.context_set_string', text='', icon='X')
                clear.data_path = 'window_manager.project_damage_builtin_search'
                clear.value = ''
            box.prop(wm,'project_damage_builtin_category',text='Category')
            from . import preset_browser
            matches = presets.builtin_matches(wm.project_damage_builtin_category, wm.project_damage_builtin_search)
            if preset_browser.advanced(context):
                preset_browser.draw_selector(box,context)
            elif matches:
                box.template_icon_view(wm,'project_damage_builtin_preset',show_labels=True,scale=6.,scale_popup=5.)
            item = presets.builtin_selected_item(wm)
            row=box.row();row.alignment='CENTER'
            row.label(text=item[1] if item else ('No preset selected' if matches else 'No matching presets'),icon='PRESET')
            mode = wm.project_damage_preset_apply_mode
            if item:
                favorite=item[0] in preset_pack.favorites()
                button=box.operator('project_damage.favorite_preset',text='Remove from Favorites' if favorite else 'Add to Favorites',icon='SOLO_ON' if favorite else 'SOLO_OFF',depress=favorite)
                button.preset=item[0]
                op = box.operator('project_damage.apply_preset', text='Apply Preset', icon='CHECKMARK')
                op.preset, op.mode = item[0], mode
            modes = box.row(align=True)
            for key, label in (('ADD', 'Add'), ('REPLACE', 'Replace')):
                button = modes.operator('wm.context_set_enum', text=label, depress=mode == key)
                button.data_path = 'window_manager.project_damage_preset_apply_mode'
                button.value = 'NONE' if mode == key else key
            return
        box=layout.box();material_controls.centered_heading(box,'User_lib',icon='PRESET')
        row=box.row(align=True);row.menu('PROJECT_DAMAGE_MT_presets',text=bpy.types.PROJECT_DAMAGE_MT_presets.bl_label)
        row.operator('project_damage.add_preset',text='',icon='ADD')
        row.operator('project_damage.add_preset',text='',icon='REMOVE').remove_active=True
        row.operator('project_damage.open_userlib',text='',icon='FILE_FOLDER')
        box.label(text='Save and reuse your effect settings')

class _Layout:
    """Reuse proven property drawers without legacy apply buttons/enable checkboxes."""
    def __init__(self,layout,key=None,raw=False):
        object.__setattr__(self,'layout',layout)
        object.__setattr__(self,'key',key)
        object.__setattr__(self,'raw',raw)
    def __getattr__(self,name):
        value=getattr(self.layout,name)
        if name in {'row','column','box','split','grid_flow','column_flow'}:
            def child(*args,**kwargs):
                title=kwargs.pop('heading',None)
                target=effect_layout.section(self.layout,title) if title else value(*args,**kwargs)
                return type(self)(target,self.key,self.raw)
            return child
        return value
    def __setattr__(self,name,value):
        if name=='use_property_split':value=False
        if name=='scale_y' and value>=1.4:value=effect_layout.SELECTOR_HEIGHT
        setattr(self.layout,name,value)
    def label(self,**kwargs):
        text=kwargs.get('text','')
        if text.strip() and len(text)<65 and kwargs.get('icon','NONE')=='NONE':
            effect_layout.heading(self.layout,text)
        else:self.layout.label(**kwargs)
    def prop_enum(self,owner,name,value,**kwargs):
        self.layout.use_property_split=False
        self.layout.scale_y=effect_layout.SELECTOR_HEIGHT
        return self.layout.prop_enum(owner,name,value,**kwargs)
    def prop(self,owner,name,**kwargs):
        if 'avoid' in name.lower():return
        if name in ENABLE_PROPS:return
        if not self.raw and not effect_layout.visible(owner,name):return
        if material_controls.is_material_property(owner,name) and material_controls.ui_section()!='MATERIAL' and not (material_controls.experimental() and material_controls.ui_section()=='GEOMETRY'):return
        if material_controls.is_material_property(owner,name):
            from .material_library import draw_property
            return draw_property(self.layout,owner,name,**kwargs)
        self.layout.use_property_split=False
        self.layout.use_property_decorate=False
        prop=owner.bl_rna.properties.get(name)
        hint=ui_image_hints.topic(owner,name)
        if hint and self.key is not None and not self.raw:
            config=dev_tools.field_config(self.key)
            if name in config.get('hidden',[]) or name in config.get('pinned',[]):return
        target=self.layout
        help_row=target.row(align=True) if hint and ui_image_hints.enabled() else None
        if help_row is not None:target=help_row
        if prop is not None and prop.type=='ENUM':
            kwargs['expand']=True
            kwargs.pop('icon_only',None)
                                                                                 
            if not kwargs.get('text','').strip():kwargs.pop('text',None)
            target=target.row(align=True)
            target.scale_y=effect_layout.SELECTOR_HEIGHT
        if self.key is not None and not self.raw:
            result=dev_tools.draw_property(target,owner,name,self.key,**kwargs)
        else:
            from . import quality_ui
            result=quality_ui.draw(target,owner,name,**kwargs)
        if help_row is not None:ui_image_hints.button(help_row,hint)
        return result
    def prop_search(self,owner,name,*args,**kwargs):
        if 'avoid' in name.lower():return
        if self.key is not None and not self.raw:
            config=dev_tools.field_config(self.key)
            if name in config.get('hidden',[]) or name in config.get('pinned',[]):return
            label=config.get('labels',{}).get(name)
            if label:kwargs['text']=label
        return self.layout.prop_search(owner,name,*args,**kwargs)
    def operator(self,name,**kwargs):
        if name.startswith('project_damage.apply_'):return SimpleNamespace()
        return self.layout.operator(name,**kwargs)

def draw_effect(layout,context,key):
    if key in REMOVED_EFFECTS:return
    dev_tools.draw_pinned(_Layout(layout,key,True),context,key)
    _draw_effect_original(layout,context,key)
    from . import displacement_shader
    displacement_shader.draw_effect(layout, source(context).project_damage, key)


PHYSICS_MODULES = {'dbevel':'dbevel','erosion':'erosion',
    'concrete':'concrete', 'concrete_v2':'concrete', 'pipes':'concrete', 'cables':'concrete',
    'lath':'lath', 'lath_v2':'lath', 'lath_riven':'lath', 'lath_framing':'lath',
    'brick':'brick', 'brick_v2':'brick',
    'edge':'edge', 'corner':'corner', 'surface':'surface',
    'buildup':'bead', 'piles':'piles', 'deposits':'deposits',
    'bullet':'bullet', 'hang':'hang', 'boolean':'boolean',
}
                                                                           
                                                                        
PHYSICS_KEYS = set(PHYSICS_MODULES) | {'glass_hit','glass_drawn','cellf'}
HIDDEN_PHYSICS_TABS = {'glass_hit', 'glass_drawn', 'erosion', 'buildup', 'piles', 'deposits'}
PHYSICS_TAB_KEYS = PHYSICS_KEYS - HIDDEN_PHYSICS_TABS


def draw_physics(layout, context, src, key):
    if key not in PHYSICS_KEYS:
        return
    from . import damage_physics, panels
    if key in {'piles','deposits'}:
        s=damage_physics.settings(src)
        box=layout.box();material_controls.centered_heading(box,'Separate',icon='MOD_EXPLODE')
        box.prop(s,'detach_remove',text='Remove from Wall')
        box.operator('project_damage.physics_fill',text='Detach Corner Piles' if key=='piles' else 'Detach Deposits',icon='MOD_PARTICLES').module=key
        return
    if key in PHYSICS_MODULES:
        if key=='hit_through':
            layout.label(text='Concrete Hit Through physics: further tuning needed',icon='INFO')
            panels.draw_fragment_controls(layout,src.project_damage,context,key)
        damage_physics.draw(layout, src.project_damage, context, PHYSICS_MODULES[key])
    elif key in {'glass_hit','glass_drawn'}:
        panels.draw_glass_pieces(layout, src.project_damage, context)
    elif key == 'cellf':
        box=layout.box();material_controls.centered_heading(box,'Fracture Type',icon='MOD_EXPLODE')
        box.prop(src.project_damage,'cellf_material',text='Fracture Type',expand=True)
        damage_physics.draw_quick_physics(layout, context)

def _draw_effect_original(layout,context,key,raw=False):
    if key in REMOVED_EFFECTS:return
    from . import panels
    from .fracture_assets import PROXIMITY_ASSETS
    if key in PROXIMITY_ASSETS:
        panels.draw_fracture_proximity_asset(_Layout(layout,key,raw),context,key)
        return
    if key in damage_generator_assets.KINDS:
        damage_generator_assets.draw(layout,context,key)
        return
    if key in {'weld','grass','gravel','foam'}:
        from . import generated_assets
        generated_assets.draw_settings(layout,context,key)
        return
    if key == 'paper_generator':
        from . import paper_generator
        paper_generator.draw(layout, source(context))
        return
    if key == 'metaltear':
        from . import metal_tear
        metal_tear.draw_settings(_Layout(layout,key,raw),context)
        return
    if key == 'peeling':
        from . import peeling
        peeling.draw_settings(layout,context)
        return
    if key == 'clay':
        from . import clay
        clay.draw_settings(layout,context)
        return
    if key == 'erosion':
        from . import voxel_erosion
        voxel_erosion.draw_settings(layout,context)
        return
    if key == 'pits':
        from . import concrete_pits
        concrete_pits.draw_settings(layout, context)
        return
    if key == 'ivy':
        from . import ivy
        ivy.draw_settings(layout, context)
        return
    if key == 'roots':
        from . import roots
        roots.draw_settings(layout, context)
        return
    if key == 'deform':
        panels.draw_impact_deform(layout, context)
        return
    if key == 'wallhole':
        panels.draw_wallhole_settings(layout,context)
        return
    if key == 'pipes':
        from . import pipes
        pipes.draw_settings(layout, context)
        return
    if key == 'cables':
        from . import cables
        cables.draw_settings(layout, context)
        return
    mapping={
        'edge':('chips','chips_noise'),'surface':('surface','surface_noise'),
        'corner':('corners',),'fracture':('fracture','fracture_realism','fracture_proximity','fracture_noise','fracture_options'),
        'fracture_through':('fracture','fracture_realism','fracture_proximity','fracture_noise','fracture_options'),
        'fracture_volume':('fracture','fracture_realism','fracture_proximity','fracture_noise','fracture_options'),
        'buildup':('buildup',),'piles':('piles',),'deposits':('deposits',),'rust':('rust',),'heat':('heat',),'coating':('coating',),
        'boolean':('breakers',),'rebar':('rebar',),
    }
    if key in mapping:
        classes=[getattr(panels,'OBJECT_PT_project_damage_'+name) for name in mapping[key] if hasattr(panels,'OBJECT_PT_project_damage_'+name)]
    elif key in TYPE_BY_ASSET:
        classes=[getattr(panels,'OBJECT_PT_project_damage_'+name) for name in mapping['fracture'] if hasattr(panels,'OBJECT_PT_project_damage_'+name)]
    else:classes=[cls for cls in panels._EFFECT_PANELS if cls.effect_module=={'hit_through':'hit','glass_through':'glass','fabric_solid':'fabric','concrete_v2':'concrete','lath_v2':'lath','lath_riven':'lath','lath_framing':'lath','brick_v2':'brick','glass_hit':'glass','glass_drawn':'glass'}.get(key,key)]
    if key=='boolean':
        from . import boolean_break
        panels.draw_boolean_controls(boolean_break._raw(layout),context)
        return
    for i,cls in enumerate(classes):
        if 'fracture_realism' in cls.__name__:continue
        if hasattr(cls,'effect_fields'):
            fields=effect_layout.group_fields(source(context).project_damage,cls.effect_module,cls.effect_fields)
            if not fields:continue
                                                                          
        inline_noise='noise' in cls.bl_label.casefold() or 'noise' in cls.__name__.casefold()
        target=layout if i==0 or inline_noise else fold(layout,'effect_'+key+'_'+str(i),cls.bl_label)
        if i==0 and key=='boolean':
            target=effect_layout.section(layout,'Cutters / Rim Damage')
        if target is None:continue
        if inline_noise:
            if 'fracture_noise' in cls.__name__:
                target=panels._ui_disclosure(layout,'fracture_noise_open','Noisify')
                if target is None:continue
            if cls.bl_label.strip():target=effect_layout.section(layout,cls.bl_label)
        proxy=SimpleNamespace(layout=_Layout(target,key,raw))
        for field in ('effect_module','effect_fields'):
            if hasattr(cls,field):setattr(proxy,field,getattr(cls,field))
        cls.draw(proxy,context)
    if key=='boolean':
        from . import boolean_break
        src=source(context)
        boolean_break.draw_fill_section(layout,context,src,lambda target:_Layout(target,key,raw))
        boolean_break.draw_fragments(layout,src)

# действие «Open Project Damage Link»
class PROJECT_DAMAGE_OT_open_info_link(bpy.types.Operator):
    bl_idname = 'project_damage.open_info_link'
    bl_label = 'Open Project Damage Link'
    bl_options = {'INTERNAL'}
    link: bpy.props.StringProperty()

    @classmethod
    def description(cls, context, properties):
        if properties.link == 'docs':
            return 'Open the bundled Project Damage documentation'
        if properties.link == 'tutorial':
            return 'Find Project Damage tutorials on the Lazy3D YouTube channel'
        return 'Open ' + SOCIAL_LINKS.get(properties.link, ('link', ''))[0]

    def execute(self, context):
        if self.link == 'docs':
            from . import editorial
            index = editorial.ROOT / 'index.html'
            if not index.is_file():
                self.report({'ERROR'}, 'The bundled documentation is missing')
                return {'CANCELLED'}
            url = index.resolve().as_uri()
            if not bpy.app.background:
                editorial.webbrowser.open(url, new=2)
            return {'FINISHED'}
        elif self.link == 'tutorial':
            url = TUTORIAL_URL
        elif self.link in SOCIAL_LINKS:
            url = SOCIAL_LINKS[self.link][1]
        else:
            return {'CANCELLED'}
        return bpy.ops.wm.url_open(url=url)


class PROJECT_DAMAGE_OT_send_feedback(bpy.types.Operator):
    bl_idname = 'project_damage.send_feedback'
    bl_label = 'Send Feedback'
    bl_description = 'Copy laz3dblender@gmail.com to the clipboard to send feedback to Lazy3D'
    bl_options = {'INTERNAL'}

    def execute(self, context):
        # копируем только адрес, письмо пользователь пишет сам
        address = 'laz3dblender@gmail.com'
        context.window_manager.clipboard = address

        def draw(popup, _context):
            col = popup.layout.column(align=True)
            col.label(text='Lazy3D feedback email copied to clipboard.', icon='CHECKMARK')
            col.label(text=address)
            col.separator()
            col.label(text='Paste it into your email app to send feedback.')

        if not bpy.app.background:
            context.window_manager.popup_menu(draw, title='Send Feedback', icon='INFO')
        return {'FINISHED'}


# раздел «Info and Documentation»
class OBJECT_PT_project_damage_info(_Panel):
    bl_label = 'Info and Documentation'
    bl_order = 5
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls,context):
        return _Panel.poll(context) and baked_materials.target(context) is None

    def draw_header(self, context):
        self.layout.label(text='', icon='INFO')

    def draw(self, context):
        box = self.layout.box()
        material_controls.centered_heading(box, 'More from Lazy 3D')
        row = box.row(align=True)
        row.alignment = 'CENTER'
        row.scale_y = 1.25
        for key in SOCIAL_LINKS:
            icon = _preview_icon('social_icons/' + key, 'URL')
            row.operator('project_damage.open_info_link', text='', icon_value=icon, emboss=False).link = key
        box.separator(factor=.75)
        buttons = box.column(align=True)
        buttons.scale_y = 1.2
        buttons.operator('project_damage.open_info_link', text='Open Documentation', icon='FILE_TEXT').link = 'docs'
        buttons.operator('project_damage.open_info_link', text='Open Video Tutorial', icon='PLAY').link = 'tutorial'
        buttons.operator('project_damage.send_feedback', text='Send Feedback', icon='URL')


# раздел «Tools»
class OBJECT_PT_project_damage_tools(_Panel):
    bl_idname='OBJECT_PT_project_damage_tools';bl_label='Tools';bl_order=4

    def draw_header(self, context):
        self.layout.label(text='', icon='TOOL_SETTINGS')

    @classmethod
    def poll(cls,context):
        return _Panel.poll(context) and baked_materials.target(context) is None and initialized(context)

    def draw(self,context):
        from . import input_prep,clean_floating,panels
        layout=self.layout;src=source(context)
        if src is None:return
        p=src.project_damage
        category=context.window_manager.project_damage_tools_category
        if category=='MASKS':category='PREP'                                                                
        navigation = layout.column(align=True)
        tabs=navigation.row(align=True);tabs.scale_y=1.5
        for key,title,_ in TOOLS_CATEGORIES:
            if key not in {'PREP', 'CLEANUP', 'OUTPUT'}:continue
            op=tabs.operator('wm.context_set_enum',text=title,depress=category==key)
            op.data_path='window_manager.project_damage_tools_category';op.value=key
        from . import editorial
        if editorial.enabled(context):
            help_row=tabs.row(align=True);help_row.scale_y=1.0
            op=help_row.operator('project_damage.editorial_open',text='',icon='HELP')
            op.asset='tools';op.section={'PREP':'Mesh Preparation','CLEANUP':'Cleanup / Trim','OUTPUT':'Output',
                                      'PERFORMANCE':'Performance'}.get(category,'Tools')
        performance = navigation.row(align=True)
        performance.scale_y = 1.5
        op = performance.operator('wm.context_set_enum', text='Performance', icon='TIME',
                                  depress=category == 'PERFORMANCE')
        op.data_path = 'window_manager.project_damage_tools_category'
        op.value = 'PERFORMANCE'
        body=layout.column(align=True)
        body.use_property_split=True;body.use_property_decorate=False
        if category=='PREP':
                                                                             
                                                                                
            panels.draw_mesh_preparation_tools(SimpleNamespace(layout=body),context)
        elif category=='CLEANUP':
            panels.draw_cleanup_tools(body,context)
        elif category == 'PERFORMANCE':
            from . import background_compute
            background_compute.draw_settings(body, context, src)
        elif category=='MASKS':
            panels.OBJECT_PT_project_damage_global.draw(SimpleNamespace(layout=_GlobalLayout(body)),context)
        elif category=='OUTPUT':
            for prop in ('preserve_data','keep_uvs','keep_selections','topology'):
                body.prop(p,prop)
            body.prop(p,'split_effect_zones')
            body.separator()


def draw_edge_axes(layout, p):
    row = layout.row(align=True)
    row.scale_y = 1.5
    for axis in 'xyz':
        row.prop(p, 'axis_' + axis + '_toggle', text=axis.upper(), toggle=True)
    if not (p.axis_x_toggle or p.axis_y_toggle or p.axis_z_toggle):
        layout.label(text='Assign at least one axis', icon='INFO')


def draw_update_controls(layout, p):
    controls=layout.split(factor=0.5,align=True)
    livebox=controls.box();livebox.use_property_split=False
    row=livebox.row(align=True);row.scale_y=1.5
    row.prop(p,'live_update',text='Live Update',toggle=True,icon='FILE_REFRESH')
    row=livebox.row(align=True);row.enabled=p.live_update
    row.prop(p,'defer_ms',text='Interactive Delay')
    edges=controls.box();edges.use_property_split=False;edges.use_property_decorate=False
    edges.prop(p,'edge_threshold',text='Edge Threshold')
    draw_edge_axes(edges, p)


# раздел «Settings | Project Damage»
class OBJECT_PT_project_damage_settings(_Panel):
    bl_label='Settings | Project Damage';bl_order=3

    def draw_header(self, context):
        self.layout.label(text='', icon='MODIFIER')

    @classmethod
    def poll(cls,context):
        if not _Panel.poll(context):return False
        if baked_materials.target(context) is not None:return True
        if generator_assets.owner(context) is not None:return True
        if source(context) is not None:return initialized(context)
        return False
    def draw(self,context):
        from . import panels
        layout=self.layout;src=source(context)
        applied=baked_materials.target(context)
        if applied is not None:
            baked_materials.draw(layout,context,applied)
            return
        if generator_assets.owner(context) is not None:
            generator_assets.draw(layout,context)
            return
        if src is None:return
        draw_target_header(layout,context)
        p=src.project_damage
        if context.object is not None and context.object.type in {'GREASEPENCIL','GPENCIL','CURVE'} and ownership.source_of(context.object)==src:
            if context.object in (src.project_damage_ivy.guide,src.project_damage_ivy.curve):
                layout.operator('project_damage.ivy_draw',text='Finish Drawing',icon='CHECKMARK').action='FINISH'
            elif context.object == src.project_damage_roots.guide:
                layout.operator('project_damage.roots_draw',text='Finish Drawing',icon='CHECKMARK').action='FINISH'
            elif not p.glass_drawn_enable:
                layout.operator('project_damage.paint_return',text='Finish Drawing',icon='CHECKMARK')
        master=layout.column(align=True);master.use_property_split=True;master.use_property_decorate=False
        master.prop(p,'global_scale',text='Master Scale')
        from . import preview_quality
        preview_quality.draw(master,p)
        master.prop(p,'seed');master.prop(p,'all_off',text='Mute All Effects')
        layout.template_list('PROJECT_DAMAGE_UL_effects','',src,'project_damage_effects',src,'project_damage_effect_index',rows=5,maxrows=8)
        rows=src.project_damage_effects
        selected_key=rows[min(src.project_damage_effect_index,len(rows)-1)].key if rows else ''
        row=layout.row(align=True);row.operator('project_damage.apply_selected',text='Apply Selected',icon='CHECKMARK')
        row.operator('project_damage.apply_all_damage',text='Apply All',icon='CHECKMARK')
        line_type=bpy.types.UILayout.bl_rna.functions['separator'].parameters.get('type')
        for _ in range(2):
            if line_type is not None:layout.separator(type='LINE')
            else:layout.separator(factor=1.5)
        growth_only=selected_key in {'ivy','roots','grass'}
        section=material_controls.ui_section(context)
        if material_controls.has_materials(selected_key) and not growth_only:
            tabs=layout.row(align=True);tabs.scale_y=2.0
            if section == 'PHYSICS' and selected_key in HIDDEN_PHYSICS_TABS:
                geometry_tab=tabs.operator('wm.context_set_enum',text='Geometry',icon='MESH_DATA',depress=True)
                geometry_tab.data_path='window_manager.project_damage_ui_section'
                geometry_tab.value='GEOMETRY'
            else:
                tabs.prop_enum(context.window_manager,'project_damage_ui_section','GEOMETRY',text='Geometry',icon='MESH_DATA')
            tabs.prop_enum(context.window_manager,'project_damage_ui_section','MATERIAL',text='Damage UV' if material_controls.experimental(context) else 'Material',icon='UV' if material_controls.experimental(context) else 'MATERIAL')
            if selected_key in PHYSICS_TAB_KEYS:
                tabs.prop_enum(context.window_manager,'project_damage_ui_section','PHYSICS',text='Separate' if selected_key in {'piles','deposits'} else 'Physics',icon='MOD_EXPLODE' if selected_key in {'piles','deposits'} else 'PHYSICS')
        rows=src.project_damage_effects
        if rows:
            key=rows[min(src.project_damage_effect_index,len(rows)-1)].key
            if key in CATALOG:
                from . import editorial
                geometry=growth_only or not ((section=='PHYSICS' and key in PHYSICS_TAB_KEYS) or (section=='MATERIAL' and material_controls.has_materials(key)))
                with editorial.scope(key,src,active=geometry):
                    material_controls.centered_heading(layout,dev_tools.effect_name(key,CATALOG[key][1]),icon=CATALOG[key][3])
                    if not enabled(src,key):layout.label(text='Hidden — settings are kept',icon='HIDE_ON')
                    if growth_only:draw_effect(layout,context,key)
                    elif section=='PHYSICS' and key in PHYSICS_TAB_KEYS:
                        draw_physics(layout,context,src,key)
                        from . import physics_workflow
                        physics_workflow.draw_replace_original(layout,src,whole_source=key!='cellf')
                    elif section=='MATERIAL' and material_controls.has_materials(key):
                        row=layout.row(align=True)
                        row.operator('project_damage.unwrap_zones',text='Quick UV Map',icon='UV')
                        row.operator('project_damage.edit_zone_uv',text='Edit Scale',icon='FULLSCREEN_ENTER')
                        material_controls.draw(layout,p,key)
                    else:draw_effect(layout,context,key)
        else:layout.label(text='Choose an effect in Add',icon='INFO')
        shading=layout.box();shading.use_property_split=False;shading.use_property_decorate=False
        row=shading.row(align=True);row.scale_y=1.5
        row.prop(p,'damage_shading',text='Set Shade Smooth',toggle=True,icon='SHADING_SOLID')
        sub=shading.column(align=True);sub.active=p.damage_shading
        sub.prop(p,'damage_smooth_angle',text='Damage Smooth Angle')
        sub.prop(p,'base_smooth_angle',text='Base Smooth Angle')
        if p.damage_shading:
            hint=shading.column(align=True)
            hint.label(text='If shading artifacts appear:',icon='INFO')
            hint.label(text='set Base Smooth Angle to 0.')

class _GlobalLayout(_Layout):
    def label(self,**kwargs):
        if kwargs.get('text')=='Mesh':return
        return self.layout.label(**kwargs)
    def prop(self,owner,name,**kwargs):
        if 'avoid' in name.lower():return
        if name in {'global_scale','global_detail','draft_enable','draft_quality','seed','all_off','topology','keep_uvs','keep_selections'}:return
        return super().prop(owner,name,**kwargs)

CLASSES=(baked_materials.PROJECT_DAMAGE_OT_baked_sides_material,baked_materials.PROJECT_DAMAGE_OT_baked_new_material,baked_materials.PROJECT_DAMAGE_UL_baked_groups,generator_assets.PROJECT_DAMAGE_OT_add_generator_asset,*dev_tools.CLASSES,material_controls.PROJECT_DAMAGE_OT_material_all,ProjectDamageEffectEntry,PROJECT_DAMAGE_OT_effect_action,PROJECT_DAMAGE_OT_apply_selected,PROJECT_DAMAGE_UL_effects,
         PROJECT_DAMAGE_OT_import_section,PROJECT_DAMAGE_OT_open_info_link,PROJECT_DAMAGE_OT_send_feedback,OBJECT_PT_project_damage,
         OBJECT_PT_project_damage_settings,OBJECT_PT_project_damage_tools,OBJECT_PT_project_damage_info,
         dev_tools.OBJECT_PT_project_damage_dev_tools)
def _registered_class(cls):
    """Find the actual RNA class, including one from before importlib.reload."""
    for base in (bpy.types.Panel, bpy.types.Operator, bpy.types.UIList,
                 bpy.types.PropertyGroup, bpy.types.Menu):
        if issubclass(cls, base):
            return base.bl_rna_get_subclass_py(cls.__name__, None)
    return None


# подключаем свойства и действия этого модуля к Blender
def register():
                                                                               
    if any(_registered_class(cls) is not None for cls in CLASSES):
        unregister()
    legacy=bpy.types.Panel.bl_rna_get_subclass_py('OBJECT_PT_project_damage_presets',None)
    if legacy is not None:bpy.utils.unregister_class(legacy)
    for cls in CLASSES:bpy.utils.register_class(cls)
    bpy.types.WindowManager.project_damage_import_section=bpy.props.EnumProperty(
        name='Import Section',items=(('NONE','Closed','No library open'),('EFFECTS','Effects Lib','Browse effect assets'),('PRESETS','Presets','Browse presets')),
        default='EFFECTS',options={'SKIP_SAVE'})
    bpy.types.WindowManager.project_damage_ui_section=bpy.props.EnumProperty(
        name='Settings',items=(('GEOMETRY','Geometry','Geometry and drawing controls'),
                               ('MATERIAL','Material','Assign materials to generated regions'),
                               ('PHYSICS','Physics','Fill damage, fracture pieces and simulation')),
        default='GEOMETRY',options={'SKIP_SAVE'})
    bpy.types.WindowManager.project_damage_tools_category=bpy.props.EnumProperty(
        name='Tools Category',items=TOOLS_CATEGORIES,default='PERFORMANCE',
        description='Show one tool category at a time')
    bpy.types.WindowManager.project_damage_add_category=bpy.props.EnumProperty(
        name='Category',
        items=(('ALL','All','Show all effects'),) + tuple((key,title,'Show effects tagged '+title) for key,title,_ in EFFECT_CATEGORIES),
        default='ALL', description='Browse a short list of effect families; Physics Support collects effects with simulation; Standalone contains independent structures, cloth and cables')
    bpy.types.WindowManager.project_damage_add_search=bpy.props.StringProperty(
        name='Search', description='Search effects by name or category tags',
        default='', options={'TEXTEDIT_UPDATE','SKIP_SAVE'})
    from .core_loader import EFFECT_VIEW_ITEMS
    from .presets import PRESET_APPLY_MODES
    bpy.types.WindowManager.project_damage_add_view = bpy.props.EnumProperty(
        name='Effect View', items=EFFECT_VIEW_ITEMS, get=_add_view_get, set=_add_view_set,
        options={'SKIP_SAVE'})
    bpy.types.WindowManager.project_damage_effect_thumbnail = bpy.props.EnumProperty(
        name='Effect', items=_thumbnail_items, get=_thumbnail_get, set=_thumbnail_set,
        options={'SKIP_SAVE'})
    bpy.types.WindowManager.project_damage_preset_apply_mode = bpy.props.EnumProperty(
        name='Preset Apply Mode', items=PRESET_APPLY_MODES, default='NONE',
        options={'SKIP_SAVE'})
    for key in ('pipe','cable'):
        for group in ('break_open','axis_open','noise_open'):
            setattr(bpy.types.WindowManager,'project_damage_'+key+'_'+group,bpy.props.BoolProperty(default=group=='axis_open',options={'SKIP_SAVE'}))
    bpy.types.WindowManager.project_damage_lath_cavity_open=bpy.props.BoolProperty(default=True,options={'SKIP_SAVE'})
    bpy.types.WindowManager.project_damage_riven_cavity_open=bpy.props.BoolProperty(default=False,options={'SKIP_SAVE'})
    bpy.types.WindowManager.project_damage_lath_back_open=bpy.props.BoolProperty(default=False,options={'SKIP_SAVE'})
    bpy.types.WindowManager.project_damage_riven_back_open=bpy.props.BoolProperty(default=True,options={'SKIP_SAVE'})
    bpy.types.WindowManager.project_damage_framing_back_open=bpy.props.BoolProperty(default=True,options={'SKIP_SAVE'})
    bpy.types.WindowManager.project_damage_lath_joints_open=bpy.props.BoolProperty(default=False,options={'SKIP_SAVE'})
    bpy.types.WindowManager.project_damage_framing_extra_open=bpy.props.BoolProperty(default=False,options={'SKIP_SAVE'})
    bpy.types.WindowManager.project_damage_framing_nails_open=bpy.props.BoolProperty(default=True,options={'SKIP_SAVE'})
    bpy.types.WindowManager.project_damage_lath_holes_open=bpy.props.BoolProperty(default=False,options={'SKIP_SAVE'})
    bpy.types.WindowManager.project_damage_concrete_spall_open=bpy.props.BoolProperty(default=False,options={'SKIP_SAVE'})
    bpy.types.WindowManager.project_damage_hang_noise_open=bpy.props.BoolProperty(default=False,options={'SKIP_SAVE'})
    bpy.types.WindowManager.project_damage_fracture_noise_open=bpy.props.BoolProperty(default=False,options={'SKIP_SAVE'})
    bpy.types.WindowManager.project_damage_brick_surface_open=bpy.props.BoolProperty(default=False,options={'SKIP_SAVE'})
    bpy.types.WindowManager.project_damage_brick_knocked_open=bpy.props.BoolProperty(default=False,options={'SKIP_SAVE'})
    bpy.types.WindowManager.project_damage_brick_loose_open=bpy.props.BoolProperty(default=False,options={'SKIP_SAVE'})
    dev_tools.register_state()
    bpy.types.Object.project_damage_effects=bpy.props.CollectionProperty(type=ProjectDamageEffectEntry)
    from . import fracture_stack
    bpy.types.Object.project_damage_effect_index=bpy.props.IntProperty(default=0,min=0,update=fracture_stack.index_changed)
    bpy.app.handlers.load_post.append(sync_scene);bpy.app.handlers.depsgraph_update_post.append(sync_scene)
                                                                             
    if hasattr(bpy.data,'objects'):sync_scene()
    elif not bpy.app.timers.is_registered(_sync_once):bpy.app.timers.register(_sync_once,first_interval=.01)
# убираем подключения модуля при выключении аддона
def unregister():
    baked_materials.invalidate()
    _PREVIEW_CACHE.clear()
    _browser_entries.cache_clear()
    global _PREVIEWS
                                                                            
    if bpy.app.timers.is_registered(_sync_once):
        bpy.app.timers.unregister(_sync_once)
    for handlers in (bpy.app.handlers.load_post, bpy.app.handlers.depsgraph_update_post):
        for handler in tuple(handlers):
            if (getattr(handler, '__module__', None) == __name__
                    and getattr(handler, '__name__', None) == 'sync_scene'):
                handlers.remove(handler)
    if _PREVIEWS is not None:
        try:
            bpy.utils.previews.remove(_PREVIEWS)
        except (ReferenceError, KeyError):
            pass                                                                 
        _PREVIEWS = None
    _PREVIEW_FILES.clear()
    for key in ('pipe','cable'):
        for group in ('break_open','axis_open','noise_open'):
            name='project_damage_'+key+'_'+group
            if hasattr(bpy.types.WindowManager,name):delattr(bpy.types.WindowManager,name)
    for name in ('project_damage_lath_cavity_open', 'project_damage_riven_cavity_open', 'project_damage_lath_back_open', 'project_damage_riven_back_open', 'project_damage_framing_back_open', 'project_damage_lath_joints_open', 'project_damage_framing_extra_open', 'project_damage_framing_nails_open', 'project_damage_lath_holes_open', 'project_damage_concrete_spall_open'):
        if hasattr(bpy.types.WindowManager,name):delattr(bpy.types.WindowManager,name)
    if hasattr(bpy.types.WindowManager,'project_damage_hang_noise_open'):del bpy.types.WindowManager.project_damage_hang_noise_open
    if hasattr(bpy.types.WindowManager,'project_damage_fracture_noise_open'):del bpy.types.WindowManager.project_damage_fracture_noise_open
    for name in ('project_damage_brick_surface_open','project_damage_brick_knocked_open','project_damage_brick_loose_open'):
        if hasattr(bpy.types.WindowManager,name):delattr(bpy.types.WindowManager,name)
    for name in ('project_damage_tools_category', 'project_damage_add_category',
                 'project_damage_add_view', 'project_damage_ui_section', 'project_damage_add_search',
                 'project_damage_import_section', 'project_damage_effect_thumbnail',
                 'project_damage_preset_apply_mode'):
        if hasattr(bpy.types.WindowManager, name):
            delattr(bpy.types.WindowManager, name)
    if hasattr(bpy.types.WindowManager, 'project_damage_dev'):
        dev_tools.unregister_state()
    else:
        dev_tools._cache.clear()
    for name in ('project_damage_effect_index', 'project_damage_effects'):
        if hasattr(bpy.types.Object, name):
            delattr(bpy.types.Object, name)
    for cls in reversed(CLASSES):
        registered = _registered_class(cls)
        if registered is not None:
            bpy.utils.unregister_class(registered)

def _sync_once():
    sync_scene()
    return None
