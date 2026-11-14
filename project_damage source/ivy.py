# рост плюща по поверхности, направляющим и свисающим кривым
# ProjectDamage ivy generator - GPL-3.0-or-later; see LICENSE.
#
# Grow Ivy: the classic ivy growth model (Thomas Luft, "Ivy Generator", 2007),
# implemented in the native core (ivy_generator.cpp). This module is the
# Blender side: settings on the object, one operator, one panel.
#   Seeds    empties of the Seeds collection; with none, the start of every
#            Grease Pencil guide stroke; with neither (and no Curve), the 3D cursor.
#   Guide    a Grease Pencil object: its strokes draw the main growth direction.
#   Curve    a Curve object: a stem grows exactly along each spline, also in
#            mid-air, and drops hanging tendrils (hanging ivy).
#   Result   "<object>_Ivy", parented to the object: material slot 0 = branches,
#            slot 1 = leaves (your materials, or simple defaults); UV map
#            "UVMap" (bark: U around, V along in the same metric scale, also on
#            the leaf petioles; each leaf the full 0..1 square, turned by Leaf
#            UV Direction); vertex groups Ivy_Branches (with petioles), Ivy_Leaves.
from . import output_collections
import bpy
import time
from pathlib import Path
import numpy as np
from mathutils import Vector

from . import core_loader, ownership, live, material_controls, background_compute


                                                                      
_pending = {}


def _default_material(kind):
    names = ('Ivy Branch Shader', 'VBranch Shader') if kind=='branch' else ('Ivy Leaves Shader',)
    for name in names:
        material=bpy.data.materials.get(name)
        if material is not None:return material
    path=Path(__file__).parent/'blends'/'ivy_mat.blend'
    if path.is_file():
        with bpy.data.libraries.load(str(path),link=False) as (available,loaded):
            name=next((n for n in names if n in available.materials),None)
            if name is not None:loaded.materials=[name]
        if name is not None and loaded.materials[0] is not None:
            material=loaded.materials[0]
            material['project_damage_ivy_library']=str(path)
            return material
    # Preserve a usable generator if a redistributed package omits the library.
    name='PD_Ivy_Branch' if kind=='branch' else 'PD_Ivy_Leaf'
    material=bpy.data.materials.get(name)
    if material is None:
        material=bpy.data.materials.new(name)
        material.diffuse_color=(.18,.12,.07,1.) if kind=='branch' else (.12,.32,.08,1.)
    return material


def ensure_materials(settings):
    previous=live._APPLYING;live._APPLYING=True
    try:
        if settings.branch_material is None:settings.branch_material=_default_material('branch')
        if settings.leaf_material is None:settings.leaf_material=_default_material('leaf')
        return settings.branch_material,settings.leaf_material
    finally:live._APPLYING=previous


def _material_changed(self,context):
                                                                             
    if live._APPLYING or live._HISTORY_RESTORING:return
    result=self.result
    if result is None or result.parent!=self.id_data or not result.get('project_damage_ivy_generated'):return
    branch,leaf=ensure_materials(self)
    if result.data.users>1:result.data=result.data.copy()
    while len(result.data.materials)<2:result.data.materials.append(branch)
    result.data.materials[0]=branch;result.data.materials[1]=leaf


def shader_groups(material):
    if material is None or material.node_tree is None:return []
    normalize=lambda text: ''.join(text.lower().replace('|',' ').split())
    label=normalize('Ivy Shader | Project Damage')
    return [node for node in material.node_tree.nodes if node.type=='GROUP' and node.node_tree is not None
            and any(normalize(name)==label for name in (node.label,node.name,node.node_tree.name))]


def draw_shader_settings(layout,material):
    for node in shader_groups(material):
        box=layout.box();box.use_property_split=False
        material_controls.centered_heading(box,'Ivy Shader | Project Damage',icon='MATERIAL')
        column=box.column(align=True)
        overlay=next((s for s in node.inputs if s.type=='BOOLEAN' and 'enable color overlay' in s.name.lower()),None)
        overlay_index=next((i for i,s in enumerate(node.inputs) if s==overlay),-1)
        for index,socket in enumerate(node.inputs):
            if (overlay is not None and not bool(overlay.default_value) and index>overlay_index
                    and socket.type in {'MENU','RGBA'}):continue
            if socket.is_linked or socket.hide_value or not hasattr(socket,'default_value') or socket.type=='SHADER':continue
            row=column.row(align=True)
            if socket.type=='BOOLEAN':
                row.scale_y=1.5;row.prop(socket,'default_value',text=socket.name,toggle=True)
            elif socket.type=='MENU':
                row.scale_y=1.5;row.prop(socket,'default_value',text=socket.name,expand=True)
            else:row.prop(socket,'default_value',text=socket.name)
        box.label(text='Shared material: changes affect all its users',icon='INFO')


