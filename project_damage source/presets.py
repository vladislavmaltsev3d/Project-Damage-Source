# читаем, сохраняем и применяем пресеты в выбранном режиме
# ProjectDamage for Blender - worn edge damage cut into mesh objects.
# Copyright (C) 2026 Cornelius Dammrich
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
"""Damage presets: the three built-in ones (Mild/Strong/Heavy) as an operator
over presets_data.PRESETS (GENERATED, see that module's own docstring), plus
the user's own library on top of Blender's native preset system - the menu
with its plus and minus, which writes an executable .py into the user's own
preset folder and shows it immediately.

Seed and mesh settings (topology, edge threshold, selection names, axis
switches) are deliberately untouched by either - a preset shapes the damage
look, not the object identity or the mesh contract.

The built-ins stay OPERATORS rather than shipped preset files: a file in the
user's preset folder is deletable, and Mild/Strong/Heavy must be equally
unremovable on both hosts. The two libraries are separate by decision (spec
docs/superpowers/specs/2026-08-26-user-presets-design.md); a preset saved in
Cinema 4D does not appear here.
"""
import bpy
import os
import time
from pathlib import Path
from datetime import datetime
from functools import lru_cache
from . import ownership
from bl_operators.presets import AddPresetBase

from . import presets_data
from . import presets_library



presets_data.PRESETS.update(presets_library.props_table())
for _retired_key in presets_library.UNAVAILABLE_PRESETS:presets_data.PRESETS.pop(_retired_key,None)



FABRIC_NOISE_PROPS=('fabric_distribution_scale','fabric_distribution_strength','fabric_shape_noise_scale','fabric_shape_noise_strength','fabric_solid')
PRESET_PROPS = sorted(presets_data.PRESETS["MILD"].keys())
PRESET_PROPS += ["preserve_data", "fract_mode", "fract_asset", "surf_mode", "surf_close_strokes", "surf_paint_snap"]
PRESET_PROPS += ["damage_shading", "damage_smooth_angle", "base_smooth_angle"]
PRESET_PROPS += ["chip_seed", "surf_seed", "surf_type", "surf_grain_x", "surf_grain_y", "surf_grain_z", "surf_knife_jitter", "surf_knife_back"]
PRESET_PROPS += ["noise_roughness", "noise_distortion"]
from .generated_geometry import FIELDS as GENERATED_FIELDS
PRESET_PROPS += list(GENERATED_FIELDS)
PRESET_PROPS += ['fract_physics','fract_physics_depth','fract_physics_close']
PRESET_PROPS += ['fract_helper_snap']
PRESET_PROPS += ["fract_l%d_%s" % (n, field) for n in (1, 2, 3)
                 for field in ("paint_enable", "paint_radius")]
PRESET_PROPS += ["fract_recipe", "fract_thickness_cap", "fract_prox_mode", "fract_prox_use_size",
                 "fract_prox_radius", "fract_prox_strength", "fract_radial_spokes",
                 "fract_radial_rings", "fract_radial_ring_fill", "fract_radial_crush",
                 "fract_glass_spokes", "fract_glass_rings", "fract_glass_branch",
                 "fract_glass_ring_fill", "fract_glass_width", "fract_glass_depth", "fract_glass_crush"]
PRESET_PROPS += ["deposit_enable", "deposit_amount", "deposit_style",
                "deposit_size_min", "deposit_size_max", "deposit_height",
                "deposit_gravity", "deposit_gravity_direction", "deposit_shelter",
                "deposit_shelter_radius", "deposit_embed", "deposit_layers",
                "deposit_porosity", "deposit_separate"]
