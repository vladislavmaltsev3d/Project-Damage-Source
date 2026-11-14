# растим корни по поверхности и направляющим
# ProjectDamage roots generator - GPL-3.0-or-later; see LICENSE.
#
# Grow Roots: the ivy growth model turned into surface roots, implemented in
# the native core (roots_generator.cpp). This module is the Blender side:
# settings on the object, one operator, the drawing helper, the settings UI.
#   Seeds    empties of the Seeds collection: each starts Roots per Seed main
#            roots radiating around it (put one at a tree's trunk base); with
#            none, the start of every guide stroke; with neither, the 3D cursor.
#   Guide    a Grease Pencil or Curve object: its strokes pull the roots along.
#   Result   "<object>_Roots", parented to the object: material slot 0 = roots
#            (your material, or a simple default); UV map "UVMap" (bark: U
#            around, V along in the same metric scale).
#   Welding  Merge by Distance welds the tube seams, the tips and any vertices
#            closer than Merge Distance, so a Displace modifier (or a
#            displacement material) moves the roots as one surface instead of
#            pulling them apart.
from . import output_collections
import bpy
import bmesh
import time
import numpy as np
from mathutils import Vector

from . import core_loader, ownership, live, material_controls, background_compute


                                                                      
_pending = {}


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
    if not p.live_update or not p.roots_enable or p.all_off:
        return
    from . import incremental
    from .build import result_of
    settings=source.project_damage_roots
    if incremental.reuse_output(bpy.context,source,result_of(source) or source,'roots',settings.result,settings):
        _pending.pop(source.name,None)
        return
    _pending[source.name] = time.monotonic()
    if not bpy.app.timers.is_registered(_live_tick):
        bpy.app.timers.register(_live_tick, first_interval=0.1)


def _settings_changed(self, context):
    if not live._APPLYING:
        background_compute.cancel(self.id_data, method='grow_roots')
        queue_update(self.id_data)


def _material_changed(self, context):
    """Material changes are slot edits, not another native growth job."""
    if live._APPLYING or live._HISTORY_RESTORING:
        return
    result = self.result
    if result is None or result.parent != self.id_data:
        return
    from .material_defaults import get
    material = self.root_material or get('roots')
    if material is not None:
        if result.data.users > 1:
            result.data = result.data.copy()
        if result.data.materials:
            result.data.materials[0] = material
        else:
            result.data.materials.append(material)


