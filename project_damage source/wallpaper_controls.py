# управляем сторонами обоев и отогнутыми полосами через пустышки
import bpy
from . import live, ownership


def finders(props):
    result = [obj for obj in props.wallpaper_finders.all_objects if obj.type == 'EMPTY'] if props.wallpaper_finders else []
    if props.wallpaper_finder is not None and props.wallpaper_finder not in result:
        result.append(props.wallpaper_finder)
    return sorted(result, key=lambda obj: obj.name)


def pack(context, source, block, factor):
    if not block.enable:
        return
    p = source.project_damage
    block.marker_only = p.wallpaper_flap_mode == 'EMPTIES'
    block.flap_markers = []
    collection = p.wallpaper_flap_collection
    if collection is None or p.wallpaper_flap_mode == 'AUTO':
        return
    graph = context.evaluated_depsgraph_get()
    inverse = source.evaluated_get(graph).matrix_world.inverted_safe()
    values = []
    for empty in sorted(collection.all_objects, key=lambda obj: obj.name):
        if empty.type != 'EMPTY':
            continue
        transform = inverse @ empty.evaluated_get(graph).matrix_world
        position = transform.translation * factor
        diameter = 2.0 * empty.empty_display_size * max(abs(v) for v in transform.to_scale()) * factor
        values.extend((*position, max(0.01, diameter)))
    block.flap_markers = values


# действие «Add Flap Empty»
class PROJECT_DAMAGE_OT_wallpaper_flap_add(bpy.types.Operator):
    bl_idname = 'project_damage.wallpaper_flap_add'
    bl_label = 'Add Flap Empty'
    bl_description = 'Place an Empty at the 3D cursor. The nearest wallpaper strip tears around this height. Move it to choose the strip; scale it to set the tear length'
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        src = ownership.context_source(context)
        return context.mode == 'OBJECT' and src is not None and src.type == 'MESH' and src.project_damage.wallpaper_enable

    def execute(self, context):
        from .params import unit_factor
        from mathutils import Matrix
        src = ownership.context_source(context)
        p = src.project_damage
        previous = live._APPLYING
        live._APPLYING = True
        try:
            collection = p.wallpaper_flap_collection
            if collection is None:
                collection = bpy.data.collections.new(src.name + ' Wallpaper Flaps')
                context.scene.collection.children.link(collection)
                p.wallpaper_flap_collection = collection
            empty = bpy.data.objects.new('Wallpaper Flap', None)
            collection.objects.link(empty)
            empty.empty_display_type = 'SPHERE'
            empty.empty_display_size = max(0.05, (p.wallpaper_tear_length_min + p.wallpaper_tear_length_max) / (4.0 * unit_factor(context)))
            empty.show_in_front = True
            empty.color = (1.0, 0.35, 0.05, 1.0)
            empty.parent = src
            empty.matrix_world = Matrix.Translation(context.scene.cursor.location)
            empty.project_damage_og_object = src
            if p.wallpaper_flap_mode == 'AUTO':p.wallpaper_flap_mode = 'BOTH'
            p.wallpaper_tears_enable = True
            p.wallpaper_flaps_enable = True
            for ob in context.selected_objects:ob.select_set(False)
            empty.select_set(True)
            context.view_layer.objects.active = empty
            context.view_layer.update()
        finally:
            live._APPLYING = previous
        live._mark_dirty(src)
        return {'FINISHED'}


# подключаем свойства и действия этого модуля к Blender
def register():
    bpy.utils.register_class(PROJECT_DAMAGE_OT_wallpaper_flap_add)


# убираем подключения модуля при выключении аддона
def unregister():
    if PROJECT_DAMAGE_OT_wallpaper_flap_add.is_registered:
        bpy.utils.unregister_class(PROJECT_DAMAGE_OT_wallpaper_flap_add)