from .effect_schema import EFFECT_FIELDS
PRESET_PROPS += [module + "_" + name for module, fields in EFFECT_FIELDS.items() for name, _ in fields]
from .rebar import FIELDS
PRESET_PROPS += ["rebar_"+row[0] for row in FIELDS]
PRESET_PROPS += ['rebar_brick_library']
PRESET_PROPS += ['boolean_wood_deepness', 'boolean_central_rebar', 'boolean_central_axis', 'boolean_central_diameter', 'boolean_central_embed']
PRESET_PROPS += ['boolean_stable_volume', 'boolean_volume_axis', 'boolean_volume_budget', 'boolean_volume_clearance', 'boolean_volume_min_length', 'boolean_volume_min_section', 'boolean_volume_origin_x', 'boolean_volume_origin_y', 'boolean_volume_origin_z']
PRESET_PROPS += ["fabric_group"] + list(FABRIC_NOISE_PROPS)
PRESET_PROPS += ['global_scale','global_detail','ui_boolean_enable']
PRESET_PROPS += ['deform_enable', 'deform_collection', 'deform_origin', 'deform_protection',
                 'deform_strength', 'deform_max_deform', 'deform_total_amplitude',
                 'deform_noise_factor', 'deform_groups_x', 'deform_groups_y', 'deform_groups_z']
PRESET_PROPS += ['deform_origin_mode', 'deform_origin_depth', 'deform_smoothness', 'deform_noise_scale',
                 'deform_protect_group', 'deform_protect_invert']
PRESET_PROPS += ['chips_mode','chips_paint_radius']
PRESET_PROPS += ['build_mode','build_paint_radius']
PRESET_PROPS += ['draw_mask_enable', 'draw_mask_mode', 'draw_mask_radius', 'draw_mask_soft', 'draw_mask_size']
PRESET_PROPS += ['fract_through_enable', 'fract_through', 'fract_through_min_piece']
PRESET_PROPS += ['fract_volume_size', 'fract_volume_rough']
PRESET_PROPS += ['cellf_prox_collection','cellf_prox_use_size','cellf_drawing','cellf_surface_drawing','cellf_draw_mode']
PRESET_PROPS += ['fract_l%d_length' % n for n in (1,2,3)]
PRESET_PROPS += ['fract_type','fract_impacts','fract_grain_x','fract_grain_y','fract_grain_z','fract_grain_pull']
PRESET_PROPS += ['fract_l%d_%s'%(n,key) for n in (1,2,3) for key in ('branches','branch_length')]
PRESET_PROPS += ['hit_through_enable','glass_through_enable','fabric_solid_enable','concrete_v2_enable','lath_v2_enable','lath_riven_enable','lath_framing_enable','brick_v2_enable','glass_hit_enable','glass_drawn_enable','glass_drawn_reach','glass_hit_collection']
PRESET_PROPS += ["rust_materials", "rust_mode"]
PRESET_PROPS += ["rust_enable", "rust_mask_source", "rust_color_attr", "rust_color_channel",
                 "rust_mask_invert", "rust_group", "rust_seed", "rust_intensity", "rust_edge_bias",
                 "rust_bottom_bias", "rust_world_gravity", "rust_gravity", "rust_noise",
                 "rust_noise_scale", "rust_detail", "rust_crust_start", "rust_crust_height",
                 "rust_crust_rough", "rust_pits", "rust_pit_start", "rust_pit_size", "rust_pit_depth",
                 "rust_hole_start", "rust_hole_depth", "rust_flakes", "rust_flake_size", "rust_paint",
                 "rust_paint_start", "rust_band", "rust_paint_flakes", "rust_craze", "rust_craze_cell",
                 "rust_blisters", "rust_blister_size", "rust_streak", "rust_streak_length"]
PRESET_PROPS += ["heat_enable", "heat_seed", "heat_intensity", "heat_diffuse", "heat_spread", "heat_duration", "heat_steps", "heat_char_start", "heat_shrink_depth", "heat_detail", "heat_block_round", "heat_cell_min", "heat_cell_max", "heat_crack_cells", "heat_crack_width", "heat_crack_depth", "heat_blisters", "heat_blister_min", "heat_blister_max", "heat_blister_low", "heat_blister_high", "heat_blister_burst", "heat_char_layer", "heat_group"]
PRESET_PROPS += ["coating_zone_enable", "coating_zone_threshold", "coating_zone_smooth", "coating_zone_inset", "coating_zone_detail", "coating_flake_side", "coating_flake_side_x", "coating_flake_side_y", "coating_flake_side_z", "coating_zone_group"]
PRESET_PROPS += ["coating_enable", "coating_amount", "coating_thickness", "coating_patch_min",
                 "coating_patch_max", "coating_jagged", "coating_flakes", "coating_curl", "coating_cracks",
                 "coating_seed", "coating_satellites", "coating_stretch", "coating_dir_x", "coating_dir_y", "coating_dir_z", "coating_edge_wrap", "coating_facing_only", "coating_facing_x", "coating_facing_y", "coating_facing_z", "coating_facing_angle", "coating_flake_size", "coating_crack_length"]


