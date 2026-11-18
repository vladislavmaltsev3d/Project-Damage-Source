# находим подходящий модуль расчёта и показываем настройки аддона
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
"""Where the compiled core comes from.

Rungs 1-4 are the dev loop; rung 5 is what a customer gets. Since the core
became GPL-3 like the addon itself (see __init__.py), it SHIPS INSIDE the
addon zip and there is nothing left to install by hand: one download, one
"Install from Disk", done.

Resolution order, first hit wins:
  1. the addon preference (Edit > Preferences > Add-ons > ProjectDamage), when set
  2. $PROJECT_DAMAGE_CORE_PY_DIR (what blender/tests/*.py drive it with)
  3. the repo's own cp3XX build dir, relative to this file (a repo-checkout
     registration)
  4. project_damage_core_path.txt next to this file, if make_dev_zip.ps1/dev_install.ps1
     wrote one - the fallback an addon INSTALLED FROM A ZIP needs, because then
     rung 3 resolves relative to the AppData scripts dir, not the repo. It
     holds ONE DIRECTORY PER LINE, one per built ABI, since the same zip may
     be opened by a cp311 and a cp313 Blender
  5. core/<platform>-<abi> next to this file - the bundled core, which is the
     only rung a customer ever uses

The dev rungs deliberately win over the bundled one: a checkout that has just
rebuilt the core must see ITS build, not whatever a zip installed months ago.

Rung 5 is one directory PER PLATFORM AND ABI, never one shared directory for
all four builds. _abi_modules matches on the ABI tag alone, and the macOS
`project_damage_core_py.cpython-313-darwin.so` carries the same cp313 identity as the
Windows `.pyd` - so a shared directory would report a wrong-platform build as
a match and make every error message lie, even though the import itself would
still pick correctly.

A rung holding a module built for ANOTHER Python ABI is skipped rather than
tried, so a later rung can still win - see _holds_other_abi below.
"""
import glob
import os
import sys

import bpy

MODULE_NAME = "project_damage_core_py"
# Loaded as a submodule of this add-on (never a top-level module, never via
# sys.path): Blender's extension policy forbids both.
QUALIFIED_NAME = __package__ + "." + MODULE_NAME


def loaded():
    """The native core if it is already loaded, else None."""
    return sys.modules.get(QUALIFIED_NAME)

_HERE = os.path.dirname(os.path.abspath(__file__))




_ABI_TAG = "cp%d%d" % sys.version_info[:2]




_REPO_DEFAULT = os.path.abspath(os.path.join(
    _HERE, "..", "..", "cpp", "build", "core_standalone_%s" % _ABI_TAG, "Release"))




_DEV_CONFIG = os.path.join(_HERE, "project_damage_core_path.txt")


_PLAT_TAG = "macos-arm64" if sys.platform == "darwin" else "win64"






_BUNDLED = os.path.join(_HERE, "core", "%s-%s" % (_PLAT_TAG, _ABI_TAG))


def addon_version():
    """Read the version through Blender for both add-ons and extensions."""
    import addon_utils

    version = addon_utils.module_bl_info(sys.modules[__package__])["version"]
    return ".".join(map(str, version))


def core_loaded_version():
    mod = loaded()
    return getattr(mod, "__version__", None) if mod is not None else None


def version_mismatch():
    """None, or (addon_version, core_version) when BOTH are real release
    versions and differ. Dev builds ("0.0.0-dev") never warn - dev flows
    stay silent."""
    core_v = core_loaded_version()
    addon_v = addon_version()
    if core_v is None or core_v == "0.0.0-dev":
        return None
    expected = getattr(sys.modules[__package__], 'CORE_VERSION', addon_v)
    if core_v == expected:
        return None
    return (addon_v, core_v)


def _redraw_interface(self, context):
    for screen in getattr(bpy.data, 'screens', ()):
        for area in screen.areas:
            area.tag_redraw()


