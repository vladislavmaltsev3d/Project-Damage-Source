# установка превью пресетов и личное избранное
import json
import os
import shutil
import tempfile
import zipfile
from pathlib import Path, PurePosixPath
import bpy
from bpy_extras.io_utils import ImportHelper

from . import extension_storage

_RELATIVE = 'previews/presets'
_favorites=None


def directory():
    # An empty user directory records an explicit removal of a migrated pack.
    try:
        return extension_storage.directory(_RELATIVE)
    except OSError:
        return extension_storage.config_path(_RELATIVE) if extension_storage.IS_EXTENSION else extension_storage.PACKAGE_DIR / _RELATIVE


def favorites_path():
    return extension_storage.config_path('favorite_presets.json')


def favorites():
    global _favorites
    if _favorites is None:
        try:
            value=json.loads(favorites_path().read_text())
            _favorites=set(v for v in value if isinstance(v,str)) if isinstance(value,list) else set()
        except (OSError,ValueError):_favorites=set()
    return _favorites


def toggle_favorite(key):
    selected=set(favorites())
    if key in selected:selected.remove(key)
    else:selected.add(key)
    path=favorites_path();path.parent.mkdir(parents=True,exist_ok=True)
    fd,tmp=tempfile.mkstemp(dir=path.parent,prefix='.favorites-',suffix='.json')
    try:
        with os.fdopen(fd,'w') as f:json.dump(sorted(selected),f)
        os.replace(tmp,path)
    finally:
        if os.path.exists(tmp):os.unlink(tmp)
    favorites().clear();favorites().update(selected)


def installed():
    from . import presets
    return any(rows[2] for key,rows in presets.builtin_catalog().items() if key!='FAVORITES')


