# пояснения при наведении на настройки и кнопки
# SPDX-License-Identifier: GPL-3.0-or-later
"""Plain-English hover guidance; registration-time only, no geometry changes."""
import inspect
import json
from pathlib import Path

try:
    HOVER_HINTS=json.loads((Path(__file__).with_name("hover_hints_data.json")).read_text(encoding="utf-8"))
except (OSError,ValueError):
    HOVER_HINTS={}


EXACT={
 'scratch_mode':'Scatter places many scratches around each guide. Single Stroke uses the drawn path itself as one scratch (splitting only where the surface changes); cluster-placement sliders are ignored',
 'glass_auto_hole':'Break through locally thin and even regions even when the whole object is a solid. Disable to keep skin-deep openings on solids; Improved thickness logic only',
 'glass_auto_hole_thickness':'Maximum local thickness that Auto Hole may penetrate, in cm. Zero chooses 0.3 times opening radius capped at 2 percent of object diagonal',
 'lath_wall_slope':'Maximum broken-wall slope measured from the surface, in degrees. Lower values ease the deep side back without moving the outer outline; 0 or 90 disables the limit',
 'lath_cavity_slope':'Maximum slope of the floor behind laths, in degrees. Lower values make plaster keys gentler; 0 or 90 disables. Holes and plaster rims stay protected',
 'lath_hole_noise_scale':'Hole-outline noise feature size as a fraction of patch size. Increase for broader lobes; noise finer than three grid cells is suppressed',
 'lath_hole_detail':'Weight of finer hole-outline noise octaves. Zero gives a smoother single octave; higher values add raggedness within the grid resolution',
 'rebar_drop_loose':'Remove unsupported internal fill. Upright bricks must rest on supported bricks, be gripped, hang at most one course or bridge one gap; steel and wood use their anchoring checks. Disable for the previous loose/connectivity behavior',
 'rebar_mortar_joints':'Add recessed mortar attached to each Boolean-fill brick. Mortar is trimmed with broken bricks and has its own material slot; some joints are omitted for weathering',
 'rebar_mortar_recess':'Recession of Boolean brick mortar as a fraction of brick-layer depth. Increase to set joints farther back from brick faces; requires Mortar Joints',
 'rebar_angle':'Rotate the steel reinforcement layout. Brick fill ignores Angle so courses remain level to world up',
 'rebar_fill':'Choose steel reinforcement, a connected brick layer, wooden boards, or a timber stud frame behind a drywall skin (Wood Framing), or torn plywood plies with splinters (Wooden Chips) inside Boolean-damaged regions. All use the same fit/layout and support checks. This is distinct from the Brick Damage effect',
 'rebar_brick_length':'Length of Boolean-fill bricks, in cm before Scale. Full-size bricks need a thick enough source; use Scale 0.5 or less for thin sculpted parts',
 'rebar_brick_height':'Course height of Boolean-fill bricks in cm. Smaller heights create more brick courses',
 'rebar_brick_depth':'Thickness of the Boolean-fill brick layer in cm. Reduce on thin parts so the layer fits beneath Cover',
 'rebar_mortar':'Joint spacing between Boolean-fill bricks in cm; also used by their support/connectivity checks. This is not the Brick Damage mortar-output switch',
 'rebar_missing':'Bricks: probability of missing free bricks, rising farther from a held neighbor. Planks: probability that an exposed board span is removed',
 'rebar_brick_broken':'Chance of a Boolean-fill brick being broken at the damaged boundary or free chain end. Increase for a more irregular exposed layer',
 'rebar_chamfer':'Worn-edge chamfer as a fraction of the smaller Boolean brick dimension. Increase for softer brick corners',
 'rebar_brick_jitter':'Small offsets and rotations of Boolean-fill bricks. Increase for irregular laying without changing the main layout',
 'rebar_plank_width':'Width of Boolean-fill boards in cm before Scale. Wider boards create a coarser wooden layer',
 'rebar_plank_thickness':'Depth of Boolean-fill boards in cm. Reduce to fit thinner source regions',
 'rebar_plank_gap':'Gap between Boolean-fill boards in cm. Increase for a more open wooden layer',
 'rebar_plank_dir':'Choose horizontal boards or vertical staves. Vertical boards often reveal more with a horizontal cut',
 'rebar_splinter':'Length of splinters at Boolean board breaks relative to board width. Increase for longer jagged wooden ends',
 'brick_corners':'Separate Bricks: wrap damage around sharp inside/outside wall corners using alternating toothed courses. Overlapping wrapped patches may still overlap their brick layouts',
 'brick_joint_fill':'Separate Bricks: generate mortar around bricks that remain. Bed joints sit above/below retained bricks; head joints require neighbors on both sides. Assign Mortar Joints material',
 'brick_remesh':'Separate Bricks: even the pocket triangles and locally remesh the damaged wall result. Flat surfaces stay in place; native UV/provenance properties are retained. Disable for the prior triangulation',
 'brick_remesh_size':'Target edge length for local wall remeshing in cm, multiplied by Brick Scale and Master Scale. Smaller values create denser geometry and cost more time; only patch zones are remeshed',
 'brick_mortar_depth':'Recession of mortar behind brick faces in cm. Separate Bricks uses this for generated mortar when Joint Fill is on; Carved uses it for joint cuts',
 'brick_brick_mode':'Separate Bricks (default) cuts an opening with a noise-damaged pocket and creates one separate brick layer inside it. Carved keeps the older brick shapes cut into the wall. Swept Low Poly uses the carved workflow',
 'brick_front_material':'Material for brick faces and sides, including the separate brick-layer object. Empty uses default brick shading on the separate layer; this does not change the source materials',
 'rebar_mode':'Around Damage creates bars near Boolean cuts; Built In lays out a fixed internal skeleton and damage reveals it',
 'rebar_fit':'Auto selects a fitting strategy; Object Box suits regular walls and columns; Follow Shape follows organic or sculpted forms',
 'global_scale':'Master size multiplier for damage geometry. Increase for larger features; it does not resize the source object',
 'global_detail':'Master sampling quality. Increase for finer geometry at higher memory and evaluation cost; use a lower value while editing',
 'all_off':'Temporarily mute every damage effect without removing its settings',
 'live_update':'Rebuild damage automatically after relevant changes. Disable while adjusting expensive effects, then use INIT_project_damage',
 'defer_ms':'Delay automatic rebuilds after edits, in milliseconds. Increase to avoid rebuilding during every slider movement',
 'preserve_data':'Transfer original materials and supported mesh attributes to the generated mesh; the original stays unchanged',
 'keep_uvs':'Preserve original UV coordinates where source-face provenance is available. Newly exposed faces require generated or transferred UVs',
 'keep_selections':'Carry supported source selections to the result; newly generated faces use their damage classifications',
 'topology':'Choose the output face layout. Triangles are predictable; merged quads or ngons can reduce visible triangulation without adding geometric detail',
 'ui_boolean_enable':'Enable Boolean cutters and nested Rebar for this source. Disabling keeps links and settings but suppresses their output',
 'brick_sharp_edge':'Keep the original render flat until a vertical break down to the brick face. On disables soft terraces, Wall Soft, render-floor roughness and Render Stick; use Broken Render Edge material with Split Materials',
 'brick_split_materials':'Expose separate face regions for front bricks, snapped bricks, mortar, backing and the sharp render edge. Assign their materials below',
 'brick_style':'Detailed uses chipped, rough, tilted bricks. Low Poly uses plain box bricks but retains the detailed render edge, cracks and core. Swept Low Poly is the previous fully swept method for flat walls; it ignores Sharp Edge and Damaged Core',
 'brick_behind':'Carved mode: Damage Pocket extends behind all removed brick leaves; Brick Leaves omits it. Separate Bricks always uses a single brick layer and pocket, and ignores this selector. Swept Low Poly retains the old carved behavior',
 'brick_damage_depth':'Extra pocket depth beyond the bricks, in cm. Separate Bricks: beyond the single layer. Carved Damage Pocket: beyond all leaves. Reaching the wall back can open a hole; ignored by Swept Low Poly',
 'brick_damage_noise':'3D displacement amplitude on the pocket floor and sides, in cm. Zero leaves an undisplaced pocket. Capped to 20 percent of Damage Scale; fades to zero beside the brick layer to preserve thickness',
 'brick_damage_scale':'Spatial size of the pocket displacement noise, in cm. Increase for broad features; decrease for finer breakup. The displacement amplitude cannot exceed 20 percent of this value',
 'brick_layers':'Carved mode: number of removable brick leaves; zero removes render only. Ignored in Separate Bricks, which creates exactly one brick layer',
 'brick_layer_inset':'Inset of each deeper leaf in Carved mode as a share of patch size. Ignored by Separate Bricks because there is only one brick layer',
 'brick_collar_fill':'Mortar between brick leaves in Carved mode. Ignored by Separate Bricks, which has one layer and open gaps',
 'brick_behind_material':'Material for the damage-pocket floor and sides. Separate bricks use Brick Faces / Sides instead. In Carved Brick Leaves mode this retains the older backing classification',
 'lath_cavity_smooth':'Smooth the cavity behind the planks, not the plaster rim. Raise to suppress spikes; 0 gives the original cavity. The thickness safety cap still applies',
 'lath_rim_smooth':'Reduce grid-scale jaggedness on plaster and hole walls. Raise for smoother walls; increase Detail to preserve smaller features. 0 gives the original walls',
 'lath_lath_optimize':'Remove redundant plank cross-sections with a geometric error tolerance. Raise for fewer polygons; lower to retain small warps and splinters. 0 keeps every section',
 'lath_material':'Material for the separate wooden-lath object. Empty uses Buildup material or a default rough wood-colored shader',
 'brick_render_material':'Material for the narrow broken render edge, independent of brick and mortar materials. Requires Sharp Edge and Split Materials; a bare wall has no render edge',
 'project_damage_effect_index':'Select which effect settings are displayed; this does not change evaluation order',
}