def _fracture_proxy_changed(self, context):
    """Rebuild affected sources only; respect Manual Update and restricted startup."""
    from . import live, background_compute, surface_guidance, ivy, roots
    surface_guidance.clear()
    for obj in getattr(bpy.data, 'objects', ()):
        p = getattr(obj, 'project_damage', None)
        if p is not None and obj.type == 'MESH' and obj.get('project_damage_uid') and not obj.get('project_damage_bfrag_stage'):
            background_compute.cancel(obj)
            live._mark_dirty(obj)
            ivy.queue_update(obj)
            roots.queue_update(obj)
    _redraw_interface(self, context)


EFFECT_VIEW_ITEMS = (
    ('COMPACT', 'Compact', 'Choose an effect in the thumbnail viewer, then apply it', 0),
    ('FULL', 'Full Preview', 'Show the full grid of effect previews', 1),
)


def _default_view_changed(self, context):
    for wm in getattr(bpy.data, 'window_managers', ()):
        if 'project_damage_add_view' in wm:
            del wm['project_damage_add_view']
    _redraw_interface(self, context)


# общие настройки аддона, не относящиеся к отдельному объекту
class ProjectDamagePreferences(bpy.types.AddonPreferences):
    bl_idname = __package__

    default_view_mode: bpy.props.EnumProperty(
        name='Default View Mode', items=EFFECT_VIEW_ITEMS, default='FULL',
        update=_default_view_changed,
    )

    preset_ui: bpy.props.EnumProperty(
        name="Preset Interface",
        items=(('LEGACY','Legacy UI','Standard Blender thumbnail selector'),
               ('ADVANCED','Advanced UI','GPU preset browser with search, scrolling and Shift zoom')),
        default='LEGACY', update=_redraw_interface,
    )

    editorial_mode: bpy.props.BoolProperty(
        name="Editorial Mode", default=True,
        description="Show offline documentation links in Geometry asset section headings",
        update=_redraw_interface,
    )

    enable_hints: bpy.props.BoolProperty(
        name="Enable Hints", default=True,
        description="Show help buttons with rendered visual examples beside complex settings",
        update=_redraw_interface,
    )

    cache_intermediates: bpy.props.BoolProperty(
        name='Cache Intermediate Geometry', default=True,
        description='Keep exact session geometry and preparation results for reuse. Reduces repeated work at the cost of RAM. Off discards intermediate caches; output quality is unchanged',
        update=lambda self,context: __import__(__package__+'.incremental',fromlist=['settings_changed']).settings_changed(self,context),
    )
    incremental_updates: bpy.props.BoolProperty(
        name='Recalculate Changed Generators Only', default=True,
        description='Recompute the edited generator and reuse unchanged damage regions. Source geometry, shared scale or dependency edits invalidate affected generators. The final combined Boolean is still rebuilt. Requires Cache Intermediate Geometry',
        update=lambda self,context: __import__(__package__+'.incremental',fromlist=['settings_changed']).settings_changed(self,context),
    )

    save_all_mesh_states: bpy.props.BoolProperty(
        name='Save All Mesh States', default=False,
        description='Off: Apply updates the existing objects and keeps the initial mesh without hidden intermediate duplicates. On: Apply Selected also keeps hidden copies of each previous setup. Existing saved backups are not deleted',
    )

    low_details_mode: bpy.props.BoolProperty(
        name='Low Details Mode', default=False, update=_redraw_interface,
        description='New effects start at 20% geometry quality. Existing effects and edited values are retained. Cell-size controls use larger cells; safe minimums still apply',
    )
    background_cpu_budget: bpy.props.IntProperty(
        name='Background CPU Budget', default=75, min=10, max=100, subtype='PERCENTAGE',
        description='Approximate active-time budget of the single background worker, not a percentage of all CPU cores. Lower values leave more resources for Blender and take longer; output quality is unchanged',
    )

    experimental_material_ui: bpy.props.BoolProperty(
        name="Materials in Geometry", default=True,
        description="Place material and library selectors beside their geometry features; Material becomes Damage UV",
        update=_redraw_interface,
    )

    fracture_surface_proxy: bpy.props.BoolProperty(
        name="Surface Guide for All Generators (Experimental)", default=False,
        description=("Off: keep the original algorithms. On: use a temporary smoothed surface "
                     "to guide fracture paths and surface-based generator orientation on bevels and relief. "
                     "Cuts, attachment positions and thickness tests use the detailed source. "
                     "Explicit curves, axes and volume-only operations keep their own placement. "
                     "Does not remesh the target. Manual Update still requires Update"),
        update=_fracture_proxy_changed,
    )

    material_library_dir:bpy.props.StringProperty(name='Material Library Folder',subtype='DIR_PATH',default='',description='Folder of the extracted Project Damage CC0 material pack')
    material_library_enabled:bpy.props.BoolProperty(name='Material Library Enabled',default=True)
    material_library_download_url:bpy.props.StringProperty(name='Material Pack Download URL',default='',description='Optional link to the separate Project Damage material pack, shown when the library is missing')

    core_dir: bpy.props.StringProperty(
        name="Core Directory",
        description=("Directory holding the compiled project_damage_core_py module. "
                     "Empty = $PROJECT_DAMAGE_CORE_PY_DIR, else the repo build directory"),
        subtype='DIR_PATH',
        default="",
    )



    manual_dir: bpy.props.StringProperty(
        name="Manual Directory",
        description=("Directory holding the built manual site (index.html). "
                     "Empty = $PROJECT_DAMAGE_MANUAL_DIR, else the addon's own manual-site folder "
                     "(blender/manual-site in a checkout)"),
        subtype='DIR_PATH',
        default="",
    )

    def draw(self, context):
        layout = self.layout
        layout.prop(self, 'default_view_mode')
        layout.prop(self, "editorial_mode", icon='HELP')
        layout.prop(self, "enable_hints", icon='QUESTION')
        performance=layout.box()
        performance.label(text='Performance', icon='PREFERENCES')
        performance.prop(self,'save_all_mesh_states')
        performance.prop(self,'low_details_mode')
        performance.label(text='Low Details defaults affect new effects only.',icon='INFO')
        performance.prop(self,'background_cpu_budget')
        performance.prop(self,'cache_intermediates')
        row=performance.row();row.enabled=self.cache_intermediates
        row.prop(self,'incremental_updates')



        box = layout.box()
        box.label(text="Experimental Geometry", icon='EXPERIMENTAL')
        box.prop(self, "fracture_surface_proxy")
        loaded_core = loaded()
        if loaded_core is not None and not hasattr(loaded_core, 'surface_guide_field'):
            box.label(text="Restart Blender to load the updated surface guide core", icon='INFO')
        from . import material_library
        material_library.draw_preferences(layout,self)
        from . import preset_pack
        preset_pack.draw_preferences(layout)
        from . import package_install
        package_install.draw_preferences(layout)





