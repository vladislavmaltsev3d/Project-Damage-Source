# связываем темы подсказок с готовыми картинками и пояснениями
# SPDX-License-Identifier: GPL-3.0-or-later
# key: title, explanation, (image file stem, caption) alternatives
HINTS={
 'boolean_placement':('Around Damage / Built-in','Around Damage creates local fill at each opening. Built-in lays out a continuous structure in the whole object; a cutaway shows the difference.',(('around_damage','Around Damage: local fill'),('built_in','Built-in: continuous structure'))),
 'boolean_fit':('Fit to the Object','Object Box uses a regular box-shaped layout. Follow Shape adapts the layout to the object silhouette.',(('object_box','Object Box'),('follow_shape','Follow Shape'))),
 'boolean_layout':('Internal Structure Layout','Grid is one layer; Double Grid places a layer near each face. Cages wrap a column rather than a flat wall.',(('grid','Grid'),('double_grid','Double Grid'),('column_cage','Column Cage'),('round_cage','Round Cage'))),
 'boolean_side':('Face / Deepness','Face placement sits under one surface. Centre placement sits in the middle. Deepness sets the offset below the chosen surface.',(('face_offset','Near the face'),('centre_offset','In the centre'))),
 'fracture_type':('Fracture Type','Choose the fracture pattern that matches the material. Wood splits with the grain; glass radiates from impact points.',(('fracture_concrete','Concrete: blocky pieces'),('fracture_wood','Wood: along the grain'),('fracture_glass','Glass: radial shards'),('fracture_soil','Soil: irregular chunks'))),
 'fracture_placement':('Procedural / Draw / Proximity','Procedural distributes damage automatically. Draw follows strokes. Proximity concentrates damage near helper objects.',(('procedural','Procedural'),('paint','Draw: follow the strokes'),('proximity','Proximity: around points'))),
 'fracture_layers':('Fracture Layers','Level 1 creates main cracks. Levels 2 and 3 add smaller branches; one drawing can guide the main network.',(('main_fracture','Main fractures'),('branched_fracture','Main + smaller branches'))),
 'inner_form':('Noisify Shards — shared noisy cutters','Enable Noisy Cutters and press Show / Update to rebuild shared interior faces. A bulge on one shard fits a recess on its neighbor. The Workbench pair is opened after processing to expose the contact surfaces. Grey is the original exterior, orange is the interior.',(('wb_noisify_off','Off: flat shared boundary'),('wb_noisify_on','On: complementary rough surfaces'))),
 'inner_resolution':('Noisify Resolution','Lower Resolution makes larger cells and fewer polygons. Higher Resolution resolves more surface detail and costs more time.',(('wb_noisify_resolution25','25%: 960 polygons'),('wb_noisify_resolution100','100%: 9,152 polygons'))),
 'physics_proxy':('Physics Proxy','The simple collision mesh drives physics. The detailed visible mesh follows it, keeping simulation lighter.',(('wb_noisify_proxy','Simple collision proxies'),('wb_noisify_detail','Detailed visible children'))),
 'fabric_mode':('Fabric Classic / Solidify','Classic is for a thin sheet of cloth. Solidify is for a closed panel with thickness and a visible inner wall.',(('fabric_classic','Classic: thin sheet'),('fabric_solidify','Solidify: thick panel'))),
 'foam_placement':('Expanding Foam Placement','Gaps needs two nearby surfaces forming a cavity. Draw places foam along your strokes even without an automatic gap.',(('foam_gaps','Gaps: between surfaces'),('foam_draw','Draw: along the stroke'))),
 'pipe_type':('Pipe Types','Pipe types change their wall, joints and broken ends. Conduit is corrugated and can contain wires.',(('pipe_copper','Copper'),('pipe_pvc','PVC'),('pipe_conduit','Conduit'))),
 'cable_type':('Cable Types','The cable preset changes the cross-section, sheath and inner conductors. Flat cable keeps a flat section; Data has thin coloured cores.',(('cable_round','Round'),('cable_flat','Flat'),('cable_armored','Armored'),('cable_data','Data'))),
 'cable_slack':('Slack and Free Ends','A held cable sags between its supports. A cut end hangs from its remaining support; the sheath and copper keep their size.',(('cable_taut','Low slack'),('cable_slack','More slack'),('cable_free','One free end'))),
 'bevel_profile':('Bevel Profile','A bevel replaces a sharp edge with a strip of faces. A rounded profile uses several segments for a smooth silhouette.',(('sharp_edge','Sharp edge'),('rounded_edge','Rounded profile'))),
 'metal_taper':('Metal Tear Taper','Draw from the narrow start toward the wide end. Taper 0 keeps an even opening; high Taper opens it progressively.',(('tear_uniform','Taper 0: uniform slit'),('tear_taper','Taper 100: narrow to wide'))),
 'draw_contours':('Open Strokes / Closed Contours','An open stroke defines a path. A closed contour defines the damaged area inside its boundary.',(('open_stroke','Open stroke: a path'),('closed_contour','Closed contour: an area'))),
}
                                                                    