def clear_pending():
    _pending.clear()
    if bpy.app.timers.is_registered(_live_tick):
        bpy.app.timers.unregister(_live_tick)


# ставим изменение в очередь, чтобы не пересчитывать прямо во время ввода
def queue_update(source):
    if live._HISTORY_RESTORING or source is None:
        return
    if live.drawing_exit_suppressed(source):return
    p = source.project_damage
    if not p.live_update or not p.ivy_enable or p.all_off:
        return
    from . import incremental
    from .build import result_of
    settings=source.project_damage_ivy
    if incremental.reuse_output(bpy.context,source,result_of(source) or source,'ivy',settings.result,settings):
        _pending.pop(source.name,None)
        return
    _pending[source.name] = time.monotonic()
    if not bpy.app.timers.is_registered(_live_tick):
        bpy.app.timers.register(_live_tick, first_interval=0.1)


def _settings_changed(self, context):
    if not live._APPLYING:
        from . import background_compute
        background_compute.cancel(self.id_data,method='grow_ivy')
        queue_update(self.id_data)


def dependencies_changed(scene, depsgraph):
    from . import live_input_state
    from .build import result_of
    moving={u.id.original for u in depsgraph.updates if u.is_updated_transform and isinstance(u.id.original,bpy.types.Object) and u.id.original.mode=='OBJECT'}
    changed = {u.id.original for u in depsgraph.updates
               if u.is_updated_geometry or u.is_updated_transform}
    if not changed:
        return
    for source in scene.objects:
        if source.type != 'MESH' or not source.project_damage.ivy_enable:
            continue
        s = source.project_damage_ivy
        objects = [source, result_of(source), s.guide, s.curve]
        if s.seeds is not None:
            objects.extend(o for o in s.seeds.all_objects if o.type == 'EMPTY')
        if any(not (obj in {source,result_of(source)} and obj in moving)
               and (obj in changed or (obj.data is not None and obj.data in changed))
               and live_input_state.changed(obj, 'ivy', local=obj in {source,result_of(source)})
               for obj in objects if obj is not None):
            queue_update(source)


def _live_tick():
    if live._HISTORY_RESTORING:
        _pending.clear()
        return None
    if live._APPLYING:
        return 0.1
    from .build import result_of
    for name, stamp in list(_pending.items()):
        source = bpy.data.objects.get(name)
        if source is None or not source.project_damage.live_update or not source.project_damage.ivy_enable or source.project_damage.all_off:
            _pending.pop(name, None)
            continue
        surface = result_of(source) or source
        if source.mode != 'OBJECT' or surface.mode != 'OBJECT':
            continue
        if time.monotonic() - stamp < source.project_damage.defer_ms / 1000.0:
            continue
                                                                              
        if source.get('project_damage_uid') in live._dirty:
            continue
        _pending.pop(name, None)
        class Worker:
            _staged_obj = None
            _staged_mesh = None
            def report(self, levels, message):
                if 'ERROR' in levels:
                    print(message)
        worker = Worker()
        live._APPLYING = True
        try:
            PROJECT_DAMAGE_OT_grow_ivy._grow(worker, bpy.context, source)
        except background_compute.Deferred:
            pass
        except Exception as exc:
            if worker._staged_obj is not None:
                bpy.data.objects.remove(worker._staged_obj, do_unlink=True)
            if worker._staged_mesh is not None and worker._staged_mesh.users == 0:
                bpy.data.meshes.remove(worker._staged_mesh)
            print('ProjectDamage Ivy live update:', exc)
        finally:
            live._APPLYING = False
    return 0.1 if _pending else None


def target_for(context):
    pinned = ownership.pinned_source(context)
    if pinned is not None:return pinned
    obj = context.active_object
    if obj is not None and obj.get('project_damage_ivy_generated'):
        parent = ownership.source_of(obj.parent)
        return parent if parent is not None and parent.type == 'MESH' else None
    source = ownership.context_source(context)
    return source if source is not None and source.type == 'MESH' else None


def set_visibility(source, enabled):
    result = source.project_damage_ivy.result
    if result is not None and result.parent == source and result.get('project_damage_ivy_generated'):
        result.hide_set(not enabled)
        result.hide_render = not enabled


def migrate_settings(source):
    """Keep settings authored in the previous standalone panel on the result."""
    from .build import result_of
    surface = result_of(source)
    current = source.project_damage_ivy
    if surface is None or current.result is not None or len(current.keys()):return
    previous = surface.project_damage_ivy
    if previous.result is None or previous.result.parent != surface:return
    for prop in previous.bl_rna.properties:
        if prop.identifier != 'rna_type':
            setattr(current, prop.identifier, getattr(previous, prop.identifier))
    world = current.result.matrix_world.copy()
    current.result.parent = source;current.result.matrix_world = world
    current.result.project_damage_og_object = source