_ABI_PREFIXES = (
    "%s.%s-" % (MODULE_NAME, _ABI_TAG),
    "%s.cpython-%d%d-" % (MODULE_NAME, sys.version_info[0], sys.version_info[1]),
)


def _module_files(path):
    """Every project_damage_core_py module file in `path`, whatever its ABI."""
    return (glob.glob(os.path.join(path, "%s*.pyd" % MODULE_NAME))
            + glob.glob(os.path.join(path, "%s*.so" % MODULE_NAME)))


def _abi_modules(path):
    """The module files in `path` built for THIS Python's ABI (empty if the
    directory holds none - or does not exist yet, as the installed rung does
    before the first Install Core)."""
    return [p for p in _module_files(path)
            if any(os.path.basename(p).startswith(pref) for pref in _ABI_PREFIXES)]


def _holds_other_abi(path):
    """True only when the directory actually HOLDS a module built for another
    Python ABI. A directory with no module in it passes: it may simply not
    exist yet, and load()'s error message must still name it in its
    tried-list. What this catches is a rung pointing at a real build of the
    WRONG interpreter - which rung 4 does by construction, since
    make_dev_zip.ps1/dev_install.ps1 bake ONE core build dir per ABI into the
    zip while the same zip may be opened by two Blender versions bundling
    different Pythons (4.2 - 5.0 is cp311, 5.1+ is cp313)."""
    return bool(_module_files(path)) and not _abi_modules(path)