PRESET_APPLY_MODES = (
    ('NONE', 'Default', 'Use the preset’s normal behavior'),
    ('ADD', 'Add', 'Keep existing effects enabled and add the preset effects'),
    ('REPLACE', 'Replace', 'Remove the existing Project Damage effects and apply the preset'),
)


def additive_effects(src, requested):
    """Keep the active variant when an incoming effect shares its core slot."""
    from . import sidebar
    families = (
        ({'concrete', 'concrete_v2', 'pipes', 'cables'}, ('concrete_', 'pipe_', 'cable_')),
        ({'lath', 'lath_v2', 'lath_riven', 'lath_framing'}, ('lath_',)),
        ({'brick', 'brick_v2'}, ('brick_',)),
        ({'fabric', 'fabric_solid'}, ('fabric_',)),
        ({'hit', 'hit_through'}, ('hit_',)),
        ({'glass', 'glass_through', 'glass_hit', 'glass_drawn'}, ('glass_',)),
    )
    selected = list(requested)
    requested = set(selected)
    excluded = []
    for keys, prefixes in families:
        active = {key for key in keys if sidebar.enabled(src, key)}
        conflicts = (keys & requested) - active
        if active and conflicts:
            selected = [key for key in selected if key not in conflicts]
            excluded.extend(prefixes)
    return selected, tuple(excluded)


def _remove_effect_stack(src):
    from . import sidebar, preset_breakers
    props = src.project_damage
    for name in sidebar.ENABLE_PROPS:
        setattr(props, name, False)
    for group in ('ivy', 'roots'):
        settings = getattr(src, 'project_damage_' + group)
        result = settings.result
        if result is not None and result.parent == src and result.get(f'project_damage_{group}_generated'):
            settings.result = None
            bpy.data.objects.remove(result, do_unlink=True)
    preset_breakers.clear(src)
    src.project_damage_effects.clear()
    from . import fracture_stack, wall_stack
    wall_stack.cleanup_outputs(src, set())
    for key in (wall_stack.SERIAL, wall_stack.EDIT, wall_stack.BASE_MATERIALS, wall_stack.PROXIMITY_SNAPSHOTS):
        if key in src:
            del src[key]
    fracture_stack.reset(src)
    src.project_damage_effect_index = 0