# показываем настройки выбранного эффекта
def draw_settings(layout, context):
    from .panels import draw_ivy_settings
    draw_ivy_settings(layout, context)


def apply_ivy(source):
    result = source.project_damage_ivy.result
    if result is None or result.parent != source:
        raise ValueError('Grow Ivy before applying it')
    world = result.matrix_world.copy()
    result.parent = None;result.matrix_world = world
    result.hide_set(False);result.hide_render = False
    if 'project_damage_ivy_generated' in result:
        del result['project_damage_ivy_generated']
    result.project_damage_og_object = None
    source.project_damage_ivy.result = None
    source.project_damage.ivy_enable = False
    rows = source.project_damage_effects
    for i in reversed(range(len(rows))):
        if rows[i].key == 'ivy':rows.remove(i)
    source.project_damage_effect_index = min(source.project_damage_effect_index, max(0,len(rows)-1))
    return result


                                                                            
                                                                           
                                                                        
                                
_RAW = ('size', 'primary_weight', 'random_weight', 'gravity_weight', 'adhesion_weight', 'guide_weight',
        'guide_radius', 'branching_probability', 'max_float_length', 'max_adhesion_distance', 'max_nodes',
        'branch_size', 'leaf_size', 'leaf_density', 'leaf_density_multiplier')


def growth_settings(s):
    """The detailed settings a grow uses: as authored, or from the simple controls."""
    if s.advanced_controls:
        return {name: getattr(s, name) for name in _RAW}
    scale = s.master_scale
    return {
        'size': 3.0 * scale,                                                              
        'branch_size': 0.15 * scale * s.branch_thickness,
        'leaf_size': 1.0 * scale * s.simple_leaf_size,
        'max_nodes': int(300 + 15000 * s.amount ** 2),                     
        'branching_probability': 0.2 * s.branching_amount ** 2,                
        'random_weight': s.randomness,
        'primary_weight': 1.0 - s.randomness,
        'adhesion_weight': 0.4 * s.climb,
        'max_adhesion_distance': 0.4 * s.climb * scale,
        'gravity_weight': 2.0 * s.droop,
        'max_float_length': 0.2 * (1.0 - s.droop) * scale,
        'guide_weight': 2.0 * s.guide_strength if s.follow_guides else 0.0,
        'guide_radius': 0.05 + 0.2 * s.guide_strength,
        'leaf_density': min(1.0, 1.4 * s.density),                        
        'leaf_density_multiplier': 1 + int(round(3.0 * s.density ** 1.5)),
    }


def _poll_gp(self, obj):
    return obj.type in {'GREASEPENCIL', 'GPENCIL'}


def _poll_curve(self, obj):
    return obj.type == 'CURVE'


