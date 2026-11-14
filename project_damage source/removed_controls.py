# старые пресеты не должны возвращать удалённые настройки
import bpy

REMOVED_CHIPS = frozenset(prefix + field for prefix in ('sec_', 'surf_sec_', 'corner_sec_')
                         for field in ('amount', 'size', 'depth'))
REMOVED_PRESET_VALUES = REMOVED_CHIPS | {'surface_secondary_material', 'erosion_mask_image',
    'erosion_mask_uv', 'erosion_mask_box', 'erosion_mask_invert', 'erosion_mask_adjust',
    'wallpaper_manual_seams'}


# убираем устаревшие значения из загружаемого пресета
def clean(values):
    result = {key: value for key, value in values.items() if key not in REMOVED_PRESET_VALUES}
    if result.get('erosion_mask_mode') == 'IMAGE': result['erosion_mask_mode'] = 'NONE'
    return result


def _zero_int(self):
    return 0


def _zero_float(self):
    return 0.0


def _ignore(self, value):
    pass


# старые файлы читаются, но удалённые настройки остаются выключенными
def install_compatibility(cls):
    for name in REMOVED_CHIPS:
        definition = cls.__annotations__[name]
        integer = definition.function == bpy.props.IntProperty
        factory = bpy.props.IntProperty if integer else bpy.props.FloatProperty
        cls.__annotations__[name] = factory(name='Removed Secondary Chips', default=0,
            get=_zero_int if integer else _zero_float, set=_ignore, options={'HIDDEN', 'SKIP_SAVE'})
    cls.__annotations__['wallpaper_manual_seams'] = bpy.props.BoolProperty(
        name='Removed Wallpaper Seam Setting', default=False, get=lambda self: False,
        set=_ignore, options={'HIDDEN', 'SKIP_SAVE'})