def dependencies_changed(scene, depsgraph):
    from . import live_input_state
    from .build import result_of
    moving={u.id.original for u in depsgraph.updates if u.is_updated_transform and isinstance(u.id.original,bpy.types.Object) and u.id.original.mode=='OBJECT'}
    changed = {u.id.original for u in depsgraph.updates
               if u.is_updated_geometry or u.is_updated_transform}
    if not changed:
        return
    for source in scene.objects:
        if source.type != 'MESH' or not source.project_damage.roots_enable:
            continue
        s = source.project_damage_roots
        objects = [source, result_of(source), s.guide]
        if s.seeds is not None:
            objects.extend(o for o in s.seeds.all_objects if o.type == 'EMPTY')
        if any(not (obj in {source,result_of(source)} and obj in moving)
               and (obj in changed or (obj.data is not None and obj.data in changed))
               and live_input_state.changed(obj, 'roots', local=obj in {source,result_of(source)})
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
        if source is None or not source.project_damage.live_update or not source.project_damage.roots_enable or source.project_damage.all_off:
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
            PROJECT_DAMAGE_OT_grow_roots._grow(worker, bpy.context, source)
        except background_compute.Deferred:
            pass
        except Exception as exc:
            if worker._staged_obj is not None:
                bpy.data.objects.remove(worker._staged_obj, do_unlink=True)
            if worker._staged_mesh is not None and worker._staged_mesh.users == 0:
                bpy.data.meshes.remove(worker._staged_mesh)
            print('ProjectDamage Roots live update:', exc)
        finally:
            live._APPLYING = False
    return 0.1 if _pending else None


def target_for(context):
    pinned = ownership.pinned_source(context)
    if pinned is not None:return pinned
    obj = context.active_object
    if obj is not None and obj.get('project_damage_roots_generated'):
        parent = ownership.source_of(obj.parent)
        return parent if parent is not None and parent.type == 'MESH' else None
    source = ownership.context_source(context)
    return source if source is not None and source.type == 'MESH' else None


def set_visibility(source, enabled):
    result = source.project_damage_roots.result
    if result is not None and result.parent == source and result.get('project_damage_roots_generated'):
        result.hide_set(not enabled)
        result.hide_render = not enabled


def apply_roots(source):
    result = source.project_damage_roots.result
    if result is None or result.parent != source:
        raise ValueError('Grow Roots before applying them')
    world = result.matrix_world.copy()
    result.parent = None;result.matrix_world = world
    result.hide_set(False);result.hide_render = False
    if 'project_damage_roots_generated' in result:
        del result['project_damage_roots_generated']
    result.project_damage_og_object = None
    source.project_damage_roots.result = None
    source.project_damage.roots_enable = False
    rows = source.project_damage_effects
    for i in reversed(range(len(rows))):
        if rows[i].key == 'roots':rows.remove(i)
    source.project_damage_effect_index = min(source.project_damage_effect_index, max(0,len(rows)-1))
    return result


                                                                            
                                                                           
                                                                        
                                
_RAW = ('size', 'roots_per_seed', 'root_length', 'length_random', 'primary_weight', 'random_weight',
        'gravity_weight', 'spread_weight', 'guide_weight', 'guide_radius', 'branching_probability',
        'max_levels', 'root_size', 'max_nodes')
# used as authored in both modes
_SHARED = ('seed', 'branch_angle', 'child_length', 'taper', 'tip_radius', 'child_radius', 'embed',
           'flare', 'flatten', 'sides', 'uv_scale')


def growth_settings(s):
    """The detailed settings a grow uses: as authored, or from the simple controls."""
    if s.advanced_controls:
        return {name: getattr(s, name) for name in _RAW}
    scale = s.master_scale
    return {
        'size': 3.0 * scale,                                                                     
        'root_length': 2.0 * s.length_amount ** 2 * scale,                    
        'root_size': 0.6 * scale * s.thickness,
        'roots_per_seed': 1 + int(round(10.0 * s.amount)),                  
        'max_nodes': int(5000 + 30000 * s.amount),                              
        'branching_probability': 0.32 * s.branching_amount ** 2,               
        'max_levels': 1 + int(round(4.0 * s.branching_amount)),             
        'random_weight': 0.7 * s.randomness,
        'primary_weight': 1.2 * (1.0 - s.randomness),
        'length_random': 0.6 * s.randomness,
        'gravity_weight': 1.6 * s.downhill ** 2,                              
        'spread_weight': 1.6 * s.spread_amount ** 2,                          
        'guide_weight': 2.0 * s.guide_strength if s.follow_guides else 0.0,
        'guide_radius': 0.05 + 0.2 * s.guide_strength,
    }


def merge_by_distance(mesh, distance):
    """Weld vertices closer than distance (object space); returns how many were removed."""
    if distance <= 0.0:
        return 0
    bm = bmesh.new()
    try:
        bm.from_mesh(mesh)
        before = len(bm.verts)
        bmesh.ops.remove_doubles(bm, verts=bm.verts[:], dist=distance)
        removed = before - len(bm.verts)
        bm.to_mesh(mesh)
    finally:
        bm.free()
    mesh.update()
    return removed


def _poll_guide(self, obj):
    return obj.type in {'GREASEPENCIL', 'GPENCIL', 'CURVE'}


# здесь хранятся значения, которые сохраняются вместе с объектом
class ProjectDamageRootsSettings(bpy.types.PropertyGroup):
    resolution: bpy.props.FloatProperty(name='Resolution',default=100.,min=10.,soft_max=300.,subtype='PERCENTAGE',update=_settings_changed,description='Tube cross-section quality; lower values reduce mesh density while retaining growth paths')
    result: bpy.props.PointerProperty(type=bpy.types.Object, options={'HIDDEN'})
    seeds: bpy.props.PointerProperty(update=_settings_changed, name='Seeds', type=bpy.types.Collection, description='Every empty in this collection starts a cluster of roots (dropped onto the surface): put one at the base of a trunk')
    guide: bpy.props.PointerProperty(update=_settings_changed, name='Guide', type=bpy.types.Object, poll=_poll_guide, description='Grease Pencil or Curve strokes the roots follow (and start from when there are no seeds)')
    seed: bpy.props.IntProperty(update=_settings_changed, name='Random Seed', default=0, min=0)
    size: bpy.props.FloatProperty(update=_settings_changed, name='Step Size', default=3.0, min=1.0, max=10.0, description='Growth step: larger is faster and coarser')
    roots_per_seed: bpy.props.IntProperty(update=_settings_changed, name='Roots per Seed', default=6, min=1, max=32, description='Main roots radiating evenly around each seed')
    root_length: bpy.props.FloatProperty(update=_settings_changed, name='Root Length', default=0.5, min=0.01, max=4.0, description='Length of a main root, as a share of the object size')
    length_random: bpy.props.FloatProperty(update=_settings_changed, name='Length Random', default=0.3, min=0.0, max=1.0, subtype='FACTOR', description='Random variation of the root lengths')
    primary_weight: bpy.props.FloatProperty(update=_settings_changed, name='Straightness', default=0.6, min=0.0, max=1.0, description='Tendency to keep growing the way it grew')
    random_weight: bpy.props.FloatProperty(update=_settings_changed, name='Wander', default=0.35, min=0.0, max=1.0, description='Random wandering')
    gravity_weight: bpy.props.FloatProperty(update=_settings_changed, name='Downhill', default=0.4, min=0.0, max=2.0, description='Pull downhill along the surface: none on flat ground, full down a wall or rock')
    spread_weight: bpy.props.FloatProperty(update=_settings_changed, name='Spread', default=0.4, min=0.0, max=2.0, description='Push away from the seed, so the roots fan out')
    guide_weight: bpy.props.FloatProperty(update=_settings_changed, name='Guide Weight', default=1.0, min=0.0, max=2.0, description='How strongly roots follow the guide strokes (side roots half as much per level)')
    guide_radius: bpy.props.FloatProperty(update=_settings_changed, name='Guide Reach', default=0.15, min=0.01, max=1.0, description='Reach of a guide stroke, as a share of the object size')
    branching_probability: bpy.props.FloatProperty(update=_settings_changed, name='Branching', default=0.08, min=0.0, max=1.0, description='Probability of a fork per root and step')
    branch_angle: bpy.props.FloatProperty(update=_settings_changed, name='Branch Angle', default=45.0, min=5.0, max=90.0, description='Angle of a side root off its parent, in degrees')
    max_levels: bpy.props.IntProperty(update=_settings_changed, name='Branch Levels', default=3, min=0, max=5, description='Generations of side roots')
    child_length: bpy.props.FloatProperty(update=_settings_changed, name='Side Root Length', default=0.5, min=0.05, max=1.0, subtype='FACTOR', description="Length of a side root, as a share of the parent's remaining length")
    root_size: bpy.props.FloatProperty(update=_settings_changed, name='Root Thickness', default=0.6, min=0.0, max=5.0, description='Base thickness of the main roots')
    taper: bpy.props.FloatProperty(update=_settings_changed, name='Taper', default=0.8, min=0.1, max=3.0, description='How fast roots thin out toward the tip')
    tip_radius: bpy.props.FloatProperty(update=_settings_changed, name='Tip Thickness', default=0.06, min=0.0, max=1.0, subtype='FACTOR', description='Thickness at the tip, as a share of the base')
    child_radius: bpy.props.FloatProperty(update=_settings_changed, name='Side Root Thickness', default=0.7, min=0.1, max=1.0, subtype='FACTOR', description="Thickness of a side root, as a share of the parent's thickness where it forks")
    embed: bpy.props.FloatProperty(update=_settings_changed, name='Embed', default=0.35, min=0.0, max=1.0, subtype='FACTOR', description='How deep roots sink into the surface: 0 lies on top, 1 is fully buried')
    flare: bpy.props.FloatProperty(update=_settings_changed, name='Trunk Flare', default=0.15, min=0.0, max=1.0, subtype='FACTOR', description='Share of each main root that rises out of the ground and thickens toward the seed, like the buttress at a trunk')
    flatten: bpy.props.FloatProperty(update=_settings_changed, name='Flatten', default=0.25, min=0.0, max=0.9, subtype='FACTOR', description='Squash the root section against the surface')
    sides: bpy.props.IntProperty(update=_settings_changed, name='Sides', default=8, min=3, max=16)
    uv_scale: bpy.props.FloatProperty(update=_settings_changed, name='Bark UV Scale', default=1.0, min=0.01, max=100.0, description='Tiling of the bark texture along and around the roots')
    max_nodes: bpy.props.IntProperty(update=_settings_changed, name='Max Points', default=20000, min=100, max=200000)
    merge_by_distance: bpy.props.BoolProperty(update=_settings_changed, name='Merge by Distance', default=True, description='Weld the tube seams, the tips and vertices that are close together, so the roots stay one surface when you displace them')
    merge_distance: bpy.props.FloatProperty(update=_settings_changed, name='Merge Distance', default=0.001, min=0.0, max=10.0, soft_max=0.1, subtype='DISTANCE', precision=4, description='Vertices closer than this (world size) are welded. Keep it below the tip thickness unless you want the tips closed to a point')
                     
    advanced_controls: bpy.props.BoolProperty(update=_settings_changed, name='Advanced Controls', default=False, description='Show and use the detailed growth settings instead of the simple controls')
    master_scale: bpy.props.FloatProperty(update=_settings_changed, name='Master Scale', default=1.0, min=0.05, max=20.0, soft_min=0.2, soft_max=5.0, description='Scales the whole root system: root length, thickness and growth step together')
    amount: bpy.props.FloatProperty(update=_settings_changed, name='Amount', default=0.5, min=0.0, max=1.0, subtype='FACTOR', description='How many main roots start at each seed (1 to 11), and how much may grow in total')
    length_amount: bpy.props.FloatProperty(update=_settings_changed, name='Length', default=0.5, min=0.0, max=1.0, subtype='FACTOR', description='How far the roots reach from their seed (times Master Scale)')
    branching_amount: bpy.props.FloatProperty(update=_settings_changed, name='Branching', default=0.5, min=0.0, max=1.0, subtype='FACTOR', description='How often roots fork into side roots, and how many generations of side roots grow')
    randomness: bpy.props.FloatProperty(update=_settings_changed, name='Randomness', default=0.5, min=0.0, max=1.0, subtype='FACTOR', description='0 grows straight, even roots; 1 wanders and varies the root lengths a lot')
    downhill: bpy.props.FloatProperty(update=_settings_changed, name='Downhill', default=0.5, min=0.0, max=1.0, subtype='FACTOR', description='How much roots run down walls, rocks and slopes (no effect on flat ground)')
    spread_amount: bpy.props.FloatProperty(update=_settings_changed, name='Spread', default=0.5, min=0.0, max=1.0, subtype='FACTOR', description='How strongly roots fan out away from their seed')
    thickness: bpy.props.FloatProperty(update=_settings_changed, name='Thickness', default=1.0, min=0.0, max=5.0, description='Root thickness (times Master Scale)')
    follow_guides: bpy.props.BoolProperty(update=_settings_changed, name='Follow Guides', default=True, description='Grow along the guide strokes')
    guide_strength: bpy.props.FloatProperty(update=_settings_changed, name='Guide Strength', default=0.5, min=0.0, max=1.0, subtype='FACTOR', description='How strongly and from how far the roots are pulled along the guide strokes (side roots half as much per level)')
    root_material: bpy.props.PointerProperty(update=_material_changed, name='Root Material', type=bpy.types.Material)


# действие «Grow Roots»
class PROJECT_DAMAGE_OT_grow_roots(bpy.types.Operator):
    bl_idname = 'project_damage.grow_roots'
    bl_label = 'Grow Roots'
    bl_description = 'Grow roots over the active mesh from the seed empties / guide strokes; replaces its previous roots'
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
            self.report({'INFO'}, 'Roots calculation queued in background')
            return {'FINISHED'}
        except Exception as exc:
            if self._staged_obj is not None and self._staged_obj.name in bpy.data.objects:
                bpy.data.objects.remove(self._staged_obj, do_unlink=True)
            if self._staged_mesh is not None and self._staged_mesh.users == 0:
                bpy.data.meshes.remove(self._staged_mesh)
            self.report({'ERROR'}, 'Grow Roots: '+str(exc))
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
            bg.queue(target,'grow_roots',lambda: PROJECT_DAMAGE_OT_grow_roots._grow_steps(Worker(),bpy.context,target))
            return {'FINISHED'}
        return bg.immediate(PROJECT_DAMAGE_OT_grow_roots._grow_steps(self,context,target))

    def _grow_steps(self, context, source=None):
        target = source if source is not None else target_for(context)
        if target.library is not None:
            raise ValueError('Make the target local before growing roots')
        s = target.project_damage_roots
                                                                              
        if not s.is_property_set('advanced_controls'):
            s.advanced_controls = s.result is not None
        core = core_loader.load()
        if core is None or not hasattr(core, 'grow_roots'):
            self.report({'ERROR'}, 'Grow Roots: restart Blender to load the updated native core')
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
            from . import paint
            guides = [np.ascontiguousarray(g, dtype=np.float64) for g in paint.read_paths(context, target, s.guide) if len(g) >= 2]
                                                                    
        seeds = []
        if s.seeds is not None:
            for o in sorted(s.seeds.all_objects, key=lambda o: o.name):
                if o.type == 'EMPTY':
                    q = to_local @ o.matrix_world.translation
                    seeds.append((q.x, q.y, q.z))
        if not seeds and not guides:
            q = to_local @ context.scene.cursor.location
            seeds.append((q.x, q.y, q.z))
        seeds = np.ascontiguousarray(np.array(seeds, dtype=np.float64).reshape(-1, 3))
        p = core.RootsParams()
        for name, value in growth_settings(s).items():
            setattr(p, name, value)
        for name in _SHARED:
            setattr(p, name, getattr(s, name))
        p.sides=max(3,min(48,round(s.sides*s.resolution/100.)))
        up = (to_local.to_3x3() @ Vector((0.0, 0.0, 1.0))).normalized()                           
        p.up_x, p.up_y, p.up_z = up.x, up.y, up.z
        r = yield from background_compute.native(core, 'grow_roots', points, polys, seeds, p, guides, **surface_guidance.kwargs(core))
        verts, faces, buv = r['points'], r['polys'], r['uvs']
        if len(faces) == 0:
            self.report({'WARNING'}, 'Grow Roots: nothing grew (check the seeds / guide and Max Points)')
            return {'CANCELLED'}
        name = target.name + '_Roots'
        old = s.result
        if old is None or not old.get('project_damage_roots_generated') or old.parent != target:
            old = next((o for o in target.children if o.type == 'MESH' and o.get('project_damage_roots_generated')), None)
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
        mesh.update(calc_edges=True)
                                            
        uv = mesh.uv_layers.new(name='UVMap')
        uv.data.foreach_set('uv', buv[faces.ravel()].astype(np.float32).ravel())
                                                                              
                                                      
        welded = 0
        if s.merge_by_distance:
            scale = max(abs(v) for v in target.matrix_world.to_scale())
            welded = merge_by_distance(mesh, s.merge_distance / max(scale, 1e-12))
        mesh.shade_smooth()
        obj = bpy.data.objects.new(name, mesh)
        self._staged_obj = obj
        obj.parent = target
        obj.matrix_parent_inverse.identity()
        material = s.root_material
        if material is None:
            from .material_defaults import get
            material = get('roots')
            s.root_material = material
        mesh.materials.append(material)
        if old is not None:
            from .apply_selected import assign_mesh
            old_mesh = old.data
            assign_mesh(old, mesh, [])
            bpy.data.objects.remove(obj)
            self._staged_obj = None
            obj = old
            if old_mesh.users == 0:
                bpy.data.meshes.remove(old_mesh)
        output_collections.link_object(context,target,obj)
        obj['project_damage_roots_generated'] = True
        obj.project_damage_og_object = target
        group=obj.vertex_groups.get('PD_Roots') or obj.vertex_groups.new(name='PD_Roots')
        group.add(list(range(len(mesh.vertices))),1.0,'REPLACE')
        s.result = obj
        target.project_damage.roots_enable = True
        set_visibility(target, not target.project_damage.all_off)
        from . import incremental
        incremental.remember_output(context,target,surface,'roots',obj,s)
        self._staged_obj = self._staged_mesh = None
        self.report({'INFO'}, 'Grow Roots: %d roots, %d points, %d vertices welded' % (r['roots'], r['nodes'], welded))
        return {'FINISHED'}


# действие «Roots Drawing»
class PROJECT_DAMAGE_OT_roots_draw(bpy.types.Operator):
    bl_idname = 'project_damage.roots_draw'
    bl_label = 'Roots Drawing'
    bl_options = {'REGISTER','UNDO'}
    action: bpy.props.EnumProperty(items=tuple((key,key.title(),'') for key in ('NEW','EDIT','DELETE','FINISH')))

    @classmethod
    def poll(cls, context):
        return target_for(context) is not None

    def execute(self, context):
        from . import paint
        from .build import result_of
        source = target_for(context);settings = source.project_damage_roots
        if self.action == 'FINISH':
            live.finish_drawing(source)
            paint._activate(context, result_of(source) or source)
            return {'FINISHED'}
        drawing = settings.guide
        if self.action == 'NEW':
            if bpy.app.version < (4,3,0):
                self.report({'ERROR'},'Quick Draw needs Blender 4.3 or newer');return {'CANCELLED'}
            gp = bpy.data.grease_pencils.new(source.name+'_RootsGuide')
            gp.layers.new('Root Guides',set_active=True).frames.new(context.scene.frame_current)
            drawing = bpy.data.objects.new(gp.name,gp)
            output_collections.link_object(context,source,drawing)
            drawing.parent = source;drawing.hide_render=True
            material = bpy.data.materials.new(gp.name+'_Ink')
            bpy.data.materials.create_gpencil_data(material)
            material.grease_pencil.color=(.55,.33,.12,1.)
            gp.materials.append(material)
            settings.guide = drawing
        if drawing is None:return {'CANCELLED'}
        if self.action == 'DELETE':
            if drawing.library is not None:return {'CANCELLED'}
            if context.object == drawing:paint._activate(context, result_of(source) or source)
            for obj in bpy.data.objects:
                if obj.project_damage_roots.guide == drawing:obj.project_damage_roots.guide=None
            bpy.data.objects.remove(drawing,do_unlink=True)
            return {'FINISHED'}
        drawing['project_damage_paint_source']=source
        if drawing.type == 'CURVE':
            paint._activate(context, drawing)
            if not bpy.app.background:
                area = next((a for a in context.screen.areas if a.type == 'VIEW_3D'), None)
                if area:
                    region = next(r for r in area.regions if r.type == 'WINDOW')
                    with context.temp_override(area=area, region=region):
                        bpy.ops.object.mode_set(mode='EDIT')
                        bpy.ops.wm.tool_set_by_id(name='builtin.draw')
        else:
            paint._start_drawing(context,drawing)
        return {'FINISHED'}


# показываем настройки выбранного эффекта
def draw_settings(layout,context):
    from .panels import draw_roots_settings
    draw_roots_settings(layout,context)


_CLASSES = (ProjectDamageRootsSettings, PROJECT_DAMAGE_OT_grow_roots, PROJECT_DAMAGE_OT_roots_draw)


# подключаем свойства и действия этого модуля к Blender
def register():
    for cls in _CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.Object.project_damage_roots = bpy.props.PointerProperty(type=ProjectDamageRootsSettings)
    bpy.types.WindowManager.project_damage_roots_section = bpy.props.EnumProperty(
        items=(('GEOMETRY','Geometry','Growth controls'),('MATERIAL','Material','Root material')),
        default='GEOMETRY', options={'SKIP_SAVE'})


# убираем подключения модуля при выключении аддона
def unregister():
    clear_pending()
    if hasattr(bpy.types.WindowManager, 'project_damage_roots_section'):
        del bpy.types.WindowManager.project_damage_roots_section
    if hasattr(bpy.types.Object,'project_damage_roots'):
        del bpy.types.Object.project_damage_roots
    for cls in reversed(_CLASSES):
        bpy.utils.unregister_class(cls)

                                                
from . import surface_guidance
