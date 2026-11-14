# быстро находим объекты аддона, не просматривая всю сцену каждый раз
"""Bounded ID lookups; never retain geometry, evaluated meshes or GPU data."""
from collections import OrderedDict
import time
import bpy
from bpy.app.handlers import persistent

_outputs = OrderedDict()
_planks = OrderedDict()
_LIMIT = 256

def _remember(cache, key, value):
    cache[key] = value
    cache.move_to_end(key)
    while len(cache) > _LIMIT:
        cache.popitem(last=False)
    return value

def output(source, stamp, *, strict_uid=False):
    if source is None:
        return None
    uid = source.get('project_damage_uid')
    key = (source.session_uid, stamp, strict_uid)
    cached = _outputs.get(key)
    if cached is not None:
        try:
            if (cached.project_damage_og_object == source and cached.get(stamp)
                    and (strict_uid or cached.type == 'MESH')
                    and (not strict_uid or cached.get(stamp) == uid)):
                _outputs.move_to_end(key)
                return cached
        except ReferenceError:
            pass
        _outputs.pop(key, None)
    if strict_uid and not uid:
        return None
    for ob in bpy.data.objects:
        if (ob.project_damage_og_object == source and ob.get(stamp)
                and (strict_uid or ob.type == 'MESH')
                and (not strict_uid or ob.get(stamp) == uid)):
            return _remember(_outputs, key, ob)
    if not strict_uid and uid is not None:
        return next((ob for ob in bpy.data.objects if ob.type == 'MESH'
                     and ob.get(stamp) == uid
                     and ob.project_damage_og_object in (None, source)), None)
    return None

def plank_owner(context):
    obj = context.active_object
    if obj is None:
        return None
    key = (context.scene.session_uid, obj.session_uid)
    now = time.monotonic()
    entry = _planks.get(key)
    if entry is not None and now < entry[0]:
        target = entry[1]
        try:
            if target is None or (target.get('pd_plank_surface')
                    and any(item.obj == obj for item in target.pd_planks.points)):
                return target
        except ReferenceError:
            pass
    target = next((ob for ob in context.scene.objects
                   if ob.get('pd_plank_surface')
                   and any(item.obj == obj for item in ob.pd_planks.points)), None)
    _remember(_planks, key, (now + .25, target))
    return target

def clear_planks():
    _planks.clear()

@persistent
def clear(*_):
    _outputs.clear()
    _planks.clear()

_HOOKS = ('load_pre', 'undo_pre', 'redo_pre')
# подключаем свойства и действия этого модуля к Blender
def register():
    clear()
    for name in _HOOKS:
        handlers = getattr(bpy.app.handlers, name)
        if clear not in handlers:
            handlers.append(clear)

# убираем подключения модуля при выключении аддона
def unregister():
    clear()
    for name in _HOOKS:
        handlers = getattr(bpy.app.handlers, name)
        if clear in handlers:
            handlers.remove(clear)