# здесь хранятся значения, которые сохраняются вместе с объектом
class ProjectDamageIvySettings(bpy.types.PropertyGroup):
    resolution: bpy.props.FloatProperty(name='Resolution',default=100.,min=10.,soft_max=300.,subtype='PERCENTAGE',update=_settings_changed,description='Tube cross-section quality; lower values reduce mesh density while retaining growth paths')
    result: bpy.props.PointerProperty(type=bpy.types.Object, options={'HIDDEN'})
    adhesion_smoothing: bpy.props.IntProperty(update=_settings_changed, name='Leaf Alignment Smoothing', default=5, min=0, max=5,
        description='Symmetric 11-point Gaussian smoothing of adhesion vectors before leaf orientation, inspired by CrazyIvy. 0 uses the previous one-sided filter')
    leaf_maturity: bpy.props.FloatProperty(update=_settings_changed, name='Leaf Maturity', default=1., min=0., max=1.,
        description='Distribute leaves by shoot maturity and ground contact; 0 keeps uniform density along the shoot')
    seeds: bpy.props.PointerProperty(update=_settings_changed, name='Seeds', type=bpy.types.Collection, description='Every empty in this collection starts an ivy root (dropped onto the surface)')
    guide: bpy.props.PointerProperty(update=_settings_changed, name='Guide', type=bpy.types.Object, poll=_poll_gp, description='Grease Pencil guides growth. At maximum Guide Strength, shoots start along the entire drawing and grow in both directions')
    curve: bpy.props.PointerProperty(update=_settings_changed, name='Curve', type=bpy.types.Object, poll=_poll_curve, description='A stem grows exactly along every spline of this Curve, also in mid-air, and drops hanging tendrils: hanging ivy')
    hang_density: bpy.props.FloatProperty(update=_settings_changed, name='Hanging Density', default=0.15, min=0.0, max=1.0, description='Chance of a hanging tendril at each growth step along the Curve')
    hang_length: bpy.props.FloatProperty(update=_settings_changed, name='Hanging Length', default=0.3, min=0.0, max=4.0, description='Longest hanging tendril, as a share of the object size (each varies down to 40% of it)')
    seed: bpy.props.IntProperty(update=_settings_changed, name='Random Seed', default=0, min=0)
    size: bpy.props.FloatProperty(update=_settings_changed, name='Ivy Size', default=3.0, min=1.0, max=10.0, description='Growth step: a larger value covers a larger area')
    primary_weight: bpy.props.FloatProperty(update=_settings_changed, name='Primary Weight', default=0.5, min=0.0, max=1.0, description='Tendency to keep growing the way it grew (toward the sky)')
    random_weight: bpy.props.FloatProperty(update=_settings_changed, name='Random Weight', default=0.5, min=0.0, max=1.0, description='Random wandering')
    gravity_weight: bpy.props.FloatProperty(update=_settings_changed, name='Gravity Weight', default=1.0, min=0.0, max=2.0, description='Pull toward the ground while floating')
    adhesion_weight: bpy.props.FloatProperty(update=_settings_changed, name='Adhesion Weight', default=0.2, min=0.0, max=1.0, description='How strongly it hugs the surface')
    guide_weight: bpy.props.FloatProperty(update=_settings_changed, name='Guide Weight', default=1.0, min=0.0, max=2.0, description='How strongly roots follow the guide strokes (side branches half as much per level)')
    guide_radius: bpy.props.FloatProperty(update=_settings_changed, name='Guide Reach', default=0.15, min=0.01, max=1.0, description='Reach of a guide stroke, as a share of the object size')
    branching_probability: bpy.props.FloatProperty(update=_settings_changed, name='Branching', default=0.05, min=0.0, max=1.0, description='Probability of a new branch per root and step')
    max_float_length: bpy.props.FloatProperty(update=_settings_changed, name='Max Float Length', default=0.1, min=0.0, max=1.0, description='How far a branch may float off the surface before it dies (share of the object size)')
    max_adhesion_distance: bpy.props.FloatProperty(update=_settings_changed, name='Max Adhesion Distance', default=0.2, min=0.0, max=4.0, description='Reach of the surface adhesion (share of the object size)')
    max_nodes: bpy.props.IntProperty(update=_settings_changed, name='Max Points', default=4000, min=100, max=50000)
    branch_size: bpy.props.FloatProperty(update=_settings_changed, name='Branch Size', default=0.15, min=0.0, max=2.0, description='Branch diameter')
    branch_sides: bpy.props.IntProperty(update=_settings_changed, name='Branch Sides', default=6, min=3, max=16)
    uv_scale: bpy.props.FloatProperty(update=_settings_changed, name='Bark UV Scale', default=1.0, min=0.01, max=100.0, description='Tiling of the bark texture along and around the branches')
    leaf_size: bpy.props.FloatProperty(update=_settings_changed, name='Leaf Size', default=1.0, min=0.0, max=5.0)
    leaf_density: bpy.props.FloatProperty(update=_settings_changed, name='Leaf Density', default=0.7, min=0.0, max=1.0)
    leaf_density_multiplier: bpy.props.IntProperty(update=_settings_changed, name='Density Multiplier', default=2, min=1, max=10)
    leaf_offset: bpy.props.FloatProperty(update=_settings_changed, name='Leaf Offset', default=0.0, min=0.0, max=5.0, description='Random extra petiole length: leaves stand off the branch for a bushier look')
    stem_length: bpy.props.FloatProperty(update=_settings_changed, name='Stem Length', default=0.5, min=0.0, max=2.0, description='Length of the small stem (petiole) joining each leaf to the branch, relative to the leaf length; 0 = leaves sit on the branch')
    stem_thickness: bpy.props.FloatProperty(update=_settings_changed, name='Stem Thickness', default=1.0, min=0.0, max=3.0, description='Thickness of the leaf stems; 0 = no stem geometry')
    stem_attach: bpy.props.FloatProperty(update=_settings_changed, name='Stem Attach', default=0.25, min=0.0, max=1.0, subtype='FACTOR', description='Where the stem meets the leaf in the texture, as a share of the leaf length from its base: match the petiole point of your leaf image')
    leaf_uv_direction: bpy.props.EnumProperty(update=_settings_changed, name='Leaf Tip in Texture', default='RIGHT',
        items=(('UP','Up','The leaf tip points to the top of the leaf image'),
               ('RIGHT','Right','The leaf tip points to the right of the leaf image'),
               ('DOWN','Down','The leaf tip points to the bottom of the leaf image'),
               ('LEFT','Left','The leaf tip points to the left of the leaf image')),
        description='Which way the leaf tip points in your leaf texture, so each leaf points along the growth direction without rotating UVs by hand')
    leaf_tilt: bpy.props.FloatProperty(update=_settings_changed, name='Leaf Tilt', default=30.0, min=0.0, max=90.0, description='Random tilt of each leaf on two axes, in degrees: the tip lifts off the surface and the leaf rolls about its length. The stem follows the leaf')
                     
    advanced_controls: bpy.props.BoolProperty(update=_settings_changed, name='Advanced Controls', default=False, description='Show and use the detailed growth settings instead of the simple controls')
    master_scale: bpy.props.FloatProperty(update=_settings_changed, name='Master Scale', default=1.0, min=0.05, max=20.0, soft_min=0.2, soft_max=5.0, description='Scales the whole ivy: growth step, branch thickness and leaf size together')
    amount: bpy.props.FloatProperty(update=_settings_changed, name='Amount', default=0.5, min=0.0, max=1.0, subtype='FACTOR', description='How much ivy grows: how far it spreads and how many branches it makes')
    branching_amount: bpy.props.FloatProperty(update=_settings_changed, name='Branching', default=0.5, min=0.0, max=1.0, subtype='FACTOR', description='How often branches split into side shoots')
    randomness: bpy.props.FloatProperty(update=_settings_changed, name='Randomness', default=0.5, min=0.0, max=1.0, subtype='FACTOR', description='0 grows in long straight runs, 1 wanders in every direction')
    climb: bpy.props.FloatProperty(update=_settings_changed, name='Cling', default=0.5, min=0.0, max=1.0, subtype='FACTOR', description='How tightly the ivy hugs the surface and how far it reaches for it')
    droop: bpy.props.FloatProperty(update=_settings_changed, name='Droop', default=0.5, min=0.0, max=1.0, subtype='FACTOR', description='How much loose shoots hang down and how soon they give up when they lose the surface')
    branch_thickness: bpy.props.FloatProperty(update=_settings_changed, name='Branch Thickness', default=1.0, min=0.0, max=5.0, description='Branch thickness (times Master Scale)')
    simple_leaf_size: bpy.props.FloatProperty(update=_settings_changed, name='Leaf Size', default=1.0, min=0.0, max=5.0, description='Leaf size (times Master Scale)')
    density: bpy.props.FloatProperty(update=_settings_changed, name='Leaf Density', default=0.5, min=0.0, max=1.0, subtype='FACTOR', description='How many leaves grow along the branches')
    follow_guides: bpy.props.BoolProperty(update=_settings_changed, name='Follow Guides', default=True, description='Grow along the guide drawing')
    guide_strength: bpy.props.FloatProperty(update=_settings_changed, name='Guide Strength', default=0.5, min=0.0, max=1.0, subtype='FACTOR', description='At 0%, guides do not attract growth. Higher values strengthen attraction. At 100%, shoots start along the entire drawing and follow it in both directions; side branches follow more loosely.')
    branch_material: bpy.props.PointerProperty(update=_material_changed, name='Branch Material', type=bpy.types.Material)
    leaf_material: bpy.props.PointerProperty(update=_material_changed, name='Leaf Material', type=bpy.types.Material)


