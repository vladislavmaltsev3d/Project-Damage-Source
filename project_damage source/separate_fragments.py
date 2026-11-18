# разделяем связные куски и заменяем предыдущий набор фрагментов
# GPL-3.0-or-later.
"""Separate Fragments: split a mesh into one object per loose piece.

Made for Through Fractures, where every fracture fragment is its own mesh
island, but it works on any mesh. The active object's damage result is split
when it has one, otherwise the active mesh itself. The split happens on a copy,
so the source and its live result are never changed (the original is only
hidden), and an Update afterwards does not touch the fragments."""
from . import output_collections
import bpy
from . import build, live, ownership

FRAGMENT_STAMP = "project_damage_fragment_of"
FRAGMENT_SCOPE = "project_damage_fragment_scope"
GLASS_COLLECTION = "project_damage_glass_fragments_collection"


def target_of(obj):
    """The mesh to split: a result as is, a source's result, else the mesh."""
    if obj is None or obj.type != 'MESH':
        return None
    if obj.get(build.RESULT_STAMP):
        return obj
    return build.result_of(obj) or obj


class ProjectDamageFragmentSettings(bpy.types.PropertyGroup):
    origin_to_pieces: bpy.props.BoolProperty(
        name="Origin to Pieces", default=True,
        description="Put each fragment's origin at the centre of its bounds")
    own_collection: bpy.props.BoolProperty(
        name="Own Collection", default=True,
        description="Put the fragments in a new collection named after the mesh")
    hide_original: bpy.props.BoolProperty(
        name="Hide Original", default=True,
        description="Hide the mesh that was split once the fragments exist")


def _fragment_collection(context, src, settings):
    parent = output_collections.destination(context,ownership.source_of(src))
    lc = _find_layer_collection(context.view_layer.layer_collection, parent)
    if lc is None or lc.exclude or lc.hide_viewport or parent.hide_viewport:
                                                             
        parent = context.scene.collection
    if not settings.own_collection:
        return parent
    coll = bpy.data.collections.new(src.name + "_fragments")
    parent.children.link(coll)
    return coll


def _find_layer_collection(lc, coll):
    if lc.collection == coll:
        return lc
    for child in lc.children:
        found = _find_layer_collection(child, coll)
        if found is not None:
            return found
    return None


def target_for_context(context):
    pinned=ownership.pinned_source(context)
    return target_of(pinned if pinned is not None else context.active_object)


def _glass_target(context):
    source = ownership.context_source(context)
    if source is None or source.type != 'MESH':
        return None, None
    props = source.project_damage
    if not (props.glass_drawn_enable or props.glass_hit_enable):
        return None, None
    return source, build.result_of(source)


def _glass_pieces(context, source, result):
    return [obj for obj in context.scene.objects
            if obj.type == 'MESH' and obj.get(FRAGMENT_STAMP) and obj.project_damage_og_object == source
            and (obj.get(FRAGMENT_SCOPE) == 'GLASS'
                 or (obj.get(FRAGMENT_SCOPE) is None and
                     (obj[FRAGMENT_STAMP] == result.name or obj.get('project_damage_glass_shard_scope'))))]


def _is_glass_collection(collection, source, result):
    if collection.get(GLASS_COLLECTION) == source.get('project_damage_uid'):
        return True
    for name in (result.name + "_fragments", source.name + "_GlassFill_fragments"):
        if collection.name == name:
            return True
        if collection.name.startswith(name + ".") and collection.name[len(name)+1:].isdecimal():
            return True
    return False


def _glass_collection(context, source, result, settings, old):
    if settings.own_collection:
        collections = {coll for obj in old for coll in obj.users_collection}
        uid = source['project_damage_uid']
        collections.update(coll for coll in bpy.data.collections if coll.get(GLASS_COLLECTION) == uid)
        for coll in sorted(collections, key=lambda c: c.name):
            if not _is_glass_collection(coll, source, result):
                continue
            layer = _find_layer_collection(context.view_layer.layer_collection, coll)
            if layer is not None and not (layer.exclude or layer.hide_viewport or coll.hide_viewport):
                return coll, False
    return _fragment_collection(context, result, settings), settings.own_collection


def _remove_glass_pieces(source, result, objects):
    from . import damage_physics
    collections = {coll for obj in objects for coll in obj.users_collection
                   if _is_glass_collection(coll, source, result)}
    for obj in objects:
        damage_physics._remove(obj)
    for coll in collections:
        if not coll.objects and not coll.children:
            bpy.data.collections.remove(coll)


