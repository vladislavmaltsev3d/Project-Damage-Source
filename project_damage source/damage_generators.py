# подключаем новые генераторы к Blender
# ProjectDamage damage generators: registration of the shared panel + every
# generator kind - GPL-3.0-or-later; see LICENSE. __init__.py registers only
# this module; see DAMAGE_GENERATORS.md.
import importlib

from . import damage_gen_common, ui_hints

# generator modules in panel order; a missing module is skipped
_KIND_MODULES = ('metal_tear_gen', 'wood_split_gen', 'tile_damage_gen', 'erosion_gen', 'blast_crater_gen')
_loaded = []


# подключаем свойства и действия этого модуля к Blender
def register():
    damage_gen_common.register()
    _loaded.clear()
    for name in _KIND_MODULES:
        try:
            mod = importlib.import_module('.' + name, __package__)
        except ImportError as exc:
            if getattr(exc, 'name', '') and exc.name.endswith(name):
                continue
            raise
        # Hover texts go in before registration, so nothing is redefined afterwards.
        ui_hints.enhance_module(mod)
        mod.register()
        _loaded.append(mod)


# убираем подключения модуля при выключении аддона
def unregister():
    for mod in reversed(_loaded):
        try:
            mod.unregister()
        except Exception as exc:
            print('ProjectDamage generators: unregister %s: %s' % (mod.__name__, exc))
    _loaded.clear()
    damage_gen_common.unregister()