# пропускаем сборки, которые не подходят этой версии Python
def _candidates():
    """Yield (label, path) for every configured source, in resolution order,
    skipping any that holds a wrong-ABI build."""
    for label, path in _all_candidates():
        if not _holds_other_abi(path):
            yield label, path


def _all_candidates():
    """Yield (label, path) for every configured source, in resolution order.
    Sources that are not actually SET (env unset, no dev-config file) are
    skipped rather than yielding a path nobody asked for."""
    try:
        prefs = bpy.context.preferences.addons[__package__].preferences
        if prefs.core_dir:
            yield "the addon preference", bpy.path.abspath(prefs.core_dir)
    except (KeyError, AttributeError):


        pass
    env = os.environ.get("PROJECT_DAMAGE_CORE_PY_DIR")
    if env:
        yield "$PROJECT_DAMAGE_CORE_PY_DIR", os.path.abspath(env)
    yield "the repo build dir (relative to this file)", _REPO_DEFAULT
    if os.path.isfile(_DEV_CONFIG):


        with open(_DEV_CONFIG, "r", encoding="utf-8-sig") as f:
            configured = f.read()


        for line in configured.splitlines():
            line = line.strip()
            if line:
                yield "project_damage_core_path.txt", os.path.abspath(line)
    yield "the core bundled with this addon", _BUNDLED


def core_dir():
    """The first candidate directory that exists on disk, else the first
    candidate at all (keeps "always returns some path" for the preferences
    panel's Resolved: label and for load()'s error message)."""
    first = None
    for _, path in _candidates():
        if first is None:
            first = path
        if os.path.isdir(path):
            return path
    return first


# загружаем ядро один раз и дальше используем уже загруженное
def load():
    """Import the core, or raise RuntimeError with something actionable.

    Cached by sys.modules: a second call after a successful one is a lookup.
    """
    mod = sys.modules.get(QUALIFIED_NAME)
    if mod is not None:
        return mod
    path = core_dir()
    try:
        import importlib.machinery, importlib.util
        suffixes = tuple(importlib.machinery.EXTENSION_SUFFIXES)
        files = sorted(f for f in _abi_modules(path) if f.endswith(suffixes))
        if not files:
            raise ImportError("no %s module for this Python in %s" % (MODULE_NAME, path))
        spec = importlib.util.spec_from_file_location(QUALIFIED_NAME, files[0])
        if spec is None or spec.loader is None:
            raise ImportError("cannot create an import spec for " + files[0])
        mod = importlib.util.module_from_spec(spec)
        sys.modules[QUALIFIED_NAME] = mod
        try:
            spec.loader.exec_module(mod)
        except BaseException:
            sys.modules.pop(QUALIFIED_NAME, None)
            raise
        return mod
    except ImportError as exc:
        tried = "; ".join("%s: %s" % (label, p) for label, p in _candidates())




        raise RuntimeError(
            "ProjectDamage: the geometry core is missing from this addon. Make sure you "
            "installed the zip for THIS system (ProjectDamage-Blender-<version>-%s.zip) "
            "and installed it in Blender rather than unpacking it by hand. "
            "(Details: cannot load %s from %s: %s. Tried, in order: %s. "
            "Blender's Python is %d.%d, so the module must carry the matching "
            "cp%d%d tag.)"
            % ("mac" if sys.platform == "darwin" else "win",
               MODULE_NAME, path, exc, tried, sys.version_info[0],
               sys.version_info[1], sys.version_info[0], sys.version_info[1])
        ) from exc


# подключаем свойства и действия этого модуля к Blender
def register():
    bpy.utils.register_class(ProjectDamagePreferences)


# убираем подключения модуля при выключении аддона
def unregister():
    bpy.utils.unregister_class(ProjectDamagePreferences)