# действие «Grow Ivy»
class PROJECT_DAMAGE_OT_grow_ivy(bpy.types.Operator):
    bl_idname = 'project_damage.grow_ivy'
    bl_label = 'Grow Ivy'
    bl_description = 'Grow ivy on the active mesh from the seed empties / guide strokes; replaces its previous ivy'
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return context.mode == 'OBJECT' and target_for(context) is not None

    def execute(self, context):
        previous = live._APPLYING
        live._APPLYING = True
        self._staged_obj = self._staged_mesh = None
        try:
            return self._grow(context)
        except background_compute.Deferred:
            self.report({'INFO'}, 'Ivy calculation queued in background')
            return {'FINISHED'}
        except Exception as exc:
            if self._staged_obj is not None and self._staged_obj.name in bpy.data.objects:
                bpy.data.objects.remove(self._staged_obj, do_unlink=True)
            if self._staged_mesh is not None and self._staged_mesh.users == 0:
                bpy.data.meshes.remove(self._staged_mesh)
            self.report({'ERROR'}, 'Grow Ivy: '+str(exc))
            return {'CANCELLED'}
        finally:
            live._APPLYING = previous

    def _grow(self,context,source=None):
        from . import background_compute as bg
        target = source if source is not None else target_for(context)
        if bg.async_for(target):
            class Worker:
                _staged_mesh=None
                _staged_obj=None
                def report(self,flags,message):
                    if 'ERROR' in flags:raise RuntimeError(message)
                    if 'WARNING' in flags:print('Project Damage:',message)
            bg.queue(target,'grow_ivy',lambda: PROJECT_DAMAGE_OT_grow_ivy._grow_steps(Worker(),bpy.context,target))
            return {'FINISHED'}
        return bg.immediate(PROJECT_DAMAGE_OT_grow_ivy._grow_steps(self,context,target))

    def _grow_steps(self, context, source=None):
        target = source if source is not None else target_for(context)
        migrate_settings(target)
        if target.library is not None:
            raise ValueError('Make the target local before growing ivy')
        s = target.project_damage_ivy
                                                                           
        if not s.is_property_set('advanced_controls'):
            s.advanced_controls = s.result is not None
        core = core_loader.load()
        if core is None or not hasattr(core, 'grow_ivy') or not hasattr(core.IvyParams(), 'leaf_tilt'):
            self.report({'ERROR'}, 'Grow Ivy: restart Blender to load the updated Ivy native core')
            return {'CANCELLED'}
                                                              
        deps = context.evaluated_depsgraph_get()
        from .build import result_of
        surface = result_of(target) or target
        ev = surface.evaluated_get(deps)
        me = ev.to_mesh()
        try:
            me.calc_loop_triangles()
            n = len(me.vertices)
            co = np.empty(n * 3, dtype=np.float64)
            me.vertices.foreach_get('co', co)
            tris = np.empty(len(me.loop_triangles) * 3, dtype=np.int32)
            me.loop_triangles.foreach_get('vertices', tris)
        finally:
            ev.to_mesh_clear()
        points = np.ascontiguousarray(co.reshape(-1, 3))
                                                                                 
        transform = target.matrix_world.inverted_safe() @ surface.matrix_world
        points = np.ascontiguousarray(points @ np.array(transform.to_3x3()).T + np.array(transform.translation))
        tris = tris.reshape(-1, 3)
        if len(points) == 0 or len(tris) == 0:
            raise ValueError('The target has no surface faces')
        polys = np.ascontiguousarray(np.column_stack([tris, tris[:, 2]]).astype(np.int32))
        to_local = target.matrix_world.inverted_safe()
                                                             
        guides = []
        if s.guide is not None:
            gp = s.guide
            gm = to_local @ gp.matrix_world
            if gp.type == 'GREASEPENCIL':                                     
                for layer in gp.data.layers:
                    frame = layer.current_frame()
                    if frame is None or frame.drawing is None:
                        continue
                    for stroke in frame.drawing.strokes:
                        pts = [gm @ Vector(p.position) for p in stroke.points]
                        if len(pts) >= 2:
                            guides.append(np.ascontiguousarray([[q.x, q.y, q.z] for q in pts], dtype=np.float64))
            else:                                                                     
                for layer in gp.data.layers:
                    frame = layer.active_frame
                    if frame is None:
                        continue
                    for stroke in frame.strokes:
                        pts = [gm @ Vector(p.co) for p in stroke.points]
                        if len(pts) >= 2:
                            guides.append(np.ascontiguousarray([[q.x, q.y, q.z] for q in pts], dtype=np.float64))
                                                                    
        curves = []
        if s.curve is not None:
            from . import paint
            curves = [np.ascontiguousarray(c, dtype=np.float64) for c in paint.read_paths(context, target, s.curve) if len(c) >= 2]
                                                                              
        seeds = []
        if s.seeds is not None:
            for o in sorted(s.seeds.all_objects, key=lambda o: o.name):
                if o.type == 'EMPTY':
                    q = to_local @ o.matrix_world.translation
                    seeds.append((q.x, q.y, q.z))
        if not seeds and not guides and not curves:
            q = to_local @ context.scene.cursor.location
            seeds.append((q.x, q.y, q.z))
        seeds = np.ascontiguousarray(np.array(seeds, dtype=np.float64).reshape(-1, 3))
        p = core.IvyParams()
        p.seed = s.seed
        for name, value in growth_settings(s).items():
            setattr(p, name, value)
        p.branch_sides = max(3,min(48,round(s.branch_sides*s.resolution/100.)))
        p.uv_scale = s.uv_scale
        p.leaf_tilt = s.leaf_tilt
        p.leaf_offset = s.leaf_offset
        p.adhesion_smoothing = s.adhesion_smoothing
        p.leaf_maturity = s.leaf_maturity
        p.stem_length = s.stem_length
        p.stem_thickness = s.stem_thickness
        p.stem_attach = s.stem_attach
        p.leaf_uv_direction = ('UP', 'RIGHT', 'DOWN', 'LEFT').index(s.leaf_uv_direction)
        p.hang_density = s.hang_density
        p.hang_length = s.hang_length
        up = (to_local.to_3x3() @ Vector((0.0, 0.0, 1.0))).normalized()                           
        p.up_x, p.up_y, p.up_z = up.x, up.y, up.z
        from . import background_compute
        r = yield from background_compute.native(core, 'grow_ivy', points, polys, seeds, p, guides, curves, **surface_guidance.kwargs(core))
        bp, bf, buv = r['branch_points'], r['branch_polys'], r['branch_uvs']
        lp, lf, luv = r['leaf_points'], r['leaf_polys'], r['leaf_uvs']
        if len(bf) == 0 and len(lf) == 0:
            self.report({'WARNING'}, 'Grow Ivy: nothing grew (check the seeds / guide / curve and Max Points)')
            return {'CANCELLED'}
                                               
        nb_, nl = len(bp), len(lp)
        verts = np.vstack([bp, lp]) if nl else bp
        faces = np.vstack([bf, lf + nb_]) if len(lf) else bf
        name = target.name + '_Ivy'
        old = s.result
        if old is None or not old.get('project_damage_ivy_generated') or old.parent != target:
            old = next((o for o in target.children if o.type == 'MESH' and o.get('project_damage_ivy_generated')), None)
        mesh = bpy.data.meshes.new(name)
        self._staged_mesh = mesh
        mesh.vertices.add(len(verts))
        mesh.vertices.foreach_set('co', verts.astype(np.float32).ravel())
        nf = len(faces)
        mesh.loops.add(nf * 4)
        mesh.polygons.add(nf)
        mesh.loops.foreach_set('vertex_index', faces.astype(np.int32).ravel())
        mesh.polygons.foreach_set('loop_start', np.arange(0, nf * 4, 4, dtype=np.int32))
        mesh.polygons.foreach_set('loop_total', np.full(nf, 4, dtype=np.int32))
        mat_index = np.concatenate([np.zeros(len(bf), dtype=np.int32), np.ones(len(lf), dtype=np.int32)])
        mesh.polygons.foreach_set('material_index', mat_index)
        mesh.update(calc_edges=True)
                                                                     
        uv = mesh.uv_layers.new(name='UVMap')
        uvs = np.empty((nf * 4, 2), dtype=np.float32)
        uvs[:len(bf) * 4] = buv[bf.ravel()]
        if len(lf):
            uvs[len(bf) * 4:] = luv.reshape(-1, 2)
        uv.data.foreach_set('uv', uvs.ravel())
        mesh.shade_smooth()
        obj = bpy.data.objects.new(name, mesh)
        self._staged_obj = obj
        obj.parent = target
        obj.matrix_parent_inverse.identity()
                                                   
        branch,leaf = ensure_materials(s)
        mesh.materials.append(branch)
        mesh.materials.append(leaf)
                                                                               
        vg_b = obj.vertex_groups.new(name='Ivy_Branches')
        vg_b.add(list(range(nb_)), 1.0, 'REPLACE')
        if nl:
            vg_l = obj.vertex_groups.new(name='Ivy_Leaves')
            vg_l.add(list(range(nb_, nb_ + nl)), 1.0, 'REPLACE')
        if old is not None:
            from .apply_selected import assign_mesh
            old_mesh = old.data
            assign_mesh(old, mesh, [g.name for g in obj.vertex_groups])
            bpy.data.objects.remove(obj)
            self._staged_obj = None
            obj = old
            if old_mesh.users == 0:
                bpy.data.meshes.remove(old_mesh)
        output_collections.link_object(context,target,obj)
        obj['project_damage_ivy_generated'] = True
        obj.project_damage_og_object = target
        s.result = obj
        target.project_damage.ivy_enable = True
        set_visibility(target, not target.project_damage.all_off)
        from . import incremental
        incremental.remember_output(context,target,surface,'ivy',obj,s)
        self._staged_obj = self._staged_mesh = None
        self.report({'INFO'}, 'Grow Ivy: %d roots, %d points, %d leaves' % (r['roots'], r['nodes'], len(lf)))
        return {'FINISHED'}