# действие «Apply ProjectDamage Preset»
class PROJECT_DAMAGE_OT_apply_preset(bpy.types.Operator):
    """Apply a built-in damage preset (sets every damage-tab value; seed and mesh settings stay)"""
    bl_idname = "project_damage.apply_preset"
    bl_label = "Apply ProjectDamage Preset"
    bl_options = {'REGISTER', 'UNDO'}
    preset: bpy.props.EnumProperty(items=[(k, presets_library.preset_title(k), "") for k in presets_data.PRESETS])
    mode: bpy.props.EnumProperty(items=PRESET_APPLY_MODES, default='NONE', options={'SKIP_SAVE'})

    @classmethod
    def poll(cls, context):
        return ownership.context_source(context) is not None and hasattr(ownership.context_source(context), "project_damage")

    def execute(self, context):
        from . import background_compute, build, live, sidebar
        src=ownership.context_source(context);p=src.project_damage
        additive = self.mode == 'ADD'
        previous=live._APPLYING;live._APPLYING=True
        try:
            if self.mode == 'REPLACE':
                _remove_effect_stack(src)
            if self.preset in presets_library.LIBRARY:
                if not presets_library.apply(src, self.preset, additive=additive, context=context):
                    self.report({'WARNING'}, 'This preset uses a different variant of an effect already on the object. Use Replace to switch variants.')
                    return {'CANCELLED'}
            else:
                excluded = ()
                from . import fracture_stack
                values=presets_data.PRESETS[self.preset]
                if not additive:
                    for key,_ in fracture_stack.layers(src):sidebar.set_enabled(src,key,False)
                if values.get('fract_enable'):
                    sidebar.set_enabled(src,'fracture',True)
                if self.mode == 'ADD':
                    values = presets_data.PRESETS[self.preset]
                    effects = [key for key, row in sidebar.CATALOG.items()
                               if row[2] != 'fract_enable' and values.get(row[2])]
                    if values.get('fract_enable'):
                        effects.append('fracture')
                    _, excluded = additive_effects(src, effects)
                for name, value in presets_data.PRESETS[self.preset].items():
                    if self.mode == 'ADD' and (name.startswith(excluded) or
                                               (name in sidebar.ENABLE_PROPS and not value)):
                        continue
                    setattr(p, name, value)
            from . import fracture_stack
            fracture_stack.flags(src)
            if self.mode != 'ADD':
                for key in sidebar.REMOVED_EFFECTS:
                    prop=sidebar.CATALOG.get(key,(None,None,key+'_enable'))[2]
                    if hasattr(p,prop):setattr(p,prop,False)
            if self.mode == 'REPLACE':
                from . import generated_assets
                for key in ('grass', 'gravel', 'paper_generator'):
                    if getattr(p, key + '_enable'):
                        continue
                    old = generated_assets.output(src, key)
                    if old is not None:
                        mesh = old.data
                        bpy.data.objects.remove(old, do_unlink=True)
                        if mesh.users == 0:
                            bpy.data.meshes.remove(mesh)
        finally:live._APPLYING=previous
        sidebar.sync(src)
        if additive:
            background_compute.cancel(src)
            live._dirty.pop(src.get('project_damage_uid'), None)
            previous=live._APPLYING;live._APPLYING=True
            try:
                result, reports = build.rebuild_object(context, src)
            finally:
                live._APPLYING=previous
            for kind, message in reports:
                self.report(kind, message)
            if result is None:
                return {'CANCELLED'}
        else:
            live.touch_prop(p,context)
        if self.preset in presets_library.LIBRARY and presets_library.LIBRARY[self.preset]['seeds']:

            try:presets_library.grow(src,context)
            except (RuntimeError,ValueError) as exc:self.report({'WARNING'},'Preset applied; grow it with Grow Ivy / Grow Roots ('+str(exc)+')')
        # Ivy / Roots looks without a seed grew nothing and said nothing:
        # ask for the start point (3D cursor + Enter) right away.
        from . import recording_tools
        missing=[k for k in ('ivy','roots') if recording_tools.needs_seed(src,k)]
        if missing and not recording_tools.request_seed(context,src,missing):
            self.report({'WARNING'},'Preset applied: place the 3D cursor where the %s should start and use Add Growth Seed'%' and '.join(missing))
        return {'FINISHED'}


def userlib_directory(create=False):
    from . import extension_storage
    return extension_storage.directory('User_lib', create=create)


def user_preset_path(filename):
    root=userlib_directory()
    path=root/filename
    if path.resolve().parent!=root.resolve() or path.suffix!='.py':
        raise ValueError('Choose a preset from User_lib')
    return path


class PROJECT_DAMAGE_MT_presets(bpy.types.Menu):
    """Only user presets stored in Project Damage's User_lib directory."""
    bl_label = "ProjectDamage Presets"
    preset_subdir = "project_damage"
    preset_operator = "script.execute_preset"
    def draw(self,context):
        try:
            directory=userlib_directory()
            paths=sorted(directory.glob('*.py')) if directory.is_dir() else []
        except OSError:
            self.layout.label(text='Cannot open the User_lib directory', icon='ERROR')
            return
        if not paths:self.layout.label(text='User_lib is empty',icon='INFO')
        for path in paths:
            if path.name.startswith('__'):continue
            self.layout.operator('project_damage.load_user_preset',text=path.stem.replace('_',' ')).filename=path.name


