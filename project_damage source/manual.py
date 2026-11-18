# открываем документацию, уроки и ссылки из интерфейса
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
"""The ProjectDamage manual: local server, deep-link routes, F1 hook.

Serves blender/manual-site/, a SEPARATE build from the C4D plugin's
cpp/manual/: every manual/src/content page carries an optional `host`
frontmatter field, and manual/vite.config.ts filters the page manifest,
search index and routes.json by it at build time, so the Blender user never
sees a C4D page (and vice versa) - `pnpm run build:blender` in repo manual/
writes blender/manual-site/, `pnpm build` writes cpp/manual/.

Mirrors cpp/project_damage_manual_host.py's server pattern almost exactly: a daemon
ThreadingHTTPServer serves the prebuilt site on 127.0.0.1, started lazily
on first open. PREFERRED_PORT 8795 - one above C4D's 8794, so both hosts
can run side by side on a dev machine, with the same ephemeral-port
fallback the C4D host uses if the preferred port is taken.

Blender has no in-app HTML view (C4D's dialog wraps CUSTOMGUI_HTMLVIEWER,
QtWebEngine - no Blender counterpart), so every open here goes to the
system browser, which is Blender's own convention too (F1's "Online
Manual" does the same). bpy.app.background gates that: under --background
there is no browser to sensibly pop, so open_manual() prints the URL and
returns it instead - the same IS_HEADLESS branch the C4D host takes in
c4dpy, and what keeps this module's own headless test from launching a
real browser window.

Site resolution mirrors core_loader.py's rung idiom, with one deliberate
difference: core_loader has a FOURTH rung (project_damage_core_path.txt, baked in by
make_dev_zip.ps1/dev_install.ps1) because the compiled core's build output
lives outside the repo tree at a path only those scripts know. The manual
site has no such external location - blender/manual-site/ is always at a
fixed place relative to this file - so getting the repo-relative rung right
through the dev_install.ps1 junction is what matters here, and that takes
os.path.realpath(__file__) as the very first step (resolved BEFORE any
".." traversal) rather than core_loader's plain abspath: a junctioned
`<addons>/project_damage/manual.py` resolves back to the real repo checkout first,
so "../manual-site" from there lands on the real blender/manual-site/, not
on whatever sits one level above the Blender addons directory. The rungs:

  1. the addon preference (Edit > Preferences > Add-ons > ProjectDamage > Manual
     Directory), when set
  2. $PROJECT_DAMAGE_MANUAL_DIR
  3. the repo's own blender/manual-site, relative to this file
     (realpath-resolved, so it also works through the dev_install.ps1
     junction)
  4. the addon-local project_damage/manual-site (the release-zip layout, see
     _ADDON_LOCAL)

A missing site (fresh clone, `pnpm run build:blender` never run) is a
loud, actionable RuntimeError naming the build command - never a silently
broken button.

The Blender pages are getting-started/what-is-project_damage-blender,
quick-start-blender; objects/project_damage-blender,
breaker-blender; reference/troubleshooting-blender, faq-blender,
credits-blender - "-blender" suffixed because the C4D pages of (almost) the
same name own the un-suffixed slugs in the shared manual/src/content/ tree.
ROUTES below targets them.
"""
import http.server
import os
import threading

import bpy
import bpy.utils.previews                                            

PREFERRED_PORT = 8795

                                                                   
                                                                          
                                                               
                                                                           
                      
_HERE = os.path.dirname(os.path.realpath(__file__))
_REPO_DEFAULT = os.path.abspath(os.path.join(_HERE, "..", "manual-site"))
                                                                    
                                                                                 
                                                      
                                                                                 
                                                       
_ADDON_LOCAL = os.path.join(_HERE, "documentation")

                                                                       
                                                       
                                                                                            
                                                                                      
                                                                          
                                                                                      
                                                                                     
                                                                         
                                         
                                                                           
                                                                      
                                                                  
ROUTES = {
    "OBJECT_PT_project_damage": "objects/project_damage-blender",
    "OBJECT_PT_project_damage_global": "objects/project_damage-blender/global",
    "OBJECT_PT_project_damage_chips": "objects/project_damage-blender/chips",
    "OBJECT_PT_project_damage_surface": "objects/project_damage-blender/surface",
    "OBJECT_PT_project_damage_buildup": "objects/project_damage-blender/buildup",
    "OBJECT_PT_project_damage_fracture": "objects/project_damage-blender/fracture",
    "OBJECT_PT_project_damage_corners": "objects/project_damage-blender/corners",
    "OBJECT_PT_project_damage_breakers": "objects/project_damage-blender/breaker",
    "OBJECT_PT_project_damage_breaker": "objects/breaker-blender/object",
}

                                                                                   
                                                                            
                                                                              
                                                                        
                                                                            
                                             
