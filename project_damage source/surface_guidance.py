# создаём упрощённую направляющую, не заменяя исходную детальную поверхность
# SPDX-License-Identifier: GPL-3.0-or-later
"""Shared opt-in orientation field; never replace Blender's target geometry.

Only generation paths call this module. No mesh reads in panel drawing or
dependency callbacks. The two-entry cache is keyed by complete input geometry.
"""
from collections import OrderedDict
import hashlib
import numpy as np
import bpy

_fields = OrderedDict()


def enabled():
    entry = bpy.context.preferences.addons.get(__package__)
    return bool(entry and getattr(entry.preferences, 'fracture_surface_proxy', False))


def clear():
    _fields.clear()


def kwargs(core):
    if not enabled():
        return {}
    if not hasattr(core, 'surface_guide_field'):
        raise RuntimeError('Surface Guide needs the updated core. Save your scene and restart Blender.')
    return {'surface_guide': True}


def field(points, polys):
    if not enabled():
        return None
    from . import core_loader
    core = core_loader.load()
    kwargs(core)
    from . import incremental
    if not incremental.cache_enabled():return core.surface_guide_field(np.ascontiguousarray(points,dtype=np.float64),np.ascontiguousarray(polys,dtype=np.int32))
    points = np.ascontiguousarray(points, dtype=np.float64)
    polys = np.ascontiguousarray(polys, dtype=np.int32)
    h = hashlib.blake2b(digest_size=20)
    h.update(repr((points.shape, polys.shape)).encode())
    h.update(points); h.update(polys)
    key = h.digest()
    if key not in _fields:
        result = core.surface_guide_field(points, polys)
        for value in result.values():
            value.flags.writeable = False
        _fields[key] = result
        while len(_fields) > 2:
            _fields.popitem(last=False)
    _fields.move_to_end(key)
    return _fields[key]


def face_normal(guidance, index, original):
    if guidance is None or index is None or index < 0:
        return original
    from mathutils import Vector
    n = Vector(guidance['face_normals'][index])
    return n if n.dot(original) > .25 else original


def bmesh_normals(bm, original=None):
    if not enabled():
        return original
    bm.verts.index_update()
    vertices = list(bm.verts)
    points = np.asarray([v.co[:] for v in vertices], np.float64).reshape(-1, 3)
    faces = []
    for face in bm.faces:
        ids = [v.index for v in face.verts]
        if len(ids) == 4:
            faces.append(ids)
        else:
            faces.extend((ids[0], ids[i], ids[i+1], ids[i+1]) for i in range(1, len(ids)-1))
    if not faces:
        return original
    return field(points, np.asarray(faces, np.int32))['vertex_normals']


def wrap_bvh(tree, points, faces):
    """Guide nearest-surface orientation; retain exact rays/contact coordinates.

    Blender's polygon BVH returns the original polygon index, including n-gons.
    Aggregate fan normals back to those indices before exposing the wrapper.
    """
    if tree is None or not enabled():
        return tree
    quads, owners = [], []
    for index, face in enumerate(faces):
        ids = list(face)
        if len(ids) == 4 and ids[-1] == ids[-2]:
            ids.pop()
        if len(ids) == 4:
            quads.append(ids); owners.append(index)
        else:
            for i in range(1, len(ids)-1):
                quads.append((ids[0], ids[i], ids[i+1], ids[i+1])); owners.append(index)
    if not quads:
        return tree
    result = field(points, np.asarray(quads, np.int32))
    normals = np.zeros((len(faces), 3), np.float64)
    np.add.at(normals, owners, result['face_normals'])
    normals /= np.maximum(np.linalg.norm(normals, axis=1, keepdims=True), 1e-20)
    class GuidedBVH:
        def find_nearest(self, *args, **kwargs):
            co, n, index, distance = tree.find_nearest(*args, **kwargs)
            if n is not None:
                n = face_normal({'face_normals': normals}, index, n)
            return co, n, index, distance
        def __getattr__(self, name):
            return getattr(tree, name)
    return GuidedBVH()
