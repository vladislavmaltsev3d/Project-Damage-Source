# SPDX-License-Identifier: GPL-3.0-or-later
"""Install Project Damage Package: updates and fixes delivered as a ZIP.

The ZIP is unpacked into a temporary folder. If it contains
``project_damage_initial_script.py`` (at the top level or in one folder
below), that script is run with an ``api`` object that knows where the add-on
lives and offers safe file operations. Without a script, nothing is changed.

Files that Blender keeps open (the compiled core ``.pyd`` modules on Windows)
cannot be replaced while the add-on is loaded. ``api.install_file`` and
``api.replace_tree`` detect this and stage the file in ``_pending_update``;
it is moved into place the next time Blender starts, before the core loads.

This module must not import anything from the add-on at import time:
``apply_pending()`` runs from ``__init__`` before the rest of the add-on.
"""
import os, shutil, tempfile, zipfile, json, time, traceback

SCRIPT_NAME = 'project_damage_initial_script.py'
ADDON_DIR = os.path.dirname(os.path.abspath(__file__))
PENDING_DIR = os.path.join(ADDON_DIR, '_pending_update')
PENDING_LIST = os.path.join(PENDING_DIR, 'pending.json')
LOG_FILE = os.path.join(ADDON_DIR, 'package_install.log')


def _log(text):
    try:
        with open(LOG_FILE, 'a', encoding='utf-8') as f:
            f.write(time.strftime('%Y-%m-%d %H:%M:%S ') + text + '\n')
    except OSError:
        pass


def _inside_addon(path):
    path = os.path.abspath(path)
    return os.path.commonpath([path, ADDON_DIR]) == ADDON_DIR


# ---------------------------------------------------------------- pending replacements
def _read_pending():
    try:
        with open(PENDING_LIST, encoding='utf-8') as f:
            return json.load(f)
    except (OSError, ValueError):
        return []


def _stage(src, dst):
    os.makedirs(PENDING_DIR, exist_ok=True)
    rows = _read_pending()
    staged = os.path.join(PENDING_DIR, 'f%04d_%s' % (len(rows), os.path.basename(dst)))
    shutil.copy2(src, staged)
    rows.append({'staged': os.path.relpath(staged, ADDON_DIR), 'target': os.path.relpath(dst, ADDON_DIR)})
    with open(PENDING_LIST, 'w', encoding='utf-8') as f:
        json.dump(rows, f, indent=1)


def apply_pending():
    """Move staged files into place. Called at add-on import, before the core loads."""
    rows = _read_pending()
    if not rows:
        return 0
    done = 0; left = []
    for row in rows:
        staged = os.path.join(ADDON_DIR, row['staged']); target = os.path.join(ADDON_DIR, row['target'])
        if not (_inside_addon(staged) and _inside_addon(target)) or not os.path.exists(staged):
            continue
        try:
            os.makedirs(os.path.dirname(target), exist_ok=True)
            os.replace(staged, target)
            done += 1
        except OSError as exc:
            left.append(row); _log('pending still locked: %s (%s)' % (row['target'], exc))
    if left:
        with open(PENDING_LIST, 'w', encoding='utf-8') as f:
            json.dump(left, f, indent=1)
    else:
        shutil.rmtree(PENDING_DIR, ignore_errors=True)
    _log('applied %d pending file(s)' % done)
    return done


def pending_count():
    return len(_read_pending())