_GLOBAL_FIELDS = (
    "all_off", "global_scale", "global_detail", "draft_enable", "draft_quality", "seed", "topology",
    "keep_uvs", "keep_selections", "mask_group", "mask_amount", "mask_size",
    "edge_threshold", "axis_x", "axis_y", "axis_z", "avoid_attr",
    "avoid_chips", "avoid_surf", "avoid_build", "avoid_fract", "avoid_corner",
)
_CHIPS_FIELDS = (
    "chips_enable", "chip_scale", "detail", "amount", "size_min", "size_max",
    "depth_min", "depth_max", "stretch", "rim_soft", "normalize",
    "norm_max_size", "offset", "random_offset", "noise_strength",
    "noise_scale", "noise_roughness", "noise_distortion", "sec_amount", "sec_size", "sec_depth",
)

                                                                           
                                                                       
                                                                
                                                                      
                                                                          
                                                                                
                                                                         
                                                                           
_MANUAL_MAP = (
    (r"^bpy\.types\.ProjectDamageProps\.(%s)$" % "|".join(_GLOBAL_FIELDS), ROUTES["OBJECT_PT_project_damage_global"]),
    (r"^bpy\.types\.ProjectDamageProps\.(%s)$" % "|".join(_CHIPS_FIELDS), ROUTES["OBJECT_PT_project_damage_chips"]),
    (r"^bpy\.types\.ProjectDamageProps\.surf_", ROUTES["OBJECT_PT_project_damage_surface"]),
    (r"^bpy\.types\.ProjectDamageProps\.build_", ROUTES["OBJECT_PT_project_damage_buildup"]),
    (r"^bpy\.types\.ProjectDamageProps\.fract_", ROUTES["OBJECT_PT_project_damage_fracture"]),
    (r"^bpy\.types\.ProjectDamageProps\.corner_", ROUTES["OBJECT_PT_project_damage_corners"]),
    (r"^bpy\.types\.ProjectDamageProps\.break", ROUTES["OBJECT_PT_project_damage_breakers"]),
    (r"^bpy\.types\.ProjectDamageBreakerProps\.", ROUTES["OBJECT_PT_project_damage_breaker"]),
    (r"^bpy\.types\.ProjectDamageProps\.", ROUTES["OBJECT_PT_project_damage"]),
)

_server = None
_port = None
                                                                        
                                                                       
_server_lock = threading.Lock()
_previews = None


class _NoReuseThreadingHTTPServer(http.server.ThreadingHTTPServer):
                                                                                    
                                                                      
                                                                
    allow_reuse_address = False


def _candidates():
    """Yield (label, path) for every configured source, in resolution
    order - core_loader.py's own _candidates() idiom. A source that is not
    actually SET (no preference, no env var) is skipped rather than
    yielding a path nobody asked for."""
    try:
        prefs = bpy.context.preferences.addons[__package__].preferences
        if prefs.manual_dir:
            yield "the addon preference", bpy.path.abspath(prefs.manual_dir)
    except (KeyError, AttributeError):
                                                                       
                                                                         
        pass
    env = os.environ.get("PROJECT_DAMAGE_MANUAL_DIR")
    if env:
        yield "$PROJECT_DAMAGE_MANUAL_DIR", os.path.abspath(env)
    yield "the repo's blender/manual-site (relative to this file)", _REPO_DEFAULT
    yield "the addon-local manual-site (release zip layout)", _ADDON_LOCAL


def site_dir():
    """The first candidate directory that actually holds a built site (an
    index.html), else the first candidate at all - mirrors core_loader.py's
    core_dir(), so "always returns some path" holds for the preferences
    panel's Resolved: label and for start_server()'s error message."""
    first = None
    for _, path in _candidates():
        if first is None:
            first = path
        if os.path.isfile(os.path.join(path, "index.html")):
            return path
    return first


