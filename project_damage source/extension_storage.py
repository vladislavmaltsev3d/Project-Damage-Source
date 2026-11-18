# данные пользователя лежат отдельно от установленной копии расширения
from pathlib import Path
import os
import shutil
import tempfile

import bpy

IS_EXTENSION = __package__.startswith('bl_ext.')
PACKAGE_DIR = Path(__file__).resolve().parent


def config_path(relative):
    """One user library per Blender profile, shared by all repositories."""
    path = bpy.utils.user_resource('CONFIG')
    if not path:
        raise OSError('Blender user configuration directory is unavailable')
    return Path(path) / 'project_damage' / relative


def previous_paths(relative):
    yield PACKAGE_DIR / relative
    scripts = bpy.utils.user_resource('SCRIPTS')
    if scripts:
        legacy = Path(scripts) / 'addons' / 'project_damage' / relative
        if legacy != PACKAGE_DIR / relative:
            yield legacy


def directory(relative, *, create=False):
    target = PACKAGE_DIR / relative
    if IS_EXTENSION:
        target = config_path(relative)
        if not target.exists():
            previous = next((path for path in previous_paths(relative)
                             if path.is_dir() and not path.is_symlink() and any(path.iterdir())), None)
            if previous is not None:
                target.parent.mkdir(parents=True, exist_ok=True)
                # Keep partial copies out of the library if Blender is interrupted.
                with tempfile.TemporaryDirectory(dir=target.parent, prefix='.pd-library-') as folder:
                    staging = Path(folder) / 'library'
                    shutil.copytree(previous, staging, ignore=_ignore_links)
                    if not target.exists():
                        os.replace(staging, target)
    if create:
        target.mkdir(parents=True, exist_ok=True)
    return target


def _ignore_links(folder, entries):
    return [name for name in entries if (Path(folder) / name).is_symlink() or name == '__pycache__']
