# oсновной файл запуска
# Project_Damage Blender Addon
# Copyright (C) 2026 Vladislav Maltsev - Lazy3D
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <https://www.gnu.org/licenses/>.

bl_info = {
    "name": "Project Damage | Procedural Destruction Toolkit",
    "author": "Lazy3D",
    "version": (1, 0, 0),
    "blender": (4, 2, 0),
    "location": "3D View > Sidebar > Project Damage",
    "description": "Complete Damage and Destruction Effect Toolkit",
    "category": "Object",
}

                                                                              
CORE_VERSION = '1.0.0'
UI_REVISION = 1

from . import ownership, new_effects, breaker, build, clean_floating, core_loader, extra_effects, gizmo, input_prep, live, manual, paint, panels, presets, props
                                                                        
                                                                            
                                                                                              
                                                                            
                                                         
from . import mesh_io as mesh_io, params as params
from . import uv_zones
from . import ivy
from . import wall_hole
from . import separate_fragments
from . import debris_output
from . import material_library
from . import ivy
from . import roots
from . import cable_generator, cloth
from . import background_compute
from . import generated_assets

                                                                             
                                                                     
                                                                         
                                                                                               
                                                                                
                                                                                    
                                     
                                                                             
                                                                          
                                        
from . import inner_rough, ui_image_hints, editorial, voxel_erosion
from . import damage_generators, wallpaper_controls, preparation_tools, runtime_lookup
_MODULES = (runtime_lookup, voxel_erosion, editorial, ui_image_hints, inner_rough, core_loader, props, input_prep, ownership, breaker, build, clean_floating,
            presets, paint, extra_effects, new_effects, separate_fragments, debris_output, material_library, panels, gizmo, uv_zones, ivy, roots, cable_generator, cloth, wall_hole, generated_assets, background_compute, damage_generators, wallpaper_controls, preparation_tools, live)


from . import structure_surface, quality_ui, preset_pack
from . import material_roles
from . import pd_retopo, plank_surface, viewport_helpers, preset_browser, cell_cuts, cell_cut_plane, paper_generator
_MODULES += (preset_pack,structure_surface,pd_retopo,plank_surface,viewport_helpers,preset_browser,cell_cuts,cell_cut_plane,paper_generator)
_MODULES += (material_roles,)

def _register_modules():
    from . import ui_hints
    registered = []
    try:
        for mod in _MODULES:
            ui_hints.enhance_module(mod)
            mod.register()
            registered.append(mod)
    except Exception:
                                                                       
                                                                        
        try:mod.unregister()
        except Exception:pass
        for mod in reversed(registered):
            try:
                mod.unregister()
            except Exception:
                import traceback
                traceback.print_exc()
        raise


_REGISTERED=False

# подключаем свойства и действия этого модуля к Blender
def register():
    global _REGISTERED
    if _REGISTERED:return
                                                                            
                                                                      
    from . import asset_updates,registration
    if getattr(asset_updates,'REGISTRATION_SAFE',0)!=1:
        import importlib
        importlib.reload(asset_updates)
    from . import ui_hints
    with registration.guard():_register_modules()
    quality_ui.register()
    ui_hints.apply_registered_descriptions()
    _REGISTERED=True

# убираем подключения модуля при выключении аддона
def unregister():
    quality_ui.unregister()
    global _REGISTERED
    from . import registration
    errors=[]
    with registration.guard():
        for mod in reversed(_MODULES):
            try:mod.unregister()
            except Exception as exc:errors.append((mod.__name__,exc))
    _REGISTERED=False
    if errors:
        for name,exc in errors:print('Project Damage unregister cleanup:',name,str(exc))
