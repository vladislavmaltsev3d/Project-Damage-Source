# сравниваем входные данные, чтобы не запускать лишний пересчёт
# SPDX-License-Identifier: GPL-3.0-or-later
"""Geometry inputs behind Blender's transform and geometry notifications."""
import hashlib
import time

import numpy as np

DRAWING_TYPES = {'GREASEPENCIL', 'GPENCIL'}
CONTROL_TYPES = DRAWING_TYPES | {'EMPTY'}
GEOMETRY_TYPES = {'MESH', 'CURVE'}
_seen = {}
_initial = {}
# One signature per object and depsgraph event: several consumers ask about the
# same drawing in the same event, and hashing every stroke point is not free.
_event = {}
_EVENT_SECONDS = .25


def begin_event(*_args):
    _event.clear()


def _event_signature(obj, local=False):
    now = time.monotonic()
    key=(obj.session_uid,local)
    hit = _event.get(key)
    if hit is not None and now - hit[0] < _EVENT_SECONDS:
        return hit[1]
    value = signature(obj, local=local)
    _event[key] = (now, value)
    return value


def _matrix(matrix):
    return tuple(value for row in matrix for value in row)


def signature(obj, *, matrix=None, local=False):
    # флаги Blender сообщают и о переоценке; сравниваем сами исходные данные
    import bpy
    from mathutils import Matrix
    if not isinstance(obj,bpy.types.Object):
        return None
    if local:matrix=Matrix.Identity(4)
    elif matrix is None and obj.type in CONTROL_TYPES | GEOMETRY_TYPES:
        child=obj;parent=obj.parent;relative=Matrix.Identity(4)
        while parent is not None:
            # локальные матрицы не дрожат от перемещения всей конструкции
            transform=child.matrix_local if child.constraints or child.parent_type!='OBJECT' else child.matrix_parent_inverse @ child.matrix_basis
            relative=transform @ relative
            if parent.get('project_damage_uid') or parent.get('project_damage_result_of') or parent.get('pd_structure_source'):
                matrix=relative
                break
            child=parent;parent=parent.parent
    if obj.type in GEOMETRY_TYPES:
        digest=hashlib.blake2b(digest_size=16)
        digest.update(repr(_matrix(obj.matrix_world if matrix is None else matrix)).encode())
        data=obj.data
        if obj.type=='MESH':
            for seq,field,width,dtype in ((data.vertices,'co',3,np.float32),(data.loops,'vertex_index',1,np.int32),(data.polygons,'loop_total',1,np.int32)):
                values=np.empty(len(seq)*width,dtype);seq.foreach_get(field,values)
                digest.update(str(len(seq)).encode());digest.update(values.tobytes())
            # веса масок тоже являются изменением входной сетки
            if len(obj.vertex_groups):
                for v in data.vertices:
                    if v.groups:digest.update(repr((v.index,tuple((g.group,round(g.weight,7)) for g in v.groups))).encode())
        else:
            for spline in data.splines:
                digest.update(repr((spline.type,spline.use_cyclic_u,spline.resolution_u)).encode())
                for point in spline.bezier_points:
                    digest.update(repr((tuple(point.co),tuple(point.handle_left),tuple(point.handle_right))).encode())
                for point in spline.points:digest.update(repr(tuple(point.co)).encode())
        for modifier in obj.modifiers:
            values=[]
            for prop in modifier.bl_rna.properties:
                if prop.is_readonly or prop.identifier in {'name','rna_type','show_expanded','show_on_cage','show_in_editmode','is_active'}:continue
                if prop.type in {'FLOAT','INT','BOOLEAN','ENUM','STRING'}:
                    value=getattr(modifier,prop.identifier)
                    values.append((prop.identifier,tuple(value) if getattr(prop,'is_array',False) else value))
            digest.update(repr((modifier.type,values)).encode())
        return digest.digest()
    if obj.type not in CONTROL_TYPES | GEOMETRY_TYPES:return None
    matrix = obj.matrix_world if matrix is None else matrix
    if obj.type == 'EMPTY':
        return (_matrix(matrix), obj.empty_display_size,
                tuple(obj.get(name) for name in ('strength', 'depth', 'petals',
                    'project_damage_hit_strength', 'project_damage_blast_strength',
                    'project_damage_metal_strength')))
    digest = hashlib.blake2b(digest_size=16)
    digest.update(repr(_matrix(matrix)).encode())
    for layer in obj.data.layers:
        if layer.hide:
            continue
        modern = hasattr(layer, 'current_frame')
        frame = layer.current_frame() if modern else layer.active_frame
        if frame is None:
            continue
        strokes = frame.drawing.strokes if modern else frame.strokes
        metadata = tuple((len(stroke.points), bool(getattr(stroke, 'cyclic', getattr(stroke, 'use_cyclic', False))),
                          bool(getattr(stroke, 'hide_stroke', False))) for stroke in strokes)
        digest.update(repr((layer.name, _matrix(layer.matrix_local) if modern else (), metadata)).encode())
        positions = frame.drawing.attributes.get('position') if modern else None
        if positions is not None:
            values = np.empty(3 * len(positions.data), dtype=np.float32)
            positions.data.foreach_get('vector', values)
            digest.update(values)
        else:
            for stroke in strokes:
                if not getattr(stroke, 'hide_stroke', False):
                    for point in stroke.points:
                        digest.update(np.asarray(point.position if modern else point.co, dtype=np.float32).tobytes())
    return digest.digest()


def remember(obj, consumer=None, *, value=None, local=False):
    if obj.type in CONTROL_TYPES | GEOMETRY_TYPES:
        value = signature(obj, local=local) if value is None else value
        if consumer is None:
            _initial[(obj.session_uid,local)] = value
        else:
            _seen[(consumer, obj.session_uid,local)] = value


def changed(obj, consumer, *, local=False):
    if getattr(obj, 'type', None) not in CONTROL_TYPES | GEOMETRY_TYPES:
        return True
    key = (consumer, obj.session_uid,local)
    current = _event_signature(obj,local)
    previous = _seen.get(key, _initial.get((obj.session_uid,local)))
    if current == previous:
        return False
    _seen[key] = current
    return True


def clear():
    _seen.clear()
    _initial.clear()
    _event.clear()