def start_server():
    """Start (or reuse) the static server; returns the bound port. Raises
    RuntimeError, naming `pnpm build`, if no candidate holds a built site."""
    global _server, _port
    with _server_lock:
        if _server is not None:
            return _port
        site = site_dir()
        if not os.path.isfile(os.path.join(site, "index.html")):
            raise RuntimeError(
                "ProjectDamage manual not found at %s - build it with `pnpm run "
                "build:blender` in the repo's manual/ folder (writes "
                "blender/manual-site/), or point Preferences > Add-ons > "
                "ProjectDamage > Manual Directory at an already-built site." % site)

        class _Handler(http.server.SimpleHTTPRequestHandler):
            def __init__(self, *args, **kwargs):
                super().__init__(*args, directory=site, **kwargs)

            def log_message(self, *args):
                pass

        try:
            server = _NoReuseThreadingHTTPServer(("127.0.0.1", PREFERRED_PORT), _Handler)
        except OSError:
                                                                        
                                                                           
                                   
            server = _NoReuseThreadingHTTPServer(("127.0.0.1", 0), _Handler)
        _server = server
        _port = server.server_address[1]
        threading.Thread(target=server.serve_forever, daemon=True).start()
        return _port


def _shutdown_server():
    """Stops the daemon thread and closes the socket - the junction-reload
    trap this guards against: a script-reload without this would leak a
    listening socket (and, on a second register, silently try to bind the
    port again from a second thread)."""
    global _server, _port
    with _server_lock:
        if _server is not None:
            _server.shutdown()
            _server.server_close()
            _server = None
            _port = None


def url_for(route=""):
    port = start_server()
    url = "http://127.0.0.1:%d/" % port
    return url + ("#/" + route) if route else url


def open_manual(route=""):
    """Open the manual in the system browser at the given hash route (or
    the landing page for route=""). Under bpy.app.background there is no
    browser to sensibly pop - prints the URL and returns it instead, the
    same branch the C4D host takes in headless c4dpy."""
    url = url_for(route)
    if bpy.app.background:
        print("[ProjectDamage] Manual (headless, no browser): %s" % url)
        return url
    import webbrowser
    webbrowser.open(url)
    return url


_icon_sources = {}                                                                     


def _load_icons():
    global _previews
    if _previews is None:
        _previews = bpy.utils.previews.new()
                                                                                       
                                                                            
                                                                           
                          
        local = os.path.join(_HERE, "icons")
        repo = os.path.abspath(os.path.join(_HERE, "..", "..", "cpp", "res"))
        for base in (local, repo):
            path = os.path.join(base, "icon2.png")
            if os.path.isfile(path):
                _previews.load("icon2", path, 'IMAGE')
                _icon_sources["icon2"] = path
                break
    return _previews


def icon_value():
    """The manual-button icon id for layout.operator(icon_value=...), or 0
    to signal "use the builtin HELP icon instead" - a stripped checkout
    with no cpp/res PNGs must never crash a panel draw. icon2 (the LIGHT
    question mark, cpp/project_damage_manual_host.py's own icon2/icon3 split) is the
    one used: it reads better against Blender's dark panel background than
    icon3, which C4D uses on its own light-background AM buttons."""
    icons = _load_icons()
    icon2 = icons.get("icon2")
    return icon2.icon_id if icon2 is not None else 0


def _manual_map():
    """bpy.utils.register_manual_map callback: Blender calls this fresh on
    every F1 press, so starting the server here (rather than at register()
    time) guarantees the returned prefix carries the port actually bound.
    A missing site degrades to an empty mapping (no match, F1 falls
    through to whatever else is registered) rather than raising into
    Blender's own dispatch code."""
    try:
        port = start_server()
    except RuntimeError:
        return "", ()
    return "http://127.0.0.1:%d/#/" % port, _MANUAL_MAP


# действие «Open ProjectDamage Manual»
class PROJECT_DAMAGE_OT_open_manual(bpy.types.Operator):
    bl_idname = "project_damage.open_manual"
    bl_label = "Open ProjectDamage Manual"
    bl_description = "Open the ProjectDamage manual in your browser, at this page"
    bl_options = {'INTERNAL'}

    route: bpy.props.StringProperty(default="")

    def execute(self, context):
        try:
            url = open_manual(self.route)
        except RuntimeError as exc:
            self.report({'ERROR'}, str(exc))
            return {'CANCELLED'}
        self.report({'INFO'}, "Opened %s" % url)
        return {'FINISHED'}


_CLASSES = (PROJECT_DAMAGE_OT_open_manual,)


# подключаем свойства и действия этого модуля к Blender
def register():
    for cls in _CLASSES:
        bpy.utils.register_class(cls)
    bpy.utils.register_manual_map(_manual_map)


# убираем подключения модуля при выключении аддона
def unregister():
    global _previews
    bpy.utils.unregister_manual_map(_manual_map)
    for cls in reversed(_CLASSES):
        bpy.utils.unregister_class(cls)
    _shutdown_server()
    if _previews is not None:
        bpy.utils.previews.remove(_previews)
        _previews = None