class PROJECT_DAMAGE_OT_open_userlib(bpy.types.Operator):
    bl_idname='project_damage.open_userlib'
    bl_label='Open User_lib Folder'
    bl_description='Open the Project Damage User_lib directory in Finder or Windows Explorer'

    def execute(self,context):
        try:return bpy.ops.wm.path_open(filepath=str(userlib_directory(create=True)))
        except (OSError,RuntimeError) as exc:
            self.report({'ERROR'},str(exc));return {'CANCELLED'}


# действие «Load User Preset»
class PROJECT_DAMAGE_OT_load_user_preset(bpy.types.Operator):
    bl_idname='project_damage.load_user_preset'
    bl_label='Load User Preset'
    bl_options={'REGISTER','UNDO'}
    filename:bpy.props.StringProperty()

    def execute(self,context):
        import sys
        from . import fracture_stack, live, sidebar
        try:
            path=user_preset_path(self.filename)
            if not path.is_file():raise ValueError('Preset not found in User_lib')
        except (OSError,ValueError) as exc:
            self.report({'ERROR'},str(exc));return {'CANCELLED'}
        previous=live._APPLYING;live._APPLYING=True
        # Preset scripts from legacy installations use these module names.
        modules = {
            'project_damage': sys.modules[__package__],
            'project_damage.ownership': ownership,
            'project_damage.fracture_stack': fracture_stack,
        }
        old_modules = {name: sys.modules.get(name) for name in modules}
        sys.modules.update(modules)
        try:
            status = bpy.ops.script.execute_preset(filepath=str(path), menu_idname='PROJECT_DAMAGE_MT_presets')
        finally:
            for name, module in old_modules.items():
                if module is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = module
            live._APPLYING = previous
        if status=={'FINISHED'}:
            context.window_manager.project_damage_user_preset=path.name
            src=ownership.context_source(context)
            if src is not None:
                if 'fracture_stack.load_preset' not in path.read_text(encoding='utf-8'):
                    fracture_stack.reset(src)
                sidebar.sync(src);live.touch_prop(src.project_damage,context)
        return status