COMMON={
 'skin_depth':'Depth of the affected surface skin. Increase to reach farther beneath the source surface; avoid values larger than thin local features',
 'thickness':'Physical thickness used by this effect. Increase for a thicker layer or shell; lengths shown in cm follow Master Scale',
 'sheet_thickness':'Thickness used when treating the source as a sheet. Match the real sheet thickness so cuts and edge effects have a sensible depth',
 'thread_radius':'Radius of generated fibres. Increase for thicker, more visible threads; very fine fibres need adequate mesh resolution',
 'fray_spacing':'Distance between frayed fibres along an edge. Decrease for denser fraying and more geometry',
 'paint_spacing':'Distance between repeated features along a stroke. Decrease for denser marks and a higher evaluation cost',
 'paint_spread':'Lateral spread of features around the drawn guide. Lower for a narrow controlled path; increase for a wider affected band',
 'paint_taper':'Taper the influence toward the ends of a drawn path. Raise for less abrupt starts and ends',
 'drawn_only':'Restrict the effect to the supplied drawing. Without eligible strokes on the current frame, no drawn damage is created',
 'paint_single':'Use the single-feature interpretation of each drawn input instead of repeated placement along it',
 'paint_partial':'Allow partial guide influence near the drawn boundary instead of requiring complete coverage',
 'taper_flip':'Reverse which end of the feature is tapered. Use when the narrow end faces the wrong way',
 'mask_falloff':'Width of the fade at the influence-mask boundary. Increase for a more gradual transition',
 'depth_var':'Random variation of feature depth. Increase for less uniform damage; keep the seed fixed while comparing',
 'edge_jagged':'Irregularity along torn or damaged edges. Increase for a rougher silhouette; fine irregularities require enough Detail',
 'edge_meander':'Wandering of the edge tear or cut path. Increase for less straight edge damage',
 'crack_reach_jitter':'Variation in how far cracks extend. Increase for a less uniform fracture reach',
 'ring_jitter':'Irregularity in ring spacing or placement. Increase to reduce perfectly concentric patterns',
 'halo_jagged':'Irregularity of the halo boundary around an impact. Increase for a less circular surrounding mark',
 'seam_snap':'Distance or tolerance for snapping generated features to seams. Increase if nearby features miss the seam; lower for tighter placement',
 'wear_mask_noise':'Random variation within the edge-wear mask. Increase to break up a uniform worn band',
 'wear_mask_scale':'Spatial scale of wear-mask noise. Increase for broader patches of wear',
 'wear_mask_width':'Width of the edge-wear influence band. Increase to spread wear farther from eligible edges',
 'mode':'Choose automatic placement or damage guided by drawn strokes. Paint needs a drawing on the current frame; an empty drawing produces no painted damage',
 'seed':'Change the repeatable random pattern without changing the feature sizes. Keep the seed fixed when comparing other settings',
 'detail':'Sampling resolution for this effect. Higher values resolve smaller features but increase mesh density and processing cost; start low for previews',
 'avoid':'Respect the source faces marked as Avoid. Turn off only if this effect should also use protected regions',
 'amount':'Requested number or density of generated features. Increase for more damage; placement may produce fewer features where there is no eligible space',
 'size_min':'Lower bound of the feature-size range. Keep it below Size Max; raise both bounds for larger features',
 'size_max':'Upper bound of the feature-size range. Increase the gap from Size Min for more size variation',
 'depth_min':'Minimum cut depth. Increase for deeper shallow features; check the available mesh thickness',
 'depth_max':'Maximum cut depth. Increase for stronger cavities; the thickness cap may limit cuts on thin regions',
 'plaster_min':'Lower depth of the front coat in centimeters. Keep it at or below Plaster Max',
 'plaster_max':'Upper depth of the front of the wooden layer below the wall surface, in centimeters',
 'render_min':'Lower depth of the plaster over the brickwork in centimeters. Set Plaster Min and Plaster Max to zero for bare brick',
 'render_max':'Upper depth of the front of the brickwork in centimeters. Higher values let patches recess farther below the surface',
 'paint_radius':'Half-width of damage around an open drawn stroke. Increase for a wider painted band; closed contours define their own area',
 'paint_snap':'Maximum distance used to associate a drawn guide with the mesh surface. Raise if nearby strokes are missed; lower to avoid affecting distant surfaces',
 'paint_close':'Treat a nearly closed drawn outline as a filled region rather than an open band',
 'drawing':'Grease Pencil or curve supplying guides on the current frame. Draw near the source surface and use the matching paint mode',
 'collection':'Collection supplying this effect\'s helper objects. Only supported object types are read; the source mesh itself is not used as a helper',
 'up_x':'X component of the local up direction used to orient courses or strips. Use a nonzero direction vector, usually (0, 0, 1)',
 'up_y':'Y component of the local up direction used to orient courses or strips. Together with X and Z defines the orientation',
 'up_z':'Z component of the local up direction used to orient courses or strips. 1 with X/Y at 0 uses local Z-up',
 'thickness_cap':'Maximum cut depth as a fraction of local mesh thickness outside intentional holes. Lower to preserve more backing; zero disables the cap',
 'key_size':'Spatial size of the plaster keys between wooden strips. Larger keys are broader; Cavity Smooth uses this scale to remove narrow spikes',
 'keys':'Amount of plaster remaining as keys between the laths. Reduce for cleaner open gaps',
 'wall_soft':'Width of the sloping transition into a cavity. Increase for a softer slope; Rim Smooth instead targets grid-scale jaggedness',
 'rim_smooth':'Anti-aliasing of the damaged rim at the sampling-grid scale. Raise to remove spikes rather than broaden the entire damage region',
 'roughness':'Geometric unevenness of the damaged floor, not shader roughness. Increase for a rougher relief; use Material properties for shading roughness',
 'lobes':'Number of overlapping blobs defining a patch outline. More lobes produce a more complex, cloud-like outline',
 'stretch':'Elongation of damage patches. 1 is approximately round; higher values create longer patches',
 'edge_bias':'Bias placement toward eligible sharp edges and corners. Increase for edge-focused damage',
 'jagged':'Irregularity of the damage outline. Increase for a rougher perimeter; Detail must be high enough to resolve it',
 'crumble':'Fine breakup of the plaster/render rim. Increase for a more crumbly edge; lower for a cleaner silhouette',
 'crumble_size':'Size of the rim-crumble noise. Increase for broader chunks; decrease for fine breakup within the available sampling resolution',
 'terraces':'Number of stepped coat levels at the damaged rim. More levels suggest layered plaster; ignored by Brick Sharp Edge',
 'terrace_skin':'Controls the thickness distribution of stepped surface coats. Use with Terraces; ignored by Brick Sharp Edge',
 'terrace_inset':'Controls how successive plaster/render terraces step inward. Ignored by Brick Sharp Edge',
 'outline_scale':'Spatial scale of the coarse noise deforming patch outlines. Increase for broader outline variation',
 'hole_chance':'Probability that an eligible damage patch becomes a through-hole. 0 creates no random holes; 1 allows every eligible patch',
 'hole_size':'Size of the through-hole relative to its parent damage patch. Increase to remove more of the backing',
 'crack_amount':'Number of cracks emitted from each eligible patch. Increase for a denser fracture network',
 'crack_width':'Width of the generated crack grooves. Increase for more visible cracks; narrow cracks require adequate Detail',
 'crack_depth':'Depth of the generated crack grooves. Increase cautiously on thin walls',
 'crack_meander':'Side-to-side wandering of crack paths. Increase for less straight cracks',
 'crack_branch':'Likelihood or strength of secondary branches along cracks. Increase for more branching',
 'crack_interior':'Allow crack paths inside the damaged area rather than only beyond its rim',
 'lath_width':'Width of each wooden strip. Increase for broader planks; Gap controls the space between strips',
 'lath_thickness':'Depth of the wooden strips. Increase for thicker wood; Optimization tolerance also scales with the smaller strip dimension',
 'lath_gap':'Space between adjacent wooden strips. Increase to expose more of the cavity',
 'lath_embed':'Length of wood extending beneath intact plaster at the patch ends. Increase to make planks appear supported',
 'lath_length':'Length scale of wooden strip pieces. Adjust with Broken and Missing to change fragmentation',
 'lath_scale':'Scale multiplier for the lath layout. Increase to make the wooden structure coarser',
 'lath_angle':'Rotation of the wooden strips within the local patch plane. Use to align the underlying construction',
 'lath_amount':'Share of eligible wooden laths generated in exposed regions. Lower to make the wood sparser',
 'lath_missing':'Share of wooden strip pieces omitted. Increase to reveal more empty gaps',
 'lath_broken':'Share of wooden strips snapped into pieces. Increase for more fragmented wood',
 'lath_splinter':'Strength or amount of splinters at broken wooden ends. Increase for rougher breaks; raises geometry cost',
 'lath_warp':'Bowing or waviness along the wooden strips. Increase for aged, distorted wood',
 'lath_tilt':'Random tilt of individual wooden strips. Increase to break uniform alignment',
 'lath_jitter':'Random variation in wooden-strip placement. Increase to reduce mechanical regularity',
 'lath_bend':'Bending of broken wooden strips. Increase for more displaced ends',
 'lath_edge':'Raggedness of wooden-strip edges. Increase for worn edges; retain enough Detail and reduce Optimization if fine features disappear',
 'cavity_depth':'Depth of the cavity behind the wooden strips. Increase for darker, deeper gaps while respecting wall thickness',
 'hole_laths':'Controls wooden laths retained across through-holes. Reduce for a more open hole',
 'drop_loose':'Remove reinforcement not held by the final damaged concrete. Disable only when unsupported bars are intentionally wanted',
}