PROPERTY_HINTS={
 'rebar_mode':'boolean_placement','rebar_fit':'boolean_fit','rebar_layout':'boolean_layout',
 'rebar_side':'boolean_side','rebar_cover':'boolean_side','boolean_wood_deepness':'boolean_side','rebar_brick_3d':'boolean_fit',
 'cellf_material':'fracture_type','rbf_material':'fracture_type','fract_mode':'fracture_placement','cellf_placement':'fracture_placement',
 'fract_prox_mode':'fracture_placement','fract_prox_collection':'fracture_placement','fract_l2_amount':'fracture_layers',
 'surface_paint_close':'draw_contours','concrete_paint_close':'draw_contours','foam_mode':'foam_placement',
 'pipe_type':'pipe_type','cable_type':'cable_type','cable_slack':'cable_slack','cable_hang':'cable_slack',
 'metaltear_taper':'metal_taper','bevel_profile':'bevel_profile','dbevel_profile':'bevel_profile',
 'fabric_solid_mode':'fabric_mode','fabric_solid_enable':'fabric_mode',
}

HINTS.update({
 'plank_pins':('Plank Destruction Points','Proximity damages boards around a point immediately. Pull Pins uses the movement from the original point position to bend and tear the boards.',(('plank_proximity','Proximity: damage here'),('plank_pull','Pull Pins: drag to tear'))),
 'cloth_sources':('Cloth Attachment','Hanging pins the top edge to a curve. Along Curve distributes sheets along a guide. Around Empties creates sheets near point helpers.',(('cloth_hanging','Hanging: pinned top edge'),('cloth_curve','Along Curve: follow a guide'),('cloth_empties','Around Empties: local sheets'))),
 'ivy_attachment':('Ivy Attachment and Hanging Stems','Surface adhesion keeps stems close to the wall. A guide curve can carry stems through the air and spawn hanging tendrils.',(('ivy_wall','Climbing along a surface'),('ivy_hanging','Curve with hanging tendrils'))),
})
PROPERTY_HINTS['dbevel_smoothness']='bevel_profile'

HINTS.update({
 'wall_normal':('Auto Wall Normal','Auto detects the wall normal separately for each damaged or painted area. The normal points through the wall, perpendicular to its face. Disable Auto to choose which local X / Y / Z faces receive runs. This does not select horizontal or vertical runs.',(('wall_normal_auto','Auto: follow each wall face'),('wall_normal_manual','Manual: selected face axis'))),
 'run_direction':('Horizontal / Vertical / Both','Direction is measured in the chosen wall plane. Horizontal creates cross-wall runs; Vertical creates risers; Both combines them. Wall Normal chooses the face plane, while this setting chooses the direction within it.',(('runs_horizontal','Horizontal: cross-wall runs'),('runs_vertical','Vertical: risers'),('runs_both','Both: runs + risers'))),
})
PROPERTY_HINTS.update({'pipe_axis_auto':'wall_normal','cable_axis_auto':'wall_normal','pipe_layout':'run_direction','cable_layout':'run_direction'})