# действие «Remove Glass Pieces»
class PROJECT_DAMAGE_OT_remove_glass_pieces(bpy.types.Operator):
    bl_idname = "project_damage.remove_glass_pieces"
    bl_label = "Remove Glass Pieces"
    bl_description = "Remove all separated glass pieces for this generator and show the live glass again"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return context.mode == 'OBJECT' and not live._APPLYING and _glass_target(context)[1] is not None

    def execute(self, context):
        source, result = _glass_target(context)
        pieces = _glass_pieces(context, source, result)
        if not pieces:
            self.report({'INFO'}, "There are no separated glass pieces to remove")
            return {'CANCELLED'}
        previous = live._APPLYING
        live._APPLYING = True
        try:
            active_was_piece = context.active_object in pieces
            _remove_glass_pieces(source, result, pieces)
            result.hide_render = False
            result.hide_set(False)
            if active_was_piece and result.name in context.view_layer.objects:
                result.select_set(True)
                context.view_layer.objects.active = result
        finally:
            live._APPLYING = previous
        self.report({'INFO'}, "Removed %d separated glass pieces" % len(pieces))
        return {'FINISHED'}


# действие «Separate Fragments»
class PROJECT_DAMAGE_OT_separate_fragments(bpy.types.Operator):
    """Split the active mesh (or its damage result) into one object per fragment"""
    bl_idname = "project_damage.separate_fragments"
    bl_label = "Separate Fragments"
    bl_options = {'REGISTER', 'UNDO'}

    origin_mode: bpy.props.EnumProperty(
        name="Origin", default='SETTINGS',
        items=(('SETTINGS', "From Settings", "Use Origin to Pieces (centre of bounds)"),
               ('MASS', "Centre of Mass", "Each piece's origin at its centre of mass and no parent, "
                "ready for a rigid-body simulation")))

    glass: bpy.props.BoolProperty(default=False, options={'HIDDEN', 'SKIP_SAVE'})

    @classmethod
    def poll(cls, context):
        return (context.mode == 'OBJECT' and not live._APPLYING
                and target_for_context(context) is not None)

    def execute(self, context):
        settings = context.scene.project_damage_fragments
        active = context.active_object
        if self.glass:
            owner, src = _glass_target(context)
        else:
            src = target_for_context(context)
            owner = ownership.source_of(src)
        if src is None:
            return {'CANCELLED'}
        if not self.glass and not len(src.data.polygons):
            self.report({'WARNING'}, src.name + " has no faces to separate")
            return {'CANCELLED'}
        previous = live._APPLYING
        live._APPLYING = True
        work = coll = None
        created_collection = False
        collection_before = set()
        selected_before = tuple(context.selected_objects) if self.glass else ()
        visibility_before = (src.hide_get(), src.hide_render) if self.glass else None
        try:
            if self.glass:
                from . import background_compute
                background_compute.cancel(owner, 'build_worn')
                with background_compute.synchronous():
                    result, reports = build.rebuild_object(context, owner)
                if result is None or any('ERROR' in level for level, _ in reports):
                    for level, message in reports:
                        self.report(level, message)
                    return {'CANCELLED'}
                src = result
                if not len(src.data.polygons):
                    self.report({'WARNING'}, src.name + " has no faces to separate")
                    return {'CANCELLED'}
                old_pieces = _glass_pieces(context, owner, src)
                coll, created_collection = _glass_collection(context, owner, src, settings, old_pieces)
                collection_before = {obj.session_uid for obj in coll.objects}
            else:
                coll = _fragment_collection(context, src, settings)
            work = src.copy()
            work.data = src.data.copy()
            if self.glass or src.get(build.RESULT_STAMP):
                from . import effect_groups
                effect_groups.weld(work.data)
                effect_groups.clear_split(work.data)
            for key in list(work.keys()):
                if key.startswith(('edgy', 'project_damage')):
                    del work[key]
                                                                              
                                                                                
            work.project_damage_og_object = owner if owner is not src or src.get(build.RESULT_STAMP) else None
            work.project_damage_effects.clear()
            work.project_damage.property_unset('fract_through_enable')
            from .sidebar import ENABLE_PROPS
            for key in ENABLE_PROPS:
                setattr(work.project_damage, key, False)
            work.name = src.name + "_frag"
            work.data.name = work.name
            work.hide_viewport = False
            work.hide_select = False
            work.hide_render = False                                              
            work.matrix_world = src.matrix_world
            coll.objects.link(work)
            work.hide_set(False)

            for obj in tuple(context.selected_objects or ()):
                if obj is not None:
                    obj.select_set(False)
            work.select_set(True)
            context.view_layer.objects.active = work
            bpy.ops.object.mode_set(mode='EDIT')
            bpy.ops.mesh.reveal()
            bpy.ops.mesh.select_all(action='SELECT')
            bpy.ops.mesh.separate(type='LOOSE')
            bpy.ops.object.mode_set(mode='OBJECT')

            pieces = [o for o in context.selected_objects if o is not None and o.type == 'MESH']
            if self.glass and len(pieces) < 2:
                raise RuntimeError("No loose glass pieces were generated")
            if self.origin_mode == 'MASS':
                                                                                  
                for piece in pieces:
                    mw = piece.matrix_world.copy()
                    piece.parent = None
                    piece.matrix_world = mw
                bpy.ops.object.origin_set(type='ORIGIN_CENTER_OF_VOLUME', center='MEDIAN')
            elif settings.origin_to_pieces:
                bpy.ops.object.origin_set(type='ORIGIN_GEOMETRY', center='BOUNDS')
                                                          
            pieces.sort(key=lambda o: -len(o.data.polygons))
            for piece in pieces:
                piece[FRAGMENT_STAMP] = src.name
                piece[FRAGMENT_SCOPE] = 'GLASS' if self.glass else 'MESH'
                if owner is not src or src.get(build.RESULT_STAMP):
                    piece.project_damage_og_object = owner
            try:
                from . import inner_rough
                inner_rough.after_fragments(context, src, pieces)
            except Exception as exc:
                self.report({'WARNING'}, 'Break Faces skipped: %s' % exc)
            from . import damage_physics
            damage_physics.after_fragments(src, pieces)
            if self.glass:
                if settings.own_collection:
                    coll[GLASS_COLLECTION] = owner['project_damage_uid']
                _remove_glass_pieces(owner, src, old_pieces)
                # A previous split may have hidden the preview under a different setting.
                src.hide_set(settings.hide_original)
                src.hide_render = settings.hide_original
            for i, piece in enumerate(pieces, 1):
                piece.name = "%s_frag_%03d" % (src.name, i)
                piece.data.name = piece.name
            work = None
            if settings.hide_original and not self.glass:
                                                                               
                src.hide_set(True)
                src.hide_render = True
                if active is not src and active.type == 'MESH':
                    active.hide_set(True)
                    active.hide_render = True
            context.view_layer.objects.active = pieces[0] if pieces else None
        except RuntimeError as exc:
            if context.object is not None and context.object.mode != 'OBJECT':
                bpy.ops.object.mode_set(mode='OBJECT')
            if self.glass and coll is not None:
                from . import damage_physics
                generated = [obj for obj in coll.objects if obj.session_uid not in collection_before]
                if work is not None and work not in generated:
                    generated.append(work)
                for obj in generated:
                    damage_physics._remove(obj)
                if created_collection and not coll.objects and not coll.children:
                    bpy.data.collections.remove(coll)
                src.hide_set(visibility_before[0])
                src.hide_render = visibility_before[1]
                for obj in tuple(context.selected_objects):
                    obj.select_set(False)
                for obj in selected_before:
                    obj.select_set(True)
                context.view_layer.objects.active = active
            elif work is not None:
                mesh = work.data
                bpy.data.objects.remove(work, do_unlink=True)
                if mesh.users == 0:
                    bpy.data.meshes.remove(mesh)
            self.report({'ERROR'}, "Separate Fragments failed: %s" % exc)
            return {'CANCELLED'}
        finally:
            live._APPLYING = previous
        self.report({'INFO'}, "Separate Fragments: %d objects from %s" % (len(pieces), src.name))
        return {'FINISHED'}


def draw_controls(layout, context):
    s = context.scene.project_damage_fragments
    col = layout.column(align=True)
    col.operator("project_damage.separate_fragments", icon='MOD_EXPLODE')
    row = col.row(align=True)
    row.prop(s, "origin_to_pieces", toggle=True)
    row.prop(s, "own_collection", toggle=True)
    row.prop(s, "hide_original", toggle=True)
    from . import inner_rough, ownership
    inner_rough.draw(layout, ownership.context_source(context), 'fragments')


_CLASSES = (ProjectDamageFragmentSettings, PROJECT_DAMAGE_OT_separate_fragments,
            PROJECT_DAMAGE_OT_remove_glass_pieces)


# подключаем свойства и действия этого модуля к Blender
def register():
    from . import damage_physics
    damage_physics.register()
    for cls in _CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.Scene.project_damage_fragments = bpy.props.PointerProperty(type=ProjectDamageFragmentSettings)


# убираем подключения модуля при выключении аддона
def unregister():
    from . import damage_physics
    damage_physics.unregister()
    del bpy.types.Scene.project_damage_fragments
    for cls in reversed(_CLASSES):
        bpy.utils.unregister_class(cls)
