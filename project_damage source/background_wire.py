# упаковываем геометрию и настройки для передачи фоновому процессу
# SPDX-License-Identifier: GPL-3.0-or-later
"""Plain-data transport for the native core. No bpy and no Blender RNA."""
import numpy as np


def encode(value, core):
    if value is None or isinstance(value, (str, bytes, bool, int, float, np.ndarray)):
        return value
    if isinstance(value, np.generic):
        return value.item()
    if isinstance(value, (list, tuple)):
        return [encode(v, core) for v in value]
    if isinstance(value, dict):
        return {k: encode(v, core) for k, v in value.items()}
    if type(value).__module__ == core.__name__:
        fields = {}
        for name in sorted(dir(value)):
            if name.startswith('_'):
                continue
            item = getattr(value, name)
            if not callable(item):
                fields[name] = encode(item, core)
        return {'__pd_native__': type(value).__name__, 'fields': fields}
    raise TypeError('Unsupported background input: '+str(type(value)))


def decode(value, core, target=None):
    if isinstance(value, dict) and '__pd_native__' in value:
        result = target if target is not None else getattr(core, value['__pd_native__'])()
        for name, item in value['fields'].items():
            if value['__pd_native__']=='Params' and name in {'corner_paint_pts','corner_paint_count','build_paint_pts','build_paint_count'}:
                continue
            if value['__pd_native__']=='BreakRebarParams' and name in {'stroke_pts','stroke_count'}:
                continue
            if value['__pd_native__']=='MetalDeformImpactor' and name in {'pts','tris'}:
                continue
            if value['__pd_native__']=='VoxelErosionParams' and name in {'world','zone_weights','mask_uvs','mask_pixels','mask_width','mask_height'}:
                continue                                                    
            if value['__pd_native__']=='MetalTearParams' and name in {'stroke_pts','stroke_count'}:
                continue
            current = getattr(result, name)
            if value['__pd_native__']=='BreakerSpec' and name=='shape' and item is None:
                continue                                                                
            if isinstance(item, dict) and '__pd_native__' in item:
                nested=decode(item, core, current)
                try:setattr(result,name,nested)
                except AttributeError:
                    if current is None:raise
            else:
                setattr(result, name, decode(item, core))
        if value['__pd_native__']=='VoxelErosionParams':
            f=value['fields']
            if f.get('world'):result.set_world(np.asarray(f['world'],dtype=np.float64))
            if f.get('zone_weights'):result.set_zone_weights(np.asarray(f['zone_weights'],dtype=np.float64))
            if f.get('mask_uvs'):result.set_mask_uvs(np.asarray(f['mask_uvs'],dtype=np.float64))
            if f.get('mask_pixels'):
                result.set_mask_image(np.asarray(f['mask_pixels'],dtype=np.float32).reshape(f['mask_height'],f['mask_width']))
        if value['__pd_native__']=='Params' and 'corner_paint_pts' in value['fields']:
            result.set_corner_paint(value['fields']['corner_paint_pts'],value['fields']['corner_paint_count'])
        if value['__pd_native__']=='Params' and 'build_paint_pts' in value['fields']:
            result.set_build_paint(value['fields']['build_paint_pts'],value['fields']['build_paint_count'])
        if value['__pd_native__']=='BreakRebarParams':
            result.set_strokes(value['fields']['stroke_pts'],value['fields']['stroke_count'])
        if value['__pd_native__']=='MetalTearParams':
            f=value['fields']
            result.set_strokes(np.asarray(f.get('stroke_pts',[]),dtype=np.float64),
                               np.asarray(f.get('stroke_count',[]),dtype=np.int32))
        if value['__pd_native__']=='MetalDeformImpactor':
            result.set_mesh(np.asarray(value['fields']['pts'],dtype=np.float64).reshape(-1,3),
                            np.asarray(value['fields']['tris'],dtype=np.int32).reshape(-1,3))
        return result
    if isinstance(value, dict):
        return {k:decode(v, core) for k,v in value.items()}
    if isinstance(value, list):
        return [decode(v, core) for v in value]
    return value