# действие «Save ProjectDamage Preset»
class PROJECT_DAMAGE_OT_add_preset(AddPresetBase, bpy.types.Operator):
    """Save the current damage settings as a preset, or remove the selected one"""
    bl_idname = "project_damage.add_preset"
    bl_label = "Save ProjectDamage Preset"
    preset_menu = "PROJECT_DAMAGE_MT_presets"
    preset_subdir = "project_damage"


    preset_defines = ["from project_damage.ownership import context_source", "project_damage = context_source(bpy.context).project_damage"]

    @classmethod
    def poll(cls, context):
        return ownership.context_source(context) is not None and hasattr(ownership.context_source(context), "project_damage")

    @property
    def preset_values(self):


        p=ownership.context_source(bpy.context).project_damage
        return ['project_damage.'+prop.identifier for prop in p.bl_rna.properties
                            if prop.identifier!='rna_type' and prop.identifier not in REMOVED_PRESET_VALUES and prop.type in {'BOOLEAN','INT','FLOAT','ENUM','STRING'}]

    def execute(self,context):
        try:
            root=userlib_directory(create=True)
            if self.remove_active or self.remove_name:
                filename=context.window_manager.project_damage_user_preset
                if not filename:raise ValueError('Choose a User_lib preset to remove')
                path=user_preset_path(filename)
                if not path.is_file():raise ValueError('Selected preset was not found')
                trash=root/'.trash';trash.mkdir(exist_ok=True)
                target=trash/(path.stem+'_'+datetime.now().strftime('%Y%m%d_%H%M%S_%f')+'.py')
                path.replace(target)
                context.window_manager.project_damage_user_preset=''
                PROJECT_DAMAGE_MT_presets.bl_label='User_lib'
                self.report({'INFO'},'Preset moved to User_lib/.trash (recoverable)')
                return {'FINISHED'}
            name=self.name.strip()
            if not name:raise ValueError('Enter a preset name')
            filename=self.as_filename(name)+'.py'
            path=user_preset_path(filename)
            if path.exists():raise ValueError('A preset with this name already exists in User_lib')
            source = ownership.context_source(context)
            p = source.project_damage
            lines=['import bpy','from project_damage.ownership import context_source',
                   'source = context_source(bpy.context)',
                   'project_damage = source.project_damage']
            for rna_path in self.preset_values:
                value=getattr(p,rna_path.split('.',1)[1])
                if not isinstance(value,(str,int,float,bool,set)):value=list(value)
                lines.append(rna_path+' = '+repr(value))

            for key in ('weld', 'grass', 'gravel', 'foam', 'paper', 'metal_gen', 'wood_gen',
                        'tile_gen', 'erosion_gen', 'crater_gen'):
                attr = 'project_damage_' + key
                settings = getattr(source, attr, None)
                if settings is None:
                    continue
                group_lines = []
                if key.endswith('_gen'):
                    group_lines.append("settings['_in_hierarchy'] = True")
                    reference = settings.get('_resolution_reference', settings.resolution)
                    group_lines.append(f"settings['_resolution_reference'] = {reference!r}")
                for prop in settings.bl_rna.properties:
                    name = prop.identifier
                    if prop.is_readonly or name in REMOVED_PRESET_VALUES or name in {'resolution_quality', 'report'}:
                        continue
                    if prop.type in {'BOOLEAN', 'INT', 'FLOAT', 'ENUM', 'STRING'}:
                        value = getattr(settings, name)
                        if getattr(prop, 'is_array', False):
                            value = list(value)
                        group_lines.append(f'settings.{name} = {value!r}')
                if group_lines:
                    lines.extend((f'settings = getattr(source, {attr!r}, None)', 'if settings is not None:'))
                    lines.extend('    ' + line for line in group_lines)
            from . import fracture_stack
            lines.append('from project_damage import fracture_stack')
            lines.append(f'fracture_stack.load_preset(source, {fracture_stack.preset_data(source)!r})')
            with path.open('x',encoding='utf-8') as file:file.write('\n'.join(lines)+'\n')
            context.window_manager.project_damage_user_preset=filename
            PROJECT_DAMAGE_MT_presets.bl_label=name
        except (OSError,ValueError,AttributeError) as exc:
            self.report({'ERROR'},'User_lib: '+str(exc));return {'CANCELLED'}
        self.report({'INFO'},'Saved preset in '+str(path));return {'FINISHED'}


_CLASSES = (PROJECT_DAMAGE_OT_apply_preset, PROJECT_DAMAGE_MT_presets, PROJECT_DAMAGE_OT_add_preset,
            PROJECT_DAMAGE_OT_open_userlib,PROJECT_DAMAGE_OT_load_user_preset)

_PRESET_PREVIEWS=None
_PRESET_ITEMS=[]
_CATALOG = None
_CATEGORY_ITEMS = []
_PREVIEW_FILES = {}
_ITEM_CACHE = {}
_BUILTIN_KEYS = tuple(presets_data.PRESETS)
_BUILTIN_NUMBERS = {key: number for number, key in enumerate(_BUILTIN_KEYS)}
_EMPTY_PRESET = ('__EMPTY__', 'No matching presets', 'Change or clear the search', 0, 2147483000)
_CATALOG_CHECK_INTERVAL = 20.0
_last_catalog_use = 0.0


def _directory_entries(folder):
    try:
        with os.scandir(folder) as entries:
            return list(entries)
    except OSError:
        return []


def _preview_files(entries):
    files = {}
    for entry in entries:
        stem, ext = os.path.splitext(entry.name)
        key = stem.upper()
        if ext.lower() != '.png' or key not in presets_data.PRESETS:
            continue
        try:
            if entry.is_file():
                stat = entry.stat()
                files[key] = (Path(entry.path), stat.st_mtime_ns, stat.st_size)
        except OSError:
            continue
    return files