# ---------------------------------------------------------------- what the package script can use
class PackageAPI:
    """Passed to the package script as ``api``.

    api.addon_dir      folder of the installed add-on
    api.package_dir    temporary folder with the unpacked ZIP (deleted afterwards)
    api.install_file(rel_src, rel_dst=None)   copy one file from the package into the add-on
    api.replace_tree(rel_src, rel_dst=None)   copy a whole folder, overwriting files
    api.remove(rel_path)                      delete a file or folder inside the add-on
    api.backup(rel_path)                      keep a copy in _backups/<time>/ before changing it
    api.log(text) / api.report(text)          message in the log and in Blender's status bar
    api.restart_required = True               ask the user to restart Blender
    """

    def __init__(self, package_dir, reporter):
        self.addon_dir = ADDON_DIR
        self.package_dir = package_dir
        self.restart_required = False
        self.messages = []
        self._report = reporter
        self._backup_root = os.path.join(ADDON_DIR, '_backups', time.strftime('%Y%m%d_%H%M%S'))

    def _src(self, rel):
        p = os.path.abspath(os.path.join(self.package_dir, rel))
        if os.path.commonpath([p, os.path.abspath(self.package_dir)]) != os.path.abspath(self.package_dir):
            raise ValueError('Package path leaves the package: ' + rel)
        return p

    def _dst(self, rel):
        p = os.path.abspath(os.path.join(ADDON_DIR, rel))
        if not _inside_addon(p):
            raise ValueError('Target outside the add-on folder: ' + rel)
        return p

    def log(self, text):
        self.messages.append(str(text)); _log('script: ' + str(text))

    def report(self, text):
        self.log(text)
        if self._report:
            self._report(str(text))

    def backup(self, rel):
        src = self._dst(rel)
        if not os.path.exists(src):
            return
        dst = os.path.join(self._backup_root, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if os.path.isdir(src):
            shutil.copytree(src, dst, dirs_exist_ok=True)
        else:
            shutil.copy2(src, dst)

    def install_file(self, rel_src, rel_dst=None):
        src = self._src(rel_src); dst = self._dst(rel_dst or rel_src)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        try:
            shutil.copy2(src, dst)
        except PermissionError:
            # loaded native module (.pyd): replace on the next start
            _stage(src, dst); self.restart_required = True
            self.log('staged for next start: ' + os.path.relpath(dst, ADDON_DIR))
            return 'staged'
        if dst.endswith(('.py', '.pyd', '.so', '.dylib')):
            self.restart_required = True
        self.log('installed ' + os.path.relpath(dst, ADDON_DIR))
        return 'installed'

    def replace_tree(self, rel_src, rel_dst=None):
        root = self._src(rel_src); base = rel_dst if rel_dst is not None else rel_src
        results = {}
        for folder, _, files in os.walk(root):
            for fn in files:
                rel = os.path.relpath(os.path.join(folder, fn), root)
                results[rel] = self.install_file(os.path.join(rel_src, rel), os.path.join(base, rel))
        return results

    def remove(self, rel):
        p = self._dst(rel)
        if p == ADDON_DIR:
            raise ValueError('Refusing to remove the add-on folder')
        if os.path.isdir(p):
            shutil.rmtree(p)
        elif os.path.exists(p):
            os.remove(p)
        self.restart_required = True
        self.log('removed ' + rel)


# ---------------------------------------------------------------- run a package
def _find_script(root):
    direct = os.path.join(root, SCRIPT_NAME)
    if os.path.isfile(direct):
        return direct
    for name in sorted(os.listdir(root)):
        p = os.path.join(root, name, SCRIPT_NAME)
        if os.path.isfile(p):
            return p
    return None


def _safe_extract(zf, target):
    target = os.path.abspath(target)
    for member in zf.infolist():
        dest = os.path.abspath(os.path.join(target, member.filename))
        if os.path.commonpath([dest, target]) != target:
            raise ValueError('Unsafe path in ZIP: ' + member.filename)
    zf.extractall(target)


def install(zip_path, reporter=None):
    """Unpack ``zip_path`` to a temp folder and run its package script.
    Returns (api, message)."""
    if not zipfile.is_zipfile(zip_path):
        raise ValueError('Not a ZIP file')
    tmp = tempfile.mkdtemp(prefix='pd_package_')
    _log('package: ' + zip_path)
    try:
        with zipfile.ZipFile(zip_path) as zf:
            _safe_extract(zf, tmp)
        script = _find_script(tmp)
        if script is None:
            raise ValueError('The ZIP has no %s; nothing was installed' % SCRIPT_NAME)
        package_dir = os.path.dirname(script)
        api = PackageAPI(package_dir, reporter)
        code = compile(open(script, encoding='utf-8').read(), script, 'exec')
        scope = {'__name__': '__project_damage_package__', '__file__': script, 'api': api,
                 'ADDON_DIR': ADDON_DIR, 'PACKAGE_DIR': package_dir}
        cwd = os.getcwd()
        try:
            os.chdir(package_dir)
            exec(code, scope)
            if callable(scope.get('run')):
                scope['run'](api)
        except Exception:
            _log(traceback.format_exc())
            raise
        finally:
            os.chdir(cwd)
        return api
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


# ---------------------------------------------------------------- Blender side
def _bpy_classes():
    import bpy
    from bpy_extras.io_utils import ImportHelper

    class PROJECT_DAMAGE_OT_install_package(bpy.types.Operator, ImportHelper):
        bl_idname = 'project_damage.install_package'
        bl_label = 'Install Project Damage Package'
        bl_description = ('Install an update or fix ZIP from Lazy3D. The ZIP is unpacked to a temporary folder and its '
                          + SCRIPT_NAME + ' is run. Only use packages from a source you trust')
        filename_ext = '.zip'
        filter_glob: bpy.props.StringProperty(default='*.zip', options={'HIDDEN'})

        def execute(self, context):
            try:
                api = install(self.filepath, reporter=lambda t: self.report({'INFO'}, t))
            except Exception as exc:
                self.report({'ERROR'}, 'Package not installed: ' + str(exc))
                return {'CANCELLED'}
            done = 'Package installed'
            if api.restart_required:
                done += '. Restart Blender to finish the update'
            self.report({'INFO'}, done)
            for window in context.window_manager.windows:
                for area in window.screen.areas:
                    area.tag_redraw()
            return {'FINISHED'}

    return (PROJECT_DAMAGE_OT_install_package,)


_CLASSES = ()


def draw_preferences(layout):
    box = layout.box()
    box.label(text='Updates and Fixes', icon='PACKAGE')
    box.operator('project_damage.install_package', text='Install Project Damage Package', icon='IMPORT')
    n = pending_count()
    if n:
        box.label(text='%d file(s) will be replaced when Blender restarts' % n, icon='INFO')


def register():
    import bpy
    global _CLASSES
    _CLASSES = _bpy_classes()
    for cls in _CLASSES:
        bpy.utils.register_class(cls)


def unregister():
    import bpy
    for cls in reversed(_CLASSES):
        try:
            bpy.utils.unregister_class(cls)
        except RuntimeError:
            pass
