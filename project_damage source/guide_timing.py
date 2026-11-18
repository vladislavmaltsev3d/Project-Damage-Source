"""Timeline handling for static Grease Pencil inputs created by Project Damage."""

import bpy


FRAME = 0
_paint_session = None


def owned_guide(obj):
    if obj is None or obj.type != 'GREASEPENCIL':
        return False
    owner = obj.get('project_damage_paint_source')
    return isinstance(owner, bpy.types.Object) and obj.parent == owner


def uses_held_frame(obj):
    return owned_guide(obj) and not any(mod.type == 'GREASE_PENCIL_TIME' and mod.show_viewport for mod in obj.modifiers)


def frame(layer, held=False):
    if hasattr(layer, 'current_frame'):
        if held and len(layer.frames) == 1 and layer.frames[0].frame_number == FRAME:
            return layer.frames[0]
        return layer.current_frame()
    return layer.active_frame


def migrate(drawing):
    if not uses_held_frame(drawing) or not drawing.data.is_editable or drawing.data.users != 1:
        return
    for layer in drawing.data.layers:
        if len(layer.frames) == 1 and layer.frames[0].frame_number > FRAME:
            layer.frames.move(layer.frames[0].frame_number, FRAME)


def static(drawing):
    return uses_held_frame(drawing) and all(
        not layer.frames or (len(layer.frames) == 1 and layer.frames[0].frame_number == FRAME)
        for layer in drawing.data.layers)


def start_paint(scene, drawing):
    global _paint_session
    stop_paint()
    if scene is None or not static(drawing):
        return
    tool = scene.tool_settings
    _paint_session = (scene, tool.use_keyframe_insert_auto)
    tool.use_keyframe_insert_auto = False


def stop_paint():
    global _paint_session
    session, _paint_session = _paint_session, None
    if session is not None:
        scene, auto_key = session
        try:
            scene.tool_settings.use_keyframe_insert_auto = auto_key
        except ReferenceError:
            pass