def description(name,kw):
    # Keep explicitly authored multi-line tooltips intact.
    if kw.get('description','').count('\n')>=2:return kw['description']
    if name in EXACT:return EXACT[name]
    from .framing import HINTS as _framing_hints
    if name in _framing_hints:return _framing_hints[name]
    from .chips import HINTS as _chips_hints
    if name in _chips_hints:return _chips_hints[name]
    old=kw.get('description','').strip()
    parts=name.split('_',1);leaf=parts[1] if len(parts)>1 else name
    if kw.get('items') and not isinstance(kw['items'],str) and not callable(kw['items']):
        choices=[str(item[1])+(': '+item[2] if len(item)>2 and item[2] else '') for item in kw['items'] if item]
        return (old+'. ' if old else '')+'Choose '+kw.get('name',name)+'. '+ '; '.join(choices)
    if leaf in COMMON:return COMMON[leaf]
    if name in COMMON:return COMMON[name]
    if name.endswith('_material') or name in {'wall_material','wallpaper_material'}:
        return 'Assign a material to this generated region. Empty retains its default or shared damage material; the source material datablocks are not edited'
    if name.endswith('_drawing'):return COMMON['drawing']
    if name.endswith('_collection'):return COMMON['collection']
    if leaf=='enable' or name.endswith('_enable'):
        return 'Enable this feature for the current source. Disable to omit it without losing its stored settings'
    if old and old not in {'; centimetres','In cm, matching Cinema 4D'}:
        old=old.replace('honour CoreInput::avoidPoly','Respect protected source faces').replace('mixed with ProjectDamageParams::seed, like heatSeed/coatingSeed','Combined with the master seed for repeatable variation')
        if 'cm' in kw.get('name','') or 'centimetres' in old:return old.rstrip('.')+'. Lengths are in centimeters; adjust Master Scale for the overall damage size'
        return old.rstrip('.')+'. Change one control at a time with the seed fixed to compare its effect'
    if leaf.endswith('_min') or leaf.endswith('_max'):
        bound='lower' if leaf.endswith('_min') else 'upper'
        return 'The '+bound+' bound for '+leaf.rsplit('_',1)[0].replace('_',' ')+'. Keep the minimum at or below the maximum; widen the range for more variation'
    if leaf.endswith('_x') or leaf.endswith('_y') or leaf.endswith('_z'):
        return 'Local-space '+leaf[-1].upper()+' component of '+leaf[:-2].replace('_',' ')+'. Set all three components together to define a nonzero direction'
    if leaf.endswith(('_depth','_width','_length','_thickness','_radius','_spacing')):
        label=leaf.replace('_',' ')
        return 'Sets '+label+'. Increase for '+('wider spacing and fewer repeated features' if leaf.endswith('_spacing') else 'a larger '+leaf.rsplit('_',1)[-1])+'; displayed centimeter lengths follow Master Scale. Check local mesh thickness when increasing depth'
    if kw.get('items'):
        return 'Choose '+kw.get('name',name).replace('_',' ')+'. Hover an individual choice for its mode description; switching keeps the other stored settings'
    if isinstance(kw.get('default'),bool):
        return 'Include '+kw.get('name',name).replace('_',' ')+' in this effect. Disable to omit this feature while retaining its other settings'
    return 'Controls '+kw.get('name',name).replace('_',' ')+' for the current source. Compare changes with a fixed seed; use a low Master Quality for faster previews'

