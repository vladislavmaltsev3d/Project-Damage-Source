# подгружаем стандартные материалы из файлов аддона по мере надобности
# SPDX-License-Identifier: GPL-3.0-or-later
"""Lazy library defaults. Explicit material overrides always win."""
from functools import lru_cache
from pathlib import Path, PurePosixPath
import bpy
from bpy.app.handlers import persistent

NAMES={
    'brick':'Bricks Material | Project Damage',
    'mortar':'Bricks Mortats Material | Project Damage',
    'rebar':'Rebars Material Rusty | Project Damage',
                                                                             
    'wood_planks':'Wood Planks Shader | Project Damage',
    'roots':'Roots Shader | Project Damage',
    'framing':'Framing Timber Shader | Project Damage',
    'grass_dry':'PD_Grass_Dry',
    'grass_fresh':'PD_Grass_Fresh',
    'gravel':'Gravel_Procedural',
}

_DATA = Path(__file__).parent / 'data'
_TEXTURE_FOLDERS = ('Concrete042A_2K-JPG', 'Metal055C_2K-JPG', 'Plaster003_2K-JPG',
                    'Rust009_2K-JPG', 'project_damage_masks')


@lru_cache(maxsize=1)
def _texture_paths():
    return {file.name.casefold(): file for folder in _TEXTURE_FOLDERS
            for file in (_DATA / folder).glob('*') if file.is_file()}


def _restore_image(image):
    if image is None or image.library is not None or image.source != 'FILE' or image.packed_file:
        return
    filename = PurePosixPath(image.filepath.replace('\\', '/')).name.casefold()
    shipped = _texture_paths().get(filename)
    if shipped is None:
        return
    previous = image.filepath
    current = Path(bpy.path.abspath(previous))
    if current.is_file() and current.resolve() != shipped.resolve():
        return
    try:
        image.filepath = str(shipped)
        image.pack()
    except (OSError, RuntimeError) as exc:
        image.filepath = previous
        print(f'Project Damage texture {filename}: {exc}')


def restore_material(material):
    seen = set()
    pending = [material.node_tree] if material is not None and material.node_tree is not None else []
    while pending:
        tree = pending.pop()
        if tree.as_pointer() in seen:
            continue
        seen.add(tree.as_pointer())
        for node in tree.nodes:
            if node.type == 'GROUP' and node.node_tree is not None:
                pending.append(node.node_tree)
            elif node.type in {'TEX_IMAGE', 'TEX_ENVIRONMENT'}:
                _restore_image(node.image)
    return material


@persistent
def _restore_on_open(_):
    for image in bpy.data.images:
        _restore_image(image)


def register():
    if _restore_on_open not in bpy.app.handlers.load_post:
        bpy.app.handlers.load_post.append(_restore_on_open)


def unregister():
    if _restore_on_open in bpy.app.handlers.load_post:
        bpy.app.handlers.load_post.remove(_restore_on_open)
    _texture_paths.cache_clear()


def get(kind):
    name=NAMES[kind]
    if kind in {'grass_dry','grass_fresh','gravel'}:
                                                                              
                                                                                
        for material in bpy.data.materials:
            if (material.get('project_damage_scatter_library_material')==name
                    and not material.get('project_damage_shader_uv')):
                return restore_material(material)
        path=Path(__file__).parent/'blends/gravel_grass_mat.blend'
        with bpy.data.libraries.load(str(path),link=False) as (source,target):
            if name not in source.materials:raise RuntimeError('Missing scatter material: '+name)
            target.materials=[name]
        material=target.materials[0]
        material['project_damage_scatter_library_material']=name
        return restore_material(material)
    material=bpy.data.materials.get(name)
    if material is not None:return restore_material(material)
    filename = {'rebar':'rebars_material.blend','wood_planks':'wood_mat.blend','roots':'roots_mat.blend','framing':'stud_material.blend'}.get(kind,'bricks_mat.blend')
    path=Path(__file__).parent/'blends'/filename
    with bpy.data.libraries.load(str(path),link=False) as (source,target):
        if name not in source.materials:
            raise RuntimeError('Default material missing in '+filename+': '+name)
        target.materials=[name]
    return restore_material(target.materials[0])
