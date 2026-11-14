# берём стандартные материалы труб из комплектного файла
# SPDX-License-Identifier: GPL-3.0-or-later
from pathlib import Path
import bpy
PIPE_NAMES={'PVC':'PVC Pipe','COPPER':'Copper Pipe Material','STEEL':'Steel Pipe','CAST_IRON':'Cast Iron Pipe','CONDUIT':'heat protective metal'}
def load(name):
    path=Path(__file__).parent/'blends/pipes_mats.blend'
    for mat in bpy.data.materials:
        if mat.get('pd_pipe_library_name')==name:return mat
    if not path.is_file():return None
    with bpy.data.libraries.load(str(path),link=False) as (available,target):
        if name not in available.materials:return None
        target.materials=[name]
    mat=target.materials[0]
    if mat is not None:mat['pd_pipe_library_name']=name
    return mat