def enhance_module(module):
    if getattr(module,'_project_damage_hints_ready',False):return
    for cls in vars(module).values():
        if not inspect.isclass(cls) or cls.__module__!=module.__name__:continue
        for name,prop in cls.__dict__.get('__annotations__',{}).items():
            kw=getattr(prop,'keywords',None)
            if kw is not None:kw['description']=HOVER_HINTS.get(cls.__name__+'.'+name) or description(name,kw)
    module._project_damage_hints_ready=True


def apply_registered_descriptions():
    """Update tooltip metadata only; preserve ID values and geometry callbacks.

    Some asset classes are constructed inside their module register(). Catch
    those here as well as supporting a live UI refresh without unregistering.
    """
    import sys
    import bpy
    seen=set();updated=0
    for module_name,module in tuple(sys.modules.items()):
        if not module_name.startswith(__package__+'.') or module is None:continue
        for cls in tuple(vars(module).values()):
            if not inspect.isclass(cls) or id(cls) in seen or not getattr(cls,'is_registered',False):continue
            seen.add(id(cls))
            if not issubclass(cls,(bpy.types.PropertyGroup,bpy.types.AddonPreferences)):continue
            for name,deferred in cls.__dict__.get('__annotations__',{}).items():
                text=HOVER_HINTS.get(cls.__name__+'.'+name)
                if not text:continue
                prop=cls.bl_rna.properties.get(name)
                if prop is None or prop.description==text:continue
                kw=dict(getattr(deferred,'keywords',{}))
                if not kw:continue
                kw['description']=text
                new=deferred.function(**kw)
                setattr(cls,name,new)
                cls.__annotations__[name]=new
                updated+=1
    return updated