def _index_catalog():
    """List each folder once. Enum callbacks and GPU draws reuse this index."""
    global _CATALOG, _PREVIEW_FILES, _CATEGORY_ITEMS
    from . import effect_categories, preset_pack
    preview_root = preset_pack.directory()
    catalog = {}
    images = {}
    assigned = set()
    for entry in sorted(_directory_entries(preview_root), key=lambda e: e.name):
        if entry.name.startswith('.') or entry.name.casefold() in {'general','generals'}:
            continue
        try:
            if not entry.is_dir():continue
        except OSError:continue
        files = _preview_files(_directory_entries(entry.path))
        for key in presets_data.PRESETS:
            if key not in files or key in assigned:continue
            group=effect_categories.preset_category(key,presets_library.LIBRARY.get(key,{}),entry.name)
            images.setdefault(group.upper(),{})[key]=files[key]
            assigned.add(key)
    for group,title,_ in effect_categories.CATEGORIES:
        category=group.upper()
        if category in images:
            catalog[category]=(title,preview_root,list(images[category]))


    favorites={key:version for files in images.values() for key,version in files.items() if key in preset_pack.favorites()}
    images['FAVORITES']=favorites
    catalog={'FAVORITES':('Favorites',preview_root,list(favorites)),**catalog}
    if catalog == _CATALOG and images == _PREVIEW_FILES:
        return False
    for category in set(_PREVIEW_FILES) | set(images):
        previous = _PREVIEW_FILES.get(category, {})
        current = images.get(category, {})
        if previous == current and catalog.get(category) == (_CATALOG or {}).get(category):
            continue
        for item in tuple(_ITEM_CACHE):
            if item[0] == category:
                del _ITEM_CACHE[item]
        if _PRESET_PREVIEWS is not None:
            for key, version in previous.items():
                token = category + '/' + key
                if current.get(key) != version and token in _PRESET_PREVIEWS:
                    del _PRESET_PREVIEWS[token]
    _CATALOG = catalog
    _matching_presets.cache_clear()
    _PREVIEW_FILES = images
    order = [key for key in catalog if key!='FAVORITES']
    _CATEGORY_ITEMS = [('FAVORITES','Favorites','Your favorite presets','SOLO_ON',10000)] + [(key, catalog[key][0], 'Browse ' + catalog[key][0] + ' presets', i)
                       for i, key in enumerate(order)]
    return True


def _check_previews():

    if time.monotonic() - _last_catalog_use >= _CATALOG_CHECK_INTERVAL:
        return None
    if _index_catalog():
        for window in bpy.context.window_manager.windows:
            for area in window.screen.areas:
                if area.type == 'VIEW_3D':
                    area.tag_redraw()
    return _CATALOG_CHECK_INTERVAL


def builtin_catalog():
    global _last_catalog_use
    _last_catalog_use = time.monotonic()
    if _CATALOG is None:
        _index_catalog()
    if not bpy.app.timers.is_registered(_check_previews):
        bpy.app.timers.register(_check_previews, first_interval=_CATALOG_CHECK_INTERVAL)
    return _CATALOG


def builtin_category_items(self, context):
    builtin_catalog()
    return _CATEGORY_ITEMS


def builtin_preview_path(category, key):
    file = _PREVIEW_FILES.get(category, {}).get(key)
    return file[0] if file else None


@lru_cache(maxsize=32)
def _matching_presets(category, terms):
    if not terms:
        return tuple((category, key) for key in _CATALOG.get(category, ('', None, ()))[2])
    found = {}
    for group, (title, _, keys) in _CATALOG.items():
        if category=='FAVORITES' and group!='FAVORITES':continue
        if category!='FAVORITES' and group=='FAVORITES':continue
        for key in keys:
            if key in found:
                continue
            description = presets_library.LIBRARY.get(key, {}).get('description', '')
            haystack = f"{key.replace('_', ' ')} {title} {description}".casefold()
            if all(term in haystack for term in terms):
                found[key] = group
    return tuple((found[key], key) for key in _BUILTIN_KEYS if key in found)


def builtin_matches(category, query=''):
    catalog = builtin_catalog()
    if category not in catalog:
        category = next((key for key in catalog if key!='FAVORITES'), 'FAVORITES')
    return _matching_presets(category, tuple(query.casefold().replace('_', ' ').split()))