HINTS['fracture_surface'] = (
 'Classic / Rounded',
 'Classic starts from eligible sharp edges and stops at folds; use it for angular walls and blocks. Rounded starts across the surface and follows smoothed normals; use it for models with beveled edges, angular walls with bevels, columns and relief. Rounded can continue across bevels that stop Classic. These change crack paths, not mesh shading. Same sphere, Seed and Amount are shown: Classic has no eligible edge starts here. Paint follows its drawn path separately.',
 (('fracture_classic', 'Classic: edge starts'), ('fracture_rounded', 'Rounded: surface starts')),
)
PROPERTY_HINTS['fract_surface'] = 'fracture_surface'


                                                                   
HINTS.update({
 'physics_fill_depth': ('Fill Depth: Auto / Whole Cut / Surface Layer',
  'Fill restores removed material as new objects. Auto fills the plaster coat for Lath and Stud Framing, and the whole cut for other supported damage. Whole Cut restores all removed depth. Surface Layer limits the fill to Surface Depth measured under the original surface. Cutaway illustration: orange is the fill; grey is the wall.',
  (('wb_physics_depth_auto','Auto: front coat on laths'),('wb_physics_depth_whole','Whole Cut: full depth'),('wb_physics_depth_surface','Surface Layer: chosen depth'))),
 'physics_fill_method': ('Fill Damage / Fill V2',
  'Fill Damage subtracts the damaged result and internal structures from the original wall: sockets remain around reinforcement. Fill V2 computes local damage regions and ignores rebar, pipes and cables, so the fill has no sockets for them. V2 is experimental and may skip regions it cannot close. Both create fill objects, not fracture pieces. Separated wall bodies must remain at their original positions.',
  (('wb_physics_fill_classic','Fill Damage: sockets'),('wb_physics_fill_v2','Fill V2: continuous'))),
 'physics_separate': ('Separate Fragments / Fracture',
  'Separate Fragments makes objects from already disconnected parts of the damaged mesh; it does not cut new cracks. Select a separated Boolean fragment and press Fracture to cut it further. Alternatively create Fill Damage first, then fracture the fills. The original Project Damage generator stays editable. Moving separated bodies disables Fill until they return to their original transforms.',
  (('physics_connected','Before: islands in one object'),('physics_separate','Separate: independent objects'))),
 'physics_fracture': ('Create Fracture Pieces',
  'First create fills OR select separated Boolean fragments. Choose Fracture Type, Piece Size and Amount Budget, optionally draw cuts, then press Fracture below these controls. Type settings alone do not run the operation. Smaller Piece Size requests more pieces; Amount Budget caps the total. Hide Fill hides the input after a successful fracture. Rigid-body simulation is a separate final step.',
  (('wb_physics_step_3_fill','Input: one restored volume'),('wb_physics_step_4_fracture','Press Fracture Fills to split it'))),
})

HINTS['physics_workflow'] = (
 'Fill → Fracture → Simulate',
 'Create damage in Geometry. In Physics press Fill Damage or Fill V2 to restore the missing volume. Choose fracture settings or draw with Knife Through, Knife Surface or Guidance, then press Fracture Fills. Add colliders and press Enable / Update Physics before playing. Open Workflow with Renders for the complete sequence.',
 (('wb_physics_step_2_damage','1. Generate damage'),('wb_physics_step_3_fill','2. Fill the missing volume'),('wb_physics_step_4_fracture','3. Press Fracture Fills'),('wb_physics_step_5_simulate','4. Enable physics and play')),
)

HINTS['physics_drawing'] = (
 'Knife Through / Knife Surface / Guidance',
 'With the Physics tab open, choose a viewport knife and draw on the target. Knife Through cuts all affected depth layers. Knife Surface cuts only directly touched pieces. Guidance is a separate layer adding denser automatic seeds near the stroke. Knife strokes must make a valid crossing or join an existing cut. Return to Physics and press Fracture Fills or Fracture Selected Fragments to apply the drawing.',
 (('wb_physics_knife_through','Knife Through'),('wb_physics_knife_surface','Knife Surface'),('wb_physics_knife_guidance','Guidance')),
)

HINTS['wallpaper_finder'] = ('Wallpaper Finder: choose the covered side', 'Add the Finder at the 3D cursor, then move it close to the side of the wall you want to cover. The nearest eligible connected wall side receives wallpaper. Move it across the wall to switch sides. Clear Finder to use all eligible walls. Finder selects the covered side; Flap Empties choose individual torn strips. Keep the Finder outside the wall, close to its face.', (('wallpaper_finder_front','Finder at the front'),('wallpaper_finder_back','Finder at the back')))

HINTS['break_surface_styles'] = ('Break Surface Presets',
    'The same cut, camera, scale and seed in every render. Presets change only the break surface shape. Strength and Noise Scale can be adjusted afterwards. Wood styles follow the cutter Grain Axis.',
    (('break_style_default','Default'), ('break_style_concrete','Rough Concrete'),
     ('break_style_stone','Smooth Stone'), ('break_style_rock','Jagged Rock'),
     ('break_style_plaster','Soft Plaster'), ('break_style_shattered','Shattered'),
     ('break_style_eroded','Eroded'), ('break_style_wood_splintered','Wood - Splintered Fibers'),
     ('break_style_wood_plywood','Wood - Plywood Layers')))

from .cell_cut_plane import HINT as _cut_plane_hint
HINTS['cell_cut_plane']=('Cut Plane — the most stable cutting mode',_cut_plane_hint,())
