# свойства объекта: исходные значения, ограничения и реакции на изменения
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
"""The parameters, mirrored from the Cinema 4D plugin.

C4D IS THE BASELINE. Every property below carries the C4D parameter id it
mirrors, the label from cpp/res/strings_us/description/oproject_damage.str verbatim, and
the PRODUCT default - the value ProjectDamageGenerator::Init writes
(cpp/source/project_damage_plugin.cpp), which is not always the .res fallback: Chips
Detail is 75% and Topology is Keep, both moved for interactive performance.
Nothing here is re-imagined for Blender.

Mapping rules from the .res, applied uniformly:
  - REAL ... UNIT PERCENT  -> FloatProperty(subtype='PERCENTAGE'), same numbers
    (100.0 means 100%); params.py divides by 100 when filling Params, exactly
    where C4D's own container-to-ProjectDamageParams read does.
  - REAL ... UNIT METER    -> plain FloatProperty, no unit, described as
    centimeters: the numbers ARE C4D centimeters (Size Min 2 means 2 cm), and
    params.py's unit_factor converts the mesh into that space before the core
    sees it and back after, so they mean the same thing on Blender's own
    default 2 m cube as on C4D's default 200 cm one. unit='LENGTH' would store
    meters and print "2 m" next to a 2 cm chip - fighting the C4D numbers
    rather than converting around them - so these stay plain floats.
  - REAL ... UNIT DEGREE   -> FloatProperty(subtype='ANGLE'): Blender stores
    radians and shows degrees, which is exactly what C4D does.
  - MIN/MAX from the .res become hard min/max; MINSLIDER/MAXSLIDER become
    soft_min/soft_max. Where the .res declares only a MIN (Amount, every
    METER field), Blender still needs a drag range: soft_max is then chosen
    here and marked `soft only` - a typed value above it is still accepted,
    so no C4D range is narrowed.

Everything absent - Optimize/Cleanup, SDS, selection names/bundles - keeps
the core's own struct default; see blender/CLAUDE.md's "Deliberately absent"
section for why each one is out.
"""
import bpy

from . import live, params


def _paint_drawing_poll(self, obj):
    return obj.type in {'GREASEPENCIL', 'GPENCIL', 'CURVE'}


def _fracture_mode_changed(self, context):
    if self.fract_mode == 'PAINT' and self.fract_l1_drawing is None and not self.get('fract_paint_initialized'):
        self['fract_paint_initialized'] = True
        defaults = {'fract_paint_scale': 35.42, 'fract_l2_paint_amount': 12,
                    'fract_l3_paint_amount': 11, 'fract_l2_paint_length': 71.88,
                    'fract_l2_paint_distortion': 7.97, 'fract_l2_paint_distortion_scale': 197.90,
                    'fract_l3_paint_distortion': 5.21}
        for name, value in defaults.items():
            if self.get(name) is None:
                self[name] = value
        for level in (2, 3):
            prefix = f'fract_l{level}_paint_length'
            if self.get(prefix + '_min') is None:
                self[prefix + '_min'] = round(.65 * getattr(self, prefix), 2)
    live.touch_prop(self, context)


def _paint_value(name, fallback):
    return dict(get=lambda self: self.get(name, fallback(self)),
                set=lambda self, value: self.__setitem__(name, value), update=live.touch_prop)


def _wallpaper_drawing_touch(self,context):
    previous=live._APPLYING;live._APPLYING=True
    try:self.wallpaper_paint_enable=self.wallpaper_drawing is not None
    finally:live._APPLYING=previous
    live.touch_prop(self,context)

TOPOLOGY_ITEMS = (
                                                                                                  
                                                                            
    ('0', "Triangles", "Raw Boolean triangles"),
    ('1', "Quads", "Coplanar triangles merged back into quads"),
    ('2', "N-gons", "Quads plus an N-gon overlay per merged region"),
    ('3', "Keep", "Restore the input topology on untouched polygons"),
)

AXIS_ITEMS = (
                                                                                                          
                                                                          
                                   
    ('1', "On", "Chains dominant on this axis are eligible for chips"),
    ('0', "Off", "Chains dominant on this axis are skipped"),
)

BREAKER_MODE_ITEMS = (
    # obreaker.h BREAKER_MODE cycle values 0/1/2; Custom (3) is DERIVED from a
    # child, never stored - mirroring EffectiveBreakerMode (project_damage_breaker.cpp).
    ('0', "Plane", "One noisy break face closed by a skirt"),
    ('1', "Sphere", "A closed noisy ball, for a local bite"),
    ('2', "Strip", "Two break faces a Thickness apart"),
)


def _sync_display(self, context):
    """Empty display follows the STORED mode and extent. A Custom child simply
    covers it visually - no parenting handler, deliberately. Plane/Strip get
    the small PLAIN_AXES click handle only; gizmo.py draws the actual
    rectangle footprint (Empties have no flat-rectangle display type)."""
    if live._HISTORY_RESTORING:
        return
    obj = self.id_data
    if obj is None or obj.type != 'EMPTY':
        return
    factor = params.unit_factor(context)
    if self.mode == '1':
        obj.empty_display_type = 'SPHERE'
        obj.empty_display_size = self.radius / factor
    else:
        obj.empty_display_type = 'PLAIN_AXES'
        obj.empty_display_size = max(self.length, self.width) * 0.1 / factor


def _sync_display_and_touch(self, context):
    """The four breaker fields (mode/length/width/radius) both drive the
    Empty's own display AND change what gets cut - _sync_display alone would
    leave a live source stale after one of these moves."""
    _sync_display(self, context)
    live.touch_prop(self, context)


def _cutter_poll(self,obj):
    return obj != self.id_data and obj.type in {'MESH','CURVE','SURFACE','FONT','META'} and not any(obj.get(k) for k in ('project_damage_result_of','project_damage_rebar_of','project_damage_deposits_of','project_damage_coating_of','project_damage_lath_of','project_damage_bricks_of'))


# здесь хранятся значения, которые сохраняются вместе с объектом
class ProjectDamageBreakerProps(bpy.types.PropertyGroup):
    # повреждения поверхности
    surf_mode: bpy.props.EnumProperty(name='Break Surface',items=(('0','Procedural','Original procedural noise'),('1','Texture','Image displacement for Plane, Sphere, Strip and Custom cutters. Dense geometry or excessive depth can slow or invalidate the cutter')),default='0',update=live.touch_prop)
    tex_path: bpy.props.StringProperty(name='Height Texture',subtype='FILE_PATH',description='PNG/JPG grayscale displacement. Empty uses bundled Concrete025 gravel displacement',update=live.touch_prop)
    tex_scale: bpy.props.FloatProperty(name='Texture Scale (cm)',default=30.,min=.1,soft_max=200.,description='Tile size in source-local space; larger values make larger texture features',update=live.touch_prop)
    tex_strength: bpy.props.FloatProperty(name='Texture Depth (cm)',default=1.5,min=0.,soft_max=10.,description='Peak-to-peak texture displacement on top of the Macro Shape, multiplied by breaker Strength. 0 = the procedural surface only (identical to Procedural)',update=live.touch_prop)
    tex_edge: bpy.props.FloatProperty(name='Mesh Edge Size (cm)',default=0.,min=0.,soft_max=5.,description='Target triangle size inside the source. 0 uses Texture Scale / 60. Increase for faster updates and fewer triangles',update=live.touch_prop)
    tex_exit: bpy.props.FloatProperty(name='Exit Detail',default=2.,min=1.,max=8.,description='Refinement multiplier near the boundary where the break exits the source; higher is denser and slower',update=live.touch_prop)
    smooth_slope: bpy.props.BoolProperty(name='Smooth Slope', default=False, update=live.touch_prop, description='Like Concrete Damage V2: the opening flares out where the break meets the surface, so the break wall runs into it as a gentle slope instead of an edge (no rim chips on it)')
    smooth_width: bpy.props.FloatProperty(name='Slope Width', default=0.35, min=0.02, max=1.0, update=live.touch_prop, description='Width of the slope band as a share of the breaker size (sphere: radius; plane: half the smaller side)')
    tex_macro: bpy.props.FloatProperty(name='Macro Shape',default=1.,min=0.,max=4.,description='Bottom layer: the procedural break surface (the same as Procedural mode) under the image displacement; 1 = the Procedural shape, 0 = only the texture',update=live.touch_prop)
    tex_offset_x: bpy.props.FloatProperty(name='Offset X (cm)',default=0.,update=live.touch_prop)
    tex_offset_y: bpy.props.FloatProperty(name='Offset Y (cm)',default=0.,update=live.touch_prop)
    tex_offset_z: bpy.props.FloatProperty(name='Offset Z (cm)',default=0.,update=live.touch_prop)
    cutter: bpy.props.PointerProperty(name="Cutter Object",type=bpy.types.Object,poll=_cutter_poll,update=live.touch_prop)
    custom_sheet: bpy.props.EnumProperty(name="Open Mesh",items=(('0','Cut One Side','Cut the side the sheet normals face'),('1','Slab','Thicken the sheet using Thickness')),default='0',update=live.touch_prop)
    # заполнение арматурой
    rebar: bpy.props.BoolProperty(name="Expose Rebar",default=True,update=live.touch_prop)
    """One instance per breaker Empty (Object.project_damage_breaker), mirroring the
    ProjectDamage Breaker object's description (cpp/res/description/obreaker.res) at
    ProjectDamageBreaker::Init's defaults. is_breaker is the marker the panels,
    list poll and build path all key on."""
    is_breaker: bpy.props.BoolProperty(default=False, update=live.touch_prop)
                                                                        
                                                                           
    enabled: bpy.props.BoolProperty(name="Enabled", default=True, update=live.touch_prop)
    mode: bpy.props.EnumProperty(name="Mode", items=BREAKER_MODE_ITEMS,
                                 default='0', update=_sync_display_and_touch)
    flip: bpy.props.BoolProperty(name="Invert", default=False, update=live.touch_prop)
    seed: bpy.props.IntProperty(name="Seed", default=12345, min=0, update=live.touch_prop)
                                                                           
                                                                  
    length: bpy.props.FloatProperty(name="Length", default=120.0, min=1.0,
                                    soft_max=1000.0, update=_sync_display_and_touch)             
    width: bpy.props.FloatProperty(name="Width", default=120.0, min=1.0,
                                   soft_max=1000.0, update=_sync_display_and_touch)              
    thickness: bpy.props.FloatProperty(name="Thickness", default=10.0, min=0.1,
                                       soft_max=200.0, update=live.touch_prop)                                 
    radius: bpy.props.FloatProperty(name="Radius", default=50.0, min=1.0,
                                    soft_max=500.0, update=_sync_display_and_touch)              
    resolution: bpy.props.FloatProperty(name="Resolution", default=100.0,
        subtype='PERCENTAGE', min=5.0, max=400.0, soft_min=5.0, soft_max=400.0, update=live.touch_prop)
                         
    strength: bpy.props.FloatProperty(name="Strength", default=100.0,
        subtype='PERCENTAGE', min=0.0, max=400.0, soft_min=0.0, soft_max=400.0, update=live.touch_prop)
    macro_h: bpy.props.FloatProperty(name="Macro Height", default=8.0,
        subtype='PERCENTAGE', min=0.0, max=50.0, soft_min=0.0, soft_max=50.0, update=live.touch_prop)
    turb_h: bpy.props.FloatProperty(name="Turbulence Height", default=7.0,
        subtype='PERCENTAGE', min=0.0, max=50.0, soft_min=0.0, soft_max=50.0, update=live.touch_prop)
    fine_h: bpy.props.FloatProperty(name="Fine Height", default=3.0,
        subtype='PERCENTAGE', min=0.0, max=50.0, soft_min=0.0, soft_max=50.0, update=live.touch_prop)
    noise_scale: bpy.props.FloatProperty(name="Noise Scale", default=100.0,
        subtype='PERCENTAGE', min=25.0, max=400.0, soft_min=25.0, soft_max=400.0, update=live.touch_prop)
                         
    rim_amp: bpy.props.FloatProperty(name="Amount", default=13.0,
        subtype='PERCENTAGE', min=0.0, max=50.0, soft_min=0.0, soft_max=50.0, update=live.touch_prop)
    rim_scale: bpy.props.FloatProperty(name="Wavelength", default=100.0,
        subtype='PERCENTAGE', min=25.0, max=400.0, soft_min=25.0, soft_max=400.0, update=live.touch_prop)


class ProjectDamageBreakerLink(bpy.types.PropertyGroup):
    """One row of ProjectDamageProps.breakers - a link to a breaker Empty, mirroring
    one entry of the C4D IN_EXCLUDE list (PROJECT_DAMAGE_BREAK_LIST, 1149)."""
    def _poll(self, obj):
        return obj is not self.id_data and ((obj.type == 'EMPTY' and obj.project_damage_breaker.is_breaker) or (obj.type in {'MESH','CURVE','SURFACE','FONT','META'} and not any(obj.get(k) for k in ('project_damage_result_of','project_damage_rebar_of','project_damage_deposits_of','project_damage_coating_of','project_damage_lath_of','project_damage_bricks_of'))))
    def _changed(self, context):
        from . import break_surface
        break_surface.cutter_link_changed(self, context)

    obj: bpy.props.PointerProperty(type=bpy.types.Object, poll=_poll,
                                   name="Breaker", update=_changed)


class ProjectDamageGuideLink(bpy.types.PropertyGroup):
    """One row of ProjectDamageProps.guides - a link to a curve object, mirroring one
    entry of the C4D IN_EXCLUDE list (PROJECT_DAMAGE_FRACT_GUIDE, 1191). The C4D list
    carries no ACCEPT filter because a spline PRIMITIVE is not an instance of
    Ospline; here the poll can be exact, since Blender has one curve type."""
    def _poll(self, obj):
        return obj.type == 'CURVE'
    obj: bpy.props.PointerProperty(type=bpy.types.Object, poll=_poll,
                                   name="Guide Spline", update=live.touch_prop)


def _prep_cache_touch(self, context):
    if not self.perf_prep_cache:
        from . import input_prep
        input_prep.clear_prep_cache(self.id_data)
    live.touch_prop(self, context)


def _background_compute_touch(self, context):
    from . import background_compute
    background_compute.setting_changed(self,context)


# здесь хранятся значения, которые сохраняются вместе с объектом
class ProjectDamageProps(bpy.types.PropertyGroup):
    paper_generator_enable: bpy.props.BoolProperty(
        name='Paper Generator', default=False, update=live.touch_prop,
        description='Generate an editable paper sheet attached to this Project Damage object')
    wallhole_enable: bpy.props.BoolProperty(name='Wall Hole (Bricks)',default=False,update=live.touch_prop)
    background_compute: bpy.props.BoolProperty(name='Background Calculation', default=False,
        update=_background_compute_touch,
        description='Defer geometry work to one low-priority worker. Keep using the previous preview until the new geometry is ready. Blender still uploads the finished mesh on its main thread')
    ivy_enable: bpy.props.BoolProperty(name='Ivy Generator', default=False)
    roots_enable: bpy.props.BoolProperty(name='Roots Generator', default=False)
    perf_prep_cache: bpy.props.BoolProperty(
        name='Mesh Prep Cache', default=False, update=_prep_cache_touch,
        description='Reuse prepared geometry while mesh coordinates, topology, triangulation, unit scale, preparation settings and backend are unchanged. Disable to clear this source cache and prepare every update. Limited to 8 sources / 256 MiB')
    perf_mesh_preparation: bpy.props.BoolProperty(
        name='Mesh Preparation', update=live.touch_prop,
        get=lambda self: bool(self.get('perf_mesh_preparation', self.get('performance_optimization', False))),
        set=lambda self, value: self.__setitem__('perf_mesh_preparation', bool(value)),
        description='Optimized orientation, local-piece solidification and duplicate restoration. Disable to use original mesh preparation; no quality reduction')
    perf_ngon_restore: bpy.props.BoolProperty(
        name='N-gon Restore', update=live.touch_prop,
        get=lambda self: bool(self.get('perf_ngon_restore', self.get('performance_optimization', False))),
        set=lambda self, value: self.__setitem__('perf_ngon_restore', bool(value)),
        description='Restore untouched n-gons in independent batches instead of one operator per n-gon; preserves UVs and materials')
    performance_optimization: bpy.props.BoolProperty(
        name='Performance Optimization', default=False, update=live.touch_prop,
        description='Use optimized multi-piece mesh preparation and batched n-gon restoration for this source. Disable to use the original algorithms. Does not lower quality or change effect settings')
    # стеклянные пробои и трещины
    glass_drawn_enable: bpy.props.BoolProperty(name='Broken Glass Drawn', default=False, update=live.touch_prop, description='Draw a closed region with Grease Pencil (Broken Glass > Holes): there is no glass inside it, and the impact cracks run from its rim across the whole pane. Shares the Broken Glass settings')
    glass_drawn_reach: bpy.props.FloatProperty(name='Crack Reach', default=8.0, min=0.5, soft_max=20.0, update=live.touch_prop, description='How far the radial cracks run from the drawn hole, x its radius (8 = across a pane)')
    glass_hit_enable: bpy.props.BoolProperty(name='Hit Glass', default=False, update=live.touch_prop, description='Bullet / stone hits in glass at the empties of the Hit Glass collection: hole, radial cracks and rings, like Broken Glass impacts. Shares the Broken Glass settings')
    glass_hit_collection: bpy.props.PointerProperty(name='Hit Empties', type=bpy.types.Collection, update=live.touch_prop, description='Every empty in this collection is a hit on the nearest glass surface')
    # бетонные повреждения и арматура
    concrete_v2_enable: bpy.props.BoolProperty(name='Concrete Damage V2', default=False, update=live.touch_prop, description='Concrete Damage with a smooth profile: one bowl easing into the surface, no steep walls or ledges. Shares the Concrete Damage settings')
    # дранка и деревянный каркас
    lath_v2_enable: bpy.props.BoolProperty(name='Lath and Plaster V2', default=False, update=live.touch_prop, description='Lath and Plaster with a smooth profile: the broken plaster eases into the surface, no steep walls or ledges. Shares the Lath and Plaster settings')
    lath_profile: bpy.props.IntProperty(name='Profile', default=0, options={'HIDDEN'}, description='internal: set from the Lath V2 tile')
    lath_riven_enable: bpy.props.BoolProperty(name='Riven Lath', default=False, update=live.touch_prop, description='Hand-split lath nailed to studs: wavy, uneven boards, joints on the studs, snapped boards hanging in holes. Shares the Lath and Plaster settings')
    lath_framing_enable: bpy.props.BoolProperty(name='Stud Framing', default=False, update=live.touch_prop, description='Drywall or plaster skin broken away over a timber stud frame: studs, plates, noggins and nails, snapped members in holes. No laths. Shares the outline, plaster, hole and crack settings of Lath and Plaster')
    lath_style: bpy.props.IntProperty(name='Style', default=0, options={'HIDDEN'}, description='internal: set from the Riven Lath and Stud Framing tiles')
    lath_smooth_width: bpy.props.FloatProperty(name='Smooth Slope', default=0.75, min=0.05, max=1.0, update=live.touch_prop, description='V2: share of the patch radius the slope spans - larger = gentler transition into the surface')
    # кирпичная кладка
    brick_v2_enable: bpy.props.BoolProperty(name='Brick Damage V2', default=False, update=live.touch_prop, description='Brick Damage with a smooth profile: the render eases into the surface, no sharp edge, steep walls or ledges. Shares the Brick Damage settings')
    brick_profile: bpy.props.IntProperty(name='Profile', default=0, options={'HIDDEN'}, description='internal: set from the Brick V2 tile')
    brick_smooth_width: bpy.props.FloatProperty(name='Smooth Slope', default=0.75, min=0.05, max=1.0, update=live.touch_prop, description='V2: share of the patch radius the slope spans - larger = gentler transition into the surface')
    concrete_profile: bpy.props.IntProperty(name='Profile', default=0, options={'HIDDEN'}, description='internal: set from the Concrete Damage V2 tile')
    concrete_smooth_width: bpy.props.FloatProperty(name='Smooth Slope', default=0.75, min=0.05, max=1.0, update=live.touch_prop, description='V2: share of the patch radius the slope spans - larger = gentler transition into the surface')
    ui_section: bpy.props.EnumProperty(name='Settings',items=(('GEOMETRY','Geometry','Geometry and drawing controls'),('MATERIAL','Material','Assign materials to generated regions')),default='GEOMETRY')
    """One instance per mesh object (Object.project_damage), like the C4D generator's
    own container. Building never writes here - the source object and its
    settings survive every Update."""


                                                                                      
    # разрывы ткани и нити
    fabric_skin_depth: bpy.props.FloatProperty(name='Skin Depth', default=0.0, description='; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=1.0)
    fabric_paint_holes_enable: bpy.props.BoolProperty(name='Draw Holes', default=True, description='Cut out the inside of closed Grease Pencil contours. Works in Classic and Solidify', update=live.touch_prop)
    fabric_fibre_model: bpy.props.EnumProperty(name='Fabric Fibres', items=(('LOOSE','Loose Fibres','Previous algorithm: independent fringe and hanging strands'),('WOVEN','Woven Fibres','New algorithm: connected warp and weft fibres follow the tear outline')), default='LOOSE', update=live.touch_prop)
    fabric_paint_edges_enable: bpy.props.BoolProperty(name='Draw Torn Edges', default=True, description='Cut a closed region, optionally across the cloth border. Solidify adds ragged slits along the drawn rim', update=live.touch_prop)
    fabric_paint_only: bpy.props.BoolProperty(name='Draw Only', default=False, description='Use only the drawn contours: no automatic holes or border tears. An empty drawing leaves the cloth intact. Turn off to combine automatic and drawn tears', update=live.touch_prop)
    fabric_paint_jagged: bpy.props.FloatProperty(name='Paint Jagged', default=0.5, description='rim raggedness, x paintRagSize', update=live.touch_prop, soft_min=0.0, soft_max=2.0)
    fabric_paint_rag_size: bpy.props.FloatProperty(name='Paint Rag Size', default=1.2, description='feature size of the ragged rim (scene units); centimetres', update=live.touch_prop, soft_min=0.0, soft_max=4.8)
    fabric_paint_edge_spacing: bpy.props.FloatProperty(name='Paint Edge Spacing', default=1.8, description='average distance between slits along a drawn torn edge; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=7.2)
    fabric_paint_edge_length_min: bpy.props.FloatProperty(name='Paint Edge Length Min', default=3.0, description='; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=12.0)
    fabric_paint_edge_length_max: bpy.props.FloatProperty(name='Paint Edge Length Max', default=14.0, description='; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=56.0)
    fabric_paint_edge_width_min: bpy.props.FloatProperty(name='Paint Edge Width Min', default=0.3, description='; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=1.2)
    fabric_paint_edge_width_max: bpy.props.FloatProperty(name='Paint Edge Width Max', default=1.6, description='; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=6.4)
    fabric_paint_hang_spacing: bpy.props.FloatProperty(name='Paint Hang Spacing', default=0.8, description='hanging threads along a drawn torn edge (needs hangEnable); centimetres', update=live.touch_prop, soft_min=0.0, soft_max=3.2)
    hit_enable: bpy.props.BoolProperty(name='Enable', default=False, description='', update=live.touch_prop)
    hit_seed: bpy.props.IntProperty(name='Seed', default=0, description='mixed with ProjectDamageParams::seed, like heatSeed/coatingSeed', update=live.touch_prop, min=0)
    hit_avoid: bpy.props.BoolProperty(name='Avoid', default=True, description='honour CoreInput::avoidPoly', update=live.touch_prop)
    hit_detail: bpy.props.FloatProperty(name='Detail', default=1.0, description='tessellation of the crater grid, plates and cracks (0.25..4)', update=live.touch_prop, soft_min=0.0, soft_max=4.0, min=0.05, max=4.0)
    hit_aim: bpy.props.EnumProperty(name='Aim', items=(('0', 'Nearest Surface', ''), ('1', 'Arrow +Z', '')), default='0', update=live.touch_prop)
    hit_radius_scale: bpy.props.FloatProperty(name='Radius Scale', default=1.0, description="multiplies every empty's radius", update=live.touch_prop, soft_min=0.0, soft_max=4.0)
    hit_proximity: bpy.props.FloatProperty(name='Proximity', default=1.5, description='reach of the falloff, x radius: an empty this far', update=live.touch_prop, soft_min=0.0, soft_max=6.0)
    hit_prox_shrink: bpy.props.FloatProperty(name='Prox Shrink', default=0.6, description='0..1 how much a weak (far) hit also shrinks, not just shallows', update=live.touch_prop, soft_min=0.0, soft_max=2.4)
    hit_oblique: bpy.props.FloatProperty(name='Oblique', default=0.5, description='aim == 1: stretch of the hit along the shot direction', update=live.touch_prop, soft_min=0.0, soft_max=2.0)
    hit_layer_thickness: bpy.props.FloatProperty(name='Layer Thickness', default=0.5, description='depth of the shattered skin; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=2.0)
    hit_core_size: bpy.props.FloatProperty(name='Core Size', default=0.32, description='x radius: the crushed centre', update=live.touch_prop, soft_min=0.0, soft_max=1.28)
    hit_core_depth: bpy.props.FloatProperty(name='Core Depth', default=0.12, description='x radius: crater depth below the skin', update=live.touch_prop, soft_min=0.0, soft_max=1.0)
    hit_core_jagged: bpy.props.FloatProperty(name='Core Jagged', default=0.7, description='ragged crater rim (0 = smooth ellipse)', update=live.touch_prop, soft_min=0.0, soft_max=2.8)
    hit_roughness: bpy.props.FloatProperty(name='Roughness', default=0.7, description='pitted aggregate floor (0 = smooth)', update=live.touch_prop, soft_min=0.0, soft_max=2.8)
    hit_aggregate_size: bpy.props.FloatProperty(name='Aggregate Size', default=0.6, description='size of the floor pits; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=2.4)
    hit_thickness_cap: bpy.props.FloatProperty(name='Thickness Cap', default=0.45, description='max crater depth / local wall thickness (0 = off)', update=live.touch_prop, soft_min=0.0, soft_max=1.8)
    hit_hole_enable: bpy.props.BoolProperty(name='Hole Enable', default=False, description='punch through the wall', update=live.touch_prop)
    hit_hole_size: bpy.props.FloatProperty(name='Hole Size', default=0.45, description='share of the crater that is open', update=live.touch_prop, soft_min=0.0, soft_max=1.8)
    hit_plate_size: bpy.props.FloatProperty(name='Plate Size', default=0.5, description='x radius: typical plate diameter at the rim', update=live.touch_prop, soft_min=0.0, soft_max=2.0)
    hit_plate_growth: bpy.props.FloatProperty(name='Plate Growth', default=0.5, description='0..1 plates get smaller toward the centre', update=live.touch_prop, soft_min=0.0, soft_max=2.0)
    hit_zone_jagged: bpy.props.FloatProperty(name='Zone Jagged', default=0.35, description='irregular outer outline of the shatter zone', update=live.touch_prop, soft_min=0.0, soft_max=1.4)
    hit_shatter: bpy.props.FloatProperty(name='Shatter', default=0.55, description='0..1 share of plates knocked out (more near the crater)', update=live.touch_prop, soft_min=0.0, soft_max=2.2)
    hit_tilt: bpy.props.FloatProperty(name='Tilt', default=2.0, description='remaining plates pushed in, x layerThickness (0 = flat)', update=live.touch_prop, soft_min=0.0, soft_max=8.0)
    hit_edge_jagged: bpy.props.FloatProperty(name='Edge Jagged', default=0.5, description='wobble of plate edges and crack lines', update=live.touch_prop, soft_min=0.0, soft_max=2.0)
    hit_chip: bpy.props.FloatProperty(name='Chip', default=0.4, description='0..1 small bites out of the plate rims', update=live.touch_prop, soft_min=0.0, soft_max=1.6)
    hit_crack_enable: bpy.props.BoolProperty(name='Crack Enable', default=True, description='', update=live.touch_prop)
    hit_crack_width: bpy.props.FloatProperty(name='Crack Width', default=0.22, description='opening at the surface; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=1.0)
    hit_crack_depth: bpy.props.FloatProperty(name='Crack Depth', default=0.6, description='depth below the plate surface; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=2.4)
    hit_radial_count: bpy.props.IntProperty(name='Radial Count', default=6, description='radial cracks running out of the shatter zone', update=live.touch_prop)
    hit_radial_length_min: bpy.props.FloatProperty(name='Radial Length Min', default=0.35, description='x radius', update=live.touch_prop, soft_min=0.0, soft_max=1.4)
    hit_radial_length_max: bpy.props.FloatProperty(name='Radial Length Max', default=1.1, description='x radius', update=live.touch_prop, soft_min=0.0, soft_max=4.4)
    hit_radial_meander: bpy.props.FloatProperty(name='Radial Meander', default=0.45, description='0 = straight', update=live.touch_prop, soft_min=0.0, soft_max=1.8)
    hit_radial_branch: bpy.props.FloatProperty(name='Radial Branch', default=0.35, description='chance of one fork per radial crack', update=live.touch_prop, soft_min=0.0, soft_max=1.4)
    hit_ring_count: bpy.props.IntProperty(name='Ring Count', default=1, description='concentric crack rings outside the zone', update=live.touch_prop)
    hit_ring_fill: bpy.props.FloatProperty(name='Ring Fill', default=0.45, description='0..1 share of each ring that is cracked', update=live.touch_prop, soft_min=0.0, soft_max=1.8)
    # рельеф штукатурки
    plaster_enable: bpy.props.BoolProperty(name='Enable', default=False, description='', update=live.touch_prop)
    plaster_seed: bpy.props.IntProperty(name='Seed', default=0, description='mixed with ProjectDamageParams::seed, like heatSeed/coatingSeed', update=live.touch_prop, min=0)
    plaster_avoid: bpy.props.BoolProperty(name='Avoid', default=True, description='honour CoreInput::avoidPoly (no relief there)', update=live.touch_prop)
    plaster_edge_length: bpy.props.FloatProperty(name='Edge Length', default=0.35, description='target edge length after subdivision; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=1.4)
    plaster_max_tris: bpy.props.IntProperty(name='Max Tris', default=1500000, description='triangle budget of the subdivided mesh', update=live.touch_prop, min=0)
    plaster_detail: bpy.props.FloatProperty(name='Detail', default=1.0, description='multiplies the resolution (2 = half edge length)', update=live.touch_prop, soft_min=0.0, soft_max=4.0, min=0.05, max=4.0)
    plaster_strength: bpy.props.FloatProperty(name='Strength', default=0.6, description='relief height: lowest trowel level to highest; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=2.4)
    plaster_sink: bpy.props.FloatProperty(name='Sink', default=0.5, description='0 = relief only builds outward, 1 = only inward', update=live.touch_prop, soft_min=0.0, soft_max=2.0)
    plaster_thickness_cap: bpy.props.FloatProperty(name='Thickness Cap', default=0.2, description='max |displacement| / local wall thickness (0 = off)', update=live.touch_prop, soft_min=0.0, soft_max=1.0)
    plaster_undulation: bpy.props.FloatProperty(name='Undulation', default=0.25, description='broad waviness under the strokes (x strength)', update=live.touch_prop, soft_min=0.0, soft_max=1.0)
    plaster_undulation_scale: bpy.props.FloatProperty(name='Undulation Scale', default=45.0, description='feature size of that waviness; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=180.0)
    plaster_coats: bpy.props.IntProperty(name='Coats', default=3, description='0..6 layers of strokes (0 = no strokes)', update=live.touch_prop)
    plaster_coverage: bpy.props.FloatProperty(name='Coverage', default=1.1, description='stroke area per coat / surface area', update=live.touch_prop, soft_min=0.0, soft_max=4.4)
    plaster_length_min: bpy.props.FloatProperty(name='Length Min', default=10.0, description='stroke length range (along the trowel motion); centimetres', update=live.touch_prop, soft_min=0.0, soft_max=40.0)
    plaster_length_max: bpy.props.FloatProperty(name='Length Max', default=28.0, description='; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=112.0)
    plaster_width_min: bpy.props.FloatProperty(name='Width Min', default=5.0, description='stroke width range (the trowel blade); centimetres', update=live.touch_prop, soft_min=0.0, soft_max=20.0)
    plaster_width_max: bpy.props.FloatProperty(name='Width Max', default=12.0, description='; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=48.0)
    plaster_angle: bpy.props.FloatProperty(name='Angle', default=0.0, description='stroke direction, degrees from horizontal (about the normal)', update=live.touch_prop, soft_min=0.0, soft_max=1.0)
    plaster_angle_random: bpy.props.FloatProperty(name='Angle Random', default=0.3, description='0..1 direction jitter (1 = +-90 deg)', update=live.touch_prop, soft_min=0.0, soft_max=1.2)
    plaster_cross_chance: bpy.props.FloatProperty(name='Cross Chance', default=0.25, description='share of strokes turned 90 deg (cross-hatched trowelling)', update=live.touch_prop, soft_min=0.0, soft_max=1.0)
    plaster_curve: bpy.props.FloatProperty(name='Curve', default=0.3, description='0..1 how much a stroke bends into an arc', update=live.touch_prop, soft_min=0.0, soft_max=1.2)
    plaster_flatten: bpy.props.FloatProperty(name='Flatten', default=0.85, description='0..1 how hard a stroke levels the plaster below it', update=live.touch_prop, soft_min=0.0, soft_max=3.4)
    plaster_tilt: bpy.props.FloatProperty(name='Tilt', default=0.35, description='trowel pitch: level change along a stroke (x strength)', update=live.touch_prop, soft_min=0.0, soft_max=1.4)
    plaster_jagged: bpy.props.FloatProperty(name='Jagged', default=0.5, description='ragged stroke outline (0 = clean rounded rectangle)', update=live.touch_prop, soft_min=0.0, soft_max=2.0)
    plaster_streaks: bpy.props.FloatProperty(name='Streaks', default=0.3, description='drag striations along the stroke (x strength)', update=live.touch_prop, soft_min=0.0, soft_max=1.2)
    plaster_ridge_height: bpy.props.FloatProperty(name='Ridge Height', default=0.45, description='x strength', update=live.touch_prop, soft_min=0.0, soft_max=1.8)
    plaster_ridge_width: bpy.props.FloatProperty(name='Ridge Width', default=0.6, description='how far the lip spreads past the rim; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=2.4)
    plaster_ridge_sharp: bpy.props.FloatProperty(name='Ridge Sharp', default=0.7, description='0 = soft rounded bead, 1 = crisp knife-cut lip', update=live.touch_prop, soft_min=0.0, soft_max=2.8)
    plaster_ridge_trail: bpy.props.FloatProperty(name='Ridge Trail', default=0.85, description='0 = even all round, 1 = lip only at the trailing end', update=live.touch_prop, soft_min=0.0, soft_max=3.4)
    plaster_grain: bpy.props.FloatProperty(name='Grain', default=0.04, description='fine surface roughness height; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=1.0)
    plaster_grain_size: bpy.props.FloatProperty(name='Grain Size', default=0.3, description='feature size (faded out below ~2 edge lengths); centimetres', update=live.touch_prop, soft_min=0.0, soft_max=1.2)
    plaster_pits: bpy.props.FloatProperty(name='Pits', default=0.3, description='0..1 share of small pits / air holes in the grain', update=live.touch_prop, soft_min=0.0, soft_max=1.2)
    plaster_crack_enable: bpy.props.BoolProperty(name='Crack Enable', default=True, description='', update=live.touch_prop)
    plaster_crack_amount: bpy.props.FloatProperty(name='Crack Amount', default=0.45, description='0..1 share of network borders that are cracked', update=live.touch_prop, soft_min=0.0, soft_max=1.8)
    plaster_crack_scale: bpy.props.FloatProperty(name='Crack Scale', default=40.0, description='typical crack cell size; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=160.0)
    plaster_crack_width: bpy.props.FloatProperty(name='Crack Width', default=0.3, description='opening at the surface (at least ~1.5 edge lengths shows); centimetres', update=live.touch_prop, soft_min=0.0, soft_max=1.2)
    plaster_crack_depth: bpy.props.FloatProperty(name='Crack Depth', default=0.25, description='; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=1.0)
    plaster_crack_meander: bpy.props.FloatProperty(name='Crack Meander', default=0.5, description='0 = straight borders', update=live.touch_prop, soft_min=0.0, soft_max=2.0)
    plaster_crack_breakup: bpy.props.FloatProperty(name='Crack Breakup', default=0.4, description='0..1 how often a crack fades out along its run', update=live.touch_prop, soft_min=0.0, soft_max=1.6)
    plaster_crack_step: bpy.props.FloatProperty(name='Crack Step', default=0.06, description='height offset between the two sides of a crack; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=1.0)
    plaster_crack_lip: bpy.props.FloatProperty(name='Crack Lip', default=0.4, description='raised, smeared filler bead beside the crack (x depth)', update=live.touch_prop, soft_min=0.0, soft_max=1.6)
    plaster_up_x: bpy.props.FloatProperty(name='Up X', default=0.0, description='', update=live.touch_prop, soft_min=0.0, soft_max=1.0)
    plaster_up_y: bpy.props.FloatProperty(name='Up Y', default=0.0, description='', update=live.touch_prop, soft_min=0.0, soft_max=1.0)
    plaster_up_z: bpy.props.FloatProperty(name='Up Z', default=1.0, description='', update=live.touch_prop, soft_min=0.0, soft_max=4.0)
    wood_enable: bpy.props.BoolProperty(name='Enable', default=False, description='', update=live.touch_prop)
    wood_seed: bpy.props.IntProperty(name='Seed', default=0, description='mixed with ProjectDamageParams::seed, like heatSeed/coatingSeed', update=live.touch_prop, min=0)
    wood_mode: bpy.props.EnumProperty(name='Mode', items=(('0', 'Automatic', ''), ('1', 'Paint', '')), default='0', update=live.touch_prop)
    wood_avoid: bpy.props.BoolProperty(name='Avoid', default=True, description='honour CoreInput::avoidPoly', update=live.touch_prop)
    wood_detail: bpy.props.FloatProperty(name='Detail', default=1.0, description='cutter grid resolution (0.25..4)', update=live.touch_prop, soft_min=0.0, soft_max=4.0, min=0.05, max=4.0)
    wood_grain_mode: bpy.props.EnumProperty(name='Grain Mode', items=(('0', 'Automatic per Part', ''), ('1', 'Fixed Axis', '')), default='0', update=live.touch_prop)
    wood_grain_x: bpy.props.FloatProperty(name='Grain X', default=0.0, description='', update=live.touch_prop, soft_min=0.0, soft_max=1.0)
    wood_grain_y: bpy.props.FloatProperty(name='Grain Y', default=0.0, description='', update=live.touch_prop, soft_min=0.0, soft_max=1.0)
    wood_grain_z: bpy.props.FloatProperty(name='Grain Z', default=1.0, description='', update=live.touch_prop, soft_min=0.0, soft_max=4.0)
    wood_amount: bpy.props.IntProperty(name='Amount', default=4, description='exact number of torn patches (fewer only if no room)', update=live.touch_prop, min=0)
    wood_size_min: bpy.props.FloatProperty(name='Size Min', default=6.0, description='patch half-width range (across the grain); centimetres', update=live.touch_prop, soft_min=0.0, soft_max=24.0)
    wood_size_max: bpy.props.FloatProperty(name='Size Max', default=14.0, description='; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=56.0)
    wood_lobes: bpy.props.IntProperty(name='Lobes', default=3, description='1..8 overlapping blobs per patch', update=live.touch_prop)
    wood_stretch: bpy.props.FloatProperty(name='Stretch', default=2.5, description='elongation ALONG the grain (1 = round)', update=live.touch_prop, soft_min=0.0, soft_max=10.0)
    wood_end_bias: bpy.props.FloatProperty(name='End Bias', default=0.5, description='0..1 pull toward plank ends (edges across the grain)', update=live.touch_prop, soft_min=0.0, soft_max=2.0)
    wood_edge_bias: bpy.props.FloatProperty(name='Edge Bias', default=0.2, description='0..1 pull toward any sharp edge (plank sides)', update=live.touch_prop, soft_min=0.0, soft_max=1.0)
    wood_paint_radius: bpy.props.FloatProperty(name='Paint Radius', default=4.0, description='half-width of the damage along an open stroke; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=16.0)
    wood_paint_snap: bpy.props.FloatProperty(name='Paint Snap', default=5.0, description='max distance of a stroke point from the surface; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=20.0)
    wood_paint_close: bpy.props.FloatProperty(name='Paint Close', default=0.15, description='auto-close gap, x stroke length (0 = only cyclic strokes)', update=live.touch_prop, soft_min=0.0, soft_max=1.0)
    wood_depth_min: bpy.props.FloatProperty(name='Depth Min', default=0.6, description='depth of the deepest fibre layer, range; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=2.4)
    wood_depth_max: bpy.props.FloatProperty(name='Depth Max', default=1.6, description='; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=6.4)
    wood_layers: bpy.props.IntProperty(name='Layers', default=3, description='1..6 stepped fibre layers, each with its own splintered end', update=live.touch_prop)
    wood_layer_inset: bpy.props.FloatProperty(name='Layer Inset', default=0.6, description='how far each deeper layer starts behind the one above (x splinter length)', update=live.touch_prop, soft_min=0.0, soft_max=2.4)
    wood_fiber_depth_var: bpy.props.FloatProperty(name='Fiber Depth Var', default=0.5, description='0..1 depth variation between neighbouring fibre strips', update=live.touch_prop, soft_min=0.0, soft_max=2.0)
    wood_thickness_cap: bpy.props.FloatProperty(name='Thickness Cap', default=0.6, description='max depth / local board thickness (0 = off)', update=live.touch_prop, soft_min=0.0, soft_max=2.4)
    wood_jagged: bpy.props.FloatProperty(name='Jagged', default=0.5, description='ragged outline noise (0 = clean)', update=live.touch_prop, soft_min=0.0, soft_max=2.0)
    wood_fiber_width: bpy.props.FloatProperty(name='Fiber Width', default=1.6, description='width of one fibre strip across the grain; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=6.4)
    wood_fiber_jitter: bpy.props.FloatProperty(name='Fiber Jitter', default=0.7, description='0..1 variation of the strip widths', update=live.touch_prop, soft_min=0.0, soft_max=2.8)
    wood_splinter_length: bpy.props.FloatProperty(name='Splinter Length', default=6.0, description='how far the pointed ends reach along the grain; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=24.0)
    wood_splinter_sharp: bpy.props.FloatProperty(name='Splinter Sharp', default=0.8, description='0 = blunt square ends, 1 = needle points', update=live.touch_prop, soft_min=0.0, soft_max=3.2)
    wood_splinter_chunk: bpy.props.FloatProperty(name='Splinter Chunk', default=0.5, description='0..1 neighbouring strips break off together in wider chunks', update=live.touch_prop, soft_min=0.0, soft_max=2.0)
    wood_splinter_ramp: bpy.props.FloatProperty(name='Splinter Ramp', default=0.5, description='0..1 fibres peel up gradually toward their tips', update=live.touch_prop, soft_min=0.0, soft_max=2.0)
    wood_floor_fibers: bpy.props.FloatProperty(name='Floor Fibers', default=0.5, description='0..1 fibrous ridges on the torn floor', update=live.touch_prop, soft_min=0.0, soft_max=2.0)
    wood_groove_enable: bpy.props.BoolProperty(name='Groove Enable', default=True, description='', update=live.touch_prop)
    wood_groove_spacing: bpy.props.FloatProperty(name='Groove Spacing', default=0.45, description='distance between growth-ring grooves; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=1.8)
    wood_groove_depth: bpy.props.FloatProperty(name='Groove Depth', default=0.12, description='groove depth; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=1.0)
    wood_groove_wave: bpy.props.FloatProperty(name='Groove Wave', default=0.5, description='waviness of the grain lines (0 = straight)', update=live.touch_prop, soft_min=0.0, soft_max=2.0)
    wood_groove_breakup: bpy.props.FloatProperty(name='Groove Breakup', default=0.5, description='0..1 grooves fade in and out along their length', update=live.touch_prop, soft_min=0.0, soft_max=2.0)
    wood_weather_halo: bpy.props.FloatProperty(name='Weather Halo', default=0.5, description='halo width around each tear, x patch size (0 = only inside)', update=live.touch_prop, soft_min=0.0, soft_max=2.0)
    wood_break_enable: bpy.props.BoolProperty(name='Break Enable', default=True, description='', update=live.touch_prop)
    wood_break_chance: bpy.props.FloatProperty(name='Break Chance', default=0.35, description='share of patches (or painted strokes) that break through', update=live.touch_prop, soft_min=0.0, soft_max=1.4)
    wood_break_size: bpy.props.FloatProperty(name='Break Size', default=0.5, description='0..1 share of the tear that is gone through the board', update=live.touch_prop, soft_min=0.0, soft_max=2.0)
    wood_check_enable: bpy.props.BoolProperty(name='Check Enable', default=True, description='', update=live.touch_prop)
    wood_check_amount: bpy.props.IntProperty(name='Check Amount', default=3, description='splits per patch, running out of the tear along the grain', update=live.touch_prop)
    wood_check_global: bpy.props.IntProperty(name='Check Global', default=6, description='extra splits over the whole surface (independent of patches)', update=live.touch_prop)
    wood_check_length_min: bpy.props.FloatProperty(name='Check Length Min', default=8.0, description='; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=32.0)
    wood_check_length_max: bpy.props.FloatProperty(name='Check Length Max', default=35.0, description='; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=140.0)
    wood_check_width: bpy.props.FloatProperty(name='Check Width', default=0.25, description='opening at the surface; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=1.0)
    wood_check_depth: bpy.props.FloatProperty(name='Check Depth', default=0.8, description='; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=3.2)
    wood_check_meander: bpy.props.FloatProperty(name='Check Meander', default=0.15, description='0 = dead straight along the grain', update=live.touch_prop, soft_min=0.0, soft_max=1.0)
    wood_knot_amount: bpy.props.IntProperty(name='Knot Amount', default=2, description='knot holes over the whole surface', update=live.touch_prop)
    wood_knot_size: bpy.props.FloatProperty(name='Knot Size', default=1.2, description='radius across the grain; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=4.8)
    wood_knot_stretch: bpy.props.FloatProperty(name='Knot Stretch', default=1.4, description='elongation along the grain', update=live.touch_prop, soft_min=0.0, soft_max=5.6)
    wood_knot_depth: bpy.props.FloatProperty(name='Knot Depth', default=1.0, description='; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=4.0)
    wood_knot_checks: bpy.props.IntProperty(name='Knot Checks', default=2, description='short splits out of each knot', update=live.touch_prop)
    wood_sliver_enable: bpy.props.BoolProperty(name='Sliver Enable', default=True, description='', update=live.touch_prop)
    wood_sliver_amount: bpy.props.FloatProperty(name='Sliver Amount', default=0.3, description='0..1 share of fibre strips that keep a sliver', update=live.touch_prop, soft_min=0.0, soft_max=1.2)
    wood_sliver_length: bpy.props.FloatProperty(name='Sliver Length', default=4.0, description='; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=16.0)
    wood_sliver_width: bpy.props.FloatProperty(name='Sliver Width', default=0.5, description='capped by the strip width; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=2.0)
    wood_sliver_thick: bpy.props.FloatProperty(name='Sliver Thick', default=0.18, description='; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=1.0)
    wood_sliver_lift: bpy.props.FloatProperty(name='Sliver Lift', default=0.35, description='curl of the tip out of the surface (x length); centimetres', update=live.touch_prop, soft_min=0.0, soft_max=1.4)
    wood_sliver_embed: bpy.props.FloatProperty(name='Sliver Embed', default=0.8, description='how far the root continues into intact wood; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=3.2)
    lath_enable: bpy.props.BoolProperty(name='Enable', default=False, description='', update=live.touch_prop)
    lath_seed: bpy.props.IntProperty(name='Seed', default=0, description='mixed with ProjectDamageParams::seed, like heatSeed/coatingSeed', update=live.touch_prop, min=0)
    lath_mode: bpy.props.EnumProperty(name='Mode', items=(('0', 'Automatic', ''), ('1', 'Paint', '')), default='0', update=live.touch_prop)
    lath_avoid: bpy.props.BoolProperty(name='Avoid', default=True, description='honour CoreInput::avoidPoly', update=live.touch_prop)
    lath_detail: bpy.props.FloatProperty(name='Detail', default=1.0, description='cutter grid resolution (0.25..4)', update=live.touch_prop, soft_min=0.0, soft_max=4.0, min=0.05, max=4.0)
    lath_amount: bpy.props.IntProperty(name='Amount', default=3, description='exact number of patches (fewer only if no room)', update=live.touch_prop, min=0)
    lath_size_min: bpy.props.FloatProperty(name='Size Min', default=20.0, description='patch radius range; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=80.0)
    lath_size_max: bpy.props.FloatProperty(name='Size Max', default=40.0, description='; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=160.0)
    lath_lobes: bpy.props.IntProperty(name='Lobes', default=4, description='1..8 overlapping blobs per patch -> cloud-like outlines', update=live.touch_prop)
    lath_stretch: bpy.props.FloatProperty(name='Stretch', default=1.8, description='max elongation of a patch (1 = round)', update=live.touch_prop, soft_min=0.0, soft_max=7.2)
    lath_edge_bias: bpy.props.FloatProperty(name='Edge Bias', default=0.1, description='0..1 pull of patches toward sharp object edges/corners', update=live.touch_prop, soft_min=0.0, soft_max=1.0)
    lath_paint_radius: bpy.props.FloatProperty(name='Paint Radius', default=8.0, description='half-width of the damage along an open stroke; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=32.0)
    lath_paint_snap: bpy.props.FloatProperty(name='Paint Snap', default=5.0, description='max distance of a stroke point from the surface; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=20.0)
    lath_paint_close: bpy.props.FloatProperty(name='Paint Close', default=0.15, description='auto-close gap, x stroke length (0 = only cyclic strokes)', update=live.touch_prop, soft_min=0.0, soft_max=1.0)
    lath_plaster_min: bpy.props.FloatProperty(name='Plaster Min / Laths Depth Min', default=1.5, description='plaster thickness range (per patch / stroke); centimetres', update=live.touch_prop, soft_min=0.0, soft_max=6.0)
    lath_plaster_max: bpy.props.FloatProperty(name='Plaster Max / Laths Depth Max', default=2.5, description='Upper depth of the wooden layer below the wall surface, in centimetres', update=live.touch_prop, soft_min=0.0, soft_max=10.0)
    lath_jagged: bpy.props.FloatProperty(name='Jagged', default=0.6, description='large-scale ragged outline (0 = smooth)', update=live.touch_prop, soft_min=0.0, soft_max=2.4)
    lath_outline_scale: bpy.props.FloatProperty(name='Outline Scale', default=1.0, description='feature size of the outline noise (x patch size)', update=live.touch_prop, soft_min=0.0, soft_max=4.0)
    lath_crumble: bpy.props.FloatProperty(name='Crumble', default=0.6, description='fine, crumbly bite along the plaster rims (0 = clean)', update=live.touch_prop, soft_min=0.0, soft_max=2.4)
    lath_crumble_size: bpy.props.FloatProperty(name='Crumble Size', default=1.5, description='feature size of the crumble, in scene units (kept >= 2 grid cells); centimetres', update=live.touch_prop, soft_min=0.0, soft_max=6.0)
    lath_terraces: bpy.props.IntProperty(name='Terraces', default=2, description='1..4 stepped plaster coats (finish, brown, scratch ...)', update=live.touch_prop)
    lath_terrace_skin: bpy.props.FloatProperty(name='Terrace Skin', default=0.35, description='share of the plaster depth taken by the outer coat', update=live.touch_prop, soft_min=0.0, soft_max=1.4)
    lath_terrace_inset: bpy.props.FloatProperty(name='Terrace Inset', default=0.12, description='each deeper coat breaks back this far inside (x patch size)', update=live.touch_prop, soft_min=0.0, soft_max=1.0)
    lath_wall_soft: bpy.props.FloatProperty(name='Wall Soft', default=0.05, description='0 = sheer broken plaster walls, 1 = soft sloped walls', update=live.touch_prop, soft_min=0.0, soft_max=1.0)
    lath_roughness: bpy.props.FloatProperty(name='Roughness', default=0.4, description='uneven floor on the plaster terraces (0 = flat)', update=live.touch_prop, soft_min=0.0, soft_max=1.6)
    lath_thickness_cap: bpy.props.FloatProperty(name='Thickness Cap', default=0.7, description='max cut depth / local wall thickness, outside holes (0 = off)', update=live.touch_prop, soft_min=0.0, soft_max=2.8)
                                                                                                              
    brick_surf_mode: bpy.props.EnumProperty(name='Back Surface', items=(('0', 'Procedural', 'As built'), ('1', 'Texture', 'Image displacement on the finished faces behind the knocked-out bricks (back leaf, collar, sockets); their rim stays put')), default='0', update=live.touch_prop)
    brick_tex_path: bpy.props.StringProperty(name='Height Texture', subtype='FILE_PATH', description='PNG/JPG grayscale displacement. Empty uses the bundled Concrete025 gravel displacement', update=live.touch_prop)
    brick_tex_scale: bpy.props.FloatProperty(name='Texture Scale (cm)', default=20., min=.1, soft_max=200., update=live.touch_prop)
    brick_tex_strength: bpy.props.FloatProperty(name='Texture Depth (cm)', default=1., min=0., soft_max=10., description='Peak-to-peak displacement. 0 = procedural only. Narrow gaps fade it out', update=live.touch_prop)
    brick_tex_edge: bpy.props.FloatProperty(name='Mesh Edge Size (cm)', default=0., min=0., soft_max=5., description='Target triangle size on the backing. 0 uses Texture Scale and Back Surface Resolution. Larger values use fewer triangles', update=live.touch_prop)
    brick_tex_offset_x: bpy.props.FloatProperty(name='Offset X (cm)', default=0., update=live.touch_prop)
    brick_tex_offset_y: bpy.props.FloatProperty(name='Offset Y (cm)', default=0., update=live.touch_prop)
    brick_tex_offset_z: bpy.props.FloatProperty(name='Offset Z (cm)', default=0., update=live.touch_prop)
                                                                                                              
    lath_surf_mode: bpy.props.EnumProperty(name='Back Surface', items=(('0', 'Procedural', 'As built'), ('1', 'Texture', 'Image displacement on the area behind the laths, like the Concrete spall surface')), default='0', update=live.touch_prop)
    lath_tex_path: bpy.props.StringProperty(name='Height Texture', subtype='FILE_PATH', description='PNG/JPG grayscale displacement. Empty uses the bundled Concrete025 gravel displacement', update=live.touch_prop)
    lath_tex_scale: bpy.props.FloatProperty(name='Texture Scale (cm)', default=20., min=.1, soft_max=200., update=live.touch_prop)
    lath_tex_strength: bpy.props.FloatProperty(name='Texture Depth (cm)', default=1., min=0., soft_max=10., description='Peak-to-peak displacement. 0 = procedural only', update=live.touch_prop)
    lath_tex_edge: bpy.props.FloatProperty(name='Mesh Edge Size (cm)', default=0., min=0., soft_max=5., description='0 uses Texture Scale / 60', update=live.touch_prop)
    lath_tex_offset_x: bpy.props.FloatProperty(name='Offset X (cm)', default=0., update=live.touch_prop)
    lath_tex_offset_y: bpy.props.FloatProperty(name='Offset Y (cm)', default=0., update=live.touch_prop)
    lath_tex_offset_z: bpy.props.FloatProperty(name='Offset Z (cm)', default=0., update=live.touch_prop)
    lath_cavity_depth: bpy.props.FloatProperty(name='Cavity Depth', default=3.0, description='how deep the dark gaps go behind the back of the laths; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=12.0)
    lath_keys: bpy.props.FloatProperty(name='Keys', default=0.35, description='0..1 share of the gaps still filled with plaster keys', update=live.touch_prop, soft_min=0.0, soft_max=1.4)
    lath_key_size: bpy.props.FloatProperty(name='Key Size', default=2.5, description='feature size of the plaster keys; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=10.0)
    lath_hole_enable: bpy.props.BoolProperty(name='Hole Enable', default=False, description='', update=live.touch_prop)
    lath_hole_chance: bpy.props.FloatProperty(name='Hole Chance', default=0.6, description='share of patches (or painted strokes) that break through', update=live.touch_prop, soft_min=0.0, soft_max=2.4)
    lath_hole_size: bpy.props.FloatProperty(name='Hole Size', default=0.45, description='0..1 share of the patch interior that is open', update=live.touch_prop, soft_min=0.0, soft_max=1.8)
    lath_hole_jagged: bpy.props.FloatProperty(name='Hole Jagged', default=0.6, description='ragged hole rim', update=live.touch_prop, soft_min=0.0, soft_max=2.4)
    lath_hole_laths: bpy.props.FloatProperty(name='Hole Laths', default=0.15, description='0..1 share of laths that still bridge a hole; the rest snap at its rim', update=live.touch_prop, soft_min=0.0, soft_max=1.0)
    lath_crack_enable: bpy.props.BoolProperty(name='Crack Enable', default=True, description='', update=live.touch_prop)
    lath_crack_amount: bpy.props.IntProperty(name='Crack Amount', default=3, description='cracks per patch (painted: per stroke section)', update=live.touch_prop)
    lath_crack_length_min: bpy.props.FloatProperty(name='Crack Length Min', default=6.0, description='; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=24.0)
    lath_crack_length_max: bpy.props.FloatProperty(name='Crack Length Max', default=20.0, description='; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=80.0)
    lath_crack_width: bpy.props.FloatProperty(name='Crack Width', default=0.3, description='opening at the surface; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=1.2)
    lath_crack_depth: bpy.props.FloatProperty(name='Crack Depth', default=0.8, description='; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=3.2)
    lath_crack_meander: bpy.props.FloatProperty(name='Crack Meander', default=0.45, description='0 = straight', update=live.touch_prop, soft_min=0.0, soft_max=1.8)
    lath_crack_branch: bpy.props.FloatProperty(name='Crack Branch', default=0.35, description='chance of one fork per crack', update=live.touch_prop, soft_min=0.0, soft_max=1.4)
    lath_crack_interior: bpy.props.FloatProperty(name='Crack Interior', default=0.0, description='share of cracks that start inside the patch', update=live.touch_prop, soft_min=0.0, soft_max=1.0)
    lath_lath_enable: bpy.props.BoolProperty(name='Lath Enable', default=True, description='', update=live.touch_prop)
    lath_lath_amount: bpy.props.FloatProperty(name='Lath Amount', default=1.0, description='0..1 share of rows that carry a lath at all', update=live.touch_prop, soft_min=0.0, soft_max=4.0)
    lath_lath_width: bpy.props.FloatProperty(name='Lath Width', default=3.5, description='strip width (along up); centimetres', update=live.touch_prop, soft_min=0.0, soft_max=14.0)
    lath_lath_thickness: bpy.props.FloatProperty(name='Lath Thickness', default=0.8, description='strip thickness (into the wall); centimetres', update=live.touch_prop, soft_min=0.0, soft_max=3.2)
    lath_lath_gap: bpy.props.FloatProperty(name='Lath Gap', default=0.9, description='gap between neighbouring strips (spacing = width + gap); centimetres', update=live.touch_prop, soft_min=0.0, soft_max=3.6)
    lath_lath_scale: bpy.props.FloatProperty(name='Lath Scale', default=1.0, description='multiplies width, thickness, gap, length and embed', update=live.touch_prop, soft_min=0.0, soft_max=4.0)
    lath_lath_angle: bpy.props.FloatProperty(name='Lath Angle', default=0.0, description='rotation of the rows about the surface normal, degrees', update=live.touch_prop, soft_min=0.0, soft_max=1.0)
    lath_lath_length: bpy.props.FloatProperty(name='Lath Length', default=120.0, description='length of one strip; butt joints are staggered (0 = endless)', update=live.touch_prop, soft_min=0.0, soft_max=480.0)
    lath_lath_embed: bpy.props.FloatProperty(name='Lath Embed', default=6.0, description='how far laths continue under the intact plaster; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=24.0)
    lath_lath_jitter: bpy.props.FloatProperty(name='Lath Jitter', default=0.4, description='0..1 per-strip variation of offset, width and thickness', update=live.touch_prop, soft_min=0.0, soft_max=1.6)
    lath_lath_tilt: bpy.props.FloatProperty(name='Lath Tilt', default=0.6, description='max per-strip slope off the row direction, degrees', update=live.touch_prop, soft_min=0.0, soft_max=2.4)
    lath_lath_warp: bpy.props.FloatProperty(name='Lath Warp', default=0.25, description='waviness of a strip in and out, x thickness', update=live.touch_prop, soft_min=0.0, soft_max=1.0)
    lath_lath_edge: bpy.props.FloatProperty(name='Lath Edge', default=0.4, description='ragged, uneven strip edges (0 = sawn straight)', update=live.touch_prop, soft_min=0.0, soft_max=1.6)
    lath_lath_missing: bpy.props.FloatProperty(name='Lath Missing', default=0.06, description='chance that a strip is missing in a patch (never two in a row)', update=live.touch_prop, soft_min=0.0, soft_max=1.0)
    lath_lath_broken: bpy.props.FloatProperty(name='Lath Broken', default=0.15, description='chance that an exposed strip away from holes is snapped', update=live.touch_prop, soft_min=0.0, soft_max=1.0)
    lath_lath_bend: bpy.props.FloatProperty(name='Lath Bend', default=1.2, description='how far snapped ends are pushed in/out, x thickness', update=live.touch_prop, soft_min=0.0, soft_max=4.8)
    lath_lath_splinter: bpy.props.FloatProperty(name='Lath Splinter', default=1.2, description='Splintered snapped ends, like Boolean wood boards: a row of sharp teeth of random length, up to 1.5 x this x strip width', update=live.touch_prop, soft_min=0.0, soft_max=4.8)
    lath_up_x: bpy.props.FloatProperty(name='Up X', default=0.0, description='object-space "up"; laths run across it', update=live.touch_prop, soft_min=0.0, soft_max=1.0)
    lath_up_y: bpy.props.FloatProperty(name='Up Y', default=0.0, description='object-space "up"; laths run across it', update=live.touch_prop, soft_min=0.0, soft_max=1.0)
    lath_up_z: bpy.props.FloatProperty(name='Up Z', default=1.0, description='object-space "up"; laths run across it', update=live.touch_prop, soft_min=0.0, soft_max=4.0)
    # полосы и отрывы обоев
    wallpaper_enable: bpy.props.BoolProperty(name='Enable', default=False, description='', update=live.touch_prop)
    wallpaper_finders: bpy.props.PointerProperty(name='Finders', type=bpy.types.Collection, update=live.touch_prop, description='Each Empty selects its nearest connected wall side. All selected sides receive wallpaper')
    wallpaper_finder: bpy.props.PointerProperty(
        name='Finder', type=bpy.types.Object, poll=lambda self, obj: obj.type == 'EMPTY',
        update=live.touch_prop,
        description='Place the Empty near the wall face to cover. Wallpaper stays on that side. Clear the field to cover all eligible walls')
    wallpaper_seed: bpy.props.IntProperty(name='Seed', default=0, description='mixed with ProjectDamageParams::seed, like heatSeed/coatingSeed', update=live.touch_prop, min=0)
    wallpaper_thickness: bpy.props.FloatProperty(name='Thickness', default=0.12, description='paper layer: torn areas are recessed exactly this deep; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=1.0)
    wallpaper_strip_width: bpy.props.FloatProperty(name='Strip Width', default=53.0, description='width of one wallpaper strip (a standard roll is 53 cm); centimetres', update=live.touch_prop, soft_min=0.0, soft_max=212.0)
    wallpaper_seam_offset: bpy.props.FloatProperty(name='Seam Offset', default=0.0, description='slides every seam sideways (0..stripWidth); centimetres', update=live.touch_prop, soft_min=0.0, soft_max=1.0)
    wallpaper_seam_dir_x: bpy.props.FloatProperty(name='Seam Dir X', default=0.0, description='"up" along the strips, object space', update=live.touch_prop, soft_min=0.0, soft_max=1.0)
    wallpaper_seam_dir_y: bpy.props.FloatProperty(name='Seam Dir Y', default=0.0, description='"up" along the strips, object space', update=live.touch_prop, soft_min=0.0, soft_max=1.0)
    wallpaper_seam_dir_z: bpy.props.FloatProperty(name='Seam Dir Z', default=1.0, description='"up" along the strips, object space', update=live.touch_prop, soft_min=0.0, soft_max=4.0)
    wallpaper_wall_angle: bpy.props.FloatProperty(name='Wall Angle', default=25.0, description='a face is papered wall when its normal is within this of horizontal', update=live.touch_prop, soft_min=0.0, soft_max=100.0)
    wallpaper_detail: bpy.props.FloatProperty(name='Detail', default=1.0, description='grid density of outlines and flaps (0.25..4)', update=live.touch_prop, soft_min=0.0, soft_max=4.0, min=0.05, max=4.0)
    wallpaper_avoid: bpy.props.BoolProperty(name='Avoid', default=True, description='honour CoreInput::avoidPoly', update=live.touch_prop)
    wallpaper_seams_enable: bpy.props.BoolProperty(name='Seams Enable', default=True, description='a fine groove along every seam', update=live.touch_prop)
    wallpaper_seam_width: bpy.props.FloatProperty(name='Seam Width', default=0.12, description='; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=1.0)
    wallpaper_seam_depth: bpy.props.FloatProperty(name='Seam Depth', default=0.6, description='x thickness (1 = down to the wall); centimetres', update=live.touch_prop, soft_min=0.0, soft_max=2.4)
    wallpaper_seam_open: bpy.props.FloatProperty(name='Seam Open', default=0.35, description='0..1: stretches where the seam gapes wider', update=live.touch_prop, soft_min=0.0, soft_max=1.4)
    wallpaper_tears_enable: bpy.props.BoolProperty(name='Tears Enable', default=True, description='', update=live.touch_prop)
    wallpaper_tear_amount: bpy.props.IntProperty(name='Tear Amount', default=2, description='torn strips; one per strip, never two side by side (fewer if the walls run out)', update=live.touch_prop)
    wallpaper_tear_length_min: bpy.props.FloatProperty(name='Tear Length Min', default=60.0, description='length of the torn area on its long side', update=live.touch_prop, soft_min=0.0, soft_max=240.0)
    wallpaper_tear_length_max: bpy.props.FloatProperty(name='Tear Length Max', default=140.0, description='', update=live.touch_prop, soft_min=0.0, soft_max=560.0)
    wallpaper_tear_width_min: bpy.props.FloatProperty(name='Tear Width Min', default=0.7, description='share of the strip width that tears away', update=live.touch_prop, soft_min=0.0, soft_max=2.8)
    wallpaper_tear_width_max: bpy.props.FloatProperty(name='Tear Width Max', default=1.0, description='(1 = seam to seam, as in the reference)', update=live.touch_prop, soft_min=0.0, soft_max=4.0)
    wallpaper_tear_diagonal: bpy.props.FloatProperty(name='Tear Diagonal', default=0.85, description='0 = level tear front, 1 = front runs from the top corner', update=live.touch_prop, soft_min=0.0, soft_max=3.4)
    wallpaper_tear_diagonal_randomness: bpy.props.FloatProperty(
        name='Tear Diagonal Randomness', subtype='PERCENTAGE', default=0.0, min=0.0, max=100.0,
        description='Varies the slope of automatic and Empty-placed tears. 0% uses Tear Diagonal. 100% samples from level to steep for each tear',
        update=live.touch_prop)
    wallpaper_tear_from_top: bpy.props.FloatProperty(name='Tear From Top', default=0.8, description='chance a tear starts at the top edge of the wall', update=live.touch_prop, soft_min=0.0, soft_max=3.2)
    wallpaper_tear_from_top_randomness: bpy.props.FloatProperty(
        name='Tear From Top Randomness', subtype='PERCENTAGE', default=0.0, min=0.0, max=100.0,
        description='Varies where the first automatic tear on each strip starts. 0% uses Tear From Top. 100% mixes tears starting at the top with tears lower down. Empty-placed tears follow their marker height',
        update=live.touch_prop)
    wallpaper_tear_side: bpy.props.IntProperty(name='Tear Side', default=0, description='long side: 0 random, 1 left seam, 2 right seam', update=live.touch_prop)
    wallpaper_tear_jagged: bpy.props.FloatProperty(name='Tear Jagged', default=0.5, description='raggedness of the torn edges', update=live.touch_prop, soft_min=0.0, soft_max=2.0)
    wallpaper_seam_crack: bpy.props.FloatProperty(name='Seam Crack', default=0.6, description='chance of a split running on down the seam below the tear', update=live.touch_prop, soft_min=0.0, soft_max=2.4)
    wallpaper_seam_crack_length: bpy.props.FloatProperty(name='Seam Crack Length', default=0.45, description='x tear length', update=live.touch_prop, soft_min=0.0, soft_max=1.8)
    wallpaper_flaps_enable: bpy.props.BoolProperty(name='Flaps Enable', default=True, description='', update=live.touch_prop)
    wallpaper_flap_amount: bpy.props.FloatProperty(name='Flap Amount', default=0.9, description='share of the torn paper still hanging on (0 = torn off)', update=live.touch_prop, soft_min=0.0, soft_max=3.6)
    wallpaper_flap_fold: bpy.props.FloatProperty(name='Flap Fold', default=170.0, description='degrees the flap folds over at the tear front (180 = flat on the wall)', update=live.touch_prop, soft_min=0.0, soft_max=680.0)
    wallpaper_flap_fold_radius: bpy.props.FloatProperty(name='Flap Fold Radius', default=0.035, description='x tear size, radius of the fold crease', update=live.touch_prop, soft_min=0.0, soft_max=1.0)
    wallpaper_flap_curl: bpy.props.FloatProperty(name='Flap Curl', default=230.0, description='degrees the hanging paper turns outward by its free end (0..300)', update=live.touch_prop, soft_min=0.0, soft_max=920.0)
    wallpaper_flap_curl_fold_randomness: bpy.props.FloatProperty(
        name='Flap Curl / Fold Randomness', subtype='PERCENTAGE', default=0.0, min=0.0, max=100.0,
        description='Varies the curl and fold angles independently for each hanging flap. 0% uses the set angles. 100% samples from 0 to 300 degrees for curl and 0 to 180 degrees for fold',
        update=live.touch_prop)
    wallpaper_flap_curl_focus: bpy.props.FloatProperty(name='Flap Curl Focus', default=2.0, description='1 = one even arc, higher = straight hang with a tight roll at the end', update=live.touch_prop, soft_min=0.0, soft_max=8.0)
    wallpaper_flap_cone: bpy.props.FloatProperty(name='Flap Cone', default=0.6, description='0..1: the roll gathers where the flap reaches farthest (a cone, as in the reference)', update=live.touch_prop, soft_min=0.0, soft_max=2.4)
    wallpaper_flap_wave: bpy.props.FloatProperty(name='Flap Wave', default=0.3, description='0..1: soft paper waviness', update=live.touch_prop, soft_min=0.0, soft_max=1.2)
    wallpaper_flap_wave_randomness: bpy.props.FloatProperty(
        name='Flap Wave Randomness', subtype='PERCENTAGE', default=0.0, min=0.0, max=100.0,
        description='Varies waviness from flap to flap. 0% uses Flap Wave. 100% samples from smooth to wavy for each flap',
        update=live.touch_prop)
    wallpaper_flap_ragged: bpy.props.FloatProperty(name='Flap Ragged', default=0.5, description="raggedness of the flap's torn end", update=live.touch_prop, soft_min=0.0, soft_max=2.0)
    wallpaper_flap_group: bpy.props.BoolProperty(name='Flap Wind Group', default=True, description="Write the hanging flaps to the vertex group 'PD_Wallpaper Flaps': 0 where the paper is on or near the wall, rising smoothly to 1 at the free end. Use it as the affected zone of a wind setup in Geometry Nodes", update=live.touch_prop)
    wallpaper_flap_wall_falloff: bpy.props.FloatProperty(name='Wall Falloff', default=0.6, min=0.01, max=1.0, description="Share of each flap's largest distance from the wall over which the weight rises from 0 to full. Lower = the weight reaches 1 closer to the wall", update=live.touch_prop)
    wallpaper_flap_height_influence: bpy.props.FloatProperty(name='Height Influence', default=0.5, min=0.0, max=1.0, description='How much the weight also grows with how far the paper hangs below where it is attached (0 = distance from the wall only)', update=live.touch_prop)
    wallpaper_flap_smooth: bpy.props.IntProperty(name='Weight Smooth', default=4, min=0, max=50, description='Smoothing passes over the flap weights', update=live.touch_prop)
    wallpaper_lifts_enable: bpy.props.BoolProperty(name='Lifts Enable', default=True, description='', update=live.touch_prop)
    wallpaper_lift_amount: bpy.props.IntProperty(name='Lift Amount', default=4, description='small curled strip edges along seams (fewer if there is no free seam)', update=live.touch_prop)
    wallpaper_lift_length_min: bpy.props.FloatProperty(name='Lift Length Min', default=10.0, description='; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=40.0)
    wallpaper_lift_length_max: bpy.props.FloatProperty(name='Lift Length Max', default=35.0, description='; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=140.0)
    wallpaper_lift_width_min: bpy.props.FloatProperty(name='Lift Width Min', default=1.5, description='; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=6.0)
    wallpaper_lift_width_max: bpy.props.FloatProperty(name='Lift Width Max', default=5.0, description='; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=20.0)
    wallpaper_lift_curl: bpy.props.FloatProperty(name='Lift Curl', default=140.0, description='degrees', update=live.touch_prop, soft_min=0.0, soft_max=560.0)
    wallpaper_paint_enable: bpy.props.BoolProperty(name='Paint Enable', default=False, description='Automatically enabled while a wallpaper drawing is assigned', update=live.touch_prop)
    wallpaper_paint_reach: bpy.props.FloatProperty(name='Paint Reach', default=5.0, description='projection distance; set it from ProjectDamageParams::surfPaintSnap; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=20.0)
    wallpaper_paint_jagged: bpy.props.FloatProperty(name='Paint Jagged', default=0.35, description='extra raggedness on the drawn outline', update=live.touch_prop, soft_min=0.0, soft_max=1.4)
    wallpaper_seam_snap: bpy.props.FloatProperty(name='Seam Snap', default=4.0, description='; centimetres', update=live.touch_prop, soft_min=0.0, soft_max=16.0)
    wood_drawing: bpy.props.PointerProperty(name="Drawing", type=bpy.types.Object, poll=_paint_drawing_poll, update=live.touch_prop)
    lath_drawing: bpy.props.PointerProperty(name="Drawing", type=bpy.types.Object, poll=_paint_drawing_poll, update=live.touch_prop)
    wallpaper_drawing: bpy.props.PointerProperty(name="Drawing", type=bpy.types.Object, poll=_paint_drawing_poll, update=_wallpaper_drawing_touch)
    fabric_holes_drawing: bpy.props.PointerProperty(name="Drawing", type=bpy.types.Object, poll=_paint_drawing_poll, update=live.touch_prop)
    fabric_edges_drawing: bpy.props.PointerProperty(name="Drawing", type=bpy.types.Object, poll=_paint_drawing_poll, update=live.touch_prop)
    hit_collection: bpy.props.PointerProperty(name="Impact Empties", type=bpy.types.Collection, update=live.touch_prop)

                                                                                        
                                                                       
                                        
    all_off: bpy.props.BoolProperty(                                                
        name="All Effects Off", default=False,
        description="Mute every damage module at once, keeping all settings", update=live.touch_prop)
                                                
    global_scale: bpy.props.FloatProperty(                                          
        name="Master Scale", default=100.0, subtype='PERCENTAGE',
        min=10.0, max=400.0, soft_min=10.0, soft_max=400.0,
        description="Master multiplier on every module's Scale", update=live.touch_prop)
                                                 
    global_detail: bpy.props.FloatProperty(                                         
        name="Master Quality", default=100.0, subtype='PERCENTAGE',
        min=10.0, max=400.0, soft_min=10.0, soft_max=400.0,
        description="Master multiplier on every module's Detail. Moves no "
                    "damage element - no Detail knob consumes randomness", update=live.touch_prop)
                                   
    draft_enable: bpy.props.BoolProperty(name='Draft Mode', default=False,
        description='Use Draft Quality for the editable preview while keeping Master Quality unchanged. Apply All always recalculates at Master Quality before baking', update=live.touch_prop)
    draft_quality: bpy.props.FloatProperty(name='Draft Quality', default=20.0,
        subtype='PERCENTAGE', min=10.0, max=400.0, soft_max=100.0,
        description='Preview quality multiplier used only in Draft Mode. Default: 20%. Minimum 10% uses coarser sampling; maximum 400% uses finer sampling and more memory. Apply All uses Master Quality instead', update=live.touch_prop)
    ui_boolean_enable: bpy.props.BoolProperty(name='Boolean Break',default=False,update=live.touch_prop)
    seed: bpy.props.IntProperty(                                                    
        name="Seed", default=5, min=0, soft_max=10000,              
        description="Drives every module's random stream", update=live.touch_prop)
                                                           
    topology: bpy.props.EnumProperty(                                               
        name="Topology", items=TOPOLOGY_ITEMS, default='3',
        description="How the result mesh is assembled from the Boolean output", update=live.touch_prop)
                                                                           
                                                                       
                      
                                                                        
                                                                         
    damage_shading: bpy.props.BoolProperty(name="Damage Shading v2", default=False, update=live.touch_prop,
        description="Preserve the source surface normals and smooth damaged surfaces without smoothing across damage rims")
    damage_smooth_angle: bpy.props.FloatProperty(name="Damage Smooth Angle", default=80., min=0., max=180., update=live.touch_prop,
        description="Maximum angle in degrees between damage faces that can share a smooth normal; higher values soften damage")
    base_smooth_angle: bpy.props.FloatProperty(name="Base Smooth Angle", default=30., min=0., max=180., update=live.touch_prop,
        description="Damage faces that meet the untouched surface at less than this angle blend into it smoothly; steeper rims stay crisp, 0 keeps every rim crisp. The untouched surface always keeps its source shading")
    preserve_data: bpy.props.BoolProperty(name="Preserve Source Data", default=True,
        description="Copy all UV maps, source materials, supported public attributes and groups to the generated result", update=live.touch_prop)
    keep_uvs: bpy.props.BoolProperty(                                               
        name="Keep UVs", default=True,
        description="Carry the active UV layer through the cut; damage "
                    "faces get a bounding-box cube projection. Off is "
                    "faster and byte-identical to a mesh with no UVs at all",
        update=live.touch_prop)
                                                                           
                                                                             
                                                                          
                                                                              
                                                                            
                                                                           
                                  
    keep_selections: bpy.props.BoolProperty(                                        
        name="Keep Selections", default=True,
        description="Carry the source mesh's material assignment through: "
                    "undamaged faces keep their own material slot, damage "
                    "classes take the slots after them. Off is faster and "
                    "byte-identical to a mesh with one material",
        update=live.touch_prop)
    quad_remesh: bpy.props.BoolProperty(name="Quad Remesh", default=False,
        description="Rebuild the whole damaged main result as four-sided faces or triangles on its existing surface. Includes Smart Remesh; may increase face count 10 to 20 times. Separate generated objects are not remeshed", update=live.touch_prop)
    quad_remesh_output: bpy.props.EnumProperty(name="Face Type", default='QUADS',
        items=(('QUADS','Quads','Four-sided output faces'),('TRIS','Triangles','Split each quad along its shorter diagonal')),
        description="Final output faces after remeshing; untouched source n-gons are not restored when Quad Remesh succeeds", update=live.touch_prop)
    quad_remesh_size: bpy.props.FloatProperty(name="Quad Size", default=1.0, min=0.5, soft_max=4.0,
        description="Flat-area face size relative to the damage edge length: larger values generally give fewer, larger faces. Small values can create very dense, slow meshes", update=live.touch_prop)
    smart_remesh: bpy.props.BoolProperty(name="Smart Remesh", default=False,
        description="Improve long thin triangles around cuts and damaged rims, keeping untouched faces, material zones and UV seams; adds rebuild time", update=live.touch_prop)
    smart_remesh_density: bpy.props.FloatProperty(name="Density", default=1.0,
        min=0.25, soft_max=4.0,
        description="Triangle density near damage: 1 matches the damage's median edge length; 2 targets half that length. Higher values add polygons and rebuild time", update=live.touch_prop)
    smart_remesh_grade: bpy.props.FloatProperty(name="Grading", default=0.6,
        min=0.1, soft_max=2.0,
        description="How quickly triangles grow away from damage: 0.6 adds 6 cm of target edge length per 10 cm distance. Lower values give a gentler, denser transition", update=live.touch_prop)
                                                                                       
                                                                      
                                                                           
                                                                         
                                                                             
                                                                         
                                                                       
    avoid_attr: bpy.props.StringProperty(                                           
        name="Avoid Selection", default="",
        description="Boolean face attribute keeping damage out of the "
                    "flagged faces' region. The damage that would have "
                    "landed there goes to the rest of the mesh instead",
        update=live.touch_prop)
    avoid_chips: bpy.props.BoolProperty(                                            
        name="Chips", default=True,
        description="Edge chips honour the Avoid Selection", update=live.touch_prop)
    avoid_surf: bpy.props.BoolProperty(                                             
        name="Surface", default=True,
        description="Surface craters honour the Avoid Selection", update=live.touch_prop)
    avoid_build: bpy.props.BoolProperty(                                            
        name="Buildup", default=True,
        description="Buildup honours the Avoid Selection", update=live.touch_prop)
    avoid_fract: bpy.props.BoolProperty(                                            
        name="Fracture", default=True,
        description="Fracture honours the Avoid Selection", update=live.touch_prop)
    avoid_corner: bpy.props.BoolProperty(                                           
        name="Corners", default=True,
        description="Corner damage honours the Avoid Selection", update=live.touch_prop)
                                                          
                                                                           
                                                                         
                                                                                
                                                                       
                                                                         
                                                                           
                                                                          
    mask_group: bpy.props.StringProperty(                                           
        name="Field", default="",
        description="Vertex group weighting where damage lands (1.0 = full "
                    "damage, 0.0 = none). Linking one never MOVES an "
                    "element, only removes or shrinks it", update=live.touch_prop)
                                                                       
    mask_amount: bpy.props.FloatProperty(                                           
        name="Mask Amount", default=100.0, subtype='PERCENTAGE',
        min=0.0, max=100.0, soft_min=0.0, soft_max=100.0, update=live.touch_prop)
                                                              
    mask_size: bpy.props.FloatProperty(                                             
        name="Mask Size", default=0.0, subtype='PERCENTAGE',
        min=0.0, max=100.0, soft_min=0.0, soft_max=100.0, update=live.touch_prop)
                                                                
    draw_mask_enable: bpy.props.BoolProperty(name='Draw Mask', default=False,
        description='Limit damage with Grease Pencil or Curve strokes; combines with the vertex-group mask', update=live.touch_prop)
    draw_mask_drawing: bpy.props.PointerProperty(name='Drawing', type=bpy.types.Object,
        poll=_paint_drawing_poll, update=live.touch_prop)
    draw_mask_mode: bpy.props.EnumProperty(name='Mode', default='0', items=(
        ('0','Paint Damage','Damage starts only near the drawing'),
        ('1','Erase Damage','Keep the drawn area clean; damage starts elsewhere')), update=live.touch_prop)
    draw_mask_radius: bpy.props.FloatProperty(name='Radius', default=5.0, min=0.001, soft_max=50.0,
        description='Reach in source-local centimeters, multiplied by Master Scale; large damage pieces can extend past it', update=live.touch_prop)
    draw_mask_soft: bpy.props.FloatProperty(name='Softness', default=50.0, min=0.0, max=100.0, subtype='PERCENTAGE',
        description='Width of the smooth fade: damage becomes less frequent near the boundary', update=live.touch_prop)
    draw_mask_size: bpy.props.FloatProperty(name='Size Falloff', default=0.0, min=0.0, max=100.0, subtype='PERCENTAGE',
        description='Also shrink damage toward the boundary; zero changes frequency only', update=live.touch_prop)
    edge_threshold: bpy.props.FloatProperty(                                        
        name="Edge Threshold", default=1.0471975511965976,               
        subtype='ANGLE', min=0.0, max=3.141592653589793,
        description="Minimum dihedral angle for an edge to count as exposed", update=live.touch_prop)
                                                                         
                                                                           
                                                         
    axis_x: bpy.props.EnumProperty(name="X", items=AXIS_ITEMS, default='1', update=live.touch_prop)        
    axis_y: bpy.props.EnumProperty(name="Y", items=AXIS_ITEMS, default='1', update=live.touch_prop)        
    axis_z: bpy.props.EnumProperty(name="Z", items=AXIS_ITEMS, default='1', update=live.touch_prop)        
                                                                              
                                                                       
    axis_x_toggle: bpy.props.BoolProperty(name='X',default=True,
        description='Enable edge damage along the local X axis',
        get=lambda self:self.axis_x=='1',set=lambda self,value:setattr(self,'axis_x','1' if value else '0'))
    axis_y_toggle: bpy.props.BoolProperty(name='Y',default=True,
        description='Enable edge damage along the local Y axis',
        get=lambda self:self.axis_y=='1',set=lambda self,value:setattr(self,'axis_y','1' if value else '0'))
    axis_z_toggle: bpy.props.BoolProperty(name='Z',default=True,
        description='Enable edge damage along the local Z axis',
        get=lambda self:self.axis_z=='1',set=lambda self,value:setattr(self,'axis_z','1' if value else '0'))

                                                  
                                                                     
                                                                                           
    live_update: bpy.props.BoolProperty(                                             
        name="Live Update", default=True,
        description="Rebuild automatically as settings or a linked breaker change", update=live.touch_prop)
                                                                    
                                                                            
                                                                             
                                                                            
                                      
    defer_ms: bpy.props.IntProperty(                                                
        name="Interactive Delay", default=300, min=0, max=2000,
        description="Milliseconds to wait after the last change before rebuilding")

                                                                                        
                   
    chips_enable: bpy.props.BoolProperty(name="Enable", default=False, update=live.touch_prop, description='Turns on chips cut into the sharp outer edges of the object\nOff: edges stay intact, no chips are generated\non: chips are cut along convex edges using the settings below')        
                                         
    # сколы на рёбрах
    chip_scale: bpy.props.FloatProperty(                                            
        name="Scale", default=100.0, subtype='PERCENTAGE',
        min=10.0, max=400.0, soft_min=10.0, soft_max=400.0, update=live.touch_prop, description='Multiplies the size and depth of eery chip at once, keeping their proportions\nLow (10%): all chips become tiny and shallow\nHigh (400%): all chips become up to four times biger and deeper')
                                                                        
                       
    detail: bpy.props.FloatProperty(                                                
        name="Detail", default=75.0, subtype='PERCENTAGE',
        min=5.0, max=800.0, soft_min=5.0, soft_max=800.0, update=live.touch_prop, description='Polygon resolution of every chip. Affects smoothness and render/update speed, not chip size\nlow (25%): few polygons, chips look faceted but update fast\nHigh (800%): many poolygons, smooth detailed cips but heavy mesh and slow updates')
                                     
    amount: bpy.props.IntProperty(                                                  
        name="Chip Count", default=120, min=0, max=50000, soft_max=2000,
        description='Total number of chips. They are spread over the edges according to edge length, so long edges get more\n0: no chips, the rest of the tab is disabled\nHigh: edges are densely coverred with chips and look heavily worn', update=live.touch_prop)
                                                                                    
                                                                        
    size_min: bpy.props.FloatProperty(                                              
        name="Size Min", default=2.0, min=0.0, soft_max=100.0,              
        description='Smallest possible chip size along the edge, in cm. Each chip picks a size between Size Min and Size Max\nLow: the smallest chips are tiny nicks\nHigh: even the smallest chips are large bites', update=live.touch_prop)
    size_max: bpy.props.FloatProperty(                                              
        name="Size Max", default=8.0, min=0.0, soft_max=100.0,
        description='Largest possible chip size along the edge, in cm\nlow: no chip grows bigger than a small nick\nHigh: some chips become large broken-off pieces', update=live.touch_prop)
    depth_min: bpy.props.FloatProperty(                                             
        name="Depth Min", default=0.5, min=0.0, soft_max=100.0,
        description='Smallest possible depth a chip cuts into the edge, in cm. Each chip picks a depth between Depth Min and Depth Max\nlow: shallow chips that only scratch the edge\nhigh: even the shalowest chips cut deep', update=live.touch_prop)
    depth_max: bpy.props.FloatProperty(                                             
        name="Depth Max", default=3.0, min=0.0, soft_max=100.0,
        description='Largest possible depth a chip cuts into the edge, in cm\nLow: all chips stay shallow\nHigh: some chips cut deep into the object', update=live.touch_prop)
                                                      
    stretch: bpy.props.FloatProperty(                                               
        name="Stretch Length", default=100.0, subtype='PERCENTAGE',
        min=25.0, max=400.0, soft_min=25.0, soft_max=400.0,
        description='Stretches chips along the edge direction only, without changing their depth\nLow (25%): short, round chips\nHigh (400%): long chips running along the edge, like a worn strip', update=live.touch_prop)
                                                                             
                                                              
    rim_soft: bpy.props.FloatProperty(                                              
        name="Rim Softness", default=0.0, subtype='PERCENTAGE',
        min=0.0, max=100.0, soft_min=0.0, soft_max=100.0,
        description='Rounds the border of each chip into a smooth fillet\n0%: sharp, vertical chip walls, a fresh hard break\n100%: soft rounded rims, an old worn and smoothed look', update=live.touch_prop)
                                                             
    normalize: bpy.props.BoolProperty(                                              
        name="Limit by Thickness", default=False,
        description='Limits chip size and depth by the local thickness of the object, so thin parts are not destroyed\nOff: chips use their full size everywhere, thin parts can be cut through\nOn: chips shrink on thin parts, the limit is set by Max Size vs Thickness', update=live.touch_prop)
                                                        
    norm_max_size: bpy.props.FloatProperty(                                         
        name="Max Size vs Thickness", default=30.0, subtype='PERCENTAGE',
        min=5.0, max=100.0, soft_min=5.0, soft_max=100.0, update=live.touch_prop, description='Maximum chip size and depth as a share of the local thickness. Works only when Limit by Thickness is on\nlow (5%): chips on thin parts become very small\nHigh (100%): chips can be as large as the part is thick')

                                                                  
                                                                          
                                                                       
    offset: bpy.props.FloatProperty(                                                
        name="Spacing Shift", default=0.0, subtype='PERCENTAGE',
        min=0.0, max=100.0, soft_min=0.0, soft_max=100.0,
        description='Slides all chips alng their edge by the same amount, without chaging their spacing. Use it to move chips away from an unlucky spot\n0%: chips stay at their original positions\n100%: chips are shifted a full step along each edge, back to a similar layout', update=live.touch_prop)
                                                
    random_offset: bpy.props.FloatProperty(                                         
        name="Spacing Randomness", default=50.0, subtype='PERCENTAGE',
        min=0.0, max=100.0, soft_min=0.0, soft_max=100.0,
        description='how much each chip moves away from even spacing along the edge\n0%: chips are spaced perfectly evenly, looks artificial\n100%: chips are placed fully randomly along the edge, natural look', update=live.touch_prop)

                                                                                  
                                                                            
                                              
    noise_strength: bpy.props.FloatProperty(                                        
        name="Noise Strength", default=0.0, min=0.0, soft_max=20.0,              
        description='pushes the surface inside chips in and out with noise, in cm, to make them look rough\n0: smooth chip surfaces, noise is off\nhigh: strongly bumpy, rough chip surfaces', update=live.touch_prop)
                                              
    noise_scale: bpy.props.FloatProperty(                                           
        name="Noise Scale", default=8.0, min=0.01, soft_max=50.0,              
        description='Size of the bumps made by Noise Strength, in cm\nLow: small, fine grain-like bumps\nHigh: large, soft waves across the chip', update=live.touch_prop)

    noise_roughness: bpy.props.FloatProperty(
        name="Noise Roughness", default=0.0, min=0.0, max=100.0, subtype='PERCENTAGE',
        description='Adds finer layers to the noise inside the damage. Requires Noise Strength above zero\n0%: the original noise, with no extra detail\n100%: the strongest fine detail, limited by the mesh resolution; the damage rim stays pinned', update=live.touch_prop)
    noise_distortion: bpy.props.FloatProperty(
        name="Noise Distortion", default=0.0, min=0.0, max=100.0, subtype='PERCENTAGE',
        description='Bends the noise pattern inside the damage. Requires Noise Strength above zero\n0%: the original undistorted pattern\n100%: strongly twisted bumps, with the same displacement strength and pinned rim', update=live.touch_prop)

                                                                                     
                                                                            
                         
                                              
    sec_amount: bpy.props.IntProperty(                                              
        name="Rim Chips per Chip", default=0, min=0, max=50000, soft_max=20,
        description='Number of small extra chips placed around the rim of each main chip\n0: off, main chips have clean rims\nHigh: every chip has a frayed rim with many small chips around it', update=live.touch_prop)
                                       
    sec_size: bpy.props.FloatProperty(                                              
        name="Size", default=50.0, subtype='PERCENTAGE',
        min=1.0, max=100.0, soft_min=1.0, soft_max=100.0, update=live.touch_prop, description='Size of the rim chips as a share of the main chip size\nLow (1%): tiny rim chips, barely visible\nHigh (100%): rim chips as big as the main chip')
                                        
    sec_depth: bpy.props.FloatProperty(                                             
        name="Depth", default=15.0, subtype='PERCENTAGE',
        min=1.0, max=100.0, soft_min=1.0, soft_max=100.0, update=live.touch_prop, description='Depth of the rim chips as a share of the main chip depth\nLow (1%): rim chips are just surface marks\nhigh (100%): rim chips cut as deep as the main chip')
                                                                      
    chip_seed: bpy.props.IntProperty(name='Seed', default=0, min=0, max=1000000,
        description='Reshuffles Edge Damage positions and shapes in Automatic and Draw. Same seed and inputs give the same pattern. Zero keeps the original pattern; higher values are different variations, not more damage. Other generators keep their own seeds',
        update=live.touch_prop)
    chip_cluster: bpy.props.FloatProperty(name="Worn Patches", default=50.0, subtype='PERCENTAGE',
        min=0.0, max=100.0, description='Groups chips into worn stretches with intact edge between them\n0%: chips are spread evenly alng the edges\n100%: chips gather in dense worn areas with long clean gaps between them', update=live.touch_prop)
    chip_fine: bpy.props.FloatProperty(name="Fine Chips", default=40.0, subtype='PERCENTAGE',
        min=0.0, max=200.0, description='Adds small flakes next to the main chips, so the edge looks broken rather than cut. Counted as a share of Chip Count\n0%: only the main chips, clean cuts\n200%: many extra small flakes, a crumbled and rough edge', update=live.touch_prop)
    chip_profile: bpy.props.FloatProperty(name="Shell Fracture", default=70.0, subtype='PERCENTAGE',
        min=0.0, max=100.0, description="Shape of the chip's inner surface\n0%: a simple round bowl\n100%: a shell-like frcture with a small step at the rim and a smooth dip toward the edge, like real stone or concrete", update=live.touch_prop)
    chip_asym: bpy.props.FloatProperty(name="One-Sided Bite", default=35.0, subtype='PERCENTAGE',
        min=0.0, max=100.0, description='how much a chip bites into one face of the edge more than the other\n0%: chips cut both faces of the edge equally\n100%: chips mostly bite into one face, more natural breaks', update=live.touch_prop)
    chip_guard: bpy.props.FloatProperty(name="Thin Wall Guard", default=0.0, subtype='PERCENTAGE',
        min=0.0, max=100.0, description='stops chips from going deeper than this share of the local wall thickness, so thin walls are not punched through\n0%: off, chips use their full depth\n100%: chips can go as deep as the wall is thick; lower values keep them shallower on thin walls', update=live.touch_prop)

                                                                                        
                                                                         
    # сеть трещин и её уровни
    fract_mode: bpy.props.EnumProperty(name="Fracture Mode", items=(
        ('PROCEDURAL', "Procedural", "Original network and Proximity workflow"),
        ('PAINT', "Paint", "One drawing defines Level 1; Level 2 and 3 Amount controls their automatic branches"),
        ('PROXIMITY', "Proximity", "Use linked points with the pattern fixed by the selected Fracture asset")),
        default='PROCEDURAL', update=_fracture_mode_changed)
    fract_asset: bpy.props.EnumProperty(name='Fracture Asset',default='fracture',update=live.touch_prop,
        items=(('fracture','Fracture','Legacy shared fracture'),
               ('fracture_concrete','Fracture Concrete','Original concrete crack network'),
               ('fracture_wood','Fracture Wood','Grain-directed crack network'),
               ('fracture_glass','Fracture Glass','Glass spokes and ring chords'),
               ('fracture_radial','Fracture Radial','Radial stars'),
               ('fracture_proximity_default','Fracture Proximity Default','Cracks only at linked proximity points'),
               ('fracture_proximity_radial','Radial Fracture Proximity','Radial cracks at linked points'),
               ('fracture_proximity_glass','Glass Fracture Proximity','Glass cracks at linked points'),
               ('fracture_through','Fracture Through','Shared through fracture'),
               ('fracture_volume','Fracture Volume','Crack-driven surface pieces and rough interior cells')))
    chips_mode: bpy.props.EnumProperty(name='Edge Placement',default='AUTO',
        items=(('AUTO','Automatic','Chips are scattered over all suitable edges of the object'),('PAINT','Draw','Chips appear only on edges close to your drawn strokes')),
        description='chooses where edge chips are allowed to appear\nAutomatic: chips are scattered over all suitable edges of the object\nDraw: chips appear only on edges close to your drawn strokes',update=live.touch_prop)
    chips_drawing: bpy.props.PointerProperty(name='Drawing',type=bpy.types.Object,poll=_paint_drawing_poll,update=live.touch_prop)
    chips_paint_radius: bpy.props.FloatProperty(name='Brush Radius (cm)',default=8.0,min=.01,soft_max=50,
        description='How close to your stroke an edge chip may be placed, in cm. Drawing on a flat face does not create new edges\nLow: only edges right unedr the stroke get chips, very precise placement\nhigh: edges far from the stroke also get chips, the drawing acts like a wide brush',update=live.touch_prop)
    surf_mode: bpy.props.EnumProperty(name="Surface Mode", items=(
        ('PROCEDURAL', "Procedural", 'Damage is spread automatically over all flat faces'),
        ('PAINT', "Paint", 'damage appears only inside closed contours you draw')),
        default='PROCEDURAL', update=live.touch_prop, description='chooses whhere surface damage appears\nprocedural: damage is spread automatically over all flat faces\nPaint: damage appears only inside closed contours you draw')
    surf_drawing: bpy.props.PointerProperty(name="Drawing", type=bpy.types.Object,
        poll=_paint_drawing_poll, update=live.touch_prop)
    surf_close_strokes: bpy.props.BoolProperty(name="Close Strokes", default=True,
        description='connects the start and end of each stroke so it forms a closed area\nOff: only strokes you closed yourself make an area\non: every stroke becomes a closed area automaticaly', update=live.touch_prop)
    surf_paint_snap: bpy.props.FloatProperty(name="Projection Distance", default=5.0,
        min=0.001, soft_max=100.0, description='How far a stroke can be from the surface and still be projected onto it, in cm\nLow: only strokes drawn right on the surface are used\nHigh: strokes far from the surface are also projected onto it',
        update=live.touch_prop)
    fract_l1_paint_enable: bpy.props.BoolProperty(name="Paint Level 1", default=True, update=live.touch_prop)
    fract_l1_drawing: bpy.props.PointerProperty(name="Drawing", type=bpy.types.Object,
        poll=_paint_drawing_poll, update=live.touch_prop)
    fract_l1_paint_radius: bpy.props.FloatProperty(name="Paint Radius", default=3.0,
        min=0.04, soft_max=100.0, description="Maximum surface corridor half-width in source-local cm; limits crack width",
        update=live.touch_prop)
    fract_l2_paint_enable: bpy.props.BoolProperty(name="Paint Level 2", default=True, update=live.touch_prop)
    fract_l2_drawing: bpy.props.PointerProperty(name="Drawing", type=bpy.types.Object,
        poll=_paint_drawing_poll, update=live.touch_prop)
    fract_l2_paint_radius: bpy.props.FloatProperty(name="Paint Radius", default=3.0,
        min=0.04, soft_max=100.0, description="Maximum surface corridor half-width in source-local cm; limits crack width",
        update=live.touch_prop)
    fract_l3_paint_enable: bpy.props.BoolProperty(name="Paint Level 3", default=True, update=live.touch_prop)
    fract_l3_drawing: bpy.props.PointerProperty(name="Drawing", type=bpy.types.Object,
        poll=_paint_drawing_poll, update=live.touch_prop)
    fract_l3_paint_radius: bpy.props.FloatProperty(name="Paint Radius", default=3.0,
        min=0.04, soft_max=100.0, description="Maximum surface corridor half-width in source-local cm; limits crack width",
        update=live.touch_prop)

    surf_enable: bpy.props.BoolProperty(name="Enable", default=False, update=live.touch_prop, description='turns on craters or cuts on the flat faces of the object\nOff: flat faces stay intact\nOn: damage is cut into flat faces using the setttings below')                
    surf_seed: bpy.props.IntProperty(name='Seed', default=0, min=0, max=1000000,
        description='Changes the random placement and shape of Surface Damage only. 0 preserves the original pattern; larger values choose other patterns without increasing damage amount or size. Master Seed also affects the pattern',
        update=live.touch_prop)
    surf_type: bpy.props.EnumProperty(name='Surface Type', default='0', update=live.touch_prop,
        items=(('0','Concrete','Round organic craters, like chipped concrete or stone'),
               ('1','Wood Knife','Long shallow cuts along the wood grain, like knife or axe marks')), description='Chooses the kind of surface damage\nConcrete: round organic craters, like chipped concrete or stone\nWood Knife: long shallow cuts along the wood grain, like knife or axe marks')
    surf_grain_x: bpy.props.FloatProperty(name='Grain X',default=0.,update=live.touch_prop,
        description="X part of the wood grain direction in the object's local axes. Sets direction only, not size\n0: the grain has no X component\nhigh (or negative): the grain turns toward the local X axis")
    surf_grain_y: bpy.props.FloatProperty(name='Grain Y',default=0.,update=live.touch_prop,
        description="Y part of the wood grain direction in the object's local axes. Sets direction only, not size\n0: the grain has no Y component\nHigh (or negative): the grain turns toward the local Y axis")
    surf_grain_z: bpy.props.FloatProperty(name='Grain Z',default=1.,update=live.touch_prop,
        description="Z part of the wood grain direction in the object's local axes. If X, Y and Z are all 0, the grin follows local Z\n0: the grain has no Z component\nHigh (or negative): the grain turns toward the local Z axis")
    surf_knife_jitter: bpy.props.FloatProperty(name='Grain Jitter (degrees)',default=10.,min=0.,max=180.,update=live.touch_prop,
        description='random angle between each cut and the grain direction, in degrees\n0: every cut follows the grain exactly\n180: cuts go in any direction, the grain is ignored')
    surf_knife_back: bpy.props.FloatProperty(name='Reverse Cuts',default=15.,min=0.,max=100.,subtype='PERCENTAGE',update=live.touch_prop,
        description='share of cuts made in the opposite direction along the grain\n0%: all cuts go the same way\n100%: all cuts go the opposite way; around 50% mixes both evenly')
    surf_scale: bpy.props.FloatProperty(                                            
        name="Scale", default=100.0, subtype='PERCENTAGE',
        min=10.0, max=400.0, soft_min=10.0, soft_max=400.0, update=live.touch_prop, description='Multiplies the size and depth of every crater at once, keeping tehir proportions\nLow (10%): all craters become tiny and shallow\nHigh (400%): all craters become up to four times bigger and deeper')
    surf_detail: bpy.props.FloatProperty(                                           
        name="Detail", default=100.0, subtype='PERCENTAGE',
        min=5.0, max=400.0, soft_min=5.0, soft_max=400.0, update=live.touch_prop, description='Polygon resolution of every crater. Affects smoothness and update speed, not crater size\nlow (25%): few polygons, faceted craters, fast updates\nhigh (400%): many polygons, smooth craters, heavy mesh and slow updates')
    surf_amount: bpy.props.IntProperty(                                             
        name="Crater Count", default=0, min=0, max=50000, soft_max=2000,
        description='Number of craters on the flat faces. They are spread by area, so large faces get more\n0: no craters, the rest of the tab is disabled\nHigh: the surface is densely covered with craters', update=live.touch_prop)
                                                                
    surf_size_min: bpy.props.FloatProperty(                                         
        name="Size Min", default=3.0, min=0.0, soft_max=100.0,
        description='smallest possible crater width, in cm. Each crater picks a size between Size Min and Size Max\nlow: the smallest craters are tiny pits\nHigh: even the smallest craters are large', update=live.touch_prop)
    surf_size_max: bpy.props.FloatProperty(                                         
        name="Size Max", default=9.0, min=0.0, soft_max=100.0,
        description='Largest possible crater width, in cm\nlow: no crater grows beyond a small pit\nHigh: some craters become large holes in the surface', update=live.touch_prop)
    surf_depth_min: bpy.props.FloatProperty(                                        
        name="Depth Min", default=0.3, min=0.0, soft_max=100.0,
        description='smallest possible crater depth, in cm. Each crater picks a depth between Depth Min and Depth Max\nLow: shallow craters that only mark the surface\nHigh: even the shallowest craters are deep', update=live.touch_prop)
    surf_depth_max: bpy.props.FloatProperty(                                        
        name="Depth Max", default=1.5, min=0.0, soft_max=100.0,
        description='Largest possible crater depth, in cm\nLow: all cratters stay shallow\nhigh: some craters go deep into the object', update=live.touch_prop)

                                                                                      
                                                                        
                                                                         
    surf_noise: bpy.props.FloatProperty(                                            
        name="Rim Roughness", default=100.0, subtype='PERCENTAGE',
        min=0.0, max=300.0, soft_min=0.0, soft_max=300.0,
        description='makes the crater outline irregular\n0%: perfectly round craters\n300%: very ragged, broken crater outlines', update=live.touch_prop)
                                                                       
    surf_noise_in: bpy.props.FloatProperty(                                         
        name="Floor Roughness", default=40.0, subtype='PERCENTAGE',
        min=0.0, max=300.0, soft_min=0.0, soft_max=300.0,
        description='Adds bumps to the bottom of each crater\n0%: smooth bowl-shaped bottom\n300%: very rough, uneven crater bottom', update=live.touch_prop)

                                                                                     
                                                                    
                                                
    surf_sec_amount: bpy.props.IntProperty(                                         
        name="Rim Chips per Crater", default=0, min=0, max=50000, soft_max=20,
        description='Number of small extra chips placed around the rim of each crater. Not used in Paint mode\n0: off, craters have clean rims\nHigh: every crater has a frayed rim with many small chips', update=live.touch_prop)
                                       
    surf_sec_size: bpy.props.FloatProperty(                                         
        name="Size", default=50.0, subtype='PERCENTAGE',
        min=1.0, max=100.0, soft_min=1.0, soft_max=100.0, update=live.touch_prop, description='Size of the rim chips as a share of the crater size\nLow (1%): tiny rim chips, barly visible\nHigh (100%): rim chips as big as the crater')
                                        
    surf_sec_depth: bpy.props.FloatProperty(                                        
        name="Depth", default=15.0, subtype='PERCENTAGE',
        min=1.0, max=100.0, soft_min=1.0, soft_max=100.0, update=live.touch_prop, description='depth of the rim chips as a share of the crater depth\nlow (1%): rim chips are just surface marks\nhigh (100%): rim chips cut as deep as the crater')
                                                                         
    surf_cluster: bpy.props.FloatProperty(name="Grouping", default=40.0, subtype='PERCENTAGE',
        min=0.0, max=100.0, description='Share of craters that form right next to an earlier one, making pitted groups\n0%: craters are spread evenly over the surface\n100%: craters gather in dense groups with clean surface between them', update=live.touch_prop)
    surf_size_bias: bpy.props.FloatProperty(name="Small Crater Bias", default=50.0, subtype='PERCENTAGE',
        min=0.0, max=100.0, description='Controls the mix of small and big craters\n0%: sizes are spread evenly between Size Min and Size Max\n100%: mostly small crtaers with only a few big ones, a natural look', update=live.touch_prop)
    surf_depth_follow: bpy.props.FloatProperty(name="Depth Follows Size", default=60.0, subtype='PERCENTAGE',
        min=0.0, max=100.0, description='Makes bigger craters deeper\n0%: depth is random and does not depend on size\n100%: depth follows size closely, big craters are always the deepest', update=live.touch_prop)
    surf_guard: bpy.props.FloatProperty(name="Thin Wall Guard", default=0.0, subtype='PERCENTAGE',
        min=0.0, max=100.0, description='stops craters from going deeeper than this share of the wall thickness, so thin walls are not punched through\n0%: off, craters use their full depth\n100%: craters can go as deep as the wall is thick; lower values keep them shallower on thin walls', update=live.touch_prop)

                                                                                        
    # наплывы на рёбрах
    build_enable: bpy.props.BoolProperty(name="Enable", default=False, update=live.touch_prop)               
    build_scale: bpy.props.FloatProperty(                                           
        name="Scale", default=100.0, subtype='PERCENTAGE',
        min=10.0, max=400.0, soft_min=10.0, soft_max=400.0, update=live.touch_prop)
    build_detail: bpy.props.FloatProperty(                                          
        name="Detail", default=100.0, subtype='PERCENTAGE',
        min=5.0, max=400.0, soft_min=5.0, soft_max=400.0, update=live.touch_prop)
    build_coverage: bpy.props.FloatProperty(                                        
        name="Edges Covered", default=0.0, subtype='PERCENTAGE',
        min=0.0, max=100.0, soft_min=0.0, soft_max=100.0,
        description="Share of inside (concave) edges that carry a bead", update=live.touch_prop)

                                                                                    
                                                                              
                                                              
                                                                
    build_width: bpy.props.FloatProperty(                                           
        name="Width", default=3.0, min=0.0, soft_max=100.0,
        description="In cm, matching Cinema 4D", update=live.touch_prop)
    build_height: bpy.props.FloatProperty(                                          
        name="Height", default=0.4, min=0.0, soft_max=100.0,
        description="In cm, matching Cinema 4D", update=live.touch_prop)
    build_irreg: bpy.props.FloatProperty(                                           
        name="Waviness", default=233.0, subtype='PERCENTAGE',
        min=0.0, max=300.0, soft_min=0.0, soft_max=300.0, update=live.touch_prop)

                                                                  
                                                            
    build_corner_prob: bpy.props.FloatProperty(                                     
        name="Corners Filled", default=50.0, subtype='PERCENTAGE',
        min=0.0, max=100.0, soft_min=0.0, soft_max=100.0,
        description="Chance of a pile in each inside corner (where inside edges meet)", update=live.touch_prop)
                                                                
    build_corner_size: bpy.props.FloatProperty(                                     
        name="Pile Size", default=3.0, min=0.0, soft_max=100.0,
        description="Pile radius in cm", update=live.touch_prop)
                                                                           
                                                                          
                                                                          
    build_blend: bpy.props.FloatProperty(                                           
        name="Blend Into Surface", default=50.0, subtype='PERCENTAGE',
        min=0.0, max=200.0, soft_min=0.0, soft_max=200.0,
        description="How smoothly a pile flows into the faces it sits on. 0 = hard lump", update=live.touch_prop)
                            
    build_shape: bpy.props.EnumProperty(name="Bead Shape", default='1', update=live.touch_prop,
        items=(('0', "Rounded", "The original half-round bead along the edge"),
               ('1', "Fillet", "Fills the inside corner as a cove running out onto both faces")))
    build_lumps: bpy.props.FloatProperty(name="Lumpiness", default=30.0, subtype='PERCENTAGE',
        min=0.0, max=100.0, description="Lumpy, uneven bead surface", update=live.touch_prop)
                                                                       
    build_mode: bpy.props.EnumProperty(name="Bead Placement", default='AUTO', update=live.touch_prop,
        items=(('AUTO', "Automatic", "Edges Covered picks inside edges at random"),
               ('PAINT', "Draw Zones", "Only inside edges near Grease Pencil or curve strokes get a bead")))
    build_drawing: bpy.props.PointerProperty(name="Drawing", type=bpy.types.Object, poll=_paint_drawing_poll,
        update=live.touch_prop)
    build_paint_radius: bpy.props.FloatProperty(name="Brush Radius (cm)", default=5.0, min=0.01, soft_max=100.0,
        update=live.touch_prop, description="How close to a stroke an inside edge must be to get a bead; it fades out toward this distance")
                                                                            
    # скопления в углах
    pile_enable: bpy.props.BoolProperty(name="Enable", default=False, update=live.touch_prop)
    pile_scale: bpy.props.FloatProperty(name="Scale", default=100.0, subtype='PERCENTAGE',
        min=10.0, max=400.0, update=live.touch_prop)
    pile_detail: bpy.props.FloatProperty(name="Resolution", default=100.0, subtype='PERCENTAGE',
        min=1.0, max=400.0, update=live.touch_prop)
    pile_height: bpy.props.FloatProperty(name="Pile Height", default=60.0, subtype='PERCENTAGE',
        min=10.0, max=200.0, description="Height of a pile as a share of its size", update=live.touch_prop)
    pile_lumps: bpy.props.FloatProperty(name="Spill Lumps", default=40.0, subtype='PERCENTAGE',
        min=0.0, max=100.0, description="Smaller heaps spilling out of the corner along the inside edges", update=live.touch_prop)


                                                                               
    # отложения
    deposit_enable: bpy.props.BoolProperty(name="Enable", default=False, update=live.touch_prop)
    deposit_scale: bpy.props.FloatProperty(name="Scale", default=100.0, subtype='PERCENTAGE',
        min=10.0, max=400.0, update=live.touch_prop)
    deposit_detail: bpy.props.FloatProperty(name="Resolution", default=100.0, subtype='PERCENTAGE',
        min=1.0, max=400.0, update=live.touch_prop)
    deposit_amount: bpy.props.IntProperty(name="Amount", default=120, min=0, max=2000,
        description="Area-sampled candidates; gravity, shelter and masks determine how many survive", update=live.touch_prop)
    deposit_style: bpy.props.EnumProperty(name="Style", items=(
        ('0', "Compact Dirt", "Compact irregular deposits"),
        ('1', "Powder", "Shallow soft deposits"),
        ('2', "Mineral Crust", "Broad rough crust patches"),
        ('3', "Organic Growth", "Taller irregular lobes")), default='0', update=live.touch_prop)
    deposit_size_min: bpy.props.FloatProperty(name="Radius Min (cm)", default=3.0, min=0.05, soft_max=50, update=live.touch_prop)
    deposit_size_max: bpy.props.FloatProperty(name="Radius Max (cm)", default=7.0, min=0.05, soft_max=50, update=live.touch_prop)
    deposit_height: bpy.props.FloatProperty(name="Half Height (cm)", default=1.0, min=0.05, soft_max=20, update=live.touch_prop)
    deposit_gravity: bpy.props.FloatProperty(name="Gravity Influence", subtype='PERCENTAGE', default=80, min=0, max=100, update=live.touch_prop)
    deposit_gravity_direction: bpy.props.FloatVectorProperty(name="Local Gravity", size=3,
        default=(0.0, 0.0, -1.0), description="Downward direction in the source mesh's local coordinates", update=live.touch_prop)
    deposit_shelter: bpy.props.FloatProperty(name="Shelter Influence", subtype='PERCENTAGE', default=0, min=0, max=100, update=live.touch_prop)
    deposit_shelter_radius: bpy.props.FloatProperty(name="Shelter Radius (cm)", default=15, min=0.1, soft_max=100, update=live.touch_prop)
    deposit_embed: bpy.props.FloatProperty(name="Attachment Embed", subtype='PERCENTAGE', default=30, min=5, max=90, update=live.touch_prop)
    deposit_layers: bpy.props.IntProperty(name="Layers", default=1, min=1, max=4, update=live.touch_prop)
    deposit_porosity: bpy.props.FloatProperty(name="Porosity", subtype='PERCENTAGE', default=0, min=0, max=100, update=live.touch_prop)
    deposit_separate: bpy.props.BoolProperty(name="Separate Deposit Object", default=False,
        description="Create closed deposit geometry as an independently editable object", update=live.touch_prop)

    coating_seed: bpy.props.IntProperty(name="Coating Seed", default=0, update=live.touch_prop)
    coating_satellites: bpy.props.FloatProperty(name="Satellite Flake-offs", default=65, subtype='PERCENTAGE', min=0, max=100, update=live.touch_prop)
    coating_stretch: bpy.props.FloatProperty(name="Directional Stretch", default=0, subtype='PERCENTAGE', min=0, max=100, update=live.touch_prop)
    coating_dir_x: bpy.props.FloatProperty(name="Stretch Direction X", default=1, update=live.touch_prop)
    coating_dir_y: bpy.props.FloatProperty(name="Stretch Direction Y", default=0, update=live.touch_prop)
    coating_dir_z: bpy.props.FloatProperty(name="Stretch Direction Z", default=0, update=live.touch_prop)
    coating_edge_wrap: bpy.props.FloatProperty(name="Edge Wrap", default=100, subtype='PERCENTAGE', min=0, max=200, update=live.touch_prop)
    coating_facing_only: bpy.props.BoolProperty(name="Facing Only", default=False, update=live.touch_prop)
    coating_facing_x: bpy.props.FloatProperty(name="Facing Direction X", default=0, update=live.touch_prop)
    coating_facing_y: bpy.props.FloatProperty(name="Facing Direction Y", default=0, update=live.touch_prop)
    coating_facing_z: bpy.props.FloatProperty(name="Facing Direction Z", default=1, update=live.touch_prop)
    coating_facing_angle: bpy.props.FloatProperty(name="Facing Angle (degrees)", default=90, min=0, max=180, update=live.touch_prop)
    coating_flake_size: bpy.props.FloatProperty(name="Flake Size", default=1, min=0.01, soft_max=5, update=live.touch_prop)
    coating_crack_length: bpy.props.FloatProperty(name="Crack Length", default=1, min=0.01, soft_max=5, update=live.touch_prop)

    fabric_enable: bpy.props.BoolProperty(name="Enable", default=False, update=live.touch_prop, description='Classic fabric: cuts the connected cloth surface and preserves its UVs. Works on open single-layer cloth without Solidify. Thickness Threshold can include the back wall of the same panel')
    fabric_solid_enable: bpy.props.BoolProperty(name='Fabric (Solidify)', default=False, update=live.touch_prop, description='Fabric tears for cloth with real thickness (a garment with Solidify, a closed shell): Boolean cuts that follow the cloth and go exactly through one panel, with a clean torn side wall and threads. Shares the Fabric settings')
    fabric_solid: bpy.props.BoolProperty(name='Solid', default=False, options={'HIDDEN'}, description='internal: set from the Fabric (Solidify) tile')
    fabric_layer_depth: bpy.props.FloatProperty(name="Thickness Threshold (cm)", default=2.0, min=0.0, soft_max=2.0, update=live.touch_prop, description="0 = a tear cuts only the layer it lies on (one side of a garment; the hole shows its inside). Above 0 = it also cuts the layer facing the other way at most this far behind - the inner wall of the same panel after Solidify - and joins both cuts with a torn side wall. Keep it below the gap to the other side of the garment")
    fabric_flat: bpy.props.BoolProperty(name="Flat", default=True, update=live.touch_prop, description="Treat the cloth as a surface: tears and torn edges are cut straight into its faces, no thickness, works on an open single-layer mesh (cloth before Solidify). Applied on its own, without the other effects. Off = the solid (Boolean) tears, where Thickness is the cut depth")
    fabric_seed: bpy.props.IntProperty(name="Seed", default=0, update=live.touch_prop, min=0, soft_max=100)
    fabric_thickness: bpy.props.FloatProperty(name="Thickness (cm)", default=0, update=live.touch_prop, min=0, soft_max=100, description="Cut depth. 0 = tears cut straight through the face. Above 0 = tears cut this deep from the surface; where the mesh is this thin or thinner they come out on the other side")
    fabric_cut_margin: bpy.props.FloatProperty(name="Cut Margin", default=0.75, update=live.touch_prop, min=0, soft_max=4)
    fabric_detail: bpy.props.FloatProperty(name="Detail", default=1, update=live.touch_prop, min=0.05, soft_max=4, max=4.0)
    fabric_avoid: bpy.props.BoolProperty(name="Avoid", default=True, update=live.touch_prop)
    fabric_holes_enable: bpy.props.BoolProperty(name="Holes Enable", default=True, update=live.touch_prop)
    fabric_hole_amount: bpy.props.IntProperty(name="Hole Amount", default=5, update=live.touch_prop, min=0, max=500)
    fabric_hole_length_min: bpy.props.FloatProperty(name="Hole Length Min (cm)", default=8, update=live.touch_prop, min=0, soft_max=100)
    fabric_hole_length_max: bpy.props.FloatProperty(name="Hole Length Max (cm)", default=24, update=live.touch_prop, min=0, soft_max=100)
    fabric_hole_width_min: bpy.props.FloatProperty(name="Hole Width Min (cm)", default=2.5, update=live.touch_prop, min=0, soft_max=100)
    fabric_hole_width_max: bpy.props.FloatProperty(name="Hole Width Max (cm)", default=7, update=live.touch_prop, min=0, soft_max=100)
    fabric_hole_jagged: bpy.props.FloatProperty(name="Hole Jagged", default=0.55, update=live.touch_prop, min=0, soft_max=4)
    fabric_hole_lobes: bpy.props.FloatProperty(name="Hole Lobes", default=0.45, update=live.touch_prop, min=0, soft_max=4)
    fabric_hole_meander: bpy.props.FloatProperty(name="Hole Meander", default=0.25, update=live.touch_prop, min=0, soft_max=4)
    fabric_hole_split_max: bpy.props.IntProperty(name="Hole Split Max", default=3, update=live.touch_prop, min=1, max=8)
    fabric_hole_bridge: bpy.props.FloatProperty(name="Hole Bridge (cm)", default=1.5, update=live.touch_prop, min=0, soft_max=100)
    fabric_hole_dir_mode: bpy.props.EnumProperty(name="Hole Dir Mode", default="1", update=live.touch_prop, items=(("0","Random","Random tear directions",),("1","Weave Axis","Follow the configured weave axis",),))
    fabric_hole_dir_x: bpy.props.FloatProperty(name="Hole Dir X", default=1, update=live.touch_prop, min=-100, soft_max=4)
    fabric_hole_dir_y: bpy.props.FloatProperty(name="Hole Dir Y", default=0, update=live.touch_prop, min=-100, soft_max=4)
    fabric_hole_dir_z: bpy.props.FloatProperty(name="Hole Dir Z", default=0, update=live.touch_prop, min=-100, soft_max=4)
    fabric_hole_dir_jitter: bpy.props.FloatProperty(name="Hole Dir Jitter", default=15, update=live.touch_prop, min=0, soft_max=180)
    fabric_edge_enable: bpy.props.BoolProperty(name="Edge Enable", default=True, update=live.touch_prop)
    fabric_edge_amount: bpy.props.IntProperty(name="Edge Amount", default=14, update=live.touch_prop, min=0, max=1000)
    fabric_edge_spacing_jitter: bpy.props.FloatProperty(name="Edge Spacing Jitter", default=0.6, update=live.touch_prop, min=0, soft_max=4)
    fabric_edge_length_min: bpy.props.FloatProperty(name="Edge Length Min (cm)", default=8, update=live.touch_prop, min=0, soft_max=100)
    fabric_edge_length_max: bpy.props.FloatProperty(name="Edge Length Max (cm)", default=35, update=live.touch_prop, min=0, soft_max=100)
    fabric_edge_width_min: bpy.props.FloatProperty(name="Edge Width Min (cm)", default=0.3, update=live.touch_prop, min=0, soft_max=100)
    fabric_edge_width_max: bpy.props.FloatProperty(name="Edge Width Max (cm)", default=3, update=live.touch_prop, min=0, soft_max=100)
    fabric_edge_taper: bpy.props.FloatProperty(name="Edge Taper", default=1, update=live.touch_prop, min=0, soft_max=4)
    fabric_edge_jagged: bpy.props.FloatProperty(name="Edge Jagged", default=0.5, update=live.touch_prop, min=0, soft_max=4)
    fabric_edge_meander: bpy.props.FloatProperty(name="Edge Meander", default=0.2, update=live.touch_prop, min=0, soft_max=4)
    fabric_edge_angle_jitter: bpy.props.FloatProperty(name="Edge Angle Jitter", default=12, update=live.touch_prop, min=0, soft_max=180)
    fabric_threads_enable: bpy.props.BoolProperty(name="Threads Enable", default=True, update=live.touch_prop)
    fabric_thread_radius: bpy.props.FloatProperty(name="Thread Radius (cm)", default=0.06, update=live.touch_prop, min=0.00001, soft_max=100)
    fabric_thread_sides: bpy.props.IntProperty(name="Thread Sides", default=6, update=live.touch_prop, min=3, max=12)
    fabric_thread_spacing: bpy.props.FloatProperty(name="Thread Spacing (cm)", default=0.35, update=live.touch_prop, min=0.01, soft_max=100)
    fabric_bridge_enable: bpy.props.BoolProperty(name="Bridge Enable", default=True, update=live.touch_prop)
    fabric_bridge_share: bpy.props.FloatProperty(name="Bridge Share", default=0.35, update=live.touch_prop, min=0, soft_max=4)
    fabric_thread_sag: bpy.props.FloatProperty(name="Thread Sag", default=0.12, update=live.touch_prop, min=0, soft_max=4)
    fabric_thread_skew: bpy.props.FloatProperty(name="Thread Skew", default=0.15, update=live.touch_prop, min=0, soft_max=4)
    fabric_thread_wobble: bpy.props.FloatProperty(name="Thread Wobble", default=0.25, update=live.touch_prop, min=0, soft_max=4)
    fabric_broken_enable: bpy.props.BoolProperty(name="Broken Enable", default=True, update=live.touch_prop)
    fabric_broken_share: bpy.props.FloatProperty(name="Broken Share", default=0.35, update=live.touch_prop, min=0, soft_max=4)
    fabric_thread_droop: bpy.props.FloatProperty(name="Thread Droop", default=0.6, update=live.touch_prop, min=0, soft_max=4)
    fabric_thread_curl: bpy.props.FloatProperty(name="Thread Curl", default=0.4, update=live.touch_prop, min=0, soft_max=4)
    fabric_thread_taper: bpy.props.FloatProperty(name="Thread Taper", default=0.45, update=live.touch_prop, min=0, soft_max=4)
    fabric_fray_enable: bpy.props.BoolProperty(name="Fray Enable", default=True, update=live.touch_prop)
    fabric_fray_spacing: bpy.props.FloatProperty(name="Fray Spacing (cm)", default=0.25, update=live.touch_prop, min=0.01, soft_max=100)
    fabric_fray_length_min: bpy.props.FloatProperty(name="Fray Length Min (cm)", default=0.2, update=live.touch_prop, min=0, soft_max=100)
    fabric_fray_length_max: bpy.props.FloatProperty(name="Fray Length Max (cm)", default=0.9, update=live.touch_prop, min=0, soft_max=100)
    fabric_hang_enable: bpy.props.BoolProperty(name="Hang Enable", default=True, update=live.touch_prop)
    fabric_hang_amount: bpy.props.IntProperty(name="Hang Amount", default=20, update=live.touch_prop, min=0, max=2000)
    fabric_hang_length_min: bpy.props.FloatProperty(name="Hang Length Min (cm)", default=3, update=live.touch_prop, min=0, soft_max=100)
    fabric_hang_length_max: bpy.props.FloatProperty(name="Hang Length Max (cm)", default=14, update=live.touch_prop, min=0, soft_max=100)
    fabric_gravity_x: bpy.props.FloatProperty(name="Gravity X", default=0, update=live.touch_prop, min=-100, soft_max=4)
    fabric_gravity_y: bpy.props.FloatProperty(name="Gravity Y", default=0, update=live.touch_prop, min=-100, soft_max=4)
    fabric_gravity_z: bpy.props.FloatProperty(name="Gravity Z", default=-1, update=live.touch_prop, min=-100, soft_max=4)
    fabric_max_threads: bpy.props.IntProperty(name="Max Threads", default=6000, update=live.touch_prop, min=0, max=50000)
    concrete_enable: bpy.props.BoolProperty(name="Enable", default=False, update=live.touch_prop)
    concrete_seed: bpy.props.IntProperty(name="Seed", default=0, update=live.touch_prop, min=0, soft_max=100)
    concrete_mode: bpy.props.EnumProperty(name="Mode", default="0", update=live.touch_prop, items=(("0","Automatic","Area-weighted spall patches",),("1","Paint","Damage follows linked Grease Pencil or Curve strokes",),))
    concrete_avoid: bpy.props.BoolProperty(name="Avoid", default=True, update=live.touch_prop)
    concrete_detail: bpy.props.FloatProperty(name="Detail", default=1, update=live.touch_prop, min=0.05, soft_max=4, max=4.0)
    concrete_amount: bpy.props.IntProperty(name="Amount", default=5, update=live.touch_prop, min=0, max=500)
    concrete_size_min: bpy.props.FloatProperty(name="Size Min (cm)", default=12, update=live.touch_prop, min=0, soft_max=100)
    concrete_size_max: bpy.props.FloatProperty(name="Size Max (cm)", default=26, update=live.touch_prop, min=0, soft_max=100)
    concrete_lobes: bpy.props.IntProperty(name="Lobes", default=4, update=live.touch_prop, min=1, max=8)
    concrete_stretch: bpy.props.FloatProperty(name="Stretch", default=1.6, update=live.touch_prop, min=0, soft_max=4)
    concrete_edge_bias: bpy.props.FloatProperty(name="Edge Bias", default=0.25, update=live.touch_prop, min=0, soft_max=4)
    concrete_paint_radius: bpy.props.FloatProperty(name="Paint Radius (cm)", default=6, update=live.touch_prop, min=0.001, soft_max=100)
    concrete_paint_snap: bpy.props.FloatProperty(name="Paint Snap (cm)", default=5, update=live.touch_prop, min=0.001, soft_max=100)
    concrete_paint_close: bpy.props.FloatProperty(name="Paint Close", default=0.15, update=live.touch_prop, min=0, soft_max=4)
    concrete_depth_min: bpy.props.FloatProperty(name="Depth Min (cm)", default=1.5, update=live.touch_prop, min=0, soft_max=100)
    concrete_depth_max: bpy.props.FloatProperty(name="Depth Max (cm)", default=4, update=live.touch_prop, min=0, soft_max=100)
    concrete_jagged: bpy.props.FloatProperty(name="Jagged", default=0.6, update=live.touch_prop, min=0, soft_max=4)
    concrete_outline_scale: bpy.props.FloatProperty(name="Outline Scale", default=1, update=live.touch_prop, min=0, soft_max=4)
    concrete_terraces: bpy.props.IntProperty(name="Terraces", default=2, update=live.touch_prop, min=1, max=4)
    concrete_terrace_skin: bpy.props.FloatProperty(name="Terrace Skin", default=0.3, update=live.touch_prop, min=0, soft_max=4)
    concrete_terrace_inset: bpy.props.FloatProperty(name="Terrace Inset", default=0.18, update=live.touch_prop, min=0, soft_max=4)
    concrete_wall_soft: bpy.props.FloatProperty(name="Wall Soft", default=0.35, update=live.touch_prop, min=0, soft_max=4)
    concrete_roughness: bpy.props.FloatProperty(name="Roughness", default=0.6, update=live.touch_prop, min=0, soft_max=4)
    concrete_aggregate_size: bpy.props.FloatProperty(name="Aggregate Size (cm)", default=0.8, update=live.touch_prop, min=0.01, soft_max=100)
    concrete_thickness_cap: bpy.props.FloatProperty(name="Thickness Cap", default=0.45, update=live.touch_prop, min=0, soft_max=4)
                                                                                                        
    concrete_surf_mode: bpy.props.EnumProperty(name='Spall Surface', items=(('0', 'Procedural', 'Roughness / Aggregate only'), ('1', 'Texture', 'Image displacement on top of the procedural spall, like the Boolean break surface')), default='0', update=live.touch_prop)
    concrete_tex_path: bpy.props.StringProperty(name='Height Texture', subtype='FILE_PATH', description='PNG/JPG grayscale displacement. Empty uses the bundled Concrete025 gravel displacement', update=live.touch_prop)
    concrete_tex_scale: bpy.props.FloatProperty(name='Texture Scale (cm)', default=20., min=.1, soft_max=200., description='Tile size in object-local space; larger values make larger texture features', update=live.touch_prop)
    concrete_tex_strength: bpy.props.FloatProperty(name='Texture Depth (cm)', default=1., min=0., soft_max=10., description='Peak-to-peak displacement on top of the spall. 0 = procedural surface only', update=live.touch_prop)
    concrete_tex_edge: bpy.props.FloatProperty(name='Mesh Edge Size (cm)', default=0., min=0., soft_max=5., description='Triangle size on the spall surface. 0 uses Texture Scale / 60. Increase for faster updates', update=live.touch_prop)
    concrete_tex_offset_x: bpy.props.FloatProperty(name='Offset X (cm)', default=0., update=live.touch_prop)
    concrete_tex_offset_y: bpy.props.FloatProperty(name='Offset Y (cm)', default=0., update=live.touch_prop)
    concrete_tex_offset_z: bpy.props.FloatProperty(name='Offset Z (cm)', default=0., update=live.touch_prop)
    concrete_hole_enable: bpy.props.BoolProperty(name="Hole Enable", default=False, update=live.touch_prop)
    concrete_hole_chance: bpy.props.FloatProperty(name="Hole Chance", default=0.6, update=live.touch_prop, min=0, soft_max=4)
    concrete_hole_size: bpy.props.FloatProperty(name="Hole Size", default=0.45, update=live.touch_prop, min=0, soft_max=4)
    concrete_hole_jagged: bpy.props.FloatProperty(name="Hole Jagged", default=0.5, update=live.touch_prop, min=0, soft_max=4)
    concrete_crack_enable: bpy.props.BoolProperty(name="Crack Enable", default=True, update=live.touch_prop)
    concrete_crack_amount: bpy.props.IntProperty(name="Crack Amount", default=4, update=live.touch_prop, min=0, max=32)
    concrete_crack_length_min: bpy.props.FloatProperty(name="Crack Length Min (cm)", default=6, update=live.touch_prop, min=0, soft_max=100)
    concrete_crack_length_max: bpy.props.FloatProperty(name="Crack Length Max (cm)", default=22, update=live.touch_prop, min=0, soft_max=100)
    concrete_crack_width: bpy.props.FloatProperty(name="Crack Width (cm)", default=0.35, update=live.touch_prop, min=0, soft_max=100)
    concrete_crack_depth: bpy.props.FloatProperty(name="Crack Depth (cm)", default=1, update=live.touch_prop, min=0, soft_max=100)
    concrete_crack_meander: bpy.props.FloatProperty(name="Crack Meander", default=0.45, update=live.touch_prop, min=0, soft_max=4)
    concrete_crack_branch: bpy.props.FloatProperty(name="Crack Branch", default=0.35, update=live.touch_prop, min=0, soft_max=4)
    concrete_crack_interior: bpy.props.FloatProperty(name="Crack Interior", default=0.3, update=live.touch_prop, min=0, soft_max=4)
    concrete_rebar_enable: bpy.props.BoolProperty(name="Rebar Enable", default=True, update=live.touch_prop)
    concrete_rebar_amount: bpy.props.FloatProperty(name="Rebar Amount", default=0.85, update=live.touch_prop, min=0, soft_max=4)
    concrete_rebar_density: bpy.props.FloatProperty(name="Rebar Density", default=1, update=live.touch_prop, min=0.05, soft_max=4)
    concrete_rebar_radius: bpy.props.FloatProperty(name="Rebar Radius (cm)", default=0.6, update=live.touch_prop, min=0, soft_max=100)
    concrete_rebar_scale: bpy.props.FloatProperty(name="Rebar Scale", default=1, update=live.touch_prop, min=0.001, soft_max=4)
    concrete_rebar_spacing_x: bpy.props.FloatProperty(name="Rebar Spacing X (cm)", default=10, update=live.touch_prop, min=0, soft_max=100)
    concrete_rebar_spacing_y: bpy.props.FloatProperty(name="Rebar Spacing Y (cm)", default=12, update=live.touch_prop, min=0, soft_max=100)
    concrete_rebar_bars_x: bpy.props.BoolProperty(name="Rebar Bars X", default=True, update=live.touch_prop)
    concrete_rebar_bars_y: bpy.props.BoolProperty(name="Rebar Bars Y", default=True, update=live.touch_prop)
    concrete_rebar_outer: bpy.props.EnumProperty(name="Rebar Outer", default="0", update=live.touch_prop, items=(("0","Horizontal","Horizontal bars in front",),("1","Vertical","Vertical bars in front",),))
    concrete_rebar_angle: bpy.props.FloatProperty(name="Rebar Angle", default=0, update=live.touch_prop, min=0, soft_max=180)
    concrete_rebar_cover: bpy.props.FloatProperty(name="Rebars Depth (cm)", default=2, update=live.touch_prop, min=0, soft_max=100)
    concrete_rebar_expose: bpy.props.FloatProperty(name="Rebar Expose", default=0.7, update=live.touch_prop, min=0, soft_max=4)
    concrete_rebar_embed: bpy.props.FloatProperty(name="Rebar Embed (cm)", default=4, update=live.touch_prop, min=0, soft_max=100)
    concrete_rebar_ribs: bpy.props.FloatProperty(name="Rebar Ribs", default=0.15, update=live.touch_prop, min=0, soft_max=4)
    concrete_rebar_sides: bpy.props.IntProperty(name="Rebar Sides", default=8, update=live.touch_prop, min=4, max=16)
    concrete_rebar_jitter: bpy.props.FloatProperty(name="Rebar Jitter", default=0.3, update=live.touch_prop, min=0, soft_max=4)
    concrete_rebar_wobble: bpy.props.FloatProperty(name="Rebar Wobble", default=0.2, update=live.touch_prop, min=0, soft_max=4)
    concrete_rebar_bend: bpy.props.FloatProperty(name="Rebar Bend", default=0.3, update=live.touch_prop, min=0, soft_max=4)
    concrete_rebar_broken: bpy.props.FloatProperty(name="Rebar Broken", default=0.35, update=live.touch_prop, min=0, soft_max=4)
    concrete_up_x: bpy.props.FloatProperty(name="Up X", default=0, update=live.touch_prop, min=-100, soft_max=4)
    concrete_up_y: bpy.props.FloatProperty(name="Up Y", default=0, update=live.touch_prop, min=-100, soft_max=4)
    concrete_up_z: bpy.props.FloatProperty(name="Up Z", default=1, update=live.touch_prop, min=-100, soft_max=4)

    fabric_group: bpy.props.StringProperty(name="Tear Vertex Group", default="", update=live.touch_prop,
        description="Optional weight-painted region; empty means the whole cloth")
    concrete_drawing: bpy.props.PointerProperty(name="Concrete Drawing", type=bpy.types.Object,
        poll=_paint_drawing_poll, update=live.touch_prop)

                                                     
    rust_mode: bpy.props.EnumProperty(name="Rust Mode", default='PAINT',
        items=(('PAINT', "Paint Contours", "Rust inside closed Grease Pencil contours"),
               ('PROCEDURAL', "Procedural", "Uniform rust or edge/bottom biases")),
        update=live.touch_prop)
    rust_drawing: bpy.props.PointerProperty(name="Rust Drawing", type=bpy.types.Object,
        poll=_paint_drawing_poll, update=live.touch_prop)
    coating_drawing: bpy.props.PointerProperty(name="Coating Drawing", type=bpy.types.Object,
        poll=_paint_drawing_poll, update=live.touch_prop)
    rust_materials: bpy.props.BoolProperty(name="Rust Shader", default=True, update=live.touch_prop,
        description="Blend rust into copies of result materials; original materials remain unchanged")
    rust_enable: bpy.props.BoolProperty(name="Rust", default=False, update=live.touch_prop,
        description="Corrode the metal (crust, pits, perforation) and its paint (front, crazing, blisters, flakes)")
    rust_mask_source: bpy.props.EnumProperty(name="Mask", default='COLOR', update=live.touch_prop,
        items=[('COLOR', "Vertex Paint", "Paint the rust into a Color Attribute (Vertex Paint mode)"),
               ('GROUP', "Vertex Group", "Use a weight-painted vertex group"),
               ('NONE', "None", "No mask: uniform Intensity, or only the Edge/Bottom tendencies")])
    rust_color_attr: bpy.props.StringProperty(name="Color Attribute", default="", update=live.touch_prop,
        description="Color attribute painted with the rust; empty = the active one")
    rust_color_channel: bpy.props.EnumProperty(name="Channel", default='LUMA', update=live.touch_prop,
        items=[('LUMA', "Luminance", ""), ('R', "Red", ""), ('G', "Green", ""), ('B', "Blue", ""), ('A', "Alpha", "")])
    rust_mask_invert: bpy.props.BoolProperty(name="Invert", default=False, update=live.touch_prop)
    rust_group: bpy.props.StringProperty(name="Vertex Group", default="", update=live.touch_prop)
    rust_seed: bpy.props.IntProperty(name="Seed", default=0, min=0, update=live.touch_prop)
    rust_intensity: bpy.props.FloatProperty(name="Intensity", subtype='PERCENTAGE', default=100, min=0, max=150, update=live.touch_prop)
    rust_edge_bias: bpy.props.FloatProperty(name="Edges", subtype='PERCENTAGE', default=0, min=0, max=150, update=live.touch_prop,
        description="Rust gathers at creases and edges")
    rust_bottom_bias: bpy.props.FloatProperty(name="Bottom", subtype='PERCENTAGE', default=0, min=0, max=150, update=live.touch_prop,
        description="Rust gathers low along gravity (sills, wheel arches)")
    rust_world_gravity: bpy.props.BoolProperty(name="World Down", default=True, update=live.touch_prop,
        description="Gravity is the world -Z (follows the object's rotation)")
    rust_gravity: bpy.props.FloatVectorProperty(name="Gravity", size=3, default=(0.0, 0.0, -1.0), update=live.touch_prop,
        description="Local gravity when World Down is off")
    rust_noise: bpy.props.FloatProperty(name="Front Raggedness", subtype='PERCENTAGE', default=60, min=0, max=100, update=live.touch_prop)
    rust_noise_scale: bpy.props.FloatProperty(name="Front Scale (cm)", default=6.0, min=0.1, soft_max=100, update=live.touch_prop)
    rust_detail: bpy.props.FloatProperty(name="Detail (cm)", default=0.6, min=0.05, soft_max=10, update=live.touch_prop)
    rust_crust_start: bpy.props.FloatProperty(name="Crust From", subtype='PERCENTAGE', default=45, min=0, max=150, update=live.touch_prop)
    rust_crust_height: bpy.props.FloatProperty(name="Scale Recess Depth (cm)", default=0.25, min=0.0, soft_max=5, update=live.touch_prop)
    rust_crust_rough: bpy.props.FloatProperty(name="Crust Roughness", subtype='PERCENTAGE', default=60, min=0, max=100, update=live.touch_prop)
    rust_pits: bpy.props.IntProperty(name="Pits", default=300, min=0, max=5000, update=live.touch_prop)
    rust_pit_start: bpy.props.FloatProperty(name="Pits From", subtype='PERCENTAGE', default=25, min=0, max=150, update=live.touch_prop)
    rust_pit_size: bpy.props.FloatProperty(name="Pit Size (cm)", default=0.4, min=0.01, soft_max=5, update=live.touch_prop)
    rust_pit_depth: bpy.props.FloatProperty(name="Pit Depth (cm)", default=0.25, min=0.0, soft_max=5, update=live.touch_prop)
    rust_hole_start: bpy.props.FloatProperty(name="Holes From", subtype='PERCENTAGE', default=90, min=0, max=150, update=live.touch_prop,
        description="Perforation where the rust exceeds this (140%+ = never)")
    rust_hole_depth: bpy.props.FloatProperty(name="Hole Depth (cm)", default=3.0, min=0.01, soft_max=50, update=live.touch_prop,
        description="Walls up to this thickness rust through; thicker ones get a pit this deep")
    rust_flakes: bpy.props.FloatProperty(name="Rim Sheets", subtype='PERCENTAGE', default=60, min=0, max=100, update=live.touch_prop)
    rust_flake_size: bpy.props.FloatProperty(name="Sheet Size (cm)", default=1.5, min=0.05, soft_max=20, update=live.touch_prop)
    rust_paint: bpy.props.BoolProperty(name="Paint Layer", default=True, update=live.touch_prop,
        description="Rust drives the paint (the coating object, Coating Thickness)")
    rust_paint_start: bpy.props.FloatProperty(name="Paint Gone From", subtype='PERCENTAGE', default=30, min=0, max=150, update=live.touch_prop)
    rust_band: bpy.props.FloatProperty(name="Front Band", subtype='PERCENTAGE', default=20, min=0, max=100, update=live.touch_prop,
        description="Width of the zone ahead of the front with crazing and blisters")
    rust_paint_flakes: bpy.props.FloatProperty(name="Paint Flakes", subtype='PERCENTAGE', default=50, min=0, max=100, update=live.touch_prop)
    rust_craze: bpy.props.FloatProperty(name="Crazing", subtype='PERCENTAGE', default=60, min=0, max=100, update=live.touch_prop)
    rust_craze_cell: bpy.props.FloatProperty(name="Crazing Cell (cm)", default=0.8, min=0.05, soft_max=10, update=live.touch_prop)
    rust_blisters: bpy.props.IntProperty(name="Blisters", default=150, min=0, max=3000, update=live.touch_prop)
    rust_blister_size: bpy.props.FloatProperty(name="Blister Size (cm)", default=0.6, min=0.05, soft_max=10, update=live.touch_prop)
    rust_streak: bpy.props.FloatProperty(name="Streaks", subtype='PERCENTAGE', default=80, min=0, max=100, update=live.touch_prop)
    rust_streak_length: bpy.props.FloatProperty(name="Streak Length (cm)", default=15.0, min=0.0, soft_max=200, update=live.touch_prop)

    heat_enable: bpy.props.BoolProperty(name="Enable Heat Damage", default=False, update=live.touch_prop)
    heat_seed: bpy.props.IntProperty(name="Heat Seed", default=0, update=live.touch_prop)
    heat_intensity: bpy.props.FloatProperty(name="Intensity", default=0.8, min=0, update=live.touch_prop)
    heat_diffuse: bpy.props.BoolProperty(name="Diffuse Heat", default=True, update=live.touch_prop)
    heat_spread: bpy.props.FloatProperty(name="Spread (cm)", default=5.0, min=0, update=live.touch_prop)
    heat_duration: bpy.props.FloatProperty(name="Duration", default=1.0, min=0.001, update=live.touch_prop)
    heat_steps: bpy.props.IntProperty(name="Diffusion Steps", default=8, min=1, max=64, update=live.touch_prop)
    heat_char_start: bpy.props.FloatProperty(name="Char Start", default=45, min=0, max=100, subtype='PERCENTAGE', update=live.touch_prop)
    heat_shrink_depth: bpy.props.FloatProperty(name="Shrink Depth (cm)", default=0.8, min=0, update=live.touch_prop)
    heat_detail: bpy.props.FloatProperty(name="Heat Detail (cm)", default=8.0, min=0.001, update=live.touch_prop)
    heat_block_round: bpy.props.FloatProperty(name="Block Rounding", default=50, min=0, max=100, subtype='PERCENTAGE', update=live.touch_prop)
    heat_cell_min: bpy.props.FloatProperty(name="Cell Size Min (cm)", default=5.0, min=0.001, update=live.touch_prop)
    heat_cell_max: bpy.props.FloatProperty(name="Cell Size Max (cm)", default=15.0, min=0.001, update=live.touch_prop)
    heat_crack_cells: bpy.props.IntProperty(name="Crack Cell Candidates", default=80, min=0, max=5000, update=live.touch_prop)
    heat_crack_width: bpy.props.FloatProperty(name="Crack Width (cm)", default=0.25, min=0, update=live.touch_prop)
    heat_crack_depth: bpy.props.FloatProperty(name="Crack Depth (cm)", default=1.0, min=0, update=live.touch_prop)
    heat_blisters: bpy.props.IntProperty(name="Blister Candidates", default=30, min=0, max=2000, update=live.touch_prop)
    heat_blister_min: bpy.props.FloatProperty(name="Blister Radius Min (cm)", default=1.0, min=0.001, update=live.touch_prop)
    heat_blister_max: bpy.props.FloatProperty(name="Blister Radius Max (cm)", default=3.0, min=0.001, update=live.touch_prop)
    heat_blister_low: bpy.props.FloatProperty(name="Blister Exposure Low", default=20, min=0, max=100, subtype='PERCENTAGE', update=live.touch_prop)
    heat_blister_high: bpy.props.FloatProperty(name="Blister Exposure High", default=75, min=0, max=100, subtype='PERCENTAGE', update=live.touch_prop)
    heat_blister_burst: bpy.props.FloatProperty(name="Burst Blisters", default=35, min=0, max=100, subtype='PERCENTAGE', update=live.touch_prop)
    heat_char_layer: bpy.props.FloatProperty(name="Char Layer Depth (cm)", default=0.1, min=0, update=live.touch_prop)
    heat_group: bpy.props.StringProperty(name="Heat Vertex Group", default="", description="Empty heats the whole mesh; a named group supplies per-vertex exposure weights", update=live.touch_prop)

    coating_zone_enable: bpy.props.BoolProperty(name="Painted Contour Zone", default=False, update=live.touch_prop)
    coating_zone_threshold: bpy.props.FloatProperty(name="Zone Weight Threshold", default=50, min=0, max=100, subtype='PERCENTAGE', update=live.touch_prop)
    coating_zone_smooth: bpy.props.FloatProperty(name="Zone Smoothing (cm)", default=1.0, min=0, update=live.touch_prop)
    coating_zone_inset: bpy.props.FloatProperty(name="Zone Inset (cm)", default=0.5, min=0, update=live.touch_prop)
    coating_zone_detail: bpy.props.FloatProperty(name="Zone Detail (cm)", default=2.0, min=0.01, update=live.touch_prop)
    coating_flake_side: bpy.props.EnumProperty(name="Flake Side", items=[('0', "All", "All island rims"), ('1', "Upper", "Rims along the local side axis"), ('2', "Lower", "Rims opposite the local side axis")], default='0', update=live.touch_prop)
    coating_flake_side_x: bpy.props.FloatProperty(name="Flake Side Axis X", default=0.0, update=live.touch_prop)
    coating_flake_side_y: bpy.props.FloatProperty(name="Flake Side Axis Y", default=0.0, update=live.touch_prop)
    coating_flake_side_z: bpy.props.FloatProperty(name="Flake Side Axis Z", default=1.0, update=live.touch_prop)
    coating_zone_group: bpy.props.StringProperty(name="Coating Vertex Group", default="", update=live.touch_prop)

                                                       
    coating_enable: bpy.props.BoolProperty(name="Coating Peeling", default=False, update=live.touch_prop)
    coating_amount: bpy.props.IntProperty(name="Patch Candidates", default=24, min=0, max=1000,
        description="Zero creates an intact coating; positive values attempt peel patches", update=live.touch_prop)
    coating_thickness: bpy.props.FloatProperty(name="Thickness (cm)", default=0.08, min=0.001, soft_max=2, update=live.touch_prop)
    coating_patch_min: bpy.props.FloatProperty(name="Patch Radius Min (cm)", default=2, min=0.01, soft_max=50, update=live.touch_prop)
    coating_patch_max: bpy.props.FloatProperty(name="Patch Radius Max (cm)", default=8, min=0.01, soft_max=100, update=live.touch_prop)
    coating_jagged: bpy.props.FloatProperty(name="Jagged Edges", default=65, subtype='PERCENTAGE', min=0, max=100, update=live.touch_prop)
    coating_flakes: bpy.props.FloatProperty(name="Lifted Flakes", default=50, subtype='PERCENTAGE', min=0, max=100, update=live.touch_prop)
    coating_curl: bpy.props.FloatProperty(name="Flake Curl", default=60, subtype='PERCENTAGE', min=0, max=100, update=live.touch_prop)
    coating_cracks: bpy.props.FloatProperty(name="Coating Cracks", default=30, subtype='PERCENTAGE', min=0, max=100, update=live.touch_prop)

                                                                                        
                                                                          
    fract_type: bpy.props.EnumProperty(name='Fracture Type',default='FRACTURE',update=live.touch_prop,
        description='Procedural network type; Paint mode uses your drawn paths instead',
        items=(('FRACTURE','Fracture','Original crack network with the existing Proximity setting'),
               ('RADIAL','Radial','Each layer crack becomes a star; Amount controls star count, independent of Proximity'),
               ('GLASS','Glass','Straight spoke stars with ring chords on the next layer; Amount controls star count'),
               ('WOOD','Wood','Splits attracted to the surface-projected local grain direction')))
    fract_impacts: bpy.props.IntProperty(name='Automatic Impacts',default=1,min=1,max=64,update=live.touch_prop,
        description='Legacy compatibility value; no longer used by network styles')
    fract_grain_x: bpy.props.FloatProperty(name='Grain X',default=0.,min=-1.,max=1.,update=live.touch_prop,
        description='Local-space grain direction; the XYZ vector is normalized internally')
    fract_grain_y: bpy.props.FloatProperty(name='Grain Y',default=0.,min=-1.,max=1.,update=live.touch_prop)
    fract_grain_z: bpy.props.FloatProperty(name='Grain Z',default=1.,min=-1.,max=1.,update=live.touch_prop)
    fract_grain_pull: bpy.props.FloatProperty(name='Grain Pull',default=.8,min=0.,max=1.,update=live.touch_prop,
        description='Attraction toward grain projected onto each face; fades on end grain. Zero vector disables grain attraction')
    fract_recipe: bpy.props.EnumProperty(name="Recipe", default='IMPROVED', update=live.touch_prop,
        items=[('IMPROVED', "Improved", "Stable per-level/per-crack randomness, depth follows width, "
                                       "depth limited by wall thickness, fallen fragments removed"),
               ('LEGACY', "Legacy", "The original fracture network, identical to older results")])
    fract_thickness_cap: bpy.props.FloatProperty(name="Thickness Cap", subtype='PERCENTAGE', default=45,
        min=0, max=100, update=live.touch_prop,
        description="Max crack depth as a share of the wall under it (0 = no cap); ignored with Pass Through")
    fract_prox_mode: bpy.props.EnumProperty(name="Proximity", default='OFF', update=live.touch_prop,
        items=[('OFF', "Off", "No proximity fractures"),
               ('DEFAULT', "Default", "Concentrate cracks around the points (empties)"),
               ('RADIAL', "Radial", "Impact pattern at each point: radial cracks, concentric rings, crushed centre"),
               ('GLASS', "Glass", "Glass impact: forking straight hairline radials, dense concentric rings")])
    fract_prox_collection: bpy.props.PointerProperty(name="Points", type=bpy.types.Collection,
        update=live.touch_prop, description="Collection whose objects (empties) are the proximity points")
    fract_prox_use_size: bpy.props.BoolProperty(name="Radius from Empty Size", default=True,
        update=live.touch_prop, description="Each empty's display size x scale is its reach")
    fract_prox_radius: bpy.props.FloatProperty(name="Distance (cm)", default=40.0, min=0.1, soft_max=500,
        update=live.touch_prop, description="Reach of a point (used when not taken from the empty size)")
    fract_prox_strength: bpy.props.FloatProperty(name="Concentration", subtype='PERCENTAGE', default=60,
        min=0, max=100, update=live.touch_prop, description="Default mode: share of each level's cracks born near the points")
    fract_radial_spokes: bpy.props.IntProperty(name="Radial Cracks", default=7, min=3, max=32, update=live.touch_prop,
        description='Spokes per star in Radial network style, or per impact in Radial Proximity')
    fract_radial_rings: bpy.props.IntProperty(name="Rings", default=3, min=0, max=12, update=live.touch_prop)
    fract_radial_ring_fill: bpy.props.FloatProperty(name="Ring Fill", subtype='PERCENTAGE', default=80,
        min=0, max=100, update=live.touch_prop, description="Share of ring segments that are cracked")
    fract_radial_crush: bpy.props.FloatProperty(name="Crushed Centre", subtype='PERCENTAGE', default=12,
        min=0, max=60, update=live.touch_prop, description="Crushed zone radius as a share of the reach")
    fract_glass_spokes: bpy.props.IntProperty(name="Radial Cracks", default=14, min=3, max=48, update=live.touch_prop,
        description='Glass network uses half this value plus one spokes per star; Glass Proximity uses its impact pattern')
    fract_glass_rings: bpy.props.IntProperty(name="Rings", default=7, min=0, max=24, update=live.touch_prop,
        description='Glass network uses half this value as chord rings on the next layer; also controls Glass Proximity rings')
    fract_glass_branch: bpy.props.FloatProperty(name="Branching", subtype='PERCENTAGE', default=50, min=0, max=100,
        update=live.touch_prop, description="Chance of each radial crack forking (up to twice) as it runs out")
    fract_glass_ring_fill: bpy.props.FloatProperty(name="Ring Fill", subtype='PERCENTAGE', default=75, min=0, max=100,
        update=live.touch_prop, description='Share of ring chord segments generated between neighbouring spokes')
    fract_glass_width: bpy.props.FloatProperty(name="Line Width (cm)", default=0.25, min=0.01, soft_max=5,
        update=live.touch_prop, description="Hairline width at the surface")
    fract_glass_depth: bpy.props.FloatProperty(name="Line Depth (cm)", default=0.4, min=0.01, soft_max=10,
        update=live.touch_prop)
    fract_glass_crush: bpy.props.FloatProperty(name="Crushed Centre", subtype='PERCENTAGE', default=5, min=0, max=40,
        update=live.touch_prop)

    fract_through_enable: bpy.props.BoolProperty(name='Fracture Through', default=False, update=live.touch_prop,
        description='Alternative fracture asset sharing the Fracture settings; cuts cracks through the wall')
    fract_physics: bpy.props.BoolProperty(name='Non-through Chunks (WIP)',default=False,update=live.touch_prop,
        description='Close crack islands and detach shallow chunks while retaining the main body; geometry only, no Blender physics')
    fract_physics_depth: bpy.props.FloatProperty(name='Chunk Depth (cm)',default=0.,min=0.,soft_max=100.,update=live.touch_prop,
        description='Target depth in cm; zero uses Level 1 Depth × Depth Multiplier. Follows Master Scale. WIP: approximate depth')
    fract_physics_close: bpy.props.IntProperty(name='Closing Passes',default=6,min=0,max=32,update=live.touch_prop,
        description='Extend/bridge open cracks into islands; more passes may create more chunks and take longer. Zero keeps the initial Through closing only')
    fract_helper_drawing: bpy.props.PointerProperty(name='Helper Drawing',type=bpy.types.Object,poll=_paint_drawing_poll,update=live.touch_prop)
    fract_helper_snap: bpy.props.FloatProperty(name='Helper Snap (cm)',default=0.,min=0.,soft_max=50.,update=live.touch_prop,
        description='Snap helper endpoints onto nearby cracks or object edges; 0 is automatic. Follows Master Scale. Requires fine enough source geometry')
    fract_through: bpy.props.BoolProperty(name='Pass Through', default=True, update=live.touch_prop,
        description='Split the solid beneath closed crack regions into separate mesh islands; disable for ordinary shallow grooves')
    fract_through_min_piece: bpy.props.FloatProperty(name='Min Piece', subtype='PERCENTAGE',
        default=0.1, min=0.0, max=10.0, precision=2, update=live.touch_prop,
        description='Remove fragment islands below this percentage of source volume; zero preserves small pieces')
    fract_volume_size: bpy.props.FloatProperty(name='Inner Piece Size (cm)', default=0.0, min=0.0, soft_max=200.0,
        update=live.touch_prop, description='Interior fragment size; zero estimates it from the surface pieces. Follows Master Scale. Smaller pieces require more processing')
    fract_volume_rough: bpy.props.FloatProperty(name='Inner Roughness', subtype='PERCENTAGE', default=50.0, min=0.0, max=100.0,
        update=live.touch_prop, description='Roughness of internal break faces; deformation fades near the surface to keep cuts aligned with the drawn or generated cracks')
    fract_enable: bpy.props.BoolProperty(name="Enable", default=False, update=live.touch_prop)               
    fract_scale: bpy.props.FloatProperty(                                           
        name="Scale", default=100.0, subtype='PERCENTAGE',
        min=10.0, max=400.0, soft_min=10.0, soft_max=400.0, update=live.touch_prop)
    fract_paint_scale: bpy.props.FloatProperty(name='Scale', default=35.42, min=10., max=400., subtype='PERCENTAGE',
        description='Scale of the painted fracture network. Multiplies width, depth and offshoot length',
        **_paint_value('fract_paint_scale', lambda self: self.fract_scale))
    fract_l2_paint_amount: bpy.props.IntProperty(name='Amount', default=12, min=0, soft_max=1000,
        description='Number of Level 2 cracks growing from the drawn strokes',
        **_paint_value('fract_l2_paint_amount', lambda self: self.fract_l2_amount))
    fract_l3_paint_amount: bpy.props.IntProperty(name='Amount', default=11, min=0, soft_max=1000,
        description='Number of Level 3 cracks growing from Level 2',
        **_paint_value('fract_l3_paint_amount', lambda self: self.fract_l3_amount))
    fract_detail: bpy.props.FloatProperty(                                          
        name="Detail", default=100.0, subtype='PERCENTAGE',
        min=5.0, max=400.0, soft_min=5.0, soft_max=400.0, update=live.touch_prop)
                                                                        
                                                                           
                                                                                    
                                              
    fract_l1_amount: bpy.props.IntProperty(                                         
        name="Amount", default=0, min=0, soft_max=1000, update=live.touch_prop)              
    fract_l1_subdiv: bpy.props.IntProperty(                                         
        name="Subdivide", default=0, min=0, max=4,
        description="Extra densifying passes after this level's cracks finish", update=live.touch_prop)
    fract_l1_width: bpy.props.FloatProperty(name="Width", default=2.0, min=0.0, soft_max=100.0,          
        description="In cm, matching Cinema 4D", update=live.touch_prop)
    fract_l1_depth: bpy.props.FloatProperty(name="Depth", default=6.0, min=0.0, soft_max=100.0,          
        description="In cm, matching Cinema 4D", update=live.touch_prop)
    fract_l1_length: bpy.props.FloatProperty(name='Fracture Length', default=0.0, min=0.0, soft_max=500.0,
        description='Maximum growth length for a Level 1 crack in source-local cm, multiplied by Fracture Scale and Master Scale. Zero means unlimited. Through mode can extend an open end by one more Length', update=live.touch_prop)
    fract_l2_length: bpy.props.FloatProperty(name='Fracture Length', default=0.0, min=0.0, soft_max=500.0,
        description='Maximum growth length for a Level 2 crack in source-local cm, multiplied by Fracture Scale and Master Scale. Zero means unlimited. Through mode can extend an open end by one more Length', update=live.touch_prop)
    fract_l3_length: bpy.props.FloatProperty(name='Fracture Length', default=0.0, min=0.0, soft_max=500.0,
        description='Maximum growth length for a Level 3 crack in source-local cm, multiplied by Fracture Scale and Master Scale. Zero means unlimited. Through mode can extend an open end by one more Length', update=live.touch_prop)
                                                                          
    fract_l2_paint_length_min: bpy.props.FloatProperty(name='Length Min (cm)', default=46.72, min=0., soft_max=500.,
        description='Lower bound of the random target length for Level 2 offshoots. Borders, masks and other cracks can stop them earlier',
        **_paint_value('fract_l2_paint_length_min', lambda self: .65 * self.fract_l2_paint_length))
    fract_l2_paint_length: bpy.props.FloatProperty(name='Length Max (cm)', default=60.0, min=0., soft_max=500.,
        description='Paint mode only. Maximum travel length of Level 2 fractures, in cm before Fracture Scale and Master Scale.\n0: use the automatic short length. Higher: longer offshoots; a mesh border, mask or another crack can stop them sooner', update=live.touch_prop)
    fract_l2_paint_distortion: bpy.props.FloatProperty(name='Distortion', default=35., min=0., max=100., subtype='PERCENTAGE',
        description='Paint mode only. Bending and irregularity of Level 2 paths on the surface.\n0%: straight surface paths. 100%: strongly curved, irregular fractures. The drawn Level 1 path stays fixed', update=live.touch_prop)
    fract_l2_paint_distortion_scale: bpy.props.FloatProperty(name='Distortion Scale (cm)', default=20.0, min=.1, soft_max=200.,
        description='Paint mode only. Distance between bends in Level 2 paths, in cm before Fracture Scale and Master Scale.\nLow: frequent small turns. High: broad, gradual curves. Used only when Distortion is above zero', update=live.touch_prop)
    fract_l3_paint_length_min: bpy.props.FloatProperty(name='Length Min (cm)', default=19.5, min=0., soft_max=500.,
        description='Lower bound of the random target length for Level 3 offshoots. Borders, masks and other cracks can stop them earlier',
        **_paint_value('fract_l3_paint_length_min', lambda self: .65 * self.fract_l3_paint_length))
    fract_l3_paint_length: bpy.props.FloatProperty(name='Length Max (cm)', default=30.0, min=0., soft_max=500.,
        description='Paint mode only. Maximum travel length of Level 3 fractures, in cm before Fracture Scale and Master Scale.\n0: use the automatic short length. Higher: longer offshoots; a mesh border, mask or another crack can stop them sooner', update=live.touch_prop)
    fract_l3_paint_distortion: bpy.props.FloatProperty(name='Distortion', default=35., min=0., max=100., subtype='PERCENTAGE',
        description='Paint mode only. Bending and irregularity of Level 3 paths on the surface.\n0%: straight surface paths. 100%: strongly curved, irregular fractures. The drawn Level 1 path stays fixed', update=live.touch_prop)
    fract_l3_paint_distortion_scale: bpy.props.FloatProperty(name='Distortion Scale (cm)', default=10.0, min=.1, soft_max=200.,
        description='Paint mode only. Distance between bends in Level 3 paths, in cm before Fracture Scale and Master Scale.\nLow: frequent small turns. High: broad, gradual curves. Used only when Distortion is above zero', update=live.touch_prop)
    fract_l1_branches: bpy.props.FloatProperty(name='Branches',default=0.,min=0.,max=16.,soft_max=4.,update=live.touch_prop,
        description='Average same-level branches per eligible crack: 0.5 means roughly every other crack; guided paths do not branch directly. Zero preserves the old result')
    fract_l2_branches: bpy.props.FloatProperty(name='Branches',default=0.,min=0.,max=16.,soft_max=4.,update=live.touch_prop,
        description='Average Level 2 branches per eligible Level 2 crack; fractional values are allowed. Zero preserves the old result')
    fract_l3_branches: bpy.props.FloatProperty(name='Branches',default=0.,min=0.,max=16.,soft_max=4.,update=live.touch_prop,
        description='Average Level 3 branches per eligible Level 3 crack; fractional values are allowed. Zero preserves the old result')
    fract_l1_branch_length: bpy.props.FloatProperty(name='Branch Length',default=.35,min=0.,max=4.,soft_max=1.,update=live.touch_prop,
        description='Branch length as a multiple of its parent crack; capped by this level’s Fracture Length when nonzero')
    fract_l2_branch_length: bpy.props.FloatProperty(name='Branch Length',default=.35,min=0.,max=4.,soft_max=1.,update=live.touch_prop,
        description='Branch length relative to its parent crack, capped by this level’s Fracture Length')
    fract_l3_branch_length: bpy.props.FloatProperty(name='Branch Length',default=.35,min=0.,max=4.,soft_max=1.,update=live.touch_prop,
        description='Branch length relative to its parent crack, capped by this level’s Fracture Length')
    fract_l1_bias: bpy.props.FloatProperty(                                         
        name="Corner Bias", default=0.0, subtype='PERCENTAGE',
        min=0.0, max=100.0, soft_min=0.0, soft_max=100.0,
        description="Share of nucleation drawn from the previous level's "
                    "junction corners (level 1 uses the object's own corners)", update=live.touch_prop)
    fract_l1_chips: bpy.props.BoolProperty(name="Chips", default=False, update=live.touch_prop)              
                                                                             
                                                                                      
    fract_l1_chip_amount: bpy.props.FloatProperty(                                  
        name="Chip Amount", default=4.0, min=0.0, max=20.0,
        soft_min=0.0, soft_max=20.0,
        description="Spall events per 100 units of crack length", update=live.touch_prop)
    fract_l1_chip_size: bpy.props.FloatProperty(                                    
        name="Chip Size", default=2.5, min=0.0, soft_max=100.0,
        description="In cm, matching Cinema 4D", update=live.touch_prop)
    fract_l1_chip_depth: bpy.props.FloatProperty(                                   
        name="Chip Depth", default=1.0, min=0.0, soft_max=100.0,
        description="In cm, matching Cinema 4D", update=live.touch_prop)
    fract_l2_amount: bpy.props.IntProperty(name="Amount", default=0, min=0, soft_max=1000, update=live.touch_prop)               
    fract_l2_subdiv: bpy.props.IntProperty(name="Subdivide", default=0, min=0, max=4, update=live.touch_prop)                    
    fract_l2_width: bpy.props.FloatProperty(name="Width", default=1.0, min=0.0, soft_max=100.0,          
        description="In cm, matching Cinema 4D", update=live.touch_prop)
    fract_l2_depth: bpy.props.FloatProperty(name="Depth", default=3.5, min=0.0, soft_max=100.0,          
        description="In cm, matching Cinema 4D", update=live.touch_prop)
    fract_l2_bias: bpy.props.FloatProperty(                                         
        name="Corner Bias", default=50.0, subtype='PERCENTAGE',
        min=0.0, max=100.0, soft_min=0.0, soft_max=100.0, update=live.touch_prop)
    fract_l2_chips: bpy.props.BoolProperty(name="Chips", default=False, update=live.touch_prop)              
    fract_l2_chip_amount: bpy.props.FloatProperty(                                  
        name="Chip Amount", default=3.0, min=0.0, max=20.0,
        soft_min=0.0, soft_max=20.0, update=live.touch_prop)
    fract_l2_chip_size: bpy.props.FloatProperty(                                    
        name="Chip Size", default=1.5, min=0.0, soft_max=100.0,
        description="In cm, matching Cinema 4D", update=live.touch_prop)
    fract_l2_chip_depth: bpy.props.FloatProperty(                                   
        name="Chip Depth", default=0.7, min=0.0, soft_max=100.0,
        description="In cm, matching Cinema 4D", update=live.touch_prop)
    fract_l3_amount: bpy.props.IntProperty(name="Amount", default=0, min=0, soft_max=1000, update=live.touch_prop)               
    fract_l3_subdiv: bpy.props.IntProperty(name="Subdivide", default=0, min=0, max=4, update=live.touch_prop)                    
    fract_l3_width: bpy.props.FloatProperty(name="Width", default=0.5, min=0.0, soft_max=100.0,          
        description="In cm, matching Cinema 4D", update=live.touch_prop)
    fract_l3_depth: bpy.props.FloatProperty(name="Depth", default=2.0, min=0.0, soft_max=100.0,          
        description="In cm, matching Cinema 4D", update=live.touch_prop)
    fract_l3_bias: bpy.props.FloatProperty(                                         
        name="Corner Bias", default=85.0, subtype='PERCENTAGE',
        min=0.0, max=100.0, soft_min=0.0, soft_max=100.0,
        description=">= 65% switches this level into shatter mode "
                    "(many small closed cells at borders/junctions)", update=live.touch_prop)
    fract_l3_chips: bpy.props.BoolProperty(name="Chips", default=False, update=live.touch_prop)             
    fract_l3_chip_amount: bpy.props.FloatProperty(                                  
        name="Chip Amount", default=2.0, min=0.0, max=20.0,
        soft_min=0.0, soft_max=20.0, update=live.touch_prop)
    fract_l3_chip_size: bpy.props.FloatProperty(                                    
        name="Chip Size", default=1.0, min=0.0, soft_max=100.0,
        description="In cm, matching Cinema 4D", update=live.touch_prop)
    fract_l3_chip_depth: bpy.props.FloatProperty(                                   
        name="Chip Depth", default=0.5, min=0.0, soft_max=100.0,
        description="In cm, matching Cinema 4D", update=live.touch_prop)

                                                                                      
                                                                     
                                                                         
                                                                           
                             
    fract_disp_strength: bpy.props.FloatProperty(                                   
        name="Strength", default=100.0, subtype='PERCENTAGE',
        min=0.0, max=300.0, soft_min=0.0, soft_max=300.0, update=live.touch_prop)
    fract_disp_scale: bpy.props.FloatProperty(                                      
        name="Scale", default=100.0, subtype='PERCENTAGE',
        min=25.0, max=400.0, soft_min=25.0, soft_max=400.0, update=live.touch_prop)

                                                                                         
                                                                           
                                                                               
    fract_tile_offset: bpy.props.FloatProperty(                                     
        name="Offset", default=0.0, subtype='PERCENTAGE',
        min=0.0, max=100.0, soft_min=0.0, soft_max=100.0,
        description="Random height offset of the tiles the cracks enclose, as a "
                    "share of each tile's own shallowest crack depth",
        update=live.touch_prop)
    fract_tile_seed: bpy.props.IntProperty(                                         
        name="Seed", default=0, min=0, max=999999,
        description="Reshuffles which tile rises and which sinks, without moving "
                    "any crack",
        update=live.touch_prop)
                                                                   
    fract_rim_jag: bpy.props.FloatProperty(name="Ragged Lips", default=40.0, subtype='PERCENTAGE',
        min=0.0, max=100.0, description="The two lips of a crack break unevenly, one side wider than the other", update=live.touch_prop)
    fract_flare: bpy.props.FloatProperty(name="Broken Mouth", default=35.0, subtype='PERCENTAGE',
        min=0.0, max=100.0, description="The crack mouth crumbles open: a wide broken lip over a narrow deep slit", update=live.touch_prop)
                                                                             
                                                                           
                                                                    
    fract_shard_enable: bpy.props.BoolProperty(name="Shard Displace", default=False, update=live.touch_prop,
        description="Move the broken pieces near the points in or out along their normal")
    fract_shard_collection: bpy.props.PointerProperty(name="Points", type=bpy.types.Collection, update=live.touch_prop,
        description="Collection of empties. Empty: uses the Proximity points")
    fract_shard_dir: bpy.props.EnumProperty(name="Direction", default='0', update=live.touch_prop,
        items=(('0', "Push In", "Pieces sink into the surface"),
               ('1', "Lift Out", "Pieces lift off the surface"),
               ('2', "Mixed", "Each piece goes in or out at random")))
    fract_shard_amount: bpy.props.FloatProperty(name="Distance (cm)", default=1.5, min=0.0, soft_max=20.0,
        update=live.touch_prop, description="How far a piece right at a point moves. Inward moves stop short of the crack floor")
    fract_shard_use_size: bpy.props.BoolProperty(name="Radius from Empty Size", default=True, update=live.touch_prop,
        description="Each empty's display size x scale is its reach")
    fract_shard_radius: bpy.props.FloatProperty(name="Radius (cm)", default=40.0, min=0.1, soft_max=500.0,
        update=live.touch_prop, description="Reach of a point (used when not taken from the empty size)")
    fract_shard_falloff: bpy.props.FloatProperty(name="Falloff", default=1.0, min=0.1, max=6.0,
        update=live.touch_prop, description="1 = linear fade to the radius. Higher = only pieces close to a point move")
    fract_shard_tilt: bpy.props.FloatProperty(name="Tilt", default=50.0, subtype='PERCENTAGE',
        min=0.0, max=100.0, description="The side of a piece nearer the point moves more, so pieces tilt", update=live.touch_prop)
    fract_shard_random: bpy.props.FloatProperty(name="Randomness", default=35.0, subtype='PERCENTAGE',
        min=0.0, max=100.0, description="Varies the distance per piece", update=live.touch_prop)
    fract_shard_seed: bpy.props.IntProperty(name="Seed", default=0, min=0, max=999999, update=live.touch_prop)
                                                                                  
    fract_depth_mult: bpy.props.FloatProperty(                                      
        name="Depth Multiplier", default=100.0, subtype='PERCENTAGE',
        min=10.0, max=400.0, soft_min=10.0, soft_max=400.0,
        description="Scales the crack depth of all three levels together", update=live.touch_prop)
                                                                             
                                                                        
                                                                           
                                                  
    fract_rim_soft: bpy.props.FloatProperty(                                        
        name="Rim Softness", default=0.0, subtype='PERCENTAGE',
        min=0.0, max=100.0, soft_min=0.0, soft_max=100.0,
        description="Rounds the fracture chip rim outward into a fillet", update=live.touch_prop)
                                                                                        
    # повреждения углов
    corner_enable: bpy.props.BoolProperty(name="Enable", default=False, update=live.touch_prop, description='Turns on damage at the outer corners of the object\noff: corners stay intact\nOn: corners are broken using the settings below')              
    corner_scale: bpy.props.FloatProperty(                                          
        name="Scale", default=100.0, subtype='PERCENTAGE',
        min=10.0, max=400.0, soft_min=10.0, soft_max=400.0, update=live.touch_prop, description='Multiplies the size and depth of every corner break at once, keeping their proportions\nLow (10%): all breaks become tiny\nhigh (400%): all breaks become up to four times bigger')
    corner_detail: bpy.props.FloatProperty(                                         
        name="Detail", default=100.0, subtype='PERCENTAGE',
        min=5.0, max=400.0, soft_min=5.0, soft_max=400.0, update=live.touch_prop, description='Polygon resolution of every corner break. Affects smoothness and update speed, not size\nlow (25%): few polygons, faceted breaks, fast updates\nHigh (400%): many polygons, smooth breaks, heavy mesh and slow updates')
    corner_prob: bpy.props.FloatProperty(                                           
        name="Corners Broken", default=0.0, subtype='PERCENTAGE',
        min=0.0, max=100.0, soft_min=0.0, soft_max=100.0,
        description='Chance that each outer corner is broken\n0%: no corners break, the rest of the tab is disabled\n100%: every outer corner is broken', update=live.touch_prop)
    corner_min_edges: bpy.props.IntProperty(                                        
        name="Min Edges at Corner", default=3, min=3, max=8, soft_min=3, soft_max=8,
        description='how many sharp outer edges must meet at a point for it to count as a corner\n3: ordinary box corners count, the most corners are found\n8: only complex points where many edges meet count, very few corners break', update=live.touch_prop)
                                                                
    corner_size_min: bpy.props.FloatProperty(                                       
        name="Size Min", default=6.0, min=0.0, soft_max=100.0,
        description='Smallest possible corner break size, in cm. Each break picks a size between Size Min and Size Max\nlow: the smallest breaks are small nicks\nhigh: even the smallest breaks are large', update=live.touch_prop)
    corner_size_max: bpy.props.FloatProperty(                                       
        name="Size Max", default=12.0, min=0.0, soft_max=100.0,
        description='Largest possible corner break size, in cm\nLow: no break grows beyond a small nick\nHigh: some corners lose large pieces', update=live.touch_prop)
    corner_depth_min: bpy.props.FloatProperty(                                      
        name="Depth Min", default=3.0, min=0.0, soft_max=100.0,
        description='Smallest possible depth of a corner break, in cm. Each break picks a depth between Depth Min and Depth Max\nLow: shallow breaks\nhigh: even the shallowest breaks are deep', update=live.touch_prop)
    corner_depth_max: bpy.props.FloatProperty(                                      
        name="Depth Max", default=6.0, min=0.0, soft_max=100.0,
        description='Largest possible depth of a corner break, in cm\nLow: all breaks stay shallow\nHigh: some corners are broken deep into the object', update=live.touch_prop)
                                                                             
                                                                  
                                                                      
                                                            
    corner_rim_soft: bpy.props.FloatProperty(                                       
        name="Rim Softness", default=0.0, subtype='PERCENTAGE',
        min=0.0, max=100.0, soft_min=0.0, soft_max=100.0,
        description='Rounds the border of each corner break into a smooth fillet\n0%: sharp break edges, a fresh hard break\n100%: soft rounded edges, an old worn look', update=live.touch_prop)

                                                                                      
                                           
                                                
    corner_sec_amount: bpy.props.IntProperty(                                       
        name="Rim Chips per Corner", default=0, min=0, max=50000, soft_max=20,
        description='Number of small extra chips placed around the rim of each corner break\n0: off, clean break rims\nhigh: every corner break has a frayed rim with many small chips', update=live.touch_prop)
                                       
    corner_sec_size: bpy.props.FloatProperty(                                       
        name="Size", default=50.0, subtype='PERCENTAGE',
        min=1.0, max=100.0, soft_min=1.0, soft_max=100.0, update=live.touch_prop, description='Size of the rim chips as a share of the corner break size\nLow (1%): tiny rim chips, barely visible\nHigh (100%): rim chips as big as the corner break')
                                        
    corner_sec_depth: bpy.props.FloatProperty(                                      
        name="Depth", default=15.0, subtype='PERCENTAGE',
        min=1.0, max=100.0, soft_min=1.0, soft_max=100.0, update=live.touch_prop, description='depth of the rim cihps as a share of the corner break depth\nLow (1%): rim chips are just surface marks\nHigh (100%): rim chips cut as deep as the corner break')
                              
    corner_style: bpy.props.EnumProperty(name="Corner Style", default='1', update=live.touch_prop,
        items=(('0', "Round Crater", 'A round bowl is cut out of the corner'),
               ('1', "Broken Corner", 'The corner is knocked off along a rough fracture face')), description='Chooses the look of a broken corner\nRound Crater: a round bowl is cut out of the corer\nBroken Corner: the corner is knocked off along a rough fracture face')
    corner_rough: bpy.props.FloatProperty(name="Roughness", default=50.0, subtype='PERCENTAGE',
        min=0.0, max=100.0, description='bumps and steps on the broken face. Works only with Broken Corner style\n0%: flat, clean fracture face\n100%: very rough, stepped fracture face', update=live.touch_prop)
    corner_tilt: bpy.props.FloatProperty(name="Uneven Break", default=50.0, subtype='PERCENTAGE',
        min=0.0, max=100.0, description='makes the break run a different distance down each edge of the corner. Works only with Broken Corner style\n0%: the break is even on all edges, looks like a neat cut\n100%: the break is very uneven, natural look', update=live.touch_prop)
    corner_flakes: bpy.props.FloatProperty(name="Edge Flakes", default=50.0, subtype='PERCENTAGE',
        min=0.0, max=100.0, description='Small flakes broken off the edges next to the corner. Works only with Broken Corner style\n0%: no flakes, only the main break\n100%: many flakes around the corner, a crumbled look', update=live.touch_prop)
    corner_mode: bpy.props.EnumProperty(name="Corner Placement", default='AUTO', update=live.touch_prop,
        items=(('AUTO', "Automatic", 'Any outer corner can break'),
               ('PAINT', "Draw", 'only corners close to your drawn strokes break')), description='Chooses which corners may break\nAutomatic: any outer corner can break\nDraw: only corners close to your drawn strokes break')
    corner_drawing: bpy.props.PointerProperty(name="Drawing", type=bpy.types.Object, poll=_paint_drawing_poll,
        update=live.touch_prop)
    corner_paint_radius: bpy.props.FloatProperty(name="Brush Radius (cm)", default=8.0, min=0.01, soft_max=100.0,
        update=live.touch_prop, description='How close a corner must be to your stroke to break, in cm\nlow: only corners right under the stroke break\nHigh: corners far from the stroke also break')

                                                                           
                                                                            
                                                                         
    guides: bpy.props.CollectionProperty(type=ProjectDamageGuideLink)                        
                                                                         
                                                                  
    guide_index: bpy.props.IntProperty(default=0)                                    

                                                                                                 
                                                                                         
                                                                           
    breakers: bpy.props.CollectionProperty(type=ProjectDamageBreakerLink)                    
                                                                          
                                                                     
                                                                             
                                     
    breaker_index: bpy.props.IntProperty(default=0)                                  
                                         
    # поверхность Boolean-среза
    break_scale: bpy.props.FloatProperty(                                           
        name="Scale", default=100.0, subtype='PERCENTAGE',
        min=10.0, max=400.0, soft_min=10.0, soft_max=400.0, update=live.touch_prop)
                                                      
    break_surf_dmg: bpy.props.BoolProperty(                                         
        name="Surface Damage on Break Faces", default=False, update=live.touch_prop)
                                                                                          
    break_rim_enable: bpy.props.BoolProperty(name="Rim Damage", default=False, update=live.touch_prop)        
                                           
    break_rim_amount: bpy.props.IntProperty(                                        
        name="Total Amount", default=60, min=0, max=50000, soft_max=500, update=live.touch_prop)
                                       
    break_rim_size: bpy.props.FloatProperty(                                        
        name="Size", default=100.0, subtype='PERCENTAGE',
        min=1.0, max=400.0, soft_min=1.0, soft_max=400.0, update=live.touch_prop)
                                        
    break_rim_depth: bpy.props.FloatProperty(                                       
        name="Depth", default=100.0, subtype='PERCENTAGE',
        min=1.0, max=400.0, soft_min=1.0, soft_max=400.0, update=live.touch_prop)



_BEVEL_DEFAULT_LOADING = False
_BEVEL_SHAPE_FIELDS={'size_mode','size_top','size_side','profile','roundness','fillet_top','fillet_side','max_share','taper'}

def _bevel_profile_defaults(self,context):
    global _BEVEL_DEFAULT_LOADING
    if _BEVEL_DEFAULT_LOADING or self.bevel_preset=='0': return
    from . import core_loader
    core=core_loader.load();block=core.EdgeBevelParams();core.edge_bevel_preset(block,int(self.bevel_preset))
    _BEVEL_DEFAULT_LOADING=True
    try:
        for name in _BEVEL_SHAPE_FIELDS:
            value=getattr(block,name)
            setattr(self,'bevel_'+name,str(value) if name in {'size_mode','profile'} else value)
    finally:_BEVEL_DEFAULT_LOADING=False
    live.touch_prop(self,context)

def _bevel_shape_touch(self,context):
    if not _BEVEL_DEFAULT_LOADING and self.bevel_preset!='0':self.bevel_preset='0'
    live.touch_prop(self,context)

from .effect_schema import EFFECT_META, EFFECT_FIELDS
_NEW_ENUMS = {
 'thickness_logic': ('Legacy','Improved'),
 'mode': ('Automatic', 'Paint'), 'material': ('Metal', 'Wood', 'Concrete / Stone'),
 'shot_mode': ('Random Slant', 'Fixed Direction'), 'grain_mode': ('Longest Axis', 'Fixed Axis'),
 'bond': ('Running', 'Stack', 'English', 'Flemish', 'Header'),
 'behind': ('Brick Leaves','Damage Pocket'),
 'brick_mode': ('Carved','Separate Bricks'),
}
for _module in ('bullet','blast','ground','brick','glass','fissure','bevel','scratch','metal','debris','rbf'):
    _lengths = dict(EFFECT_FIELDS[_module])
    for _field, _kind, _default, _description in EFFECT_META[_module]:
        if _module == 'brick' and (_field == 'surf_mode' or _field.startswith('tex_')):
            continue                                                                 
        _kw = dict(name=_field.replace('_',' ').title() + (' (cm)' if _lengths[_field] else ''),
                   default=_default, description=_description, update=live.touch_prop)
        if _module == 'brick' and _field in {'render_min', 'render_max'}:
            bound = _field.rsplit('_', 1)[1].title()
            _kw['name'] = f'Plaster {bound} / Bricks Depth {bound}'
        if _field=='detail':_kw['min']=0.05
        if _module=='bevel' and _field in _BEVEL_SHAPE_FIELDS:_kw['update']=_bevel_shape_touch
        if _kind == 'bool': _prop = bpy.props.BoolProperty(**_kw)
        elif _kind == 'int32_t' and (_field in _NEW_ENUMS or _field in {'profile','size_mode','style','crush_aim'}):
            _enum = {'rbf': {'material': ('Concrete','Wood','Fabric','Glass','Soil')},
                     'bevel': {'profile': ('Chamfer','Round','Cove','Step'), 'size_mode': ('Units','Relative')},
                     'scratch': {'mode': ('Scatter','Single Stroke'), 'style': ('Fine','Keyed / Gouged','Scuffed','Custom'), 'profile': ('V Groove','U Groove')},
                     'metal': {'style': ('Impact','Crush'), 'crush_aim': ('Nearest Surface','Empty +Z')},
                     'brick': {'style': ('Detailed','Low Poly','Swept Low Poly')}}.get(_module,{}).get(_field, _NEW_ENUMS.get(_field))
            _kw['default'] = str(_default)
            _prop = bpy.props.EnumProperty(**_kw, items=tuple((str(i),label,label) for i,label in enumerate(_enum)))
        elif _kind == 'int32_t': _prop = bpy.props.IntProperty(**_kw, min=0, soft_max=max(100,_default))
        else: _prop = bpy.props.FloatProperty(**_kw, soft_min=min(0,_default), soft_max=max(10,_default*4))
        ProjectDamageProps.__annotations__[_module+'_'+_field] = _prop


                                                                              
_RUSTY_UI = {
                                        
    'seed': ('Seed', 0, None, 100), 'detail': ('Resolution', 0.05, 4.0, 2.0),
    'amount': ('Amount', 0, 2000, 40), 'size_min': ('Size Min (cm)', 0.01, None, 30.0), 'size_max': ('Size Max (cm)', 0.01, None, 60.0),
    'size_bias': ('Size Bias', 0.0, 1.0, None), 'min_spacing': ('Min Spacing', 0.0, None, 3.0),
    'lobes': ('Lobes', 1, 24, 10), 'lobe_spread': ('Lobe Spread', 0.0, 2.0, None),
    'jagged': ('Jagged', 0.0, 3.0, None), 'roughness': ('Roughness', 0.0, 4.0, 2.0), 'scallop': ('Scalloped Rim', 0.0, 5.0, 3.0),
    'merge': ('Merge', 0.0, 2.0, 1.0), 'lace': ('Lace', 0.0, 3.0, 1.5),
    'pit_amount': ('Pits', 0, 2000, 120), 'pit_size': ('Pit Size', 0.0, 2.0, 0.5), 'pit_spread': ('Pit Spread', 1.0, None, 6.0),
    'crack_amount': ('Cracks', 0, 32, 8), 'crack_length': ('Crack Length', 0.0, None, 4.0), 'crack_width': ('Crack Width (cm)', 0.0001, None, 1.0),
    'crack_meander': ('Crack Meander', 0.0, 2.0, None), 'crack_fork': ('Crack Fork', 0.0, 1.0, None), 'crack_pits': ('Pits on Cracks', 0.0, 1.0, None),
    'flow': ('Downhill Flow', 0.0, 1.0, None), 'stretch': ('Downhill Stretch', 0.0, 3.0, 1.0),
    'thickness': ('Thickness (cm)', 0.0001, None, 5.0), 'min_bridge': ('Min Bridge (cm)', 0.0, None, 1.0),
    'paint_size': ('Trail Size (cm)', 0.01, None, 10.0), 'paint_spacing': ('Trail Spacing', 0.2, None, 4.0), 'paint_gaps': ('Trail Gaps', 0.0, 1.0, None),
    'paint_crack': ('Trail Crack', None, None, None), 'paint_snap': ('Projection Distance (cm)', 0.0001, None, 20.0),
    'paint_close': ('Auto Close', 0.0, 1.0, None), 'paint_single': ('Dot Size (cm)', 0.0, None, 10.0),
    'enable': ('Enable', None, None, None), 'avoid': ('Avoid', None, None, None),
}
for _field, _kind, _default, _description in EFFECT_META['rusty']:
    _name = 'rusty_' + _field
    if _field == 'mode':
        _prop = bpy.props.EnumProperty(name='Placement', items=(('0', 'Automatic', 'Scatter hole clusters'), ('1', 'Paint', 'Cut holes where you draw: dots, closed outlines and open trails')),
                                       default='0', description=_description, update=live.touch_prop)
    elif _field.startswith('down_'):
        _prop = bpy.props.FloatProperty(name=_field, default=_default, options={'HIDDEN'})
    else:
        _label, _lo, _hi, _soft = _RUSTY_UI[_field]
        _kw = dict(name=_label, default=_default, description=_description, update=live.touch_prop)
        if _lo is not None: _kw['min'] = _lo
        if _hi is not None: _kw['max'] = _hi
        if _soft is not None: _kw['soft_max'] = _soft
        _prop = (bpy.props.BoolProperty if _kind == 'bool' else bpy.props.IntProperty if _kind == 'int32_t' else bpy.props.FloatProperty)(**_kw)
    ProjectDamageProps.__annotations__[_name] = _prop


def _deform_touch(self, context):
    from . import impact_deform_tools
    impact_deform_tools.touch(self, context)


def _deform_origin_picked(self, context):
    from . import impact_deform_tools
    impact_deform_tools.origin_picked(self, context)


                                                                               
ProjectDamageProps.__annotations__['deform_enable'] = bpy.props.BoolProperty(name='Enable', default=False, update=_deform_touch, description='Dent the mesh at impact empties before every other effect (the Deformable Mesh Component deformation)')
ProjectDamageProps.__annotations__['deform_collection'] = bpy.props.PointerProperty(name='Impacts', type=bpy.types.Collection, update=_deform_touch, description='Every empty is an impact: radius = its display size x scale, strength = its project_damage_deform_strength property (default 1)')
ProjectDamageProps.__annotations__['deform_origin'] = bpy.props.PointerProperty(name='Deformation Origin', type=bpy.types.Object, update=_deform_origin_picked, description='Vertices are pushed toward this point (the OriginComponent). Empty = the object origin')
ProjectDamageProps.__annotations__['deform_protection'] = bpy.props.PointerProperty(name='Protection Boxes', type=bpy.types.Collection, update=_deform_touch, description='Cube empties: vertices inside never move, and nothing is pushed into them')
ProjectDamageProps.__annotations__['deform_strength'] = bpy.props.FloatProperty(name='Strength', default=1.0, min=0.0, soft_max=5.0, update=live.touch_prop, description='x every impact strength')
ProjectDamageProps.__annotations__['deform_max_deform'] = bpy.props.FloatProperty(name='Max Deform', default=0.75, min=0.0, max=1.0, update=live.touch_prop, description='MaxDeformPercentage: how far toward the origin a vertex may be pushed')
ProjectDamageProps.__annotations__['deform_total_amplitude'] = bpy.props.FloatProperty(name='Noise Amplitude', default=1.0, min=0.0, soft_max=5.0, update=live.touch_prop, description='TotalAmplitude: intensity of the noise layers')
ProjectDamageProps.__annotations__['deform_noise_factor'] = bpy.props.FloatProperty(name='Noise Factor', default=0.3, min=0.0, soft_max=2.0, update=live.touch_prop, description='NoiseFactor: share of the noise in the dent')
for _a in 'xyz':
    ProjectDamageProps.__annotations__['deform_groups_' + _a] = bpy.props.IntProperty(name='Groups ' + _a.upper(), default=8, min=1, max=128, update=live.touch_prop, description='Vertex groups along this axis (search acceleration only)')
ProjectDamageProps.__annotations__['deform_signature'] = bpy.props.IntProperty(name='Signature', default=0, options={'HIDDEN'})

                                                                
ProjectDamageProps.__annotations__['deform_origin_mode'] = bpy.props.EnumProperty(name='Origin', default='AUTO', update=_deform_touch, items=(
    ('AUTO', 'Auto', 'Every impact pushes straight into the surface under it (its own origin behind the surface). No empty needed'),
    ('EMPTY', 'Empty', 'Every vertex is pushed toward the Deformation Origin empty'),
    ('OBJECT', 'Object Origin', 'Every vertex is pushed toward the object origin')))
ProjectDamageProps.__annotations__['deform_origin_depth'] = bpy.props.FloatProperty(name='Origin Depth', default=1.0, min=0.05, soft_max=4.0, update=_deform_touch, description='Auto: how far behind the surface each origin sits, x the impact radius. Deeper = a wider, flatter push')
ProjectDamageProps.__annotations__['deform_smoothness'] = bpy.props.FloatProperty(name='Smoothness', default=0.0, min=0.0, max=1.0, subtype='FACTOR', update=_deform_touch, description='Smooth the dents along the surface: softer rims, calmer noise. 0 = the original look')
ProjectDamageProps.__annotations__['deform_noise_scale'] = bpy.props.FloatProperty(name='Noise Scale', default=1.0, min=0.0, soft_max=8.0, update=_deform_touch, description='Noise frequency: higher = more, finer ripples, lower = broad waves. 1 = the original look')
ProjectDamageProps.__annotations__['deform_protect_group'] = bpy.props.StringProperty(name='Protection Zone', default='', update=_deform_touch, description='Vertex group painted with the protected areas: weight 1 never moves, 0.5 moves half')
ProjectDamageProps.__annotations__['deform_protect_invert'] = bpy.props.BoolProperty(name='Invert', default=False, update=_deform_touch, description='Paint the areas that may dent instead of the protected ones')

ProjectDamageProps.__annotations__['hang_enable'] = bpy.props.BoolProperty(name='Enable', default=False, description='', update=live.touch_prop)
ProjectDamageProps.__annotations__['hang_seed'] = bpy.props.IntProperty(name='Seed', default=0, min=0, soft_max=20, description='', update=live.touch_prop)
ProjectDamageProps.__annotations__['hang_mode'] = bpy.props.EnumProperty(name='Holes', items=(('0','Auto','Own holes'),('1','Breakers','The linked Boolean breakers make the holes')), default='0', update=live.touch_prop)
ProjectDamageProps.__annotations__['hang_avoid'] = bpy.props.BoolProperty(name='Avoid', default=True, description='keep away from Avoid faces', update=live.touch_prop)
ProjectDamageProps.__annotations__['hang_amount'] = bpy.props.IntProperty(name='Amount', default=2, min=0, soft_max=20, description='holes (Auto)', update=live.touch_prop)
ProjectDamageProps.__annotations__['hang_thickness'] = bpy.props.FloatProperty(
    name='Thickness', default=100.0, min=1.0, max=1000.0, soft_max=400.0,
    subtype='PERCENTAGE', update=live.touch_prop,
    description='Maximum wall thickness for automatic through holes. 100% allows twice the opening diameter. Raise it for thicker walls without increasing the hole size')
ProjectDamageProps.__annotations__['hang_size_min'] = bpy.props.FloatProperty(name='Size Min', default=30.0, min=0.0, soft_max=120.0, description='smallest opening diameter (cm)', update=live.touch_prop)
ProjectDamageProps.__annotations__['hang_size_max'] = bpy.props.FloatProperty(name='Size Max', default=55.0, min=0.0, soft_max=220.0, description='largest opening diameter (cm)', update=live.touch_prop)
ProjectDamageProps.__annotations__['hang_jagged'] = bpy.props.FloatProperty(name='Jagged', default=0.6, min=0.0, soft_max=2.4, description='ragged outline', update=live.touch_prop)
ProjectDamageProps.__annotations__['hang_flare'] = bpy.props.FloatProperty(name='Flare', default=0.35, min=0.0, soft_max=1.4, description='wider at the hit face', update=live.touch_prop)
ProjectDamageProps.__annotations__['hang_roughness'] = bpy.props.FloatProperty(name='Roughness', default=1.2, min=0.0, soft_max=4.8, description='procedural wall noise (cm)', update=live.touch_prop)
ProjectDamageProps.__annotations__['hang_surf_mode'] = bpy.props.EnumProperty(name='Hole Surface', items=(('0','Procedural','Rough walls'),('1','Texture','Image displacement on top')), default='0', update=live.touch_prop)
ProjectDamageProps.__annotations__['hang_tex_path'] = bpy.props.StringProperty(name='Height Texture', subtype='FILE_PATH', description='grayscale height image; empty = bundled Concrete025', update=live.touch_prop)
ProjectDamageProps.__annotations__['hang_tex_scale'] = bpy.props.FloatProperty(name='Tex Scale', default=20.0, min=0.0, soft_max=80.0, description='texture tile (cm)', update=live.touch_prop)
ProjectDamageProps.__annotations__['hang_tex_strength'] = bpy.props.FloatProperty(name='Tex Strength', default=1.0, min=0.0, soft_max=4.0, description='texture depth (cm)', update=live.touch_prop)
ProjectDamageProps.__annotations__['hang_tex_offset_x'] = bpy.props.FloatProperty(name='Tex Offset X', default=0.0, min=0.0, soft_max=1.0, description='', update=live.touch_prop)
ProjectDamageProps.__annotations__['hang_tex_offset_y'] = bpy.props.FloatProperty(name='Tex Offset Y', default=0.0, min=0.0, soft_max=1.0, description='', update=live.touch_prop)
ProjectDamageProps.__annotations__['hang_tex_offset_z'] = bpy.props.FloatProperty(name='Tex Offset Z', default=0.0, min=0.0, soft_max=1.0, description='', update=live.touch_prop)
ProjectDamageProps.__annotations__['hang_tex_edge'] = bpy.props.FloatProperty(name='Tex Edge', default=0.0, min=0.0, soft_max=1.0, description='mesh edge (cm), 0 = scale / 60', update=live.touch_prop)
ProjectDamageProps.__annotations__['hang_bars_enable'] = bpy.props.BoolProperty(name='Bars Enable', default=True, description='reinforcement bars', update=live.touch_prop)
ProjectDamageProps.__annotations__['hang_bar_spacing'] = bpy.props.FloatProperty(name='Bar Spacing', default=15.0, min=0.0, soft_max=60.0, description='bar spacing (cm)', update=live.touch_prop)
ProjectDamageProps.__annotations__['hang_bar_radius'] = bpy.props.FloatProperty(name='Bar Radius', default=0.6, min=0.0, soft_max=2.4, description='bar radius (cm)', update=live.touch_prop)
ProjectDamageProps.__annotations__['hang_bar_depth'] = bpy.props.FloatProperty(name='Bar Depth', default=0.5, min=0.0, soft_max=2.0, description='bar layer depth, share of the wall', update=live.touch_prop)
ProjectDamageProps.__annotations__['hang_bar_broken'] = bpy.props.FloatProperty(name='Bar Broken', default=0.65, min=0.0, soft_max=2.6, description='chance a crossing bar is snapped', update=live.touch_prop)
ProjectDamageProps.__annotations__['hang_droop'] = bpy.props.FloatProperty(name='Droop', default=0.7, min=0.0, soft_max=2.8, description='how far loose bar ends bend down', update=live.touch_prop)
ProjectDamageProps.__annotations__['hang_bar_bow'] = bpy.props.FloatProperty(name='Bar Bow', default=0.6, min=0.0, soft_max=2.4, description='sag of intact bars', update=live.touch_prop)
ProjectDamageProps.__annotations__['hang_cables_enable'] = bpy.props.BoolProperty(name='Cables Enable', default=True, description='cables', update=live.touch_prop)
ProjectDamageProps.__annotations__['hang_cable_amount'] = bpy.props.IntProperty(name='Cable Amount', default=3, min=0, soft_max=24, description='cables per hole', update=live.touch_prop)
ProjectDamageProps.__annotations__['hang_cable_radius'] = bpy.props.FloatProperty(name='Cable Radius', default=0.18, min=0.0, soft_max=1.4, description='cable radius (cm)', update=live.touch_prop)
ProjectDamageProps.__annotations__['hang_cable_length'] = bpy.props.FloatProperty(name='Cable Length', default=1.0, min=0.0, soft_max=4.0, description='cable length, x hole radius', update=live.touch_prop)
ProjectDamageProps.__annotations__['hang_cable_sag'] = bpy.props.FloatProperty(name='Cable Sag', default=0.5, min=0.0, soft_max=2.0, description='how floppy the cables are', update=live.touch_prop)
ProjectDamageProps.__annotations__['hang_cable_loops'] = bpy.props.FloatProperty(name='Cable Loops', default=0.35, min=0.0, soft_max=1.4, description='share of cables slung as loops', update=live.touch_prop)
ProjectDamageProps.__annotations__['hang_sides'] = bpy.props.IntProperty(name='Sides', default=8, min=0, soft_max=32, description='tube sides', update=live.touch_prop)
ProjectDamageProps.__annotations__['hang_gravity_x'] = bpy.props.FloatProperty(name='Gravity X', default=0.0, options={'HIDDEN'}, update=live.touch_prop)
ProjectDamageProps.__annotations__['hang_gravity_y'] = bpy.props.FloatProperty(name='Gravity Y', default=0.0, options={'HIDDEN'}, update=live.touch_prop)
ProjectDamageProps.__annotations__['hang_gravity_z'] = bpy.props.FloatProperty(name='Gravity Z', default=-1.0, options={'HIDDEN'}, update=live.touch_prop)

for _module in ('hit','glass'):
    ProjectDamageProps.__annotations__[_module+'_through_enable']=bpy.props.BoolProperty(
        name='Through Asset',default=False,update=live.touch_prop,
        description='Alternative to the ordinary asset, sharing its impact and material settings')
    ProjectDamageProps.__annotations__[_module+'_through']=bpy.props.BoolProperty(
        name='Pass Through',default=True,update=live.touch_prop,
        description='Detach impact plates or glass shards as loose mesh islands')
    ProjectDamageProps.__annotations__[_module+'_through_gap']=bpy.props.FloatProperty(
        name='Piece Gap (cm)',default=.15 if _module=='hit' else 0.,min=0.,soft_max=5.,update=live.touch_prop,
        description='Gap between pieces; for Glass, zero gives a hairline split so the shards fit together with no visible gap. Follows Master Scale')
    ProjectDamageProps.__annotations__[_module+'_through_min_piece']=bpy.props.FloatProperty(
        name='Min Piece (%)',default=.001 if _module=='hit' else .02,min=0.,max=10.,precision=4,update=live.touch_prop,
        description='Remove slivers below this percentage of the smallest source-part volume')
ProjectDamageProps.__annotations__['hit_through_max_wall']=bpy.props.FloatProperty(
    name='Max Through Wall (cm)',default=40.,min=0.,soft_max=200.,update=live.touch_prop,
    description='Walls up to this thickness split through; thicker walls use Chunk Depth instead')
ProjectDamageProps.__annotations__['hit_chunk_depth']=bpy.props.FloatProperty(
    name='Chunk Depth',default=.35,min=.001,soft_max=2.,update=live.touch_prop,
    description='Chunk depth as a multiple of impact radius for thick walls')
ProjectDamageProps.__annotations__['hit_through_remove']=bpy.props.FloatProperty(
    name='Remove Chunks',default=0.,min=0.,max=1.,update=live.touch_prop,
    description='Fraction of hit-zone chunks removed entirely, from zero to one')
ProjectDamageProps.__annotations__['glass_through_rings']=bpy.props.IntProperty(
    name='Shard Rings',default=2,min=1,soft_max=10,update=live.touch_prop,
    description='Number of concentric crack rings forming shard rows, limited by Ring Count; last ring closes')
ProjectDamageProps.__annotations__['glass_through_fall']=bpy.props.FloatProperty(
    name='Remove Shards',default=0.,min=0.,max=1.,update=live.touch_prop,
    description='Fraction of glass shards knocked out completely, from zero to one')
                                             
ProjectDamageProps.__annotations__['glass_pass_through']=bpy.props.BoolProperty(
    name='Make Pass Through',default=False,update=live.touch_prop,
    description='Hit Glass / Glass Drawn: the cracks around the hole cut through the mesh, so the shards between them come apart as loose pieces')
ProjectDamageProps.__annotations__['glass_fill_hole']=bpy.props.BoolProperty(
    name='Fill Hole',default=False,update=live.touch_prop,
    description='Restore the original glass removed by the opening and divide it into loose pieces using Cell Fracture')
ProjectDamageProps.__annotations__['glass_fill_count']=bpy.props.IntProperty(
    name='Shards',default=40,min=2,max=500,soft_max=150,update=live.touch_prop,
    description='Number of Cell Fracture shards per connected opening')
ProjectDamageProps.__annotations__['glass_fill_origin']=bpy.props.PointerProperty(
    name='Density Centre',type=bpy.types.Object,poll=lambda self,obj: obj.type=='EMPTY',update=live.touch_prop,
    description='Empty that places the density centre in the nearest glass opening. Clear it to use the automatic centre')
ProjectDamageProps.__annotations__['glass_fill_center']=bpy.props.FloatProperty(
    name='Denser at Centre',default=.7,min=0.,max=1.,update=live.touch_prop,
    description='Low: more even shard sizes. High: smaller shards cluster near the centre and larger ones reach the rim')
for _name,_default,_max in (('min_volume',.05,10.),('min_thickness',.3,5.),('chunk_size',0.,100.),('chunk_jitter',.5,1.)):
    _meta=next(row for row in EFFECT_META['debris'] if row[0]==_name)
    _kw=dict(name=_name.replace('_',' ').title()+(' (cm³)' if _name=='min_volume' else ' (cm)' if _name in {'min_thickness','chunk_size'} else ''),
             default=_default,min=0.,soft_max=_max,description=_meta[3],update=live.touch_prop)
    if _name=='chunk_jitter':_kw['max']=1.
    ProjectDamageProps.__annotations__['debris_'+_name]=bpy.props.FloatProperty(**_kw)
ProjectDamageProps.__annotations__['debris_max_pieces']=bpy.props.IntProperty(name='Max Pieces',default=400,min=1,max=100000,
    description='Maximum generated chunk objects; keep the largest pieces when the cap is exceeded',update=live.touch_prop)
ProjectDamageProps.__annotations__['debris_inner_uv_scale']=bpy.props.FloatProperty(
    name='Inner UV Scale (cm)',default=10.0,min=.001,soft_max=100.,update=live.touch_prop,
    description='Centimeters per UV unit on broken inner faces; follows Master Scale. Outer faces inherit the active source UV map when UV preservation is enabled')
ProjectDamageProps.__annotations__['debris_smooth_angle']=bpy.props.FloatProperty(
    name='Smooth Angle (degrees)',default=60.,min=0.,max=180.,update=live.touch_prop,
    description='Blend exterior normals across source faces within this angle; broken interior faces stay flat. 60 keeps box corners sharp and cylinder sides smooth')
for _name,_default,_min,_max in (
    ('pieces',40,1,1500),('max_pieces',1500,1,10000),('glass_spokes',16,3,256)):
    ProjectDamageProps.__annotations__['rbf_'+_name]=bpy.props.IntProperty(
        name=_name.replace('_',' ').title(),default=_default,min=_min,max=_max,update=live.touch_prop,
        description=next(r[3] for r in EFFECT_META['rbf'] if r[0]==_name))
for _name,_default,_min,_max in (
    ('gap',0.,0.,1000.),('size_variation',-1.,-1.,1.),('min_piece',.0002,0.,.5),
    ('cut_spacing',0.,0.,10000.),('impact_radius',20.,.001,10000.),('impact_density',8.,1.,100.),
    ('rubble',-1.,-1.,1.),('grain_x',0.,-1.,1.),('grain_y',0.,-1.,1.),('grain_z',1.,-1.,1.),
    ('grain_stretch',5.,1.,100.),('weave_stretch',2.,1.,100.),('glass_ring_growth',1.9,1.01,10.),
    ('inner_uv_scale',10.,.001,10000.),('smooth_angle',60.,0.,180.)):
    ProjectDamageProps.__annotations__['rbf_'+_name]=bpy.props.FloatProperty(
        name=_name.replace('_',' ').title(),default=_default,min=_min,max=_max,update=live.touch_prop,
        description=next(r[3] for r in EFFECT_META['rbf'] if r[0]==_name))
ProjectDamageProps.__annotations__['rbf_density']=bpy.props.FloatProperty(name='Density (kg/m³)',
    default=0.,min=0.,soft_max=8000.,update=live.touch_prop,
    description='Mass = physical piece volume × density; 0 uses the selected material density')
ProjectDamageProps.__annotations__['rbf_physics']=bpy.props.BoolProperty(name='Legacy Physics',default=False,
    update=live.touch_prop,description='Unused compatibility value; Rigid Body Fracture creates geometry only')
ProjectDamageProps.__annotations__['rbf_impact_collection']=bpy.props.PointerProperty(name='Impact Points',type=bpy.types.Collection,update=live.touch_prop)
for _name in ('rbf_cuts_drawing','rbf_impacts_drawing'):
    ProjectDamageProps.__annotations__[_name]=bpy.props.PointerProperty(name='Drawing',type=bpy.types.Object,poll=_paint_drawing_poll,update=live.touch_prop)
                                                                                    
def _cellf_meta(name):
    return next(r for r in EFFECT_META['cellf'] if r[0]==name)
for _name,_items in (('material',('Wood','Glass','Concrete','Rock','Soil')),
                     ('planar',('Auto','Volume','Planar')),
                     ('stroke_mode',('Crack Lines','Shatter Zone','Both'))):
    _row=_cellf_meta(_name)
    ProjectDamageProps.__annotations__['cellf_'+_name]=bpy.props.EnumProperty(
        name=_name.replace('_',' ').title(),default=str(_row[2]),update=live.touch_prop,description=_row[3],
        items=tuple((str(i),label,label) for i,label in enumerate(_items)))
for _name in ('enable','keep_remainder'):
    _row=_cellf_meta(_name)
    ProjectDamageProps.__annotations__['cellf_'+_name]=bpy.props.BoolProperty(
        name='Enable' if _name=='enable' else 'Intact Remainder',default=_row[2],update=live.touch_prop,description=_row[3])
for _name,_min,_max in (('seed',0,100000),('pieces',1,2000),('max_pieces',2,5000),('glass_spokes',3,64),('guidance_amount',0,2000)):
    _row=_cellf_meta(_name)
    ProjectDamageProps.__annotations__['cellf_'+_name]=bpy.props.IntProperty(
        name=_name.replace('_',' ').title(),default=_row[2],min=_min,max=_max,update=live.touch_prop,description=_row[3])
for _name,_label,_min,_max,_soft in (
    ('gap','Gap (cm)',0.,1000.,5.),('size_variation','Size Variation',-1.,1.,1.),
    ('roughness','Roughness',-1.,1.5,1.5),('rough_scale','Rough Scale',.05,10.,3.),('detail','Detail',.01,4.,2.),
    ('planar_ratio','Planar Ratio',.01,1.,.5),
    ('prox_radius','Proximity Radius (cm)',.001,100000.,200.),('prox_density','Proximity Density',1.,100.,20.),
    ('prox_falloff','Proximity Falloff',.25,8.,4.),('rubble','Rubble',-1.,1.,1.),('remainder_reach','Remainder Reach',.1,20.,5.),
    ('stroke_spacing','Crack Spacing (cm)',0.,10000.,50.),
    ('guidance_radius','Radius (cm)',.001,100000.,200.),
    ('guidance_depth','Inward Reach (cm)',0.,100000.,200.),
    ('guidance_falloff','Concentration',.25,8.,4.),
    ('guidance_distance','Projection Distance (cm)',.001,100000.,20.),
    ('grain_x','Grain X',-1.,1.,1.),('grain_y','Grain Y',-1.,1.,1.),('grain_z','Grain Z',-1.,1.,1.),
    ('grain_stretch','Grain Stretch',1.,100.,20.),('splinter','Splinter',0.,2.,2.),
    ('glass_ring_growth','Glass Ring Growth',1.15,10.,4.),
    ('glass_branch','Glass Branching',0.,1.,1.),('glass_wander','Glass Wander',0.,2.,2.),('glass_hole','Glass Hole',0.,1.,1.),
    ('inner_uv_scale','Inner UV Scale (cm)',.001,10000.,100.),('smooth_angle','Smooth Angle (degrees)',0.,180.,180.)):
    _row=_cellf_meta(_name)
    ProjectDamageProps.__annotations__['cellf_'+_name]=bpy.props.FloatProperty(
        name=_label,default=_row[2],min=_min,max=_max,soft_max=_soft,update=live.touch_prop,description=_row[3])
ProjectDamageProps.__annotations__['cellf_prox_collection']=bpy.props.PointerProperty(
    name='Proximity',type=bpy.types.Collection,update=live.touch_prop,
    description='Empties and objects in this collection make the pieces smaller near them. An empty reaches its display size (with Use Empty Size), other objects reach Proximity Radius around their surface points')
ProjectDamageProps.__annotations__['cellf_prox_use_size']=bpy.props.BoolProperty(
    name='Use Empty Size',default=True,update=live.touch_prop,
    description="An empty's display size x scale is its reach; off uses Proximity Radius for every object")
ProjectDamageProps.__annotations__['cellf_drawing']=bpy.props.PointerProperty(
    name='Drawing',type=bpy.types.Object,poll=_paint_drawing_poll,update=live.touch_prop)
ProjectDamageProps.__annotations__['cellf_surface_drawing']=bpy.props.PointerProperty(
    name='Surface Cut Drawing',type=bpy.types.Object,poll=_paint_drawing_poll,update=live.touch_prop,
    description='Split only pieces directly touched by the strokes; deeper rows stay intact')
ProjectDamageProps.__annotations__['cellf_guidance_drawing']=bpy.props.PointerProperty(
    name='Guidance Drawing',type=bpy.types.Object,poll=_paint_drawing_poll,update=live.touch_prop,
    description='Density guide, separate from the cut drawings. Smaller pieces gather around these strokes; they do not cut an exact seam')
def _cell_layer_changed(self, context):
    from . import paint
    paint.cell_layer_changed(self, context)

ProjectDamageProps.__annotations__['cellf_curve_type']=bpy.props.EnumProperty(
    name='Drawing Layer',items=(('GUIDANCE','Guidance','Primary layer: add smaller cells near this drawing before applying both cut layers'),('THROUGH','Knife Through','Split crossed cells through the depth; incomplete strokes leave cells unchanged'),('SURFACE','Knife Surface','Split only directly touched cells when the drawing crosses them'),('PLANE','Cut Plane','Cut shards with an editable mesh plane; the most stable cutting mode')),default='THROUGH',
    update=_cell_layer_changed,
    description='Select a drawing layer or Cut Plane. Guidance, both knife layers and the assigned Cut Plane remain active together')
from . import cell_cut_plane as _cell_cut_plane
_cell_cut_plane.register_props(ProjectDamageProps)
ProjectDamageProps.__annotations__['cellf_knife_width']=bpy.props.FloatProperty(
    name='Knife Width (mm)', default=1.0, min=0.0, soft_max=5.0, max=100.0,
    update=live.touch_prop,
    description='Width of the hand-drawn glass cut. Zero keeps a hairline split; larger values open a visible seam')
ProjectDamageProps.__annotations__['cellf_draw_tool']=bpy.props.EnumProperty(
    name='Drawing Tool',items=(('CUT','Draw Cut','Cut cells along the drawing'),('GUIDANCE','Guidance','Concentrate smaller cells near a drawing without cutting along it')),default='CUT')
ProjectDamageProps.__annotations__['cellf_cut_extend']=bpy.props.BoolProperty(
    name='Complete Short Cuts (Legacy)',default=False,update=live.touch_prop,
    description='Legacy saved setting, no longer used. Both values preserve finite stroke ends; a stroke must divide a cell before any change is accepted')
ProjectDamageProps.__annotations__['cellf_draw_mode']=bpy.props.EnumProperty(
    name='Draw Cuts',items=(('THROUGH','Through Cut','Project the stroke through the object; stop on earlier crossing cuts'),
                           ('SURFACE','Surface Cut','Split only the pieces the stroke directly touches')),
    default='THROUGH')
for _name in ('bevel_drawing','scratch_drawing','bullet_drawing','ground_drawing','brick_drawing','fissure_drawing','glass_holes_drawing','glass_impacts_drawing','rusty_drawing'):
    ProjectDamageProps.__annotations__[_name] = bpy.props.PointerProperty(name=_name.replace('_',' ').title(), type=bpy.types.Object, poll=_paint_drawing_poll, update=live.touch_prop)
for _name in ('metal_collection','metal_crush_collection'):
    ProjectDamageProps.__annotations__[_name] = bpy.props.PointerProperty(name=_name.replace('_',' ').title(),type=bpy.types.Collection,update=live.touch_prop)
from . import damaged_bevel as _damaged_bevel
_damaged_bevel.register_props(ProjectDamageProps)
ProjectDamageProps.__annotations__['bevel_preset'] = bpy.props.EnumProperty(name='Profile Defaults',items=tuple((str(i),v,'') for i,v in enumerate(('Custom','Dwarf Wall','Round Top','Flat Top','Dropper','Half Batter','Full Batter','Bull Nose','Channel'))),default='0',update=_bevel_profile_defaults)
ProjectDamageProps.__annotations__['blast_collection'] = bpy.props.PointerProperty(name='Blast Empties',type=bpy.types.Collection,update=live.touch_prop)


for _name, _label in (('wallpaper_material', 'Printed Paper'),
                       ('wallpaper_back_material', 'Paper Backing'),
                       ('wall_material', 'Exposed Wall')):
    ProjectDamageProps.__annotations__[_name] = bpy.props.PointerProperty(
        name=_label, type=bpy.types.Material, update=live.touch_prop,
        description='Optional material override for this wallpaper region; empty preserves the default damage material')


ProjectDamageProps.__annotations__["brick_split_materials"] = bpy.props.BoolProperty(
    name="Separate Material Regions", default=True, update=live.touch_prop)

from . import rebar
ProjectDamageProps.__annotations__['rebar_existing_damage'] = bpy.props.BoolProperty(
    name='Use Existing Damage', default=False, update=live.touch_prop,
    description='Fill the existing cut volumes of active damage effects without making another cut or Helper')
_REBAR_ENUMS={'material':rebar.REBAR_FILL_ITEMS,'plank_dir':rebar.REBAR_PLANK_DIR_ITEMS,'mode':rebar.REBAR_MODE_ITEMS,'fit':rebar.REBAR_FIT_ITEMS,'type':rebar.REBAR_TYPE_ITEMS,'layout':rebar.REBAR_LAYOUT_ITEMS,'axis':rebar.REBAR_AXIS_ITEMS,'side':rebar.REBAR_SIDE_ITEMS,'outer':rebar.REBAR_OUTER_ITEMS}
for _field,_kind,_default,_length in rebar.FIELDS:
    _kw=dict(name=_field.replace('_',' ').title()+(' (cm)' if _length else ''),default=_default,update=live.touch_prop)
    if _field=='detail':_kw['min']=0.05
    if _field in _REBAR_ENUMS:
        _kw['default']=str(_default);_prop=bpy.props.EnumProperty(**_kw,items=_REBAR_ENUMS[_field])
    elif _kind=='bool':_prop=bpy.props.BoolProperty(**_kw)
    elif _kind=='int32_t':_prop=bpy.props.IntProperty(**_kw,min=0)
    else:_prop=bpy.props.FloatProperty(**_kw,soft_min=0,soft_max=10 if _field=='amount' else max(2,_default*4))
    ProjectDamageProps.__annotations__['rebar_'+('fill' if _field=='material' else _field)]=_prop
ProjectDamageProps.__annotations__['rebar_drawing']=bpy.props.PointerProperty(name='Bar Curve',type=bpy.types.Object,poll=lambda self,obj:obj.type=='CURVE',update=live.touch_prop)
from . import boolean_ui_controls
boolean_ui_controls.add_properties(ProjectDamageProps)
from . import displacement_shader
displacement_shader.add_properties(ProjectDamageProps, live.touch_prop)
from . import chips as _chips
_chips.add_properties(ProjectDamageProps)
from . import asset_updates
asset_updates.add_properties(ProjectDamageProps)
ProjectDamageProps.__annotations__['rebar_brick_library']=bpy.props.StringProperty(name='Brick Models',subtype='FILE_PATH',description='Optional .blend library containing mesh objects named brick. Empty uses the bundled library. Original model materials are retained unless a Brick Material override is assigned',update=live.touch_prop)
ProjectDamageProps.__annotations__['rebar_brick_collection']=bpy.props.PointerProperty(name='Brick Collection',type=bpy.types.Collection,description='Use mesh objects from this collection and its child collections as custom bricks. Names do not matter. Originals stay untouched. Empty uses bundled brick models',update=live.touch_prop)
ProjectDamageProps.__annotations__['rebar_brick_3d']=bpy.props.BoolProperty(name='3D Bricks (Experimental)',default=False,description='Replace Boolean fill bricks with detailed library models, scaled and cut to each brick. Mortar is disabled. Dense models can slow Live Update',update=live.touch_prop)
ProjectDamageProps.__annotations__['rebar_preset']=bpy.props.EnumProperty(name='Rebar Defaults',items=rebar.REBAR_PRESET_ITEMS,default='0',update=rebar._apply_rebar_preset)
ProjectDamageProps.__annotations__['rebar_material']=bpy.props.PointerProperty(name='Rebar Material',type=bpy.types.Material,update=live.touch_prop)
from . import break_surface
break_surface.add_properties(ProjectDamageProps, ProjectDamageBreakerProps)
ProjectDamageProps.__annotations__['hang_cable_material']=bpy.props.PointerProperty(name='Cable Material',type=bpy.types.Material,update=live.touch_prop)
ProjectDamageProps.__annotations__['plaster_master_scale']=bpy.props.FloatProperty(name='Master Scale',default=100.,min=.01,soft_max=400.,subtype='PERCENTAGE',update=live.touch_prop)

from . import material_controls
material_controls.register_properties(ProjectDamageProps, live.touch_prop)

for _field,_default,_description in (
    ('cavity_smooth',.6,'Smooth the cut floor behind wooden laths without smoothing plaster rims or holes. Raise to remove spikes; 0 restores the old cavity'),
    ('rim_smooth',.5,'Anti-alias plaster and hole walls at the cutter-grid scale. Raise to reduce spikes; increase Detail to retain fine edges; 0 restores old walls'),
    ('lath_optimize',.5,'Remove redundant plank cross-sections while retaining bends and splinters. Raise for fewer polygons; 0 retains all sections')):
    ProjectDamageProps.__annotations__['lath_'+_field]=bpy.props.FloatProperty(name=_field.replace('_',' ').title(),default=_default,min=0,max=1,description=_description,update=live.touch_prop)

for _field,_default,_maximum,_description in (
    ('wall_slope',72.,90.,'Maximum broken-wall slope in degrees; lower softens walls; zero or 90 disables'),
    ('cavity_slope',40.,90.,'Maximum cavity-floor slope in degrees; lower gives softer plaster keys; zero or 90 disables'),
    ('hole_noise_scale',.3,2.,'Hole outline noise feature size relative to patch size'),
    ('hole_detail',.35,1.,'Fine hole outline noise; zero gives a smoother single octave')):
    ProjectDamageProps.__annotations__['lath_'+_field]=bpy.props.FloatProperty(name=_field.replace('_',' ').title(),default=_default,min=0,max=_maximum,description=_description,update=live.touch_prop)

from . import riven_lath
riven_lath.add_properties(ProjectDamageProps, live.touch_prop)
from . import stud_framing
stud_framing.add_properties(ProjectDamageProps, live.touch_prop)


@bpy.app.handlers.persistent
def _migrate_removed_brick_plane(_dummy=None):
                                                                            
                                                                                 
    objects = getattr(bpy.data, 'objects', None)
    if objects is None:
        return False                                                          
    for obj in objects:
        saved = getattr(obj, 'project_damage', None) if obj.type == 'MESH' else None
        if saved is not None:saved['wallpaper_paint_enable']=saved.wallpaper_drawing is not None
        if saved is not None and saved.peeling_enable and not saved.get('_peeling_resolution_version', 0):
            if saved.get('peeling_resolution') is None and saved.get('peeling_detail') is None:
                saved['peeling_detail']=24
            saved['_peeling_resolution_version']=1
        if saved is not None and saved.get('brick_brick_mode') in (2, '2'):
            saved['brick_brick_mode'] = 0                              
    return True


def _deferred_brick_mode_migration():
    return None if _migrate_removed_brick_plane() else 0.1


def _registered_property_class(name):
    actual = bpy.types.PropertyGroup.bl_rna_get_subclass_py(name, None)
    if actual is not None and actual.__module__ != __name__:
        raise RuntimeError('Project Damage RNA name is owned by another module: ' + name)
    return actual


def _remove_migration_handlers():
    for callback in tuple(bpy.app.handlers.load_post):
        if (getattr(callback, '__module__', None) == __name__
                and getattr(callback, '__name__', None) == '_migrate_removed_brick_plane'):
            bpy.app.handlers.load_post.remove(callback)


# подключаем свойства и действия этого модуля к Blender
def register():
                                                                                                
                                                                               
                                                     
    classes = (ProjectDamageBreakerProps, ProjectDamageBreakerLink,
               ProjectDamageGuideLink, ProjectDamageProps)
    created, pointers, actual = [], [], {}
    try:
        for cls in classes:
            existing = _registered_property_class(cls.__name__)
            if existing is None:
                if cls is ProjectDamageProps:
                    cls.__annotations__['breakers'] = bpy.props.CollectionProperty(type=actual['ProjectDamageBreakerLink'])
                    cls.__annotations__['guides'] = bpy.props.CollectionProperty(type=actual['ProjectDamageGuideLink'])
                bpy.utils.register_class(cls)
                created.append(cls)
                existing = cls
                                                                               
                                                                                
            actual[cls.__name__] = existing
        for prop, name in (('project_damage_breaker','ProjectDamageBreakerProps'),
                           ('project_damage','ProjectDamageProps')):
            if not hasattr(bpy.types.Object, prop):
                setattr(bpy.types.Object, prop, bpy.props.PointerProperty(type=actual[name]))
                pointers.append(prop)
    except Exception:
        for prop in reversed(pointers):delattr(bpy.types.Object, prop)
        for cls in reversed(created):bpy.utils.unregister_class(cls)
        raise
    if not _migrate_removed_brick_plane() and not bpy.app.timers.is_registered(_deferred_brick_mode_migration):
        bpy.app.timers.register(_deferred_brick_mode_migration, first_interval=0.1)
    _remove_migration_handlers()
    bpy.app.handlers.load_post.append(_migrate_removed_brick_plane)


# убираем подключения модуля при выключении аддона
def unregister():
    if bpy.app.timers.is_registered(_deferred_brick_mode_migration):
        bpy.app.timers.unregister(_deferred_brick_mode_migration)
    _remove_migration_handlers()
    for prop in ('project_damage','project_damage_breaker'):
        if hasattr(bpy.types.Object, prop):delattr(bpy.types.Object, prop)
    for name in ('ProjectDamageProps','ProjectDamageGuideLink',
                 'ProjectDamageBreakerLink','ProjectDamageBreakerProps'):
        actual = _registered_property_class(name)
        if actual is not None:bpy.utils.unregister_class(actual)



                                                                          
ProjectDamageProps.__annotations__['fabric_solid_bevel']=bpy.props.FloatProperty(name='Edge Bevel',default=0.8,min=0.0,max=1.0,subtype='FACTOR',update=live.touch_prop,description='Fabric (Solidify): rounds the torn rim of every hole on both faces of the cloth. 1 = fully round edge (half the cloth thickness), 0 = sharp edges')
ProjectDamageProps.__annotations__['fabric_solid_bevel_segments']=bpy.props.IntProperty(name='Bevel Segments',default=3,min=1,max=12,update=live.touch_prop,description='Fabric (Solidify): smoothness of the rounded rim (rings per quarter circle)')
ProjectDamageProps.__annotations__['fissure_master_scale']=bpy.props.FloatProperty(name='Master Scale',default=20.0,min=1.0,max=400.0,soft_min=1.0,soft_max=400.0,subtype='PERCENTAGE',update=live.touch_prop,description='Ground Fissures: scales every fissure size (width, depth, slabs, branches, rubble) on top of the global Master Scale. Lower it for small objects')


                                                                          
from . import boolean_break as _boolean_break


def _break_variant_get(self):
    return _boolean_break.variant_get(self)


def _break_variant_set(self, value):
    _boolean_break.variant_set(self, value)


def _break_objects_update(self, context):
    _boolean_break.break_objects_changed(self, context)


ProjectDamageBreakerLink.__annotations__['from_collection']=bpy.props.BoolProperty(default=False,options={'HIDDEN'})
ProjectDamageProps.__annotations__['break_variant']=bpy.props.EnumProperty(name='Break Type',items=_boolean_break.VARIANT_ITEMS,get=_break_variant_get,set=_break_variant_set,description='What sits inside the break. All types share the breakers; switching keeps every setting')
ProjectDamageProps.__annotations__['break_objects']=bpy.props.PointerProperty(name='Boolean Objects',type=bpy.types.Collection,poll=lambda self,coll:self.id_data is None or self.id_data.name not in coll.all_objects,update=_break_objects_update,description="Every mesh in this collection (and its child collections) cuts this object, like the Boolean modifier's Collection operand. They show in the breaker list with a collection icon. A collection holding this object itself cannot be picked")
ProjectDamageProps.__annotations__['break_separate']=bpy.props.BoolProperty(name='Separate Fragments',default=False,update=live.touch_prop,description='Every loose part the breakers cut off becomes its own object (origin at its centre of mass, no parent); the main body stays the result. Apply All turns them into ordinary objects')
ProjectDamageProps.__annotations__['break_separate_min']=bpy.props.FloatProperty(name='Min Size (cm)',default=1.0,min=0.0,soft_max=50.0,update=live.touch_prop,description='Loose parts smaller than this stay with the main body instead of becoming objects. Follows Master Scale')


                                                   
from . import pipes as _pipes
for _name, _prop in _pipes.PROPS.items():
    ProjectDamageProps.__annotations__[_name] = _prop


                                                      
from . import cables as _cables
for _name, _prop in _cables.PROPS.items():
    ProjectDamageProps.__annotations__[_name] = _prop


                               
from . import concrete_pits as _concrete_pits
for _name, _prop in _concrete_pits.PROPS.items():
    ProjectDamageProps.__annotations__[_name] = _prop
_register_before_pits = register
_unregister_before_pits = unregister


# подключаем свойства и действия этого модуля к Blender
def register():
    _register_before_pits()
    _concrete_pits.register()


# убираем подключения модуля при выключении аддона
def unregister():
    _concrete_pits.unregister()
    _unregister_before_pits()

                                                                                
ProjectDamageProps.__annotations__['bullet_halo_enable'].keywords['default']=False
ProjectDamageProps.__annotations__['bullet_material'].keywords['default']='2'
ProjectDamageProps.__annotations__['foam_enable']=bpy.props.BoolProperty(name='Expanding Foam',default=False,update=live.touch_prop)
for _key, _label in (('weld','Welded Edges'),('grass','Grass Scatter'),('gravel','Gravel Scatter')):
    ProjectDamageProps.__annotations__[_key+'_enable'] = bpy.props.BoolProperty(name=_label, default=False, update=live.touch_prop)
    ProjectDamageProps.__annotations__[_key+'_drawing'] = bpy.props.PointerProperty(name='Drawing', type=bpy.types.Object, poll=lambda self, ob: ob.type in {'GREASEPENCIL','GPENCIL','CURVE'}, update=live.touch_prop)

                                                                           
from re import sub as _pd_rename_label
for _pd_property in ProjectDamageProps.__annotations__.values():
    _pd_keywords=getattr(_pd_property,'keywords',None)
    if _pd_keywords is not None and 'name' in _pd_keywords:
        _pd_keywords['name']=_pd_rename_label(r'\bCover\b','Deepness',_pd_keywords['name'])

                                                                              
                                                                            
for _name,_value in {
    'corner_prob':50.,'corner_style':'0','corner_depth_min':3.,'corner_depth_max':5.,
    'corner_size_min':12.,'corner_size_max':24.,'build_shape':'0','build_coverage':50.,
    'fract_glass_crush':0.,'plaster_max_tris':500000,
    'ground_rim_fade':4.4,'ground_pothole_chance':.2,'ground_pothole_depth':3.,
    'ground_rubble_amount':75,'ground_rubble_size_min':1.,'ground_rubble_size_max':2.,
    'ground_crack_width':.2,'ground_crack_depth':.1,'ground_crack_amount':3,
}.items():
    ProjectDamageProps.__annotations__[_name].keywords['default']=_value
ProjectDamageProps.__annotations__['hit_layer_thickness'].keywords.update(soft_max=10.,max=10.)
ProjectDamageProps.__annotations__['hit_plate_size'].keywords.update(min=.01,soft_min=.01)


                                                                 
ProjectDamageProps.__annotations__['fabric_tear_style']=bpy.props.EnumProperty(name='Tear Shape',default='1',update=live.touch_prop,items=(('0','Classic','The old shapes: ragged lenses, split into holes with fabric bridges'),('1','Realistic','Tears that follow the weave: rips along a yarn, corner (L) tears with a folded flap, worn holes with yarns still spanning them, punctures; the rim steps along the yarns and the fringe points along them')),description='Fabric Classic and Solidify: the shape of the procedural tears')
ProjectDamageProps.__annotations__['fabric_tear_rip']=bpy.props.FloatProperty(name='Rips',default=0.4,min=0.0,max=1.0,subtype='FACTOR',update=live.touch_prop,description='Realistic: share of rips - straight tears along one yarn (or a bias tear), opened by the pulled edges, with hairline tips')
ProjectDamageProps.__annotations__['fabric_tear_corner']=bpy.props.FloatProperty(name='Corner Tears',default=0.25,min=0.0,max=1.0,subtype='FACTOR',update=live.touch_prop,description='Realistic: share of three-cornered (L) tears - a snag on a nail: two legs along the warp and the weft, the flap between them folded back')
ProjectDamageProps.__annotations__['fabric_tear_worn']=bpy.props.FloatProperty(name='Worn Holes',default=0.25,min=0.0,max=1.0,subtype='FACTOR',update=live.touch_prop,description='Realistic: share of worn holes - abrasion (knees, elbows): squarish oval along the grain, the yarns of one family still spanning it, small worn spots around')
ProjectDamageProps.__annotations__['fabric_tear_puncture']=bpy.props.FloatProperty(name='Punctures',default=0.1,min=0.0,max=1.0,subtype='FACTOR',update=live.touch_prop,description='Realistic: share of punctures - a small hole with short slits along the grain')
ProjectDamageProps.__annotations__['fabric_grain_snap']=bpy.props.FloatProperty(name='Grain Snap',default=0.85,min=0.0,max=1.0,subtype='FACTOR',update=live.touch_prop,description='Realistic: share of tears that run exactly along the warp or the weft (Hole Dir is the warp). The rest are bias tears at any angle')
ProjectDamageProps.__annotations__['fabric_yarn_size']=bpy.props.FloatProperty(name='Yarn Size (cm)',default=0.25,min=0.02,soft_max=2.0,update=live.touch_prop,description='Realistic: width of a yarn bundle - the step of the torn rim. Smaller is finer and slower. Follows Master Scale')
ProjectDamageProps.__annotations__['fabric_flap_lift']=bpy.props.FloatProperty(name='Flap Lift',default=0.6,min=0.0,max=1.0,subtype='FACTOR',update=live.touch_prop,description='Realistic: how far the flap of a corner tear is folded back out of the hole (0 lies in place, 1 folded right over)')

                                                                      
                                                                             
FABRIC_TEAR_PRESETS = {
    '1': dict(tear_style='1', tear_rip=0.1, tear_corner=0.0, tear_worn=0.8, tear_puncture=0.1, grain_snap=0.95,
              yarn_size=0.2, hole_amount=4, hole_length_min=6.0, hole_length_max=14.0, hole_width_min=4.0,
              hole_width_max=9.0, hole_jagged=0.4, hole_meander=0.2, hole_split_max=3, bridge_share=0.45,
              broken_share=0.3),
    '2': dict(tear_style='1', tear_rip=0.3, tear_corner=0.7, tear_worn=0.0, tear_puncture=0.0, grain_snap=0.95,
              yarn_size=0.15, flap_lift=0.7, hole_amount=3, hole_length_min=5.0, hole_length_max=12.0,
              hole_width_min=1.0, hole_width_max=3.0, hole_jagged=0.4, hole_meander=0.2, bridge_share=0.25,
              broken_share=0.35),
    '3': dict(tear_style='1', tear_rip=0.85, tear_corner=0.15, tear_worn=0.0, tear_puncture=0.0, grain_snap=0.9,
              yarn_size=0.35, flap_lift=0.6, hole_amount=5, hole_length_min=12.0, hole_length_max=35.0,
              hole_width_min=1.5, hole_width_max=4.0, hole_jagged=0.6, hole_meander=0.35, bridge_share=0.3,
              broken_share=0.35),
    '4': dict(tear_style='1', tear_rip=0.0, tear_corner=0.0, tear_worn=0.4, tear_puncture=0.6, grain_snap=0.9,
              yarn_size=0.12, hole_amount=25, hole_length_min=1.0, hole_length_max=4.0, hole_width_min=0.6,
              hole_width_max=2.0, hole_jagged=0.5, hole_meander=0.2, hole_split_max=4, bridge_share=0.35,
              broken_share=0.3),
    '5': dict(tear_style='1', tear_rip=0.4, tear_corner=0.2, tear_worn=0.3, tear_puncture=0.1, grain_snap=0.6,
              yarn_size=0.3, flap_lift=0.8, hole_amount=12, hole_length_min=8.0, hole_length_max=30.0,
              hole_width_min=2.0, hole_width_max=8.0, hole_jagged=1.0, hole_meander=0.6, hole_split_max=3,
              bridge_share=0.35, broken_share=0.45, edge_enable=True, edge_amount=20),
    '6': dict(tear_style='0'),
}
FABRIC_TEAR_PRESET_ITEMS = (
    ('0', 'Custom', 'Keep the current values'),
    ('1', 'Old Jeans', 'Worn holes with the weft still spanning them, a few punctures (knees, seat)'),
    ('2', 'Snagged Shirt', 'Corner (L) tears from a nail with folded flaps, short rips'),
    ('3', 'Ripped Canvas', 'Long rips along the grain, some corner tears (tents, sails, sacks)'),
    ('4', 'Moth-Eaten', 'Many small holes and punctures with worn spots around'),
    ('5', 'Shredded Rag', 'Everything mixed, bias tears, ragged rims, torn edges'),
    ('6', 'Classic Lenses', 'The old shapes: ragged lenses split into holes'),
)


def _apply_fabric_tear_preset(self, context):
    values = FABRIC_TEAR_PRESETS.get(self.fabric_tear_preset)
    if values:
        for key, value in values.items():
            if hasattr(self, "fabric_" + key):
                setattr(self, "fabric_" + key, value)
    live.touch_prop(self, context)


ProjectDamageProps.__annotations__['fabric_tear_preset']=bpy.props.EnumProperty(name='Tear Preset',items=FABRIC_TEAR_PRESET_ITEMS,default='0',update=_apply_fabric_tear_preset,description='Fabric Classic and Solidify: set the tear shape, mix and sizes for a typical cloth look. Edit the values afterwards as you like')


                                                             
from . import asset_noise as _asset_noise
_asset_noise.add_properties(ProjectDamageProps)

from . import recording_tools
recording_tools.add_properties(ProjectDamageProps)


# Authored Fracture help: preserve exact three-line wording.
ProjectDamageProps.__annotations__['fract_enable'].keywords['description'] = 'Turns the Fracture effect on for this obejct. Only one Fracture asset can be active, they all share the same settings\nOff: no cracks are made\non: cracks are cut into the surface with the settings below'
ProjectDamageProps.__annotations__['fract_mode'].keywords['description'] = 'chooses where the cracks come from\nProcedural: the generator places all cracks over the whole object by itself\nPaint: you draw the Level 1 cracks, Levels 2 and 3 branch from your strokes. Proximity: cracks only around the points you place'
ProjectDamageProps.__annotations__['fract_type'].keywords['description'] = 'Pattern the cracks follow in Procedural mode. The typed assets (Concrete, Wood, Glass, Radial) fix it for you\nFracture: a free wandering network that closes into plates\nRadial, Glass, Wood: stars, glass stars with rings, or splits along the grain'
ProjectDamageProps.__annotations__['fract_l1_paint_radius'].keywords['description'] = 'Paint mode, in source-local cm. Maximum distance used to attach a drawn stroke to the real surface; also limits the crack width. Low (0.04): only strokes almost touching the surface produce a narrow cut. Higher: strokes can reach recessed relief and the crack can be wider; Width remains the requested width.'
ProjectDamageProps.__annotations__['fract_l2_paint_radius'].keywords['description'] = "Paint mode, in source-local cm. Limits this level's crack width and the default reach of its branches. Low (0.04): narrow cuts and short branches. Higher: wider cuts and longer branches; Width and Fracture Length remain limits."
ProjectDamageProps.__annotations__['fract_l3_paint_radius'].keywords['description'] = "Paint mode, in source-local cm. Limits this level's crack width and the default reach of its branches. Low (0.04): narrow cuts and short branches. Higher: wider cuts and longer branches; Width and Fracture Length remain limits."
ProjectDamageProps.__annotations__['fract_scale'].keywords['description'] = 'Multiplies width, depth, length and chip size of all cracks at once, keeping their proportions. Handy when the pattern looks right and you only need it bigger or smaller\nLow (10%): thin, shallow hairline cracks\nhigh (400%): wide, deep, heavily broken cracks'
ProjectDamageProps.__annotations__['fract_detail'].keywords['description'] = 'Polygon density along each crack. Changes how smoth and finely broken the edges are and how fast the effect updates, not the crack size\nlow (25%): few polygons, straight faceted edges, fast updates. Good while tuning\nHigh (400%): finely broken rough edges, heavy mesh and slow updates. Raise it at the end'
ProjectDamageProps.__annotations__['fract_tile_offset'].keywords['description'] = 'Raises or sinks the pieces (tiles) enclosed by cracks, like a cracked slab that has setled. Measured as a share of the crack depth around each tile, so a tile never sinks below its own crack. Shows best with Level 2 and 3\n0%: all tiles stay flush with the surface\n100%: tiles shift up and down by up to the full crack depth'
ProjectDamageProps.__annotations__['fract_tile_seed'].keywords['description'] = 'Reshuffles which tiles go up and which go down, without moving any crack. Chnage it when the offset pattern looks unlucky\nAny number: one arrangement of raised and sunk tiles\nanother number: a different arrangement, there is no low or high, only variants'
ProjectDamageProps.__annotations__['fract_depth_mult'].keywords['description'] = 'Scales the depth of the cracks of all three levels together, without changing their width. Not usde in Paint mode\nlow (10%): shallow surface cracks\nHigh (400%): deep grooves that cut far into the object'
ProjectDamageProps.__annotations__['fract_rim_soft'].keywords['description'] = 'Rounds the rim of the chips along the cracks outward into a soft fillet, like worn or weathered damage\n0%: sharp, fresh chip rims\n100%: rounded, smoothed rims. The diffrence is subtle and shows in close-up'
ProjectDamageProps.__annotations__['fract_rim_jag'].keywords['description'] = 'Makes the two lips of a crack break unevenly, so one side is wider than the other and the width changes along the crack\n0%: both lips are even, a clean groove\n100%: strongly uneven, ragged lips'
ProjectDamageProps.__annotations__['fract_flare'].keywords['description'] = 'crumbles the opening of the crack: a wide broken lip at the surface over a narrow, deep slit\n0%: straight walls, the crack is as wide at the top as at the bottom\n100%: a wide crumbled mouth narrowing into a thin slit'
ProjectDamageProps.__annotations__['fract_recipe'].keywords['description'] = 'which version of the crack generator is used\nLegacy: the original network, gives exactly the same result as older files\nImproved: more stable randomness, depth follows width, depth is limited by wall thickness, fallen bits are removed'
ProjectDamageProps.__annotations__['fract_thickness_cap'].keywords['description'] = 'Improved recipe only. The deepest a crack may go, as a share of the wall thickness under it, so thin walls are not cut thorugh\n0%: no cap, cracks keep their full Depth even in thin parts\n100%: cracks may go as deep as the whole wall. Lower values keep them shallower in thin areas'
ProjectDamageProps.__annotations__['fract_l1_amount'].keywords['description'] = 'Number of Level 1 cracks on the whole object. They start on the edges of the object. In Fracture Glass and Fracture Radial this is the number of stars instead\n0: this level is off, and the levels below it have nothing to grow from\nHigh: a dense network, on a cube many cracks end up on hiden faces'
ProjectDamageProps.__annotations__['fract_l1_subdiv'].keywords['description'] = 'Extra passes after the Level 1 cracks are finished. Each pass adds new cracks in the big uncracked areas betwen them, filling the gaps evenly\n0: only the Amount cracks\n4: four extra passes, a much denser and more even network'
ProjectDamageProps.__annotations__['fract_l1_width'].keywords['description'] = 'Width of the Level 1 cracks at the surface, in cm. Level 1 cracks are the main, widest ones\nlow: thin hairline cracks\nhigh: wide open gaps'
ProjectDamageProps.__annotations__['fract_l1_depth'].keywords['description'] = 'Depth of the Level 1 cracks, in cm. Also multiplied by Depth Multiplier. Also used by Fracture Proximity Default as its Depth\nLow: shallow scratches on the surface\nhigh: deep cuts far into the object'
ProjectDamageProps.__annotations__['fract_l1_branches'].keywords['description'] = 'Avarage number of side branches growing from each Level 1 crack. Fractions work: 0.5 means roughly every other crack gets one branch\n0: no branches, the original pattern\nhigh: every crack forks several times, a tree-like network'
ProjectDamageProps.__annotations__['fract_l1_branch_length'].keywords['description'] = 'Length of a branch comapred with the crack it grows from (1 = as long as the parent). Works only when Branches is above 0 and is also limited by Fracture Length\nlow: short stubs off the main crack\nHigh: long branches that run as far as the parent crack'
ProjectDamageProps.__annotations__['fract_l1_length'].keywords['description'] = 'Extends Level 1 crack travel beyond its automatic range, in centimetres before Fracture Scale. 0 keeps automatic growth. Higher values relax the length stop instead of shortening cracks. Actual mesh borders, masks and existing crack intersections can still stop growth. Rounded is recommended for beveled edges.'
ProjectDamageProps.__annotations__['fract_l1_bias'].keywords['description'] = 'How many Level 1 cracks start near corners instead of random places. For Level 1 these are the corners of the object, like real stress cracks\n0%: cracks start anywhere along the edges\n100%: cracks start from the corners. From 65% up the level switches to shatter mode: many short cracks that close into small cells'
ProjectDamageProps.__annotations__['fract_l1_chips'].keywords['description'] = 'Turns on chips, small spalls broken out of the rim, along the Level 1 cracks\noff: clean grooves without chips\nOn: chips along the rims, set by Chip Amount, Chip Size and Chip Depth'
ProjectDamageProps.__annotations__['fract_l1_chip_amount'].keywords['description'] = 'How many chips are made per 100 cm of Level 1 crack length. Not used in Paint mode\n0: no chips\n20: the rim is chipped allmost all along the crack'
ProjectDamageProps.__annotations__['fract_l1_chip_size'].keywords['description'] = 'size of each chip along the rim of a Level 1 crack, in cm. Big chips on the main cracks read as heavy damage\nLow: tiny nicks on the rim\nHigh: large broken-out pieces, best seen in close-up'
ProjectDamageProps.__annotations__['fract_l1_chip_depth'].keywords['description'] = 'How deep each Level 1 chip goes into the surface, in cm. Keep it below the crack Depth for a natural look\nlow: flat surface flakes\nHigh: deep spalls next to the crack'
ProjectDamageProps.__annotations__['fract_l2_amount'].keywords['description'] = 'Number of Level 2 cracks on the whole object. They start on the Level 1 cracks. In Fracture Glass and Fracture Radial this is the number of stars instead\n0: this level is off, and the levels below it have nothing to grow from\nHigh: a dense network, on a cube many cracks end up on hiden faces'
ProjectDamageProps.__annotations__['fract_l2_subdiv'].keywords['description'] = 'Extra passes after the Level 2 cracks are finished. Each pass adds new cracks in the big uncracked areas betwen them, filling the gaps evenly\n0: only the Amount cracks\n4: four extra passes, a much denser and more even network'
ProjectDamageProps.__annotations__['fract_l2_width'].keywords['description'] = 'Width of the Level 2 cracks at the surface, in cm. usually about half of Level 1\nlow: thin hairline cracks\nhigh: wide open gaps'
ProjectDamageProps.__annotations__['fract_l2_depth'].keywords['description'] = 'Depth of the Level 2 cracks, in cm. Also multiplied by Depth Multiplier. Usually about half of Level 1\nLow: shallow scratches on the surface\nhigh: deep cuts far into the object'
ProjectDamageProps.__annotations__['fract_l2_branches'].keywords['description'] = 'Avarage number of side branches growing from each Level 2 crack. Fractions work: 0.5 means roughly every other crack gets one branch\n0: no branches, the original pattern\nhigh: every crack forks several times, a tree-like network'
ProjectDamageProps.__annotations__['fract_l2_branch_length'].keywords['description'] = 'Length of a branch comapred with the crack it grows from (1 = as long as the parent). Works only when Branches is above 0 and is also limited by Fracture Length\nlow: short stubs off the main crack\nHigh: long branches that run as far as the parent crack'
ProjectDamageProps.__annotations__['fract_l2_length'].keywords['description'] = 'Extends Level 2 crack travel beyond its automatic range, in centimetres before Fracture Scale. 0 keeps automatic growth. Higher values relax the length stop instead of shortening cracks. Actual mesh borders, masks and existing crack intersections can still stop growth. Rounded is recommended for beveled edges.'
ProjectDamageProps.__annotations__['fract_l2_bias'].keywords['description'] = 'How many Level 2 cracks start near corners instead of random places. For Level 2 these are the junctions and ends of the Level 1 cracks, so cracks gather around crossings\n0%: cracks start anywhere on the cracks above and on edges\n100%: cracks start from junctions and crack ends. From 65% up the level switches to shatter mode: many short cracks that close into small cells'
ProjectDamageProps.__annotations__['fract_l2_chips'].keywords['description'] = 'Turns on chips, small spalls broken out of the rim, along the Level 2 cracks\noff: clean grooves without chips\nOn: chips along the rims, set by Chip Amount, Chip Size and Chip Depth'
ProjectDamageProps.__annotations__['fract_l2_chip_amount'].keywords['description'] = 'How many chips are made per 100 cm of Level 2 crack length. Not used in Paint mode\n0: no chips\n20: the rim is chipped allmost all along the crack'
ProjectDamageProps.__annotations__['fract_l2_chip_size'].keywords['description'] = 'size of each chip along the rim of a Level 2 crack, in cm. Keep them smaller than on Level 1\nLow: tiny nicks on the rim\nHigh: large broken-out pieces, best seen in close-up'
ProjectDamageProps.__annotations__['fract_l2_chip_depth'].keywords['description'] = 'How deep each Level 2 chip goes into the surface, in cm. keep it below the Level 2 Depth\nlow: flat surface flakes\nHigh: deep spalls next to the crack'
ProjectDamageProps.__annotations__['fract_l3_amount'].keywords['description'] = 'Number of Level 3 cracks on the whole object. They start on the Level 2 cracks. In Fracture Glass and Fracture Radial this is the number of stars instead\n0: this level is off\nHigh: a dense network, on a cube many cracks end up on hiden faces'
ProjectDamageProps.__annotations__['fract_l3_subdiv'].keywords['description'] = 'Extra passes after the Level 3 cracks are finished. Each pass adds new cracks in the big uncracked areas betwen them, filling the gaps evenly\n0: only the Amount cracks\n4: four extra passes, a much denser and more even network'
ProjectDamageProps.__annotations__['fract_l3_width'].keywords['description'] = 'Width of the Level 3 cracks at the surface, in cm. The finest cracks, usually thiner than Level 2\nlow: thin hairline cracks\nhigh: wide open gaps'
ProjectDamageProps.__annotations__['fract_l3_depth'].keywords['description'] = 'Depth of the Level 3 cracks, in cm. Also multiplied by Depth Multiplier. Usually the shallowest level\nLow: shallow scratches on the surface\nhigh: deep cuts far into the object'
ProjectDamageProps.__annotations__['fract_l3_branches'].keywords['description'] = 'Avarage number of side branches growing from each Level 3 crack. Fractions work: 0.5 means roughly every other crack gets one branch\n0: no branches, the original pattern\nhigh: every crack forks several times, a tree-like network'
ProjectDamageProps.__annotations__['fract_l3_branch_length'].keywords['description'] = 'Length of a branch comapred with the crack it grows from (1 = as long as the parent). Works only when Branches is above 0 and is also limited by Fracture Length\nlow: short stubs off the main crack\nHigh: long branches that run as far as the parent crack'
ProjectDamageProps.__annotations__['fract_l3_length'].keywords['description'] = 'Extends Level 3 crack travel beyond its automatic range, in centimetres before Fracture Scale. 0 keeps automatic growth. Higher values relax the length stop instead of shortening cracks. Actual mesh borders, masks and existing crack intersections can still stop growth. Rounded is recommended for beveled edges.'
ProjectDamageProps.__annotations__['fract_l3_bias'].keywords['description'] = 'How many Level 3 cracks start near corners instead of random places. Level 3 uses 85% by default, which gives crushed zones at the crack crossings\n0%: cracks start anywhere on the cracks above and on edges\n100%: cracks start from junctions and crack ends. From 65% up the level switches to shatter mode: many short cracks that close into small cells'
ProjectDamageProps.__annotations__['fract_l3_chips'].keywords['description'] = 'Turns on chips, small spalls broken out of the rim, along the Level 3 cracks\noff: clean grooves without chips (default for Level 3)\nOn: chips along the rims, set by Chip Amount, Chip Size and Chip Depth'
ProjectDamageProps.__annotations__['fract_l3_chip_amount'].keywords['description'] = 'How many chips are made per 100 cm of Level 3 crack length. Not used in Paint mode\n0: no chips\n20: the rim is chipped allmost all along the crack'
ProjectDamageProps.__annotations__['fract_l3_chip_size'].keywords['description'] = 'size of each chip along the rim of a Level 3 crack, in cm. Small chips keep the fine network clean\nLow: tiny nicks on the rim\nHigh: large broken-out pieces, best seen in close-up'
ProjectDamageProps.__annotations__['fract_l3_chip_depth'].keywords['description'] = 'How deep each Level 3 chip goes into the surface, in cm. Keep it below the Level 3 Depth\nlow: flat surface flakes\nHigh: deep spalls next to the crack'
ProjectDamageProps.__annotations__['fract_prox_mode'].keywords['description'] = 'Chooses how the proximity points are used. Procedural mode only; the Proximity assets set it for you\noff: the points are ignored\nDefault: cracks gather at the points. Radial / Glass: an impact pattern at each point'
ProjectDamageProps.__annotations__['fract_prox_collection'].keywords['description'] = 'Collection whose objects (Empties) are the proximity points. Add Proximity Point creates an Empty at the 3D Cursor and the collection if needed\nEmpty or missing: no proximity cracks, the Proximity assets show a warning\nSeveral points: several clusters or impacts, the cracks are shared between them'
ProjectDamageProps.__annotations__['fract_prox_use_size'].keywords['description'] = 'takes the reach of each point from its Empty: display size multiplied by scale\nOff: every point uses the same Distance\nOn: each point has its own reach, set by scaling the Empty in the viewport'
ProjectDamageProps.__annotations__['fract_prox_radius'].keywords['description'] = 'Reach of a point when Radius from Empty Size is off, in cm. In Radial and Glass Proximity it sets the size of the whole impact: crack length and ring spacing\nlow: a small, tight cluster or impact right at the point\nHigh: cracks spread far from the point, a large impact over the whole face'
ProjectDamageProps.__annotations__['fract_prox_strength'].keywords['description'] = 'Default proximity only. Share of the cracks of each level that start near the points insted of anywhere on the object. It gathers cracks, it does not cut them off at the reach\n0%: the points have no effect\n100%: almost all cracks start around the points'
ProjectDamageProps.__annotations__['fract_radial_spokes'].keywords['description'] = 'Number of cracks (spokes) running out from the center. In Fracture Radial it is per star, in Radial Fracture Proximity per point\nLow (3): a few long spokes\nHigh (32): a dense burst of cracks'
ProjectDamageProps.__annotations__['fract_radial_rings'].keywords['description'] = 'Radial Fracture Proximity. Number of concentric ring cracks arond each point\n0: radial cracks only, no rings\nhigh (12): many rings, a web-like pattern'
ProjectDamageProps.__annotations__['fract_radial_ring_fill'].keywords['description'] = 'Radial Fracture Proximity. Share of the ring segments between the radial cracks that are actually cracked\nlow: broken, scattered ring pieces\n100%: complete closed rings'
ProjectDamageProps.__annotations__['fract_radial_crush'].keywords['description'] = 'Radial Fracture Proximity. Size of the crushed zone at the point, as a share of the reach\n0%: the cracks meet in a clean point\nHigh (60%): a large crushed crater in the midle'
ProjectDamageProps.__annotations__['fract_glass_spokes'].keywords['description'] = 'Number of straight cracks runing out of each impact. Fracture Glass uses half of this value plus one per star (14 gives 8 spokes), Glass Fracture Proximity uses it as is\nLow (3): a few long straight cracks\nhigh (48): a dense starburst'
ProjectDamageProps.__annotations__['fract_glass_rings'].keywords['description'] = 'Number of concentric ring cracks around each impact. Fracture Glass uses half of this value and builds the rings from the next level, so Level 2 Amount must be abvoe 0\n0: spokes only, no rings\nHigh (24): many rings, a dense spider web'
ProjectDamageProps.__annotations__['fract_glass_ring_fill'].keywords['description'] = 'Share of the ring pieces between neighbouring spokes that are actually cracked\nLow: broken, scattered ring pieces\n100%: complete closed rings'
ProjectDamageProps.__annotations__['fract_glass_branch'].keywords['description'] = 'Glass Fracture Proximity. Chance that each radial crack forks, up to two times, as it runs out\n0%: straight single cracks\n100%: every crack forks, a tree-like patern'
ProjectDamageProps.__annotations__['fract_glass_width'].keywords['description'] = 'Glass Fracture Proximity. Width of the crack lines at the surface, in cm. Independent of the Level Width\nlow: fine hairlines\nHigh: wide, clearly visible cracks'
ProjectDamageProps.__annotations__['fract_glass_depth'].keywords['description'] = 'glass Fracture Proximity. Depth of the crack lines, in cm. Keep it below the glass thickness\nlow: surface scratches\nHigh: cracks that go deep into the pane'
ProjectDamageProps.__annotations__['fract_glass_crush'].keywords['description'] = 'Glass Fracture Proximity. Size of the crushed zone at the impact, as a share of the reach. Use it for a hard hit\n0%: the cracks meet in a clean point\nHigh (40%): a crushed hole area in the middle'
ProjectDamageProps.__annotations__['fract_grain_x'].keywords['description'] = "X part of the wood grain direction, in the object's local axes. Only the ratio of X, Y and Z matters. All three at 0 turns the grain off\n0: the grain has no X component\n1: fibres run along X, use it for a beam lying along X. Mixing two axes gives diagonal spilts"
ProjectDamageProps.__annotations__['fract_grain_y'].keywords['description'] = "Y part of the wood grain direction, in the object's local axes. Only the ratio of X, Y and Z matters\n0: the grain has no Y component\n1: fibres run along Y. Negative values point the same axis the other way and give the same splits"
ProjectDamageProps.__annotations__['fract_grain_z'].keywords['description'] = "Z part of the wood grain direction, in the object's local axes. The default (0, 0, 1) is for upright posts\n0: the grain has no Z component\n1: fibres run along Z, upright like a post or a log standing on its end"
ProjectDamageProps.__annotations__['fract_grain_pull'].keywords['description'] = 'How strongly each crack is pulled toward the grain direction. On end-grain faces the pull fades out, so they crack freely\n0: the grain is ignored, cracks wander like concrete\n1: cracks run straight along the grain as long splits, like a dried beam'
ProjectDamageProps.__annotations__['fract_disp_strength'].keywords['description'] = 'How strongly the crack lines wander and how much micro releif their walls get, for all three levels. Not used in Paint mode\n0%: smooth, clean crack lines\n300%: very jagged, rough crack lines'
ProjectDamageProps.__annotations__['fract_disp_scale'].keywords['description'] = 'size of the noise waves along the cracks\nlow (25%): fine, tight jitter\nHigh (400%): long, broad waves'
ProjectDamageProps.__annotations__['fract_proximity_amount'].keywords['description'] = 'Number of cracks around the points. The Amount is shared between all points and does not depend on the Level Amounts\n0: no cracks at all\nhigh: a dense cluster of cracks around each point'
ProjectDamageProps.__annotations__['fract_mode'].keywords['items'] = tuple((item[0], item[1], {'PROCEDURAL': 'The generator places all cracks over the whole object, using the Amount of each level', 'PAINT': 'You draw the Level 1 cracks; Levels 2 and 3 branch from them automatically', 'PROXIMITY': 'cracks only around the points you place, with the pattern of the selected Fracture asset'}.get(item[0], item[2]), *item[3:]) for item in ProjectDamageProps.__annotations__['fract_mode'].keywords['items'])
ProjectDamageProps.__annotations__['fract_type'].keywords['items'] = tuple((item[0], item[1], {'FRACTURE': 'Irregular cracks that wander, kink and close into plates, like concrete or stone', 'RADIAL': 'Each crack becomes a star of wandering cracks; Amount is the number of stars', 'GLASS': 'Stars of straight spokes with ring chords from the next level; Amount is the number of stars', 'WOOD': 'long splits pulled along the wood grain'}.get(item[0], item[2]), *item[3:]) for item in ProjectDamageProps.__annotations__['fract_type'].keywords['items'])
ProjectDamageProps.__annotations__['fract_recipe'].keywords['items'] = tuple((item[0], item[1], {'IMPROVED': 'Stable randomness, depth follows width and is limited by wall thickness, fallen bits removed', 'LEGACY': 'The original network, same result as older files'}.get(item[0], item[2]), *item[3:]) for item in ProjectDamageProps.__annotations__['fract_recipe'].keywords['items'])
ProjectDamageProps.__annotations__['fract_prox_mode'].keywords['items'] = tuple((item[0], item[1], {'OFF': 'The points are ignored', 'DEFAULT': 'The normal crack network gathers around the points', 'RADIAL': 'An impact at each point: radial cracks, rings and a crushed centre', 'GLASS': 'a glass impact at each point: forking straight hairlines and dense rings'}.get(item[0], item[2]), *item[3:]) for item in ProjectDamageProps.__annotations__['fract_prox_mode'].keywords['items'])

                                                                  
ProjectDamageProps.__annotations__['fract_surface']=bpy.props.EnumProperty(name='Surface',default='CLASSIC',update=live.touch_prop,description='How cracks follow the surface',items=(('CLASSIC','Classic','Cracks start at sharp edges and stop at edges and folds'),('ROUNDED','Rounded','Recommended for beveled edges and curved models. Cracks follow the surface across bevels; real borders and sharp creases can still stop them. Length 0 uses automatic travel; higher values extend it')))


# Authored Bead, Scratches, Fabric, Concrete Hit, Plaster, Wallpaper and Bullet help.
if 'build_enable' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['build_enable'].keywords['description'] = 'Turns Beads on: a raised bead along the inside (concave) edges of the object, like sealant, a weld seam or dried mortar. Outside edges never get a bead\nOff: no beads\non: beads on the inside edges, set by the settings below'
if 'build_mode' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['build_mode'].keywords['description'] = 'Chooses which inside edges get a bead\nAutomatic: edges are picked at random by Edges Covered\nDraw Zones: only inside edges near your strokes get a bead. An empty drawing gives no bead'
if 'build_drawing' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['build_drawing'].keywords['description'] = 'Grease Pencil or curve object with the strokes that mark which inside edges get a bead (Draw Zones only). Draw along the edges. New Drawing creates one, Draw / Edit lets you draw on the object\nempty or no drawing: nothing is made from strokes\nMore strokes: more places get the effect'
if 'build_paint_radius' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['build_paint_radius'].keywords['description'] = 'Draw Zones only. How far from a stroke an inside edge is still picked up, in cm\nLow: only an edge right under the stroke is used, draw precisely\nhigh: edges near the stroke are covered too, the bead can run past the stroke end'
if 'build_coverage' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['build_coverage'].keywords['description'] = 'Share of all inside edges that carry a bead (Automatic only). The edges are picked by chance\n0%: no bead at all. Low values: just one or two edges\n100%: every inside edge of the object has a bead'
if 'build_scale' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['build_scale'].keywords['description'] = 'Multiplies Width and Height of the bead together, keeping its proportions\nLow (10%): a thin hairline bead\nhigh (400%): a thick, heavy bead'
if 'build_detail' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['build_detail'].keywords['description'] = 'Polygon density of the bead along the edge. Changes how fine the result is and how fast it updates, not its size\nlow (25%): a faceted bead, fast updates\nHigh (400%): a smooth bead with fine lumps, heavier mesh'
if 'build_width' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['build_width'].keywords['description'] = 'Width of the bead across the edge, in cm (multiplied by Scale). The Fillet shape takes its size from Width\nLow: a thin line in the corner\nHigh: a wide bead reaching far onto both faces'
if 'build_height' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['build_height'].keywords['description'] = 'How far the bead rises out of the corner, in cm (multiplied by Scale). Used by the Rounded shape\nlow: a flat smear in the corner\nHigh: a tall, rounded bead'
if 'build_irreg' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['build_irreg'].keywords['description'] = 'How much the width and height swell and shrink along the edge\n0%: an even bead, same thickness along the whole edge\n300%: a strongly swelling bead. With the Fillet shape high values break the cove into sharp peaks'
if 'build_lumps' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['build_lumps'].keywords['description'] = 'Small lumps and dents on the surface of the bead. Needs a high Resolution to show\n0%: a smooth bead surface\n100%: a lumpy, uneven surface, like hand-applied filler'
if 'build_shape' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['build_shape'].keywords['description'] = 'Cross-section of the bead\nRounded: a narrow half-round bead lying in the corner\nFillet: fills the corner as a cove that runs out onto both faces'
if 'scratch_enable' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['scratch_enable'].keywords['description'] = 'Turns Scratches Draw on: fine scratches cut into the surface along strokes you draw\nOff: no scratches\nOn: scratches along the strokes of the Drawing'
if 'scratch_drawing' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['scratch_drawing'].keywords['description'] = 'Grease Pencil or curve object with the strokes that guide the scratches. Draw where the object gets scratched. New Drawing creates one, Draw / Edit lets you draw on the object\nempty or no drawing: nothing is made from strokes\nMore strokes: more places get the effect'
if 'scratch_mode' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['scratch_mode'].keywords['description'] = 'How a stroke is turned into scratches\nScatter: a band of many short scratches around each stroke\nSingle Stroke: exactly one scratch per stroke, following your hand. The placement sliders are ignored'
if 'scratch_style' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['scratch_style'].keywords['description'] = 'Ready-made look of the scratches. Fine, Keyed / Gouged and Scuffed load their own shape values, Custom shows all shape settings for editing\nFine: thin light hairlines. Keyed / Gouged: fewer, deeper, longer cuts\nScuffed: many very shallow marks, a worn band. Custom: your own values'
if 'scratch_size_scale' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['scratch_size_scale'].keywords['description'] = 'Multiplies length, width and depth of all scratches, in every style\nLow: tiny, barely visible scratches\nhigh: long, wide, deep scratches'
if 'scratch_detail' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['scratch_detail'].keywords['description'] = 'Polygon density of the grooves. Changes how fine the result is and how fast it updates, not its size\nLow: coarse broken grooves, very thin scratches may dissapear\nHigh: crisp continuous grooves, heavy mesh'
if 'scratch_seed' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['scratch_seed'].keywords['description'] = 'Changes the random pattern of the scratches without changing their size or number\nevery number gives a different arrangement\nthere is no low or high, only different variants'
if 'scratch_density' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['scratch_density'].keywords['description'] = 'Number of scratches per unit of stroke length (Scatter mode)\nlow: a few scattered scratches along the stroke\nHigh: a dense band of scratches'
if 'scratch_max_scratches' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['scratch_max_scratches'].keywords['description'] = 'Hard limit on the number of scratches over all strokes, shared evenly by stroke length\nLow: the band thins out even with a high Density\nHigh (600): Density alone decides the number'
if 'scratch_spread' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['scratch_spread'].keywords['description'] = 'The farthest a scratch can land from its stroke, in cm\nLow: a narrow band tight on the stroke\nHigh: a wide band of scratches around the stroke'
if 'scratch_falloff' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['scratch_falloff'].keywords['description'] = 'How the scratches are spread across the band\n1: even density over the whole band width\nhigh: scratches crowd near the stroke and thin out to the sides'
if 'scratch_angle_jitter' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['scratch_angle_jitter'].keywords['description'] = 'Spread of the scratch direction around the stroke direction, in degrees\n0: all scratches run parallel to the stroke\nHigh: scratches point in many directions'
if 'scratch_cross_share' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['scratch_cross_share'].keywords['description'] = 'Share of scratches at a completely random angle, crossing the band\n0: no crossing scratches\nhigh: many scratches cut across the band'
if 'scratch_along' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['scratch_along'].keywords['description'] = 'Share of scratches placed exactly on the stroke and along it, as if drawn with a point\n0: all scratches are scattered around the stroke\n1: the scratches line up on the stroke itself'
if 'scratch_paint_snap' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['scratch_paint_snap'].keywords['description'] = 'How far a stroke can be from the surface and still be projected onto it, in cm\nLow: only strokes drawn right on the surface are used\nHigh: strokes away from the surface are used too, they may reach other parts of the object'
if 'scratch_wrap_angle' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['scratch_wrap_angle'].keywords['description'] = 'A scratch stops where the surface turns by more than this angle, in degrees\nLow: scratches stop at any small bend\nhigh (140): scratches run over sharp edges onto the next face'
if 'scratch_length_min' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['scratch_length_min'].keywords['description'] = 'Shortest scratch, in cm. Keep it at or below Length Max\nLow: short nicks are possible\nHigh: no short scratches, every scratch is long'
if 'scratch_length_max' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['scratch_length_max'].keywords['description'] = 'Longest scratch, in cm. Keep it at or above Length Min\nlow: only short nicks\nHigh: long scratches are possible'
if 'scratch_length_bias' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['scratch_length_bias'].keywords['description'] = 'Share of short scratches within the length range\n0: mostly long scratches\n1: mostly short scratches, 0.5 is an even mix'
if 'scratch_width' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['scratch_width'].keywords['description'] = 'Width of a groove at its widest point, in cm\nLow: hairline scratches\nHigh: wide grooves'
if 'scratch_width_var' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['scratch_width_var'].keywords['description'] = 'Random variation of the width from scratch to scratch\n0: every scratch has the same width\n1: a mix of thin and wide scratches'
if 'scratch_depth' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['scratch_depth'].keywords['description'] = 'Depth of a groove at its deepest point, in cm\nlow: scratches that barely break the surface\nHigh: deep cuts'
if 'scratch_depth_var' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['scratch_depth_var'].keywords['description'] = 'Random variation of the depth from scratch to scratch\n0: every scratch has the same depth\n1: a mix of shallow and deep scratches'
if 'scratch_profile' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['scratch_profile'].keywords['description'] = 'Cross-section of the groove\nV Groove: a sharp groove, like a cut with a point\nU Groove: a rounded groove, like a dragged blunt object'
if 'scratch_taper' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['scratch_taper'].keywords['description'] = 'Share of the length at each end where the scratch narrows to nothing (0 to 0.5)\n0: blunt ends, the groove starts and stops at full width\n0.5: the scratch narrows over its whole length, a needle shape'
if 'scratch_gouge' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['scratch_gouge'].keywords['description'] = 'A deep start that fades out toward the end, like a point dragged across the surface\n0: even depth along the scratch\n1: a deep start and a shallow tail'
if 'scratch_curve' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['scratch_curve'].keywords['description'] = 'How much a scratch bends over its length\n0: straight scratches\nhigh: clearly curved scratches'
if 'scratch_wobble' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['scratch_wobble'].keywords['description'] = 'Small sideways waver along the scratch, as a share of its length\n0: a clean line\nHigh: a shaky, wavering line'
if 'scratch_gaps' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['scratch_gaps'].keywords['description'] = 'How much a scratch skips, leaving a broken line\n0: continuous scratches\nhigh: dashed, intermittent scratches'
if 'scratch_bundle_chance' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['scratch_bundle_chance'].keywords['description'] = 'Share of scratches that become a bundle of parallel lines, like grit under a sliding object\n0: single lines only\n1: every scratch is a bundle'
if 'scratch_bundle_min' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['scratch_bundle_min'].keywords['description'] = 'Fewest lines in one bundle. Keep it at or below Bundle Max\nLow: bundles can be just two lines\nHigh: every bundle has many lines'
if 'scratch_bundle_max' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['scratch_bundle_max'].keywords['description'] = 'Most lines in one bundle. Keep it at or above Bundle Min\nlow: narrow bundles of few lines\nHigh: wide bundles of many lines are possible'
if 'scratch_bundle_gap' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['scratch_bundle_gap'].keywords['description'] = 'Distance between the lines of a bundle, in cm\nLow: tight lines that almost merge\nHigh: clearly separate parallel lines'
if 'scratch_bundle_thin' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['scratch_bundle_thin'].keywords['description'] = 'Width and depth of bundle lines compared with a normal scratch\nLow: very fine bundle lines\n1: bundle lines as strong as a single scratch'
if 'scratch_scuff_enable' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['scratch_scuff_enable'].keywords['description'] = 'Adds a paper-thin band around each scratch. It barely changes the shape but carries a selection a material can use for a dull scuff mark\nOff: grooves only\non: a very shallow band around each groove'
if 'scratch_scuff_width' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['scratch_scuff_width'].keywords['description'] = 'Width of the scuff band as a multiple of the groove width\nLow: a narrow band hugging the groove\nHigh: a wide band around the groove'
if 'scratch_scuff_depth' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['scratch_scuff_depth'].keywords['description'] = 'Depth of the scuff band, in cm. Keep it tiny, it is meant for the material, not for relief\nlow: the band is only a selection\nHigh: the band becomes a visible shallow dent'
if 'scratch_mask_falloff' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['scratch_mask_falloff'].keywords['description'] = 'Width of the soft fade at the edge of the scratch mask that materials can use, in cm. The geometry does not change\nLow: a hard mask edge\nHigh: a soft, gradual mask edge'
if 'fabric_enable' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_enable'].keywords['description'] = 'Turns Fabric Classic on: holes, slits and threads torn into a thin cloth sheet. A thick block does not become cloth by this\nOff: the cloth stays intact\nOn: tears and threads with the settings below'
if 'fabric_solid_enable' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_solid_enable'].keywords['description'] = 'Turns Fabric Solidify on: the same tears for cloth with real thickness, like a sheet with a Solidify modifier. Switches Mesh Preparation to Use Modifiers\nOff: no tears\non: tears cut through the full thickness with rounded rims'
if 'fabric_placement' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_placement'].keywords['description'] = 'Chooses where the tears come from\nProcedural: tears all over the cloth from Interior Tears and Edge Tears\nDraw: only the tears you draw, an empty drawing leaves the cloth intact'
if 'fabric_paint_holes_enable' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_paint_holes_enable'].keywords['description'] = 'Draw only. Turns strokes of the holes drawing into holes. A stroke gives a hole along it, a closed loop a hole of that shape\nOff: drawn holes are not used\nOn: the strokes are cut into the cloth as holes'
if 'fabric_holes_drawing' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_holes_drawing'].keywords['description'] = 'Grease Pencil or curve object with the strokes that become holes (Draw placement). New Drawing creates one, Draw / Edit lets you draw on the object\nempty or no drawing: nothing is made from strokes\nMore strokes: more places get the effect'
if 'fabric_paint_edges_enable' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_paint_edges_enable'].keywords['description'] = 'Draw only. Turns strokes that start at the border of the cloth into torn slits\noff: drawn torn edges are not used\nOn: slits are cut along the strokes'
if 'fabric_edges_drawing' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_edges_drawing'].keywords['description'] = 'Grease Pencil or curve object with the strokes that become torn edge slits (Draw placement). Start the strokes at the cloth border. New Drawing creates one, Draw / Edit lets you draw on the object\nempty or no drawing: nothing is made from strokes\nMore strokes: more places get the effect'
if 'fabric_paint_jagged' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_paint_jagged'].keywords['description'] = 'Draw only. Raggedness of the rim of drawn holes, as a share of Rim Detail Size\n0: smooth, clean-cut rims\nHigh: ragged, torn rims'
if 'fabric_paint_rag_size' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_paint_rag_size'].keywords['description'] = 'Draw only. Size of the bumps of the ragged rim, in cm\nLow: fine, small raggedness\nhigh: large, coarse bites in the rim'
if 'fabric_paint_edge_spacing' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_paint_edge_spacing'].keywords['description'] = 'Draw only. Distance between the slits cut along a torn edge stroke\nLow: many slits close together\nHigh: few, widely spaced slits'
if 'fabric_paint_edge_length_min' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_paint_edge_length_min'].keywords['description'] = 'Draw only. Shortest drawn edge slit. Keep it at or below Length Max\nlow: short nicks are possible\nHigh: every slit is long'
if 'fabric_paint_edge_length_max' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_paint_edge_length_max'].keywords['description'] = 'Draw only. Longest drawn edge slit. Keep it at or above Length Min\nLow: only short nicks\nHigh: long slits deep into the cloth are possible'
if 'fabric_paint_edge_width_min' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_paint_edge_width_min'].keywords['description'] = 'Draw only. Narrowest drawn edge slit. Keep it at or below Width Max\nLow: thin cuts are possible\nhigh: every slit is wide'
if 'fabric_paint_edge_width_max' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_paint_edge_width_max'].keywords['description'] = 'Draw only. Widest drawn edge slit. Keep it at or above Width Min\nLow: only thin cuts\nHigh: wide V-shaped gaps are possible'
if 'fabric_detail' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_detail'].keywords['description'] = 'Polygon density of the torn rims and threads. Changes how fine the result is and how fast it updates, not its size\nLow: coarse blocky rims, fast updates\nhigh: fine rims and round threads, heavy mesh and slow updates'
if 'fabric_seed' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_seed'].keywords['description'] = 'Changes the random pattern of the tears without changing their size or number\nevery number gives a different arrangement\nthere is no low or high, only different variants'
if 'fabric_layer_depth' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_layer_depth'].keywords['description'] = 'For garments with two layers (front and back of a shirt). Decides if a tear cuts only its own layer or also the layer behind it, in cm\n0: a tear cuts only the layer it lies on, you see the inside of the garment through the hole\nHigh: layers closer than this distance are cut together, the hole goes through both sides'
if 'fabric_tear_preset' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_tear_preset'].keywords['description'] = 'Loads shape, mix and sizes of a typical cloth look. Edit the values afterwards as you like\nCustom: keeps your current values\nOld Jeans, Snagged Shirt, Ripped Canvas, Moth-Eaten, Shredded Rag, Classic Lenses: from a few worn holes to heavy shredding'
if 'fabric_tear_style' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_tear_style'].keywords['description'] = 'Shape family of the procedural tears\nClassic: ragged lens-shaped holes that can split into several holes with fabric bridges between them\nRealistic: tears that follow the weave, with stepped rims and a fringe along the yarns'
if 'fabric_tear_rip' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_tear_rip'].keywords['description'] = 'Realistic only. Share of rips: straight tears along one yarn, opened by the pulled edges. Only the ratio of the four tear types matters\n0: no rips\n1: mostly rips'
if 'fabric_tear_corner' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_tear_corner'].keywords['description'] = 'Realistic only. Share of corner (L) tears: a snag on a nail, two legs along warp and weft with a folded flap\n0: no corner tears\n1: mostly corner tears'
if 'fabric_tear_worn' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_tear_worn'].keywords['description'] = 'Realistic only. Share of worn holes: squarish worn areas (knees, elbows) with yarns still spanning them\n0: no worn holes\n1: mostly worn holes'
if 'fabric_tear_puncture' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_tear_puncture'].keywords['description'] = 'Realistic only. Share of punctures: small holes with short slits along the grain\n0: no punctures\n1: mostly punctures'
if 'fabric_grain_snap' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_grain_snap'].keywords['description'] = 'Realistic only. Share of tears that run exactly along the warp or the weft. The rest are bias tears at any angle\n0: tears at random angles\n1: every tear runs along or across the weave'
if 'fabric_yarn_size' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_yarn_size'].keywords['description'] = 'Realistic only. Width of a yarn bundle, the step size of the torn rim, in cm. Smaller is finer and slower\nLow: a finely stepped rim, like thin shirt fabric\nHigh: a coarse stepped rim, like canvas or burlap'
if 'fabric_flap_lift' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_flap_lift'].keywords['description'] = 'Realistic only. How far the flap of a corner tear is folded back out of the hole\n0: the flap lies in place, the tear is a thin L-shaped line\n1: the flap is folded right over and the hole is open'
if 'fabric_holes_enable' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_holes_enable'].keywords['description'] = 'Turns Interior Tears on: holes inside the cloth\nOff: no interior holes, the values are kept\nOn: holes set by the settings below'
if 'fabric_hole_amount' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_hole_amount'].keywords['description'] = 'Number of holes on the whole cloth\n0: no interior holes\nHigh: many holes'
if 'fabric_hole_length_min' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_hole_length_min'].keywords['description'] = 'Shortest hole, measured along the tear, in cm. Keep it at or below Length Max\nlow: small holes are possible\nHigh: every hole is a long rip'
if 'fabric_hole_length_max' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_hole_length_max'].keywords['description'] = 'Longest hole, measured along the tear, in cm. Keep it at or above Length Min\nLow: only short holes\nHigh: long rips across the cloth are possible'
if 'fabric_hole_width_min' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_hole_width_min'].keywords['description'] = 'Narrowest hole, measured across the tear, in cm. Keep it at or below Width Max\nLow: thin slits are possible\nhigh: every hole is wide open'
if 'fabric_hole_width_max' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_hole_width_max'].keywords['description'] = 'Widest hole, measured across the tear, in cm. Keep it at or above Width Min\nLow: only narrow slits\nHigh: wide open holes are possible'
if 'fabric_hole_jagged' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_hole_jagged'].keywords['description'] = 'Raggedness of the hole outline (Classic tears)\n0: smooth lens shapes\nHigh: torn, ragged outlines'
if 'fabric_hole_lobes' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_hole_lobes'].keywords['description'] = 'Large bulges in the hole outline (Classic tears)\n0: a simple oval outline\nhigh: lobed, irregular holes'
if 'fabric_hole_meander' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_hole_meander'].keywords['description'] = 'How much a long hole wanders sideways instead of running straight (Classic tears)\n0: straight holes\nHigh: winding, curved holes'
if 'fabric_hole_split_max' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_hole_split_max'].keywords['description'] = 'The most pieces a tear can split into, with fabric bridges left between them (Classic tears)\n1: one continuous hole per tear\nHigh: a tear breaks into a row of seperate holes'
if 'fabric_hole_bridge' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_hole_bridge'].keywords['description'] = 'Width of the fabric bridges between the pieces of a split tear, in cm\nLow: thin strips of fabric between the pieces\nHigh: wide bridges, the pieces look like separate holes'
if 'fabric_hole_dir_mode' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_hole_dir_mode'].keywords['description'] = 'How the tears are oriented\nRandom: every tear has its own direction\nWeave Axis: tears line up with the weave set by Hole Dir X / Y / Z'
if 'fabric_hole_dir_x' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_hole_dir_x'].keywords['description'] = 'X component of the warp direction (the lengthwise yarn) that tears follow, in object space. Set X, Y and Z together, they form one direction. Default (1, 0, 0)\n0: no part of the direction along X\n1: the direction points along X (mixed values give a diagonal)'
if 'fabric_hole_dir_y' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_hole_dir_y'].keywords['description'] = 'Y component of the warp direction (the lengthwise yarn) that tears follow, in object space. Set X, Y and Z together, they form one direction. Default (1, 0, 0)\n0: no part of the direction along Y\n1: the direction points along Y (mixed values give a diagonal)'
if 'fabric_hole_dir_z' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_hole_dir_z'].keywords['description'] = 'Z component of the warp direction (the lengthwise yarn) that tears follow, in object space. Set X, Y and Z together, they form one direction. Default (1, 0, 0)\n0: no part of the direction along Z\n1: the direction points along Z (mixed values give a diagonal)'
if 'fabric_hole_dir_jitter' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_hole_dir_jitter'].keywords['description'] = 'Random spread around the hole direction, in degrees\n0: all tears exactly parallel\nHigh: tears deviate strongly from the weave'
if 'fabric_edge_enable' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_edge_enable'].keywords['description'] = 'Turns Edge Tears on: slits that start at the border of the cloth and taper inward\nOff: no edge tears, the values are kept\non: torn slits along the borders'
if 'fabric_edge_amount' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_edge_amount'].keywords['description'] = 'Number of slits along all borders\n0: no edge tears\nHigh: a densely torn border'
if 'fabric_edge_spacing_jitter' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_edge_spacing_jitter'].keywords['description'] = 'How unevenly the slits are spaced along the border\n0: evenly spaced slits\nhigh: slits in clusters with long untorn stretches'
if 'fabric_edge_length_min' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_edge_length_min'].keywords['description'] = 'Shortest slit, how far it runs into the cloth, in cm. Keep it at or below Length Max\nLow: short nicks are possible\nHigh: every slit runs deep'
if 'fabric_edge_length_max' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_edge_length_max'].keywords['description'] = 'Longest slit, how far it runs into the cloth, in cm. Keep it at or above Length Min\nlow: only short nicks in the border\nHigh: long tears deep into the cloth are possible'
if 'fabric_edge_width_min' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_edge_width_min'].keywords['description'] = 'Narrowest slit at the border, in cm. Keep it at or below Width Max\nLow: thin cuts are possible\nHigh: every slit is wide'
if 'fabric_edge_width_max' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_edge_width_max'].keywords['description'] = 'Widest slit at the border, in cm. Keep it at or above Width Min\nLow: only thin cuts\nhigh: wide V-shaped gaps are possible'
if 'fabric_edge_taper' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_edge_taper'].keywords['description'] = 'How quickly a slit narrows from the border to its tip\n0: parallel sides, a slot\nHigh: a slit that closes quickly into a sharp point'
if 'fabric_edge_jagged' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_edge_jagged'].keywords['description'] = 'Raggedness of the slit sides\n0: clean cut sides\nHigh: torn, ragged sides'
if 'fabric_edge_meander' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_edge_meander'].keywords['description'] = 'How much a slit wanders sideways\n0: straight slits\nhigh: winding slits'
if 'fabric_edge_angle_jitter' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_edge_angle_jitter'].keywords['description'] = 'Random spread of the slit direction away from square to the border, in degrees\n0: slits run straight in from the border\nHigh: slits run in at slanted angles'
if 'fabric_threads_enable' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_threads_enable'].keywords['description'] = 'Adds thread geometry around the tears: bridging, broken and frayed threads\nOff: clean holes without threads\nOn: threads around the holes, set in the three boxes below'
if 'fabric_thread_radius' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_thread_radius'].keywords['description'] = 'Thickness of a thread, in cm\nLow: fine fibres, very thin threads need a high Resolution\nhigh: thick, clearly visible threads'
if 'fabric_thread_sides' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_thread_sides'].keywords['description'] = 'Number of sides of a thread tube. Only visible in extreme close-ups\n3: triangular threads, light mesh\n12: round threads, heavy mesh'
if 'fabric_thread_spacing' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_thread_spacing'].keywords['description'] = 'Distance between neighbouring threads along the rim, in cm\nLow: many threads close together\nHigh: few, widely spaced threads'
if 'fabric_bridge_enable' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_bridge_enable'].keywords['description'] = 'Threads that still span the gap of a tear from one side to the other\noff: no spanning threads\nOn: threads cross the holes'
if 'fabric_bridge_share' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_bridge_share'].keywords['description'] = 'Share of the possible threads that bridge the gap\n0: no bridges\nHigh: many threads across every hole'
if 'fabric_thread_sag' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_thread_sag'].keywords['description'] = 'How much a bridging thread hangs down under Gravity\n0: taut, straight threads\nHigh: threads sag in loose arcs'
if 'fabric_thread_skew' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_thread_skew'].keywords['description'] = 'How far a bridging thread runs at an angle instead of straight across\n0: threads cross straight\nhigh: threads cross at slanted angles'
if 'fabric_thread_wobble' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_thread_wobble'].keywords['description'] = 'Small waves along a thread\n0: smooth threads\nHigh: kinked, wavy threads'
if 'fabric_broken_enable' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_broken_enable'].keywords['description'] = 'Threads broken in the middle that dangle from the rim\nOff: no loose ends\nOn: broken threads hang out of the holes'
if 'fabric_broken_share' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_broken_share'].keywords['description'] = 'Share of the possible threads that are broken and dangling\n0: no broken threads\nhigh: many loose threads'
if 'fabric_thread_droop' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_thread_droop'].keywords['description'] = 'How much a broken thread hangs down\n0: broken threads stick out straight\nHigh: broken threads hang down'
if 'fabric_thread_curl' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_thread_curl'].keywords['description'] = 'How much a broken thread curls at its end\n0: straight loose ends\nHigh: curled, kinked ends'
if 'fabric_thread_taper' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_thread_taper'].keywords['description'] = 'How much a broken thread thins toward its end\n0: the thread keeps its thickness to the end\n1: the end thins to a point'
if 'fabric_fray_enable' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_fray_enable'].keywords['description'] = 'Short fibre stubs along the torn rim\nOff: a clean rim\non: a fuzzy, frayed rim'
if 'fabric_fray_length_min' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_fray_length_min'].keywords['description'] = 'Shortest frayed stub, in cm. Keep it at or below Fray Length Max\nLow: very short fuzz is possible\nHigh: every stub is long'
if 'fabric_fray_length_max' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_fray_length_max'].keywords['description'] = 'Longest frayed stub, in cm. Keep it at or above Fray Length Min\nlow: only a short fuzz\nHigh: long fringes are possible'
if 'fabric_gravity_x' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_gravity_x'].keywords['description'] = 'X component of the direction that sagging and drooping threads hang toward, in object space. Set X, Y and Z together, they form one direction. Default (0, 0, -1), down\n0: no part of the direction along X\n1: the direction points along X (mixed values give a diagonal)'
if 'fabric_gravity_y' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_gravity_y'].keywords['description'] = 'Y component of the direction that sagging and drooping threads hang toward, in object space. Set X, Y and Z together, they form one direction. Default (0, 0, -1), down\n0: no part of the direction along Y\n1: the direction points along Y (mixed values give a diagonal)'
if 'fabric_gravity_z' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_gravity_z'].keywords['description'] = 'Z component of the direction that sagging and drooping threads hang toward, in object space. Set X, Y and Z together, they form one direction. Default (0, 0, -1), down\n0: no part of the direction along Z\n1: the direction points along Z (mixed values give a diagonal)'
if 'fabric_max_threads' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_max_threads'].keywords['description'] = 'Limit on the total number of threads, shared between all tears. Keeps the mesh light\nLow: only a few threads per hole\nHigh: the spacing and share settings alone decide the number'
if 'fabric_solid_bevel' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_solid_bevel'].keywords['description'] = 'Fabric Solidify only. Rounds the torn rim of every hole on both faces of the cloth\n0: sharp, knife-cut rim edges\n1: a fully round rim, half the cloth thickness'
if 'fabric_solid_bevel_segments' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['fabric_solid_bevel_segments'].keywords['description'] = 'Fabric Solidify only. Smoothness of the rounded rim, rings per quarter circle\n1: a single chamfer\nHigh: a smooth round rim, heavier mesh'
if 'hit_enable' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['hit_enable'].keywords['description'] = 'Turns Concrete Hit on: the impact of a heavy blow at each empty, a crushed crater, broken skin plates and cracks\nOff: no impacts\nOn: one impact per empty in Impact Empties'
if 'hit_radius_scale' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['hit_radius_scale'].keywords['description'] = 'Multiplies the radius of every impact empty at once\nLow: damage zones smaller than the empties\nHigh: larger damage zones'
if 'hit_collection' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['hit_collection'].keywords['description'] = 'Collection with the impact empties. Each empty is one hit, its sphere radius is the radius of the damage zone. Add Hit creates the collection and an empty at the 3D cursor\nEmpty collection: no impacts\nMore empties: more hits, scale each empty to set its size'
if 'hit_detail' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['hit_detail'].keywords['description'] = 'Polygon density of the plates, crater and cracks. Changes how fine the result is and how fast it updates, not its size\nLow: coarse plates and blocky cracks, fast updates\nHigh: crisp cracks and a finely broken crater, heavy mesh'
if 'hit_seed' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['hit_seed'].keywords['description'] = 'Changes the random pattern of the plates and cracks without changing their size or number\nevery number gives a different arrangement\nthere is no low or high, only different variants'
if 'hit_layer_thickness' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['hit_layer_thickness'].keywords['description'] = 'Depth of the shattered outer skin, in cm\nlow: a thin skin, the plates are flakes\nHigh: thick plates with deep gaps between them'
if 'hit_plate_size' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['hit_plate_size'].keywords['description'] = 'Typical plate diameter at the rim of the zone, x radius\nLow: many small plates\nHigh: a few large plates'
if 'hit_plate_growth' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['hit_plate_growth'].keywords['description'] = 'How much the plates get smaller toward the centre\n0: plates of the same size everywhere\n1: large plates at the rim, small fragments near the crater'
if 'hit_zone_jagged' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['hit_zone_jagged'].keywords['description'] = 'Irregularity of the outer outline of the shatter zone\n0: a round zone\nhigh: a ragged, uneven zone outline'
if 'hit_shatter' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['hit_shatter'].keywords['description'] = 'Share of plates knocked out, more near the crater\n0: all plates stay in place, only cracked\n1: most plates are gone, the zone is a shallow hole'
if 'hit_tilt' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['hit_tilt'].keywords['description'] = 'How far the remaining plates are pushed in and tilted, x Layer Thickness\n0: flat plates, flush with the wall\nHigh: plates pushed in and tilted unevenly'
if 'hit_edge_jagged' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['hit_edge_jagged'].keywords['description'] = 'Irregularity of the plate edges\n0: straight plate edges\nHigh: rough, broken plate edges'
if 'hit_chip' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['hit_chip'].keywords['description'] = 'Small bites out of the plate rims\n0: clean plate rims\nhigh: heavily chipped rims'
if 'hit_core_size' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['hit_core_size'].keywords['description'] = 'Size of the crushed centre, x radius\nLow: a small crater\nHigh: a large crater that takes most of the zone'
if 'hit_core_depth' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['hit_core_depth'].keywords['description'] = 'Depth of the crater below the skin, x radius\nLow: a shallow dent\nhigh: a deep crater'
if 'hit_core_jagged' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['hit_core_jagged'].keywords['description'] = 'Raggedness of the crater rim\n0: a smooth elliptic rim\nHigh: a ragged, broken rim'
if 'hit_roughness' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['hit_roughness'].keywords['description'] = 'Unevenness of the crater floor (shape, not shader roughness)\n0: a smooth crater floor\nHigh: a rough, broken floor'
if 'hit_aggregate_size' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['hit_aggregate_size'].keywords['description'] = 'Size of the pits in the crater floor, in cm, like exposed aggregate\nLow: a fine, sandy floor\nhigh: coarse pits like large gravel'
if 'hit_thickness_cap' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['hit_thickness_cap'].keywords['description'] = 'The deepest the damage may cut, as a share of the local wall thickness. Protects thin walls\nLow: the damage stays shallow on thin walls\nHigh: the damage may cut almost through'
if 'hit_hole_enable' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['hit_hole_enable'].keywords['description'] = 'Punches a hole through the wall in the crater centre\nOff: a crater with a floor\nOn: a through-hole in the middle of the crater'
if 'hit_hole_size' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['hit_hole_size'].keywords['description'] = 'Size of the through-hole compared with the crater\nlow: a small hole in the crater floor\nHigh: a hole as large as the crater'
if 'hit_crack_enable' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['hit_crack_enable'].keywords['description'] = 'Adds radial cracks and crack rings around the shatter zone\nOff: only the plates and the crater\nOn: cracks run out into the wall'
if 'hit_crack_width' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['hit_crack_width'].keywords['description'] = 'Width of the crack grooves\nLow: hairline cracks\nHigh: wide, open cracks'
if 'hit_crack_depth' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['hit_crack_depth'].keywords['description'] = 'Depth of the crack grooves\nlow: shallow surface cracks\nHigh: deep cracks'
if 'hit_radial_count' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['hit_radial_count'].keywords['description'] = 'Number of radial cracks running out of the zone\n0: no radial cracks\nHigh: a star of many cracks'
if 'hit_radial_length_min' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['hit_radial_length_min'].keywords['description'] = 'Shortest radial crack, x radius. Keep it at or below Radial Length Max\nLow: short cracks close to the zone are possible\nHigh: every crack reaches far'
if 'hit_radial_length_max' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['hit_radial_length_max'].keywords['description'] = 'Longest radial crack, x radius. Keep it at or above Radial Length Min\nLow: only short cracks\nhigh: long cracks far into the wall are possible'
if 'hit_radial_meander' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['hit_radial_meander'].keywords['description'] = 'How much the radial cracks wander\n0: straight cracks\nHigh: winding cracks'
if 'hit_radial_branch' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['hit_radial_branch'].keywords['description'] = 'Chance of a fork on each radial crack\n0: single cracks\n1: every crack forks once'
if 'hit_ring_count' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['hit_ring_count'].keywords['description'] = 'Number of concentric crack rings outside the zone\n0: no rings\nHigh: several rings around the impact'
if 'hit_ring_fill' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['hit_ring_fill'].keywords['description'] = 'Share of each ring that is actually cracked\nLow: short ring pieces\n1: complete rings'
if 'plaster_enable' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['plaster_enable'].keywords['description'] = 'Turns Plaster Displacement on: hand-trowelled plaster relief with strokes, ridges, grain and hairline cracks. It changes the relief, it does not cut holes\nOff: the wall stays flat\nOn: the surface is subdivided and displaced'
if 'plaster_zone_group' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['plaster_zone_group'].keywords['description'] = 'Vertex group that limits the relief. Weight 1 is full relief, 0 is none. Assign Selected and Weight Paint fill it\nEmpty: the whole mesh gets plaster\nA group: plaster only where the weight is above 0'
if 'plaster_master_scale' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['plaster_master_scale'].keywords['description'] = 'Scales all sizes of the plaster at once: strokes, ridges, grain and cracks\nLow: small fine strokes, like a smooth finish\nHigh: large, coarse strokes'
if 'plaster_seed' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['plaster_seed'].keywords['description'] = 'Changes the random pattern of the strokes and cracks without changing their size or number\nevery number gives a different arrangement\nthere is no low or high, only different variants'
if 'plaster_detail' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['plaster_detail'].keywords['description'] = 'Density of the subdivided mesh that carries the relief\nLow: soft blurry relief and missing fine grain, fast updates\nhigh: sharp ridges and fine grain, heavy mesh'
if 'plaster_max_tris' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['plaster_max_tris'].keywords['description'] = 'Triangle budget of the subdivided mesh. Resolution is lowered automaticaly to stay under it\nLow: a light mesh, large walls lose detail\nHigh: full detail on large walls, slow updates'
if 'plaster_strength' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['plaster_strength'].keywords['description'] = 'Height of the relief from the lowest to the highest trowel level, in cm\nLow: a barely textured wall\nHigh: a heavy, rustic relief'
if 'plaster_sink' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['plaster_sink'].keywords['description'] = 'Whether the relief builds outward or inward from the original surface. The look stays the same, only the level shifts\n0: the plaster only builds outward\n1: the relief only goes into the wall'
if 'plaster_thickness_cap' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['plaster_thickness_cap'].keywords['description'] = 'The deepest the relief may go into the wall, as a share of the local wall thickness. Protects thin walls\nlow: the relief stays shallow on thin walls\nHigh: almost no limit, only matters on very thin walls'
if 'plaster_undulation' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['plaster_undulation'].keywords['description'] = 'Broad waviness under the strokes, x Strength\n0: a flat wall under the strokes\nHigh: the whole wall swells and dips'
if 'plaster_undulation_scale' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['plaster_undulation_scale'].keywords['description'] = 'Size of the waves of the undulation, in cm\nLow: short, bumpy waves\nHigh: long, gentle waves'
if 'plaster_coats' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['plaster_coats'].keywords['description'] = 'Number of layers of trowel strokes (0 to 6)\n0: no strokes, only undulation, grain and cracks\nhigh: many overlapping layers, a busy surface'
if 'plaster_coverage' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['plaster_coverage'].keywords['description'] = 'Stroke area per coat compared with the wall area\nLow: separate strokes with flat wall between them\nHigh: the strokes cover the wall several times over'
if 'plaster_length_min' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['plaster_length_min'].keywords['description'] = 'Shortest stroke along the trowel motion, in cm. Keep it at or below Length Max\nLow: short dabs are possible\nHigh: every stroke is long'
if 'plaster_length_max' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['plaster_length_max'].keywords['description'] = 'Longest stroke along the trowel motion, in cm. Keep it at or above Length Min\nlow: only short dabs\nHigh: long sweeping strokes are possible'
if 'plaster_width_min' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['plaster_width_min'].keywords['description'] = 'Narrowest stroke (the trowel blade), in cm. Keep it at or below Width Max\nLow: narrow strokes are possible\nHigh: every stroke is wide'
if 'plaster_width_max' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['plaster_width_max'].keywords['description'] = 'Widest stroke (the trowel blade), in cm. Keep it at or above Width Min\nLow: only narrow strokes\nhigh: wide strokes are possible'
if 'plaster_angle' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['plaster_angle'].keywords['description'] = 'Main direction of the strokes, in degrees from horizontal, measured around the wall normal\n0: horizontal strokes\n90: vertical strokes'
if 'plaster_angle_random' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['plaster_angle_random'].keywords['description'] = 'Random spread of the stroke direction (1 = up to 90 degrees each way)\n0: all strokes parallel\n1: strokes in all directions'
if 'plaster_cross_chance' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['plaster_cross_chance'].keywords['description'] = 'Share of strokes turned by 90 degrees, for cross-hatched trowelling\n0: all strokes in the main direction\n1: strokes in both directions'
if 'plaster_curve' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['plaster_curve'].keywords['description'] = 'How much a stroke bends into an arc\n0: straight strokes\n1: strongly curved, fan-like strokes'
if 'plaster_flatten' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['plaster_flatten'].keywords['description'] = 'How hard a stroke levels the plaster below it\n0: strokes add relief but do not smooth what is under them\nHigh: each stroke presses a flat patch into the surface'
if 'plaster_tilt' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['plaster_tilt'].keywords['description'] = 'Change of level along a stroke, like the trowel pitch, x Strength\n0: every stroke is level\nhigh: strokes slope noticeably from one end to the other'
if 'plaster_jagged' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['plaster_jagged'].keywords['description'] = 'Raggedness of the stroke outline\n0: smooth stroke edges\nHigh: ragged, torn stroke edges'
if 'plaster_streaks' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['plaster_streaks'].keywords['description'] = 'Drag lines along the stroke, x Strength\n0: smooth stroke surfaces\nHigh: strong striations along each stroke'
if 'plaster_ridge_height' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['plaster_ridge_height'].keywords['description'] = 'Height of the plaster lip pushed up at the stroke rim, x Strength\n0: no ridges, strokes blend into each other\nHigh: tall, pronounced ridges'
if 'plaster_ridge_width' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['plaster_ridge_width'].keywords['description'] = 'How far the ridge spreads past the stroke rim, in cm\nlow: a thin, narrow lip\nHigh: a broad, soft ridge'
if 'plaster_ridge_sharp' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['plaster_ridge_sharp'].keywords['description'] = 'Profile of the ridge\n0: a soft, rounded bead\nHigh: a crisp, knife-cut lip'
if 'plaster_ridge_trail' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['plaster_ridge_trail'].keywords['description'] = 'Where along the stroke rim the ridge forms\n0: an even ridge all round the stroke\nHigh: a ridge only at the trailing end, where the trowel lifted'
if 'plaster_grain' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['plaster_grain'].keywords['description'] = 'Height of the fine surface roughness, in cm. Only shows when Grain Size is large enough for the mesh\n0: smooth plaster\nHigh: a sandy, coarse surface'
if 'plaster_grain_size' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['plaster_grain_size'].keywords['description'] = 'Size of the grain bumps, in cm. Grain finer than about two mesh edges is faded out, so small sizes need a high Resolution\nLow: fine sand\nhigh: coarse lumps'
if 'plaster_pits' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['plaster_pits'].keywords['description'] = 'Share of small pits and air holes in the grain\n0: no pits\n1: a porous, pitted surface'
if 'plaster_crack_enable' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['plaster_crack_enable'].keywords['description'] = 'Adds a network of fine shrinkage cracks\nOff: no cracks\nOn: hairline cracks over the plaster'
if 'plaster_crack_amount' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['plaster_crack_amount'].keywords['description'] = 'Density of the crack network\nLow: a few isolated cracks\nHigh: a dense network'
if 'plaster_crack_scale' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['plaster_crack_scale'].keywords['description'] = 'Typical size of a crack cell, in cm\nlow: small cells, a fine crazing\nHigh: large cells, long cracks'
if 'plaster_crack_width' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['plaster_crack_width'].keywords['description'] = 'Width of the crack grooves\nLow: hairlines\nHigh: open cracks'
if 'plaster_crack_depth' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['plaster_crack_depth'].keywords['description'] = 'Depth of the crack grooves\nLow: barely visible cracks\nhigh: deep cracks'
if 'plaster_crack_meander' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['plaster_crack_meander'].keywords['description'] = 'Side-to-side wandering of the cracks\n0: straight cracks\nHigh: winding cracks'
if 'plaster_crack_breakup' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['plaster_crack_breakup'].keywords['description'] = 'How often a crack fades out along its run\n0: continuous cracks\nHigh: broken, interupted cracks'
if 'plaster_crack_step' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['plaster_crack_step'].keywords['description'] = 'Height offset between the two sides of a crack, in cm\n0: both sides level\nHigh: one side stands higher, a stepped crack'
if 'plaster_crack_lip' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['plaster_crack_lip'].keywords['description'] = 'Raised, smeared filler bead beside the crack, x Crack Depth\n0: a clean crack\nhigh: a crack with a raised filled lip, like a repaired crack'
if 'plaster_up_x' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['plaster_up_x'].keywords['description'] = 'X component of the up direction of the wall. Horizontal strokes and Angle are measured from it, in object space. Set X, Y and Z together, they form one direction. Default (0, 0, 1)\n0: no part of the direction along X\n1: the direction points along X (mixed values give a diagonal)'
if 'plaster_up_y' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['plaster_up_y'].keywords['description'] = 'Y component of the up direction of the wall. Horizontal strokes and Angle are measured from it, in object space. Set X, Y and Z together, they form one direction. Default (0, 0, 1)\n0: no part of the direction along Y\n1: the direction points along Y (mixed values give a diagonal)'
if 'plaster_up_z' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['plaster_up_z'].keywords['description'] = 'Z component of the up direction of the wall. Horizontal strokes and Angle are measured from it, in object space. Set X, Y and Z together, they form one direction. Default (0, 0, 1)\n0: no part of the direction along Z\n1: the direction points along Z (mixed values give a diagonal)'
if 'wallpaper_enable' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['wallpaper_enable'].keywords['description'] = 'Turns Wallpaper on: paper strips with thickness on the walls, with open seams, torn strips with hanging flaps and lifted edges\nOff: no wallpaper\nOn: walls are papered and damaged'
if 'wallpaper_drawing' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['wallpaper_drawing'].keywords['description'] = 'Grease Pencil or curve object with the strokes that outline areas where the paper is torn away. Without a drawing the tears are placed automatically. New Drawing creates one, Draw / Edit lets you draw on the object\nempty or no drawing: nothing is made from strokes\nMore strokes: more places get the effect'
if 'wallpaper_paint_reach' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['wallpaper_paint_reach'].keywords['description'] = 'How far a stroke can be from the wall and still be projected onto it\nLow: only strokes drawn right on the wall are used\nHigh: strokes away from the wall are used too'
if 'wallpaper_paint_jagged' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['wallpaper_paint_jagged'].keywords['description'] = 'Extra raggedness of the drawn tear outline\n0: the tear follows your outline exactly\nhigh: a ragged, torn edge around your outline'
if 'wallpaper_seam_snap' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['wallpaper_seam_snap'].keywords['description'] = 'Distance within which a drawn tear edge snaps to the nearest seam\nLow: the tear edge stays where you drew it\nHigh: edges near a seam jump onto it, like paper that tears off along the seam'
if 'wallpaper_detail' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['wallpaper_detail'].keywords['description'] = 'Polygon density of the paper, tear edges and flaps. Changes how fine the result is and how fast it updates, not its size\nLow: coarse tear edges and faceted flaps, fast updates\nHigh: finely torn edges and smooth curls, heavy mesh'
if 'wallpaper_seed' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['wallpaper_seed'].keywords['description'] = 'Changes the random pattern of the tears without changing their size or number\nevery number gives a different arrangement\nthere is no low or high, only different variants'
if 'wallpaper_thickness' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['wallpaper_thickness'].keywords['description'] = 'Thickness of the paper, in cm\nlow: thin paper, the tears are barely a step\nHigh: thick paper with a clear step at every edge'
if 'wallpaper_strip_width' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['wallpaper_strip_width'].keywords['description'] = 'Width of one wallpaper strip, in cm. A standard roll is 53 cm\nLow: many narrow strips\nHigh: few wide strips, tears get wider too'
if 'wallpaper_seam_offset' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['wallpaper_seam_offset'].keywords['description'] = 'Slides every seam sideways, in cm (0 to Strip Width)\n0: seams at the default position\nhigh: all seams shift. Use it to move a seam away from a corner or a door'
if 'wallpaper_seam_dir_x' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['wallpaper_seam_dir_x'].keywords['description'] = 'X component of the "up" direction along the strips, in object space. Set X, Y and Z together, they form one direction. Default (0, 0, 1), vertical strips\n0: no part of the direction along X\n1: the direction points along X (mixed values give a diagonal)'
if 'wallpaper_seam_dir_y' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['wallpaper_seam_dir_y'].keywords['description'] = 'Y component of the "up" direction along the strips, in object space. Set X, Y and Z together, they form one direction. Default (0, 0, 1), vertical strips\n0: no part of the direction along Y\n1: the direction points along Y (mixed values give a diagonal)'
if 'wallpaper_seam_dir_z' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['wallpaper_seam_dir_z'].keywords['description'] = 'Z component of the "up" direction along the strips, in object space. Set X, Y and Z together, they form one direction. Default (0, 0, 1), vertical strips\n0: no part of the direction along Z\n1: the direction points along Z (mixed values give a diagonal)'
if 'wallpaper_wall_angle' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['wallpaper_wall_angle'].keywords['description'] = 'A face is papered when its normal is within this angle of horizontal, in degrees. Floors and ceilings stay bare\nLow: only exactly vertical walls get paper\nHigh: sloped faces, like attic walls, get paper too'
if 'wallpaper_seams_enable' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['wallpaper_seams_enable'].keywords['description'] = 'Cuts the joints between the strips\nOff: the paper is one continuous sheet\nOn: a visible joint between every two strips'
if 'wallpaper_seam_width' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['wallpaper_seam_width'].keywords['description'] = 'Width of the gap at a seam, in cm\nLow: tight, barely visible joints\nhigh: wide gaps between strips'
if 'wallpaper_seam_depth' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['wallpaper_seam_depth'].keywords['description'] = 'Depth of the seam gap, x Thickness (1 = down to the wall)\nLow: a shallow line in the paper\nHigh: a gap down to the wall'
if 'wallpaper_seam_open' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['wallpaper_seam_open'].keywords['description'] = 'Stretches of the seam where it gapes wider\n0: an even seam\n1: seams gape open along many stretches'
if 'wallpaper_tears_enable' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['wallpaper_tears_enable'].keywords['description'] = 'Tears paper off the strips\nOff: intact paper\non: torn strips with flaps'
if 'wallpaper_tear_amount' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['wallpaper_tear_amount'].keywords['description'] = 'Number of torn strips, one per strip, never two side by side. Fewer if the walls run out of strips\nLow: a single tear\nHigh: many torn strips'
if 'wallpaper_tear_length_min' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['wallpaper_tear_length_min'].keywords['description'] = 'Shortest torn area along the strip, in cm. Keep it at or below Tear Length Max\nlow: short tears near the top are possible\nHigh: every tear runs far down'
if 'wallpaper_tear_length_max' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['wallpaper_tear_length_max'].keywords['description'] = 'Longest torn area along the strip, in cm. Keep it at or above Tear Length Min\nLow: only short tears\nHigh: tears running far down the wall are possible'
if 'wallpaper_tear_width_min' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['wallpaper_tear_width_min'].keywords['description'] = 'Narrowest tear, as a share of the strip width. Keep it at or below Tear Width Max\nLow: narrow tears inside the strip are possible\nHigh: every tear is wide'
if 'wallpaper_tear_width_max' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['wallpaper_tear_width_max'].keywords['description'] = 'Widest tear, as a share of the strip width (1 = seam to seam). Keep it at or above Tear Width Min\nlow: only narrow tears\n1: the full strip width can be torn'
if 'wallpaper_tear_diagonal' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['wallpaper_tear_diagonal'].keywords['description'] = 'Shape of the tear front\n0: a level tear front across the strip\n1: the front runs diagonally down from the top corner'
if 'wallpaper_tear_from_top' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['wallpaper_tear_from_top'].keywords['description'] = 'Chance that a tear starts at the top edge of the wall\n0: tears start anywhere on the strip\n1: every tear starts at the top edge'
if 'wallpaper_tear_side' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['wallpaper_tear_side'].keywords['description'] = 'Which seam the long side of the tear follows\n0: a random side for each tear\n1 or 2: all tears hang on the left or on the right seam'
if 'wallpaper_tear_jagged' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['wallpaper_tear_jagged'].keywords['description'] = 'Raggedness of the torn edges\n0: smooth torn edges\nHigh: ragged, frayed edges'
if 'wallpaper_seam_crack' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['wallpaper_seam_crack'].keywords['description'] = 'Chance of a split running on down the seam below a tear\n0: the seam is intact below the tear\n1: the seam splits below every tear'
if 'wallpaper_seam_crack_length' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['wallpaper_seam_crack_length'].keywords['description'] = 'Length of that split, x tear length\nlow: a short split\nHigh: a split running far down the wall'
if 'wallpaper_flaps_enable' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['wallpaper_flaps_enable'].keywords['description'] = 'Keeps the torn paper as a hanging flap\nOff: the torn paper is removed, only the bare patch remains\nOn: the paper hangs from the tear front'
if 'wallpaper_flap_amount' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['wallpaper_flap_amount'].keywords['description'] = 'Share of the torn paper still hanging on\n0: torn off, nothing hangs\nHigh: almost all torn paper hangs down'
if 'wallpaper_flap_fold' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['wallpaper_flap_fold'].keywords['description'] = 'How far the flap folds over at the tear front, in degrees (180 = flat on the wall)\nLow: the flap sticks out from the wall\n180: the flap lies folded flat against the wall'
if 'wallpaper_flap_fold_radius' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['wallpaper_flap_fold_radius'].keywords['description'] = 'Radius of the fold crease, x tear size\nLow: a sharp crease\nhigh: a soft, rounded fold'
if 'wallpaper_flap_curl' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['wallpaper_flap_curl'].keywords['description'] = 'How far the hanging paper turns outward toward its free end, in degrees (0 to 300)\n0: a flat flap\nHigh: the end rolls up into a curl'
if 'wallpaper_flap_curl_focus' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['wallpaper_flap_curl_focus'].keywords['description'] = 'Where the curl happens along the flap\n1: one even arc over the whole flap\nHigh: a straight hang with a tight roll at the end'
if 'wallpaper_flap_cone' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['wallpaper_flap_cone'].keywords['description'] = 'How much the roll gathers where the flap reaches farthest, giving a cone-shaped roll\n0: a cylindrical roll\n1: a strongly conical roll'
if 'wallpaper_flap_wave' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['wallpaper_flap_wave'].keywords['description'] = 'Soft waviness of the paper\n0: smooth, stiff paper\n1: wavy, soft paper'
if 'wallpaper_flap_ragged' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['wallpaper_flap_ragged'].keywords['description'] = 'Raggedness of the torn end of the flap\n0: a clean flap end\nhigh: a ragged, torn flap end'
if 'wallpaper_lifts_enable' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['wallpaper_lifts_enable'].keywords['description'] = 'Lifts small edges of paper along free seams\nOff: all seams lie flat\nOn: some seam edges peel and curl'
if 'wallpaper_lift_amount' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['wallpaper_lift_amount'].keywords['description'] = 'Number of lifted edges. Fewer if there is no free seam\n0: no lifts\nHigh: many peeling seams'
if 'wallpaper_lift_length_min' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['wallpaper_lift_length_min'].keywords['description'] = 'Shortest lifted edge along the seam, in cm. Keep it at or below Lift Length Max\nLow: short peeled spots are possible\nHigh: every lift is long'
if 'wallpaper_lift_length_max' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['wallpaper_lift_length_max'].keywords['description'] = 'Longest lifted edge along the seam, in cm. Keep it at or above Lift Length Min\nlow: only short peeled spots\nHigh: long peeled stretches are possible'
if 'wallpaper_lift_width_min' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['wallpaper_lift_width_min'].keywords['description'] = 'Narrowest lift, how far the paper is peeled off from the seam, in cm. Keep it at or below Lift Width Max\nLow: a narrow lifted edge is possible\nHigh: every lift is a wide band'
if 'wallpaper_lift_width_max' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['wallpaper_lift_width_max'].keywords['description'] = 'Widest lift, how far the paper is peeled off from the seam, in cm. Keep it at or above Lift Width Min\nLow: only narrow lifted edges\nhigh: wide peeled bands are possible'
if 'wallpaper_lift_curl' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['wallpaper_lift_curl'].keywords['description'] = 'How far a lifted edge curls, in degrees\nLow: the edge just stands off the wall\nHigh: the edge rolls up'
if 'bullet_enable' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_enable'].keywords['description'] = 'Turns Bullet Impacts on: shot holes whose shape depends on the Material Type. Thin objects are shot through and get an exit wound\nOff: no shots\nOn: holes with the settings below'
if 'bullet_material' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_material'].keywords['description'] = 'Material the shots hit. Decides the shape of the wound and which settings box is shown\nMetal: a clean hole with a torn crown. Wood: a crushed hole with splinters along the grain\nConcrete / Stone: a wide spall crater with a bore hole in the middle'
if 'bullet_mode' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_mode'].keywords['description'] = 'How the shots are placed\nAutomatic: shots and bursts spread over the object by the Distribute settings\nPaint: shots follow your strokes, a tiny stroke or a dot gives a single shot'
if 'bullet_drawing' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_drawing'].keywords['description'] = 'Grease Pencil or curve object with the strokes that place the shots (Paint mode). New Drawing creates one, Draw / Edit lets you draw on the object\nempty or no drawing: nothing is made from strokes\nMore strokes: more places get the effect'
if 'bullet_paint_spacing' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_paint_spacing'].keywords['description'] = 'Paint mode. Distance between shots along a stroke, in cm\nLow: a dense line of shots, holes merge\nhigh: a sparse line of separate shots'
if 'bullet_paint_spread' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_paint_spread'].keywords['description'] = 'Paint mode. How far shots scatter sideways from the stroke, in cm\n0: shots exactly on the stroke\nHigh: a wide band of shots around the stroke'
if 'bullet_paint_snap' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_paint_snap'].keywords['description'] = 'Paint mode. How far a stroke can be from the surface and still be projected onto it, in cm\nLow: only strokes drawn right on the surface are used\nHigh: strokes away from the surface are used too'
if 'bullet_paint_close' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_paint_close'].keywords['description'] = 'Paint mode. Treats a nearly closed stroke as a filled area instead of a line\nlow: only exactly closed loops are filled\nHigh: loosely closed loops are filled with shots too'
if 'bullet_paint_single' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_paint_single'].keywords['description'] = 'Paint mode. Strokes shorter than this become one single shot, in cm\nLow: even short strokes give a line of shots\nHigh: longer strokes still count as one shot'
if 'bullet_detail' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_detail'].keywords['description'] = 'Polygon density of the holes and craters. Changes how fine the result is and how fast it updates, not its size\nLow: faceted holes and craters, fast updates\nHigh: round holes and finely broken craters, heavy mesh'
if 'bullet_seed' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_seed'].keywords['description'] = 'Changes the random pattern of the shots without changing their size or number\nevery number gives a different arrangement\nthere is no low or high, only different variants'
if 'bullet_amount' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_amount'].keywords['description'] = 'Number of shots on the whole object. Placement can give fewer where there is no room\nLow: a few hits\nhigh: a heavily shot-up surface'
if 'bullet_clusters' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_clusters'].keywords['description'] = 'Number of bursts the clustered shots are grouped into\n0: no bursts, every shot is placed on its own\nHigh: many small bursts'
if 'bullet_cluster_share' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_cluster_share'].keywords['description'] = 'Share of the shots that belong to a burst\n0: all shots scattered evenly\n1: all shots in bursts'
if 'bullet_cluster_spread' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_cluster_spread'].keywords['description'] = 'Radius of a burst, in cm\nLow: tight groups of holes\nHigh: loose, wide groups'
if 'bullet_min_spacing' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_min_spacing'].keywords['description'] = 'Closest distance between two hole centres, x the sum of their radii\nBelow 1: holes may overlap and merge\nhigh: holes keep a clear distance from each other'
if 'bullet_size_min' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_size_min'].keywords['description'] = 'Smallest hole, in cm. Keep it at or below Size Max\nLow: small calibre holes are possible\nHigh: every hole is large'
if 'bullet_size_max' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_size_max'].keywords['description'] = 'Largest hole, in cm. Keep it at or above Size Min\nlow: only small holes\nHigh: large holes and craters are possible'
if 'bullet_size_bias' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_size_bias'].keywords['description'] = 'Share of small holes within the size range\n0: mostly large holes\n1: mostly small holes, 0.5 is an even spread'
if 'bullet_penetration' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_penetration'].keywords['description'] = 'A wall up to this thick is shot through, in cm\nLow: shots stop in the wall, no exit wound\nHigh: shots go through and blow out an exit wound on the back'
if 'bullet_penetration_var' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_penetration_var'].keywords['description'] = 'Random variation of the penetration from shot to shot\n0: every shot penetrates the same\n1: some shots go through and others stop'
if 'bullet_thickness_cap' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_thickness_cap'].keywords['description'] = 'The deepest a shot that does not go through may cut, as a share of the local thickness. Protects thin walls from almost-holes\nLow: shallow craters on thin walls\nhigh: craters may cut almost through'
if 'bullet_shot_mode' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_shot_mode'].keywords['description'] = 'Direction of the shots\nRandom Slant: every shot has its own slanted direction, holes stretched in many directions\nFixed Direction: all shots come from Shot X / Y / Z, like fire from one position'
if 'bullet_shot_x' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_shot_x'].keywords['description'] = 'X component of the direction the shots travel (Fixed Direction). Straight into the face gives round holes, slanted gives stretched holes, in object space. Set X, Y and Z together, they form one direction. Default (0, 1, 0)\n0: no part of the direction along X\n1: the direction points along X (mixed values give a diagonal)'
if 'bullet_shot_y' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_shot_y'].keywords['description'] = 'Y component of the direction the shots travel (Fixed Direction). Straight into the face gives round holes, slanted gives stretched holes, in object space. Set X, Y and Z together, they form one direction. Default (0, 1, 0)\n0: no part of the direction along Y\n1: the direction points along Y (mixed values give a diagonal)'
if 'bullet_shot_z' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_shot_z'].keywords['description'] = 'Z component of the direction the shots travel (Fixed Direction). Straight into the face gives round holes, slanted gives stretched holes, in object space. Set X, Y and Z together, they form one direction. Default (0, 1, 0)\n0: no part of the direction along Z\n1: the direction points along Z (mixed values give a diagonal)'
if 'bullet_oblique' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_oblique'].keywords['description'] = 'Random Slant only. How slanted the random shots are (1 = up to about 70 degrees)\n0: all shots hit straight on, round holes\n1: steep shots with long, stretched craters'
if 'bullet_elongate' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_elongate'].keywords['description'] = 'How much a slanted shot stretches its hole and crater\n0: round holes even for slanted shots\nHigh: strongly stretched, teardrop holes'
if 'bullet_halo_enable' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_halo_enable'].keywords['description'] = 'Adds a very shallow ring of damaged surface around each hole\nOff: the surface is untouched around the wound\non: a shallow damaged ring around every hole'
if 'bullet_halo_scale' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_halo_scale'].keywords['description'] = 'Radius of the halo, x size\nLow: a ring tight around the hole\nHigh: a wide ring'
if 'bullet_halo_depth' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_halo_depth'].keywords['description'] = 'Depth of the halo, in cm\nLow: only a trace on the surface, mostly for materials\nHigh: a visible shallow dish around the hole'
if 'bullet_halo_jagged' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_halo_jagged'].keywords['description'] = 'Irregularity of the halo outline\n0: a round halo\nhigh: a ragged halo'
if 'bullet_pit_amount' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_pit_amount'].keywords['description'] = 'Number of small pits around each impact, from fragments and debris\n0: no pits\nHigh: many pits around every hole'
if 'bullet_pit_size' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_pit_size'].keywords['description'] = 'Size of a pit, x size\nLow: tiny dots\nHigh: large pits'
if 'bullet_pit_spread' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_pit_spread'].keywords['description'] = 'How far the pits scatter from the hole, x size\nlow: pits close to the hole\nHigh: pits spread far around it'
if 'bullet_conc_spall_chance' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_conc_spall_chance'].keywords['description'] = 'Concrete / Stone. Share of shots that blow out a conical spall crater\n0: only small bore holes\n1: every shot leaves a wide crater'
if 'bullet_conc_spall_scale' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_conc_spall_scale'].keywords['description'] = 'Concrete / Stone. Radius of the spall crater, x size\nLow: a small crater around the hole\nHigh: a wide crater'
if 'bullet_conc_spall_depth' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_conc_spall_depth'].keywords['description'] = 'Concrete / Stone. Depth of the cone in the centre, x size\nLow: a shallow, dish-like crater\nhigh: a deep, steep cone'
if 'bullet_conc_skin_depth' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_conc_skin_depth'].keywords['description'] = 'Concrete / Stone. Shallow outer terrace where the surface skin flaked off, x size\n0: the crater starts directly at the surface\nHigh: a wide stepped terrace around the crater'
if 'bullet_conc_bore_depth' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_conc_bore_depth'].keywords['description'] = 'Concrete / Stone. Depth of the bore hole below the cone, x size\nLow: a shallow dent in the crater floor\nHigh: a deep, dark bore hole'
if 'bullet_conc_jagged' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_conc_jagged'].keywords['description'] = 'Concrete / Stone. Lobed, ragged outline of the spall\n0: a round crater\nhigh: a ragged, lobed crater'
if 'bullet_conc_roughness' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_conc_roughness'].keywords['description'] = 'Concrete / Stone. Pitted, shell-like crater floor\n0: a smooth cone\nHigh: a rough, broken floor'
if 'bullet_conc_aggregate' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_conc_aggregate'].keywords['description'] = 'Concrete / Stone. Size of the pits in the crater floor, x size\nLow: a fine floor\nHigh: coarse pits like exposed gravel'
if 'bullet_conc_exit_scale' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_conc_exit_scale'].keywords['description'] = 'Concrete / Stone. Size of the exit crater on the back face compared with the entry\nlow: a small exit hole\nHigh: a wide blow-out on the back, as in real concrete'
if 'bullet_conc_cracks' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_conc_cracks'].keywords['description'] = 'Concrete / Stone. Number of hairline cracks running out of each spall\n0: no cracks\nHigh: a star of cracks around each crater'
if 'bullet_crack_width' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_crack_width'].keywords['description'] = 'Width of the cracks around the craters, in cm\nLow: hairlines\nhigh: open cracks'
if 'bullet_crack_depth' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_crack_depth'].keywords['description'] = 'Depth of the cracks around the craters, in cm\nLow: surface cracks\nHigh: deep cracks'
if 'bullet_conc_crack_length' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_conc_crack_length'].keywords['description'] = 'Concrete / Stone. Length of the cracks, x spall radius\nLow: short cracks inside the crater area\nHigh: long cracks reaching far out'
if 'bullet_metal_hole_jagged' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_metal_hole_jagged'].keywords['description'] = 'Metal. Raggedness of the hole outline\n0: a clean round hole\nHigh: a torn, ragged hole'
if 'bullet_metal_dish' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_metal_dish'].keywords['description'] = 'Metal. How deep the plate is dented in around the hole, x size\n0: a flat plate around the hole\nhigh: a deep dent around the hole'
if 'bullet_metal_dish_width' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_metal_dish_width'].keywords['description'] = 'Metal. Radius of the dent, x size\nLow: a tight dent\nHigh: a wide, shallow dish'
if 'bullet_metal_lip_height' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_metal_lip_height'].keywords['description'] = 'Metal. Height of the raised burr crown on the entry side, x size\n0: no crown\nHigh: a tall crown of torn metal'
if 'bullet_metal_lip_width' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_metal_lip_width'].keywords['description'] = 'Metal. Width of the crown, x size\nlow: a narrow crown\nHigh: a wide crown'
if 'bullet_metal_petals' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_metal_petals'].keywords['description'] = 'Metal. Number of torn petals in the crown\nLow: a few large petals\nHigh: many narrow petals'
if 'bullet_metal_petal_tear' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_metal_petal_tear'].keywords['description'] = 'Metal. How deep the tears between the petals go\n0: a closed ring\n1: seperate petals torn apart'
if 'bullet_metal_curl' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_metal_curl'].keywords['description'] = 'Metal. How much the crown curls in over the hole\n0: the crown stands straight\n1: the petals curl over the hole'
if 'bullet_metal_exit_burr' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_metal_exit_burr'].keywords['description'] = 'Metal. Length of the petals on the exit (back) side, x size\n0: a clean exit hole\nhigh: long petals bent outward on the back'
if 'bullet_metal_dent_chance' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_metal_dent_chance'].keywords['description'] = 'Metal. Share of shots that only dent the plate, even when it is thin enough to go through\n0: every shot makes a hole\n1: only dents, no holes'
if 'bullet_grain_mode' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_grain_mode'].keywords['description'] = 'Wood. How the direction of the wood grain is found\nLongest Axis: the grain runs along the longest side of the object\nFixed Axis: the grain follows Grain X / Y / Z'
if 'bullet_grain_x' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_grain_x'].keywords['description'] = 'X component of the wood grain direction (Fixed Axis). Splinters and splits run along it, in object space. Set X, Y and Z together, they form one direction. Default (0, 0, 1)\n0: no part of the direction along X\n1: the direction points along X (mixed values give a diagonal)'
if 'bullet_grain_y' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_grain_y'].keywords['description'] = 'Y component of the wood grain direction (Fixed Axis). Splinters and splits run along it, in object space. Set X, Y and Z together, they form one direction. Default (0, 0, 1)\n0: no part of the direction along Y\n1: the direction points along Y (mixed values give a diagonal)'
if 'bullet_grain_z' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_grain_z'].keywords['description'] = 'Z component of the wood grain direction (Fixed Axis). Splinters and splits run along it, in object space. Set X, Y and Z together, they form one direction. Default (0, 0, 1)\n0: no part of the direction along Z\n1: the direction points along Z (mixed values give a diagonal)'
if 'bullet_wood_crush' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_wood_crush'].keywords['description'] = 'Wood. Depth of the crushed funnel around the hole, x size\n0: a clean hole\nHigh: a deep crushed funnel'
if 'bullet_wood_splinter' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_wood_splinter'].keywords['description'] = 'Wood. How far splinter strips reach along the grain, x size\n0: no splinters\nhigh: long splinters along the grain'
if 'bullet_wood_fiber_width' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_wood_fiber_width'].keywords['description'] = 'Wood. Width of the splinter strips, x size\nLow: thin fibres\nHigh: wide strips'
if 'bullet_wood_exit_scale' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_wood_exit_scale'].keywords['description'] = 'Wood. Radius of the tear-out on the back, x size\nLow: a small exit hole\nHigh: a large torn-out area on the back'
if 'bullet_wood_exit_depth' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_wood_exit_depth'].keywords['description'] = 'Wood. Depth of the exit tear-out, as a share of the board thickness\nlow: a shallow tear-out\nHigh: a tear-out almost through the board'
if 'bullet_wood_slivers' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_wood_slivers'].keywords['description'] = 'Wood. Number of loose splinters added at the exit tear-out\n0: no loose splinters\nHigh: many splinters sticking out'
if 'bullet_wood_sliver_length' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_wood_sliver_length'].keywords['description'] = 'Wood. Length of the loose splinters, x size\nLow: short splinters\nhigh: long splinters'
if 'bullet_wood_split_chance' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_wood_split_chance'].keywords['description'] = 'Wood. Chance of a split along the grain through the hole\n0: no splits\n1: every hole has a split'
if 'bullet_wood_split_length' in ProjectDamageProps.__annotations__:ProjectDamageProps.__annotations__['bullet_wood_split_length'].keywords['description'] = 'Wood. Length of the split, x size\nLow: a short split\nHigh: a long crack along the grain'
if 'build_mode' in ProjectDamageProps.__annotations__:
    ProjectDamageProps.__annotations__['build_mode'].keywords['items'] = tuple((item[0], item[1], {'AUTO': 'Inside edges are picked at random by Edges Covered', 'PAINT': 'Only inside edges near your strokes get a bead; an empty drawing gives no bead'}.get(item[0], item[2]), *item[3:]) for item in ProjectDamageProps.__annotations__['build_mode'].keywords['items'])
if 'build_shape' in ProjectDamageProps.__annotations__:
    ProjectDamageProps.__annotations__['build_shape'].keywords['items'] = tuple((item[0], item[1], {'0': 'A narrow half-round bead lying in the corner', '1': 'fills the corner as a cove that runs out onto both faces'}.get(item[0], item[2]), *item[3:]) for item in ProjectDamageProps.__annotations__['build_shape'].keywords['items'])
if 'scratch_mode' in ProjectDamageProps.__annotations__:
    ProjectDamageProps.__annotations__['scratch_mode'].keywords['items'] = tuple((item[0], item[1], {'0': 'Many short scratches scattered around each stroke', '1': 'Each stroke becomes exactly one scratch; placement sliders are ignored'}.get(item[0], item[2]), *item[3:]) for item in ProjectDamageProps.__annotations__['scratch_mode'].keywords['items'])
if 'scratch_style' in ProjectDamageProps.__annotations__:
    ProjectDamageProps.__annotations__['scratch_style'].keywords['items'] = tuple((item[0], item[1], {'0': 'Thin, light hairline scratches', '1': 'Fewer, deeper and longer cuts', '2': 'many very shallow marks, a worn band', '3': 'Shows all shape, bundle and scuff settings for your own values'}.get(item[0], item[2]), *item[3:]) for item in ProjectDamageProps.__annotations__['scratch_style'].keywords['items'])
if 'scratch_profile' in ProjectDamageProps.__annotations__:
    ProjectDamageProps.__annotations__['scratch_profile'].keywords['items'] = tuple((item[0], item[1], {'0': 'A sharp groove, like a cut with a point', '1': 'A rounded groove, like a dragged blunt object'}.get(item[0], item[2]), *item[3:]) for item in ProjectDamageProps.__annotations__['scratch_profile'].keywords['items'])
if 'fabric_placement' in ProjectDamageProps.__annotations__:
    ProjectDamageProps.__annotations__['fabric_placement'].keywords['items'] = tuple((item[0], item[1], {'PROCEDURAL': 'Tears placed automatically from Interior Tears and Edge Tears', 'DRAW': 'Only the tears you draw; an empty drawing leaves the cloth intact'}.get(item[0], item[2]), *item[3:]) for item in ProjectDamageProps.__annotations__['fabric_placement'].keywords['items'])
if 'fabric_tear_style' in ProjectDamageProps.__annotations__:
    ProjectDamageProps.__annotations__['fabric_tear_style'].keywords['items'] = tuple((item[0], item[1], {'0': 'Ragged lens-shaped holes that can split, with fabric bridges between the pieces', '1': 'Tears that follow the weave: rips, corner tears with a flap, worn holes, punctures'}.get(item[0], item[2]), *item[3:]) for item in ProjectDamageProps.__annotations__['fabric_tear_style'].keywords['items'])
if 'fabric_tear_preset' in ProjectDamageProps.__annotations__:
    ProjectDamageProps.__annotations__['fabric_tear_preset'].keywords['items'] = tuple((item[0], item[1], {'0': 'Keep the current values', '1': 'Worn holes with the weft still spanning them and a few punctures, like knees and seat of old jeans', '2': 'Corner (L) tears from a nail with folded flaps and short rips', '3': 'long rips along the grain and some corner tears, like tents, sails or sacks', '4': 'Many small holes and punctures with worn spots around them', '5': 'Everything mixed: bias tears, ragged rims and torn edges', '6': 'The old Classic shapes: ragged lenses split into holes'}.get(item[0], item[2]), *item[3:]) for item in ProjectDamageProps.__annotations__['fabric_tear_preset'].keywords['items'])
if 'fabric_hole_dir_mode' in ProjectDamageProps.__annotations__:
    ProjectDamageProps.__annotations__['fabric_hole_dir_mode'].keywords['items'] = tuple((item[0], item[1], {'0': 'Every tear gets its own random direction', '1': 'Tears follow the weave set by Hole Dir X / Y / Z'}.get(item[0], item[2]), *item[3:]) for item in ProjectDamageProps.__annotations__['fabric_hole_dir_mode'].keywords['items'])
if 'bullet_material' in ProjectDamageProps.__annotations__:
    ProjectDamageProps.__annotations__['bullet_material'].keywords['items'] = tuple((item[0], item[1], {'0': 'A clean hole with a torn crown of petals, exit burr on the back', '1': 'A crushed hole with splinters along the grain and a tear-out on the back', '2': 'a wide conical spall crater with a bore hole and hairline cracks'}.get(item[0], item[2]), *item[3:]) for item in ProjectDamageProps.__annotations__['bullet_material'].keywords['items'])
if 'bullet_mode' in ProjectDamageProps.__annotations__:
    ProjectDamageProps.__annotations__['bullet_mode'].keywords['items'] = tuple((item[0], item[1], {'0': 'Shots and bursts spread over the object by the Distribute settings', '1': 'Shots follow your Grease Pencil or curve strokes'}.get(item[0], item[2]), *item[3:]) for item in ProjectDamageProps.__annotations__['bullet_mode'].keywords['items'])
if 'bullet_shot_mode' in ProjectDamageProps.__annotations__:
    ProjectDamageProps.__annotations__['bullet_shot_mode'].keywords['items'] = tuple((item[0], item[1], {'0': 'Every shot gets its own slanted direction', '1': 'All shots travel along Shot X / Y / Z'}.get(item[0], item[2]), *item[3:]) for item in ProjectDamageProps.__annotations__['bullet_shot_mode'].keywords['items'])
if 'bullet_grain_mode' in ProjectDamageProps.__annotations__:
    ProjectDamageProps.__annotations__['bullet_grain_mode'].keywords['items'] = tuple((item[0], item[1], {'0': 'The grain runs along the longest side of the object', '1': 'The grain follows Grain X / Y / Z'}.get(item[0], item[2]), *item[3:]) for item in ProjectDamageProps.__annotations__['bullet_grain_mode'].keywords['items'])

from . import voxel_erosion as _voxel_erosion
for _name, _prop in _voxel_erosion.PROPS.items():
    ProjectDamageProps.__annotations__[_name] = _prop

                                                 
ProjectDamageProps.__annotations__['rusty_thickness'].keywords.update(max=100.0, soft_max=100.0)

                                                     
for _name, _value in {'build_coverage':100.0,'build_scale':250.0,'build_detail':400.0,'build_shape':'0','build_width':0.74,'build_height':0.40,'build_irreg':81.97,'build_lumps':30.0}.items():
    ProjectDamageProps.__annotations__[_name].keywords['default'] = _value

from . import metal_tear as _metal_tear
ProjectDamageProps.__annotations__.update(_metal_tear.PROPS)

from . import clay as _clay
for _name,_prop in _clay.PROPS.items():ProjectDamageProps.__annotations__[_name]=_prop

                                                                            
from . import peeling as _peeling
for _name,_prop in _peeling.PROPS.items():ProjectDamageProps.__annotations__[_name]=_prop

                                                                               
from . import wood_wall as _wood_wall
ProjectDamageProps.__annotations__.update({
 'concrete_wall_type':bpy.props.EnumProperty(name='Wall Type',items=[('CONCRETE','Concrete','Mineral cavity'),('WOOD','Wooden','Grain-aligned broken wooden skin over a straight cavity')],default='CONCRETE',update=_wood_wall.changed),
 'concrete_wood_thickness':bpy.props.FloatProperty(name='Wood Layer Thickness (cm)',default=2,min=.05,soft_max=20,update=live.touch_prop),
 'concrete_wood_width':bpy.props.FloatProperty(name='Plank Width (cm)',default=12,min=.2,soft_max=40,update=live.touch_prop),
 'concrete_wood_splinter':bpy.props.FloatProperty(name='Splinter Length (cm)',default=6,min=0,soft_max=30,update=live.touch_prop),
 'concrete_wood_angle':bpy.props.FloatProperty(name='Grain Angle',default=0,min=-180,max=180,update=live.touch_prop),
})



                                                                            
ProjectDamageProps.__annotations__.update({
    'frame_restructure_angle': bpy.props.FloatProperty(name='Break Angle', default=35., min=1., max=179., description='Relative rotation in degrees between supporting fragments that breaks unprotected timber members'),
    'frame_restructure_minimum': bpy.props.IntProperty(name='Minimum Connections', default=2, min=1, max=32, description='Keep at least this many continuous timber connections per fragment pair, limited by available members. Other members may break'),
    'frame_restructure_collisions': bpy.props.BoolProperty(name='Break on Intersection', default=True, description='Break or shorten timber that crosses the remaining wall surfaces; keep the original embed at anchors. Protected connections seek a clear route'),
})

                                                                           
for _name in ('size_min', 'size_max', 'surf_size_min', 'surf_size_max', 'corner_size_min', 'corner_size_max', 'concrete_size_min', 'concrete_size_max', 'lath_size_min', 'lath_size_max', 'brick_size_min', 'brick_size_max', 'hang_size_min', 'hang_size_max', 'ground_size_min', 'ground_size_max', 'bullet_size_min', 'bullet_size_max', 'glass_hole_size_min', 'glass_hole_size_max', 'rusty_size_min', 'rusty_size_max'):
    _size_default = ProjectDamageProps.__annotations__[_name].keywords
    _size_default['default'] *= 2.5

                                                                      
ProjectDamageProps.__annotations__['fabric_thread_wobble'].keywords['description'] = 'Waviness and over/under weave of surviving threads; also adds small waves to loose fibres. 0: no weave offset or waves. Higher: stronger waviness. Roots remain attached to the rim.'
ProjectDamageProps.__annotations__['fabric_thread_curl'].keywords['description'] = 'Smooth curl along broken threads and rim fringe. 0: no curl (Droop can still bend them). Higher: curled tips; nearby fibres bend together. Length stays bounded by the gap.'
ProjectDamageProps.__annotations__['fabric_thread_skew'].keywords['description'] = 'Sideways bow of intact bridge threads. 0: no sideways bow; sag and weave may remain. 1: full sideways bow, limited to the local yarn spacing. Both ends stay on the rim.'
ProjectDamageProps.__annotations__['fabric_fray_length_min'].keywords['description'] = 'Shortest frayed fibre in cm; keep at or below Fray Length Max. 0: some fibres disappear. Higher: longer minimum fringe. Adjacent fibres vary smoothly; narrow tears limit length to prevent overlap.'
ProjectDamageProps.__annotations__['fabric_fray_length_max'].keywords['description'] = 'Longest frayed fibre in cm; keep at or above Fray Length Min. Low: short fuzz. High: longer fringe, still limited to less than half the local tear width. Nearby lengths vary together.'

from . import fissure_realism
fissure_realism.add_properties(ProjectDamageProps)

from . import ground_depth
ground_depth.add_properties(ProjectDamageProps)

from . import damage_proximity
damage_proximity.add_properties(ProjectDamageProps)
_register_before_proximity = register
_unregister_before_proximity = unregister
# подключаем свойства и действия этого модуля к Blender
def register():
    damage_proximity.register()
    try:
        _register_before_proximity()
    except Exception:
        damage_proximity.unregister()
        raise
# убираем подключения модуля при выключении аддона
def unregister():
    _unregister_before_proximity()
    damage_proximity.unregister()

for _module in ('concrete','lath','brick'):
    ProjectDamageProps.__annotations__[_module+'_model_type']=bpy.props.EnumProperty(
        name='Model Type',items=[('ARCHITECTURAL','Architectural Models','Existing planar wall and slab algorithm'),
                                ('ORGANIC','Organic Models','Follow connected local surface charts on curved, scanned and sculpted models; reject unrelated surfaces and abrupt height jumps')],
        default='ARCHITECTURAL',update=live.touch_prop)

ProjectDamageProps.__annotations__['boolean_wood_experimental']=bpy.props.BoolProperty(name='Legacy compatibility value',default=False,options={'HIDDEN'},description='Deprecated saved-scene value; ignored by Boolean wood placement')

ProjectDamageProps.__annotations__['fissure_depth'].keywords['description']='Global fissure depth in cm before Master Scale. Also proportionally scales slice thickness and side-branch depth. Low: shallow fissure and thin slices; high: deeper fissure and thicker slices. Open surfaces are automatically extended down; closed meshes remain limited by their actual thickness and Thickness Cap'

for _module in ('concrete','lath','brick'):
    _items=ProjectDamageProps.__annotations__[_module+'_mode'].keywords['items']
    ProjectDamageProps.__annotations__[_module+'_mode'].keywords['items']=tuple(
        ('2','Objects','Use a closed mesh object to define the damage outline and maximum cutting depth') if row[0]=='2' else row for row in _items)

ProjectDamageProps.__annotations__['fabric_distribution_scale']=bpy.props.FloatProperty(name='Noise Scale (cm)',default=30.,min=.01,soft_max=200.,update=live.touch_prop,description='Size of the procedural noise patches controlling tear placement. Minimum 0.01 cm: tiny scattered patches. Larger values: broader clusters. Tear dimensions stay unchanged')
ProjectDamageProps.__annotations__['fabric_distribution_strength']=bpy.props.FloatProperty(name='Clustering',default=.8,min=0.,max=1.,subtype='FACTOR',update=live.touch_prop,description='How strongly the noise texture groups automatic tears. 0%: uniform surface sampling. 100%: concentrate tears in high-noise patches. Hole Amount remains the requested count')
ProjectDamageProps.__annotations__['fabric_shape_noise_scale']=bpy.props.FloatProperty(name='Contour Noise Scale (cm)',default=6.,min=.01,soft_max=100.,update=live.touch_prop,description='Wavelength of the smooth noise on each tear outline. Minimum 0.01 cm: fine irregularities. Larger values: broad rounded lobes. Resolution limits the smallest visible feature')
ProjectDamageProps.__annotations__['fabric_shape_noise_strength']=bpy.props.FloatProperty(name='Contour Irregularity',default=.65,min=0.,max=1.,subtype='FACTOR',update=live.touch_prop,description='Strength of the rounded noise outline. 0%: an ellipse. 100%: irregular worn holes with smooth transitions; no cross-shaped punctures. Does not change a drawn contour')
ProjectDamageProps.__annotations__['fabric_tear_style'].keywords['items']=(('0','Lenses','Elongated ragged lens tears with fabric bridges'),('1','Noise Contours','Smooth irregular holes shaped by coherent noise; no pointed puncture stars'))
ProjectDamageProps.__annotations__['fabric_tear_style'].keywords['description']='Procedural tear outline: Lenses follows an elongated rip; Noise Contours uses smooth noise-shaped holes without pointed stars'

ProjectDamageProps.__annotations__['boolean_stable_volume']=bpy.props.BoolProperty(name='Organic Placement — Stable Volume',default=False,update=live.touch_prop,description='Experimental Boolean Bricks, Planks and Laths placement. Off: keep the existing placement algorithm. On: Bricks use a fixed volume grid. Wooden Planks/Laths use one outer surface layer in parallel rows, leaving the centre empty. Board Thickness controls layer depth. Uses the volume/thickness from Mesh Preparation')
ProjectDamageProps.__annotations__['boolean_volume_axis']=bpy.props.EnumProperty(name='Grid Plane',items=(('0','Auto','Use the smallest object-bounds dimension as the grid depth axis'),('1','X','Grid depth along object X'),('2','Y','Grid depth along object Y'),('3','Z','Grid depth along object Z')),default='0',update=live.touch_prop,description='One common grid plane for every part. Auto chooses the smallest object dimension; X, Y and Z fix the depth direction. Board Rotation turns the grid within this plane. Cutters reveal this grid from any side')
ProjectDamageProps.__annotations__['boolean_volume_budget']=bpy.props.IntProperty(name='Candidate Budget',default=4096,min=16,max=50000,update=live.touch_prop,description='Maximum row strips for wooden outer layers, or grid cells for Bricks, evaluated near damage before filtering. 16: small layouts only. 50000: dense layouts and longer calculations. If exceeded, the previous result is kept; no partial grid is applied')
ProjectDamageProps.__annotations__['boolean_volume_clearance']=bpy.props.FloatProperty(name='Part Clearance (cm)',default=.1,min=0.,soft_max=5.,update=live.touch_prop,description='Extra space between rigid cells, in cm before Scale and Master Scale. 0: only the existing board gap/mortar joint. Higher values: more clearance and fewer parts; parts remain at their physical size')
ProjectDamageProps.__annotations__['boolean_volume_min_length']=bpy.props.FloatProperty(name='Minimum Piece Length (cm)',default=2.,min=0.,soft_max=50.,update=live.touch_prop,description='Reject clipped fragments shorter than this along the board/brick direction, before Scale and Master Scale. 0 keeps all lengths. Higher values discard small end remnants without stretching any part')
ProjectDamageProps.__annotations__['boolean_volume_min_section']=bpy.props.FloatProperty(name='Minimum Cross Section',default=.1,min=0.,max=1.,subtype='FACTOR',update=live.touch_prop,description='Minimum retained width (and, for Bricks, thickness) relative to the original part. 0% keeps narrow remnants; 100% requires a full row width. Wooden layer depth is controlled separately by Board Thickness')
for _axis in ('x','y','z'):
    ProjectDamageProps.__annotations__['boolean_volume_origin_'+_axis]=bpy.props.FloatProperty(name='Grid Origin '+_axis.upper()+' (cm)',default=0.,soft_min=-200.,soft_max=200.,update=live.touch_prop,description='Shift the brick grid along object '+_axis.upper()+', in cm before Scale and Master Scale. For wooden Stable Outer Layer, Y is Row Offset across the rows; the other coordinates are unused. 0 anchors the pattern; positive/negative values slide it')

def _wood_deepness_get(self):
    value=self.get('boolean_wood_deepness')
    if value is None:
        old=self.get('rebar_cover')
        value=old if old is not None and abs(float(old)-3.)>1e-6 else 12.
    return max(0.,min(50.,float(value)))

def _wood_deepness_set(self,value):
    self['boolean_wood_deepness']=max(0.,min(50.,float(value)))

ProjectDamageProps.__annotations__['boolean_wood_deepness']=bpy.props.FloatProperty(name='Deepness (cm)',default=12.,min=0.,max=50.,soft_max=50.,get=_wood_deepness_get,set=_wood_deepness_set,update=live.touch_prop,description='Depth of the wooden layer beneath the original surface, before Scale and Master Scale. Default 12 cm. 0 places it at the surface; 50 requests the deepest position. The actual depth stops at the local source thickness, keeping the boards inside instead of removing them. If a board is too thick to fit, its thickness is reduced locally. The entered value stays unchanged. Applies to Boolean Wood Planks and Laths only')
ProjectDamageProps.__annotations__['base_smooth_angle'].keywords['description'] += '. If shading artifacts appear, set Base Smooth Angle to 0.'
ProjectDamageProps.__annotations__['boolean_stable_volume'].keywords['description']='Experimental Boolean Bricks volume placement. Off: original brick placement. On: one fixed object-space grid clipped to the prepared source volume and damage openings. Does not affect wooden Planks or Laths'

ProjectDamageProps.__annotations__['boolean_central_rebar'] = bpy.props.BoolProperty(default=False, update=live.touch_prop)
ProjectDamageProps.__annotations__['boolean_central_axis'] = bpy.props.EnumProperty(
    name='Structure Direction', items=(('0','Follow Mesh','Find internal paths through separate columns, beams and curved structural members in all directions'),
    ('1','X','Along the local X axis'),('2','Y','Along the local Y axis'),('3','Z','Along the local Z axis')),
    default='0', update=live.touch_prop, description='Follow Mesh creates multiple central bars through structural members in all directions and follows their bends. X, Y and Z restrict path tracing to that local axis. Paths are clipped to damage openings plus anchoring length')
ProjectDamageProps.__annotations__['boolean_central_diameter'] = bpy.props.FloatProperty(
    name='Diameter (cm)', default=12., min=.1, max=1000., soft_max=100., update=live.touch_prop,
    description='Maximum diameter of each central bar before Master Scale. Default 12 cm. Minimum 0.1 cm makes thin rods; larger values make thicker cores, up to 1000 cm. Narrow structural members automatically limit the diameter to keep the steel inside')
ProjectDamageProps.__annotations__['boolean_central_embed'] = bpy.props.FloatProperty(
    name='Anchoring Length (cm)', default=20., min=0., max=1000., soft_max=100., update=live.touch_prop,
    description='Length of steel retained inside the surviving mesh on each end of a damage opening. 0 ends at the opening; larger values anchor the bar farther into the object, up to 1000 cm. Default 20 cm; limited by the original object boundary')


def _corner_draw_mode_update(self, context):
    if self.corner_mode == 'PAINT' and self.corner_prob != 100.0:
        was_applying = live._APPLYING
        live._APPLYING = True
        try:
            self.corner_prob = 100.0
        finally:
            live._APPLYING = was_applying
    live.touch_prop(self, context)

ProjectDamageProps.__annotations__['corner_mode'].keywords['update'] = _corner_draw_mode_update
from .removed_controls import install_compatibility as _install_removed_compatibility
_install_removed_compatibility(ProjectDamageProps)

ProjectDamageProps.__annotations__['glass_paint_impact_size'].keywords['description'] = 'Radius of the damaged opening around each Hit Empty, in centimetres, before Master Scale. Small: a compact hit. Large: a wider broken opening; Crack Reach extends the cracks relative to this radius'
ProjectDamageProps.__annotations__['wallpaper_tear_amount'].keywords.update(name='Flaps Amount', min=0, max=200, soft_max=50, description='Target number of automatic torn areas. With Hanging Flaps enabled each tear keeps a flap. 0: no automatic tears. Higher: more non-overlapping tears, including neighbouring strips and different heights. Available wall area and Tear Length limit how many fit')
ProjectDamageProps.__annotations__['wallpaper_flap_amount'].keywords.update(name='Hanging Paper', min=0.0, max=1.0, soft_max=1.0, description='Fraction of each torn patch retained as a hanging flap, not a flap count. 0: remove the torn paper. 1: keep its full length. Change Flaps Amount to create more tears')
ProjectDamageProps.__annotations__['wallpaper_flap_mode'] = bpy.props.EnumProperty(name='Flap Placement', items=(('AUTO','Automatic','Place the requested number of tears'),('EMPTIES','Empties','Only tear strips at the marker empties'),('BOTH','Both','Marker tears first, plus automatic tears')), default='AUTO', update=live.touch_prop)
ProjectDamageProps.__annotations__['wallpaper_flap_collection'] = bpy.props.PointerProperty(name='Flap Empties', type=bpy.types.Collection, update=live.touch_prop, description='Each Empty selects the nearest wallpaper strip. Move it to choose the strip and height; scale it to set the tear length. Empty diameter is the tear length')

for _generator_key in ('gen_metal','gen_wood','gen_tiles','gen_erosion','gen_crater'):
    ProjectDamageProps.__annotations__[_generator_key+'_enable'] = bpy.props.BoolProperty(name='Enable Generator', default=False, update=live.touch_prop)

for _rebar_resolution_name in ('rebar_resolution', 'concrete_rebar_resolution', 'hang_rebar_resolution', 'surface_rebar_resolution'):
    ProjectDamageProps.__annotations__[_rebar_resolution_name] = bpy.props.FloatProperty(
        name='Rebar Resolution', default=1.0, min=0.0, max=1.0, precision=2,
        description='Independent reinforcement detail factor. 0: four sides and the minimum length segments needed to retain bends. 1: original detail. Draft preview uses 0; Apply All restores this chosen value',
        update=live.touch_prop)

ProjectDamageProps.__annotations__['lath_wood_master_scale'] = bpy.props.FloatProperty(name='Wood Master Scale',default=100.,min=1.,max=1000.,subtype='PERCENTAGE',update=live.touch_prop,description='Scale laths and supporting studs together, including spacing, widths, thickness and board lengths. 100% keeps their specified dimensions; 1% makes small fine rows; 1000% makes a coarse timber structure. Damage openings do not change')
ProjectDamageProps.__annotations__['wallpaper_master_scale'] = bpy.props.FloatProperty(name='Master Scale',default=100.,min=1.,max=1000.,subtype='PERCENTAGE',update=live.touch_prop,description='Scale wallpaper strip width, seam width, thickness, tear and lift lengths together. 100% uses their specified dimensions; lower values make a finer pattern, higher values make broader strips and larger tears. Does not scale the object or the number of flaps')

for _module in ('concrete','lath','brick','hang'):
    ProjectDamageProps.__annotations__[_module+'_edge_wall_depth']=bpy.props.FloatProperty(name='Edge Wall Depth',default=0.,min=0.,max=100.,subtype='PERCENTAGE',update=live.touch_prop,description='Depth of the steep wall before the recessed surface, as a percentage of damage radius. 0: previous slope; 100: deep rim, limited by local object thickness')

ProjectDamageProps.__annotations__['baked_sides_material']=bpy.props.PointerProperty(name='Sides Material',type=bpy.types.Material,description='Material for the inner cut faces of all applied fracture pieces from this Apply operation. Exterior faces keep their materials')

from . import generated_geometry
generated_geometry.add_properties(ProjectDamageProps)

from . import material_update
material_update.register_callbacks(ProjectDamageProps.__annotations__)
