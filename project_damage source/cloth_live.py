# обновляем ткань только после изменения её входных данных
import hashlib
import time
import bpy
from bpy.app.handlers import persistent

_pending={}
_last={}
_targets_cache={}
_revisions={}


def _key(settings):
    return settings.id_data.session_uid


def _targets(scene):
    key=scene.session_uid
    cached=_targets_cache.get(key)
    if cached is None or cached[0]!=len(scene.objects):
        owners=[obj for obj in scene.objects if obj.get('project_damage_cloth_asset')]
        cached=(len(scene.objects),owners)
        _targets_cache[key]=cached
    result=[]
    for owner in cached[1]:
        try:
            if owner.name in scene.objects:result.append((owner.project_damage_cloth,owner))
        except ReferenceError:continue
    legacy=getattr(scene,'project_damage_cloth',None)
    if legacy and legacy.result:
        source=legacy.curve if legacy.source!='EMPTIES' else next((obj for obj in legacy.empties.all_objects if obj.type=='EMPTY'),None) if legacy.empties else None
        if source is not None and not source.get('project_damage_cloth_asset'):result.append((legacy,source))
    return result


def _sources(settings,scene):
    objects=[]
    if settings.source=='EMPTIES':
        if settings.empties:objects.extend(obj for obj in settings.empties.all_objects if obj.type=='EMPTY')
    elif settings.curve:objects.append(settings.curve)
    if settings.colliders:
        objects.extend(obj for obj in settings.colliders.all_objects if obj.type=='MESH' and obj!=settings.result)
    elif settings.source!='HANGING':
        from .cloth import _is_sheet
        candidates=[obj for obj in scene.objects if obj.type=='MESH' and not obj.hide_get() and not _is_sheet(obj)]
        if len(candidates)==1:objects.extend(candidates)
    return objects


def signature(settings,scene):
    from . import live_input_state
    values=[]
    for prop in settings.bl_rna.properties:
        if prop.identifier in {'rna_type','result','live_update','material'}:continue
        value=getattr(settings,prop.identifier)
        if prop.type=='POINTER':value=value.session_uid if isinstance(value,bpy.types.ID) else None
        elif getattr(prop,'is_array',False):value=tuple(value)
        values.append((prop.identifier,value))
    for obj in _sources(settings,scene):
        values.append((obj.session_uid,tuple(value for row in obj.matrix_world for value in row)))
        if obj.type=='CURVE':
            for spline in obj.data.splines:
                values.append((spline.type,spline.use_cyclic_u,spline.resolution_u))
                if spline.type=='BEZIER':values.append(tuple((tuple(p.co),tuple(p.handle_left),tuple(p.handle_right)) for p in spline.bezier_points))
                else:values.append(tuple(tuple(p.co) for p in spline.points))
        elif obj.type in {'GREASEPENCIL','GPENCIL'}:
            values.append(live_input_state.signature(obj))
        elif obj.type=='MESH':
            values.append(live_input_state.signature(obj))
    return hashlib.blake2b(repr(values).encode(),digest_size=16).digest()


def changed(settings,context):
    from . import cloth,live
    if cloth._building or live._APPLYING or live._HISTORY_RESTORING:return
    if not settings.live_update:
        _pending.pop(_key(settings),None)
        return
    if settings.result is None:return
    scene=settings.id_data if isinstance(settings.id_data,bpy.types.Scene) else getattr(context,'scene',None)
    if scene is None:return
    owner=settings.id_data if isinstance(settings.id_data,bpy.types.Object) else settings.curve if settings.source!='EMPTIES' else next((obj for obj in settings.empties.all_objects if obj.type=='EMPTY'),None) if settings.empties else None
    if owner is not None:queue(settings,owner,scene)


def queue(settings,owner,scene):
    if not settings.live_update:return
    _pending[_key(settings)]=(settings,owner,scene,time.monotonic())
    if not bpy.app.timers.is_registered(tick):bpy.app.timers.register(tick,first_interval=.05)


def generated(settings,owner,scene):
    _pending.pop(_key(settings),None)
    _last[_key(settings)]=signature(settings,scene)
    _targets_cache.pop(scene.session_uid,None)


# проверяем изменения источников ткани перед обновлением
def tick():
    from . import cloth,live
    if live._HISTORY_RESTORING:
        _pending.clear();return None
    now=time.monotonic()
    for key,(settings,owner,scene,stamp) in list(_pending.items()):
        try:
            delay=max(.1,getattr(owner.project_damage,'defer_ms',300)/1000.)
            if now-stamp<delay:continue
            _pending.pop(key,None)
            if not settings.live_update or owner.name not in scene.objects or scene.name not in bpy.data.scenes:continue
            if signature(settings,scene)==_last.get(key):continue
            layer=next((layer for layer in scene.view_layers if owner.name in layer.objects),None)
            if layer is None:continue
            with bpy.context.temp_override(scene=scene,view_layer=layer):
                result=cloth.generate(bpy.context,settings,owner)
            if result!={'FINISHED'}:print('ProjectDamage Cloth: previous result kept; check the curve or empties')
        except (ReferenceError,RuntimeError,ValueError) as exc:
            _pending.pop(key,None);print('ProjectDamage Cloth live update:',exc)
        return .05 if _pending else None
    return .05 if _pending else None


@persistent
def depsgraph_changed(scene,depsgraph):
    from . import cloth,live
    if cloth._building or live._APPLYING or live._HISTORY_RESTORING or not hasattr(bpy.data,'objects'):return
    events=[event for event in depsgraph.updates if event.is_updated_geometry or event.is_updated_transform or isinstance(event.id.original,bpy.types.Collection)]
    if not events:return
    changed_ids={event.id.original.session_uid for event in events}
    for settings,owner in _targets(scene):
        if not settings.live_update or settings.result is None:continue
        dependencies={coll.session_uid for coll in (settings.empties if settings.source=='EMPTIES' else None,settings.colliders) if coll is not None}
        for obj in _sources(settings,scene):
            dependencies.add(obj.session_uid)
            if obj.data is not None:dependencies.add(obj.data.session_uid)
        if changed_ids & dependencies and signature(settings,scene)!=_last.get(_key(settings)):
            queue(settings,owner,scene)


@persistent
def clear(*args):
    _pending.clear();_last.clear();_targets_cache.clear();_revisions.clear()


# подключаем свойства и действия этого модуля к Blender
def register():
    for handlers,fn in ((bpy.app.handlers.depsgraph_update_post,depsgraph_changed),(bpy.app.handlers.load_pre,clear),(bpy.app.handlers.undo_pre,clear),(bpy.app.handlers.redo_pre,clear)):
        if fn not in handlers:handlers.append(fn)


# убираем подключения модуля при выключении аддона
def unregister():
    clear()
    if bpy.app.timers.is_registered(tick):bpy.app.timers.unregister(tick)
    for handlers,fn in ((bpy.app.handlers.depsgraph_update_post,depsgraph_changed),(bpy.app.handlers.load_pre,clear),(bpy.app.handlers.undo_pre,clear),(bpy.app.handlers.redo_pre,clear)):
        if fn in handlers:handlers.remove(fn)