class PROJECT_DAMAGE_PT_ivy(bpy.types.Panel):
    bl_label = 'Ivy Generator'
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "⚒ Project Damage"
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        return target_for(context) is not None

    def draw(self, context):
        draw_settings(self.layout, context)


# действие «Ivy Drawing»
class PROJECT_DAMAGE_OT_ivy_draw(bpy.types.Operator):
    bl_idname = 'project_damage.ivy_draw'
    bl_label = 'Ivy Drawing'
    bl_options = {'REGISTER','UNDO'}
    action: bpy.props.EnumProperty(items=tuple((key,key.replace('_',' ').title(),'') for key in ('NEW','EDIT','DELETE','FINISH','CURVE_NEW','CURVE_EDIT','CURVE_DELETE')))

    @classmethod
    def poll(cls, context):
        return target_for(context) is not None

    def execute(self, context):
        from . import paint
        from .build import result_of
        source = target_for(context);settings = source.project_damage_ivy
        if self.action == 'FINISH':
            live.finish_drawing(source)
            paint._activate(context, result_of(source) or source)
            return {'FINISHED'}
        if self.action.startswith('CURVE_'):
            return self._curve(context, source, settings)
        drawing = settings.guide
        if self.action == 'NEW':
            if bpy.app.version < (4,3,0):
                self.report({'ERROR'},'Quick Draw needs Blender 4.3 or newer');return {'CANCELLED'}
            gp = bpy.data.grease_pencils.new(source.name+'_IvyGuide')
            gp.layers.new('Ivy Growth Guides',set_active=True).frames.new(context.scene.frame_current)
            drawing = bpy.data.objects.new(gp.name,gp)
            output_collections.link_object(context,source,drawing)
            drawing.parent = source;drawing.hide_render=True
            material = bpy.data.materials.new(gp.name+'_Ink')
            bpy.data.materials.create_gpencil_data(material)
            material.grease_pencil.color=(.12,.8,.16,1.)
            gp.materials.append(material)
            settings.guide = drawing
        if drawing is None:return {'CANCELLED'}
        if self.action == 'DELETE':
            if drawing.library is not None:return {'CANCELLED'}
            if context.object == drawing:paint._activate(context, result_of(source) or source)
            for obj in bpy.data.objects:
                if obj.project_damage_ivy.guide == drawing:obj.project_damage_ivy.guide=None
            bpy.data.objects.remove(drawing,do_unlink=True)
            return {'FINISHED'}
        drawing['project_damage_paint_source']=source
        paint._start_drawing(context,drawing)
        return {'FINISHED'}


    def _curve(self, context, source, settings):
        from . import paint
        from .build import result_of
        curve = settings.curve
        if self.action == 'CURVE_NEW':
            data = bpy.data.curves.new(source.name+'_IvyCurve', 'CURVE')
            data.dimensions = '3D'
            curve = bpy.data.objects.new(data.name, data)
            output_collections.link_object(context,source,curve)
            curve.parent = source;curve.hide_render = True
            settings.curve = curve
        if curve is None:return {'CANCELLED'}
        if self.action == 'CURVE_DELETE':
            if curve.library is not None:return {'CANCELLED'}
            if context.object == curve:paint._activate(context, result_of(source) or source)
            for obj in bpy.data.objects:
                if obj.project_damage_ivy.curve == curve:obj.project_damage_ivy.curve = None
            bpy.data.objects.remove(curve, do_unlink=True)
            return {'FINISHED'}
        curve['project_damage_paint_source'] = source
        paint._activate(context, curve)
                                                                               
                                                                          
                                          
        draw = context.scene.tool_settings.curve_paint_settings
        for attr, value in (('depth_mode', 'SURFACE'), ('use_stroke_endpoints', True), ('surface_offset', 0.0)):
            try:setattr(draw, attr, value)
            except (AttributeError, TypeError):pass
        if bpy.app.background:return {'FINISHED'}
        area = next((a for a in context.screen.areas if a.type == 'VIEW_3D'), None)
        if area:
            region = next(r for r in area.regions if r.type == 'WINDOW')
            with context.temp_override(area=area, region=region):
                bpy.ops.object.mode_set(mode='EDIT')
                bpy.ops.wm.tool_set_by_id(name='builtin.draw')
            area.spaces.active.show_region_ui = True
        return {'FINISHED'}


_CLASSES = (ProjectDamageIvySettings, PROJECT_DAMAGE_OT_grow_ivy, PROJECT_DAMAGE_OT_ivy_draw)


# подключаем свойства и действия этого модуля к Blender
def register():
    for cls in _CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.Object.project_damage_ivy = bpy.props.PointerProperty(type=ProjectDamageIvySettings)
    bpy.types.WindowManager.project_damage_ivy_section = bpy.props.EnumProperty(
        items=(('GEOMETRY','Geometry','Growth controls'),('MATERIAL','Material','Branch and leaf materials')),
        default='GEOMETRY', options={'SKIP_SAVE'})


# убираем подключения модуля при выключении аддона
def unregister():
    clear_pending()
    if hasattr(bpy.types.WindowManager, 'project_damage_ivy_section'):
        del bpy.types.WindowManager.project_damage_ivy_section
    del bpy.types.Object.project_damage_ivy
    for cls in reversed(_CLASSES):
        bpy.utils.unregister_class(cls)

                                                
from . import surface_guidance