def install(path):
    from . import presets,presets_library
    # архив содержит только данные и картинки, никакой код из него не выполняется
    with zipfile.ZipFile(path) as z:
        entries=z.infolist()
        if len(entries)>10000 or sum(i.file_size for i in entries)>1024**3:
            raise ValueError('Preset pack is too large')
        members = {}
        for info in entries:
            name=PurePosixPath(info.filename)
            if name.is_absolute() or '..' in name.parts or '\\' in info.filename or (info.external_attr>>16)&0o170000==0o120000:
                raise ValueError('Invalid path in preset pack')
            if not name.parts or name.parts[0]=='__MACOSX':continue
            key = name.as_posix().casefold()
            if key in members:raise ValueError('Duplicate archive entries')
            members[key] = info
        manifests = [info for info in members.values() if not info.is_dir()
                     and len(PurePosixPath(info.filename).parts) <= 2
                     and PurePosixPath(info.filename).name == 'manifest.json']
        if not manifests:raise ValueError('This is not a Project Damage preset pack')
        if len(manifests)!=1:raise ValueError('The ZIP contains more than one preset manifest')
        manifest_info=manifests[0]
        manifest_root=PurePosixPath(manifest_info.filename).parent
        if manifest_info.file_size>16*1024**2:raise ValueError('Preset catalog is too large')
        manifest=json.loads(z.read(manifest_info))
        if not isinstance(manifest,dict):raise ValueError('Invalid preset catalog')
        if manifest.get('format')!='project-damage-presets' or manifest.get('version')!=1:
            raise ValueError('Unsupported preset pack format')
        records=manifest.get('presets')
        if not isinstance(records,dict) or not records:raise ValueError('The preset pack is empty')
        images={}
        for key,record in records.items():
            if key not in presets.presets_data.PRESETS or key in presets_library.UNAVAILABLE_PRESETS:continue
            if not isinstance(record,dict):raise ValueError('Invalid preset entry')
            name=record.get('preview','');rel=PurePosixPath(name)
            if ('\\' in name or '..' in rel.parts or len(rel.parts)!=3 or rel.parts[0]!='presets'
                    or rel.suffix.lower()!='.png' or rel.stem.upper()!=key):
                raise ValueError('Invalid preset preview: '+key)
            info = members.get((manifest_root / rel).as_posix().casefold())
            if info is None or info.is_dir():raise ValueError('Missing preview: '+key)
            if info.file_size>32*1024**2:raise ValueError('Preset preview is too large')
            images[info]=rel
        if not images:raise ValueError('No compatible presets. Update Project Damage first')
        root = directory()
        root.parent.mkdir(parents=True,exist_ok=True)
        stage=Path(tempfile.mkdtemp(prefix='.preset-install-',dir=root.parent))
        previous=root.with_name('.presets-previous')
        try:
            for name,rel in images.items():
                data=z.read(name)
                if not data.startswith(b'\x89PNG\r\n\x1a\n'):raise ValueError('Invalid PNG preview')
                target=stage/Path(*rel.parts[1:]);target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(data)
            (stage/'manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2))
            if previous.exists():raise ValueError('A previous installation backup needs to be restored first')
            if root.exists():os.replace(root,previous)
            try:os.replace(stage,root)
            except Exception:
                if previous.exists():os.replace(previous,root)
                raise
            if previous.exists():shutil.rmtree(previous)
        finally:
            if stage.exists():shutil.rmtree(stage)
    presets._CATALOG=None;presets._index_catalog()
    return len(images)


def remove():
    from . import presets,preset_browser
    # убираем только установленный набор, избранное и сцена остаются
    root = directory()
    if root.is_symlink():raise ValueError('Preset folder is a symbolic link; remove the link manually')
    preset_browser.close_browsers()
    try:
        if root.exists():shutil.rmtree(root)
        if extension_storage.IS_EXTENSION:root.mkdir(parents=True, exist_ok=True)
    finally:
        presets._CATALOG=None;presets._index_catalog()


class PROJECT_DAMAGE_OT_install_presets(bpy.types.Operator,ImportHelper):
    bl_idname='project_damage.install_builtin_presets'
    bl_label='Install Built-in Presets'
    bl_description='Select the Project Damage preset ZIP; previews are saved in the Project Damage library'
    filename_ext='.zip'
    filter_glob:bpy.props.StringProperty(default='*.zip',options={'HIDDEN'})
    def execute(self,context):
        try:count=install(self.filepath)
        except (OSError,ValueError,KeyError,TypeError,zipfile.BadZipFile) as exc:
            self.report({'ERROR'},str(exc));return {'CANCELLED'}
        for window in context.window_manager.windows:
            for area in window.screen.areas:area.tag_redraw()
        self.report({'INFO'},f'Installed {count} built-in presets')
        return {'FINISHED'}


class PROJECT_DAMAGE_OT_remove_presets(bpy.types.Operator):
    bl_idname='project_damage.remove_builtin_presets'
    bl_label='Remove Built-in Presets'
    bl_description='Remove the installed preset library. Existing objects, User_lib and Favorites are kept. Install the ZIP again to restore the library'
    def execute(self,context):
        try:remove()
        except (OSError,ValueError) as exc:
            self.report({'ERROR'},str(exc));return {'CANCELLED'}
        for window in context.window_manager.windows:
            for area in window.screen.areas:area.tag_redraw()
        self.report({'INFO'},'Preset library removed; install the ZIP to restore it')
        return {'FINISHED'}


class PROJECT_DAMAGE_OT_favorite_preset(bpy.types.Operator):
    bl_idname='project_damage.favorite_preset'
    bl_label='Add to Favorites'
    bl_description='Add this preset to Favorites; click again to remove it. Its original category is unchanged'
    preset:bpy.props.StringProperty()
    def execute(self,context):
        from . import presets
        if self.preset not in presets.presets_data.PRESETS:return {'CANCELLED'}
        try:toggle_favorite(self.preset)
        except OSError as exc:self.report({'ERROR'},str(exc));return {'CANCELLED'}
        presets._CATALOG=None;presets._index_catalog()
        for window in context.window_manager.windows:
            for area in window.screen.areas:area.tag_redraw()
        return {'FINISHED'}


def draw_preferences(layout):
    box=layout.box();box.label(text='Built-in Presets',icon='PRESET')
    if installed():
        box.label(text='Preset library installed',icon='CHECKMARK')
        box.operator('project_damage.remove_builtin_presets',icon='TRASH')
    else:box.operator('project_damage.install_builtin_presets',icon='IMPORT')


CLASSES=(PROJECT_DAMAGE_OT_install_presets,PROJECT_DAMAGE_OT_remove_presets,PROJECT_DAMAGE_OT_favorite_preset)
def register():
    for cls in CLASSES:bpy.utils.register_class(cls)
def unregister():
    global _favorites
    _favorites=None
    for cls in reversed(CLASSES):bpy.utils.unregister_class(cls)