def _preview_item(category, key):
    global _PRESET_PREVIEWS
    cache_key = category, key
    if cache_key in _ITEM_CACHE:
        return _ITEM_CACHE[cache_key]
    icon = 0
    path = builtin_preview_path(category, key)
    if path is not None:
        from bpy.utils import previews
        if _PRESET_PREVIEWS is None:
            _PRESET_PREVIEWS = previews.new()
        token = category + '/' + key
        try:
            if token not in _PRESET_PREVIEWS:
                _PRESET_PREVIEWS.load(token, str(path), 'IMAGE')
            icon = _PRESET_PREVIEWS[token].icon_id
        except (OSError, RuntimeError):
            pass
    entry = presets_library.LIBRARY.get(key)
    description = entry['description'] if entry else 'Apply ' + key.title() + ' damage'
    item = (key, presets_library.preset_title(key), description, icon, _BUILTIN_NUMBERS[key])
    _ITEM_CACHE[cache_key] = item
    return item


def _current_matches(wm):
    return builtin_matches(wm.project_damage_builtin_category, wm.project_damage_builtin_search)


def builtin_preview_items(self, context):
    return [_preview_item(category, key) for category, key in _current_matches(self)] or [_EMPTY_PRESET]


def builtin_selected(wm):
    selection = wm.get('_pd_builtin_preset_number', 0)
    matches = _current_matches(wm)
    return next(((category, key) for category, key in matches if _BUILTIN_NUMBERS[key] == selection),
                matches[0] if matches else None)


def builtin_selected_item(wm):
    item = builtin_selected(wm)
    return _preview_item(*item) if item else None


def _builtin_selection_get(wm):
    selected = builtin_selected(wm)
    return _BUILTIN_NUMBERS[selected[1]] if selected else _EMPTY_PRESET[4]


def _builtin_selection_set(wm, number):
    wm['_pd_builtin_preset_number'] = number


def select_builtin(wm, key):
    _builtin_selection_set(wm, _BUILTIN_NUMBERS[key])


# подключаем свойства и действия этого модуля к Blender
def register():
    for cls in _CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.WindowManager.project_damage_user_preset=bpy.props.StringProperty(default='',options={'SKIP_SAVE'})
    bpy.types.WindowManager.project_damage_preset_library=bpy.props.EnumProperty(
        name='Library',items=(('BUILTIN','Built-in Presets','Bundled looks'),('USER','User_lib','Save and load your own presets')),
        default='BUILTIN')
    bpy.types.WindowManager.project_damage_builtin_category=bpy.props.EnumProperty(
        name='Category',items=builtin_category_items,default=0,options={'SKIP_SAVE'})
    bpy.types.WindowManager.project_damage_builtin_search=bpy.props.StringProperty(
        name='Search Presets', description='Search built-in presets across all categories',
        options={'SKIP_SAVE', 'TEXTEDIT_UPDATE'})
    bpy.types.WindowManager.project_damage_builtin_preset=bpy.props.EnumProperty(
        name='Built-in Preset',items=builtin_preview_items,
        get=_builtin_selection_get,set=_builtin_selection_set)


# убираем подключения модуля при выключении аддона
def unregister():
    global _PRESET_PREVIEWS, _CATALOG
    if bpy.app.timers.is_registered(_check_previews):
        bpy.app.timers.unregister(_check_previews)
    if _PRESET_PREVIEWS is not None:
        from bpy.utils import previews
        previews.remove(_PRESET_PREVIEWS);_PRESET_PREVIEWS=None
    _CATALOG = None
    _PRESET_ITEMS.clear()
    _CATEGORY_ITEMS.clear()
    _ITEM_CACHE.clear()
    _matching_presets.cache_clear()
    _PREVIEW_FILES.clear()
    for name in ('project_damage_preset_library','project_damage_builtin_preset','project_damage_user_preset','project_damage_builtin_category','project_damage_builtin_search'):
        if hasattr(bpy.types.WindowManager,name):delattr(bpy.types.WindowManager,name)
    for cls in reversed(_CLASSES):
        bpy.utils.unregister_class(cls)

PRESET_PROPS += ['fract_surface']

from .removed_controls import REMOVED_PRESET_VALUES
PRESET_PROPS = [name for name in PRESET_PROPS if name not in REMOVED_PRESET_VALUES]
