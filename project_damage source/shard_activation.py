# освобождаем соседние осколки при контакте с уже движущимися частями
"""Precompute neighboring shard releases from contacts in the rigid-body simulation."""
import numpy as np
import bpy
from mathutils import Vector
from mathutils.bvhtree import BVHTree


def _matrix(obj):
    return np.asarray(obj.matrix_world, dtype=np.float64).copy()


def _transform(points, matrix):
    return points @ matrix[:3, :3].T + matrix[:3, 3]


def _shard(obj):
    mesh = obj.data
    points = np.empty((len(mesh.vertices), 3), np.float32)
    mesh.vertices.foreach_get('co', points.ravel())
    mesh.calc_loop_triangles()
    triangles = np.empty((len(mesh.loop_triangles), 3), np.int32)
    mesh.loop_triangles.foreach_get('vertices', triangles.ravel())
    return points, triangles.tolist(), _matrix(obj)


def _samples(points):
    if len(points) > 128:
        points = points[np.linspace(0, len(points)-1, 128, dtype=np.int32)]
    return points


def _touches(current, before, tree, held_tree, margin):
    if tree.overlap(held_tree):
        return True
    for previous, point in zip(_samples(before), _samples(current)):
        if margin > 0 and held_tree.find_nearest(Vector(point), margin)[0] is not None:
            return True
        ray = Vector(point - previous)
        if ray.length_squared > 1e-16:
            if held_tree.ray_cast(Vector(previous), ray.normalized(), ray.length)[0] is not None:
                return True
    return False


def propagate(context, pieces, held_data, hits, start, end, margin):
    """Release touched neighbors on the next frame and replay each new wave.

    Key all releases on the normal rigid-body kinematic channel so playback,
    Blender's cache and Quick Bake reproduce the same activation order.
    """
    from .damage_physics import _hold_piece, _release_hit

    scene = context.scene
    world = scene.rigidbody_world
    original = scene.frame_current, scene.frame_subframe
    enabled = world.enabled
    released = {obj: max(start + 1, frame) for obj, frame in hits.items()}
    frame_to_check = min(released.values()) + 1
    pending = {obj: (tree, lo, hi) for obj, tree, lo, hi in held_data
               if released.get(obj, end+1) > frame_to_check + 1}
    if not pending:
        return
    shards = {obj: _shard(obj) for obj in pieces}
    # Detect motion separately from a resting contact to avoid instant cascades
    # across a pane whose shards initially touch.
    motion = max(1e-7, 0.00002 / (scene.unit_settings.scale_length or 1.0))
    try:
        world.enabled = True
        while pending and frame_to_check < end:
            with context.temp_override(point_cache=world.point_cache):
                if bpy.ops.ptcache.free_bake.poll():
                    bpy.ops.ptcache.free_bake()
            previous = {}
            pending_objects = list(pending)
            bounds_lo = np.asarray([pending[obj][1] for obj in pending_objects])
            bounds_hi = np.asarray([pending[obj][2] for obj in pending_objects])
            found = set()
            for frame in range(start, end):
                scene.frame_set(frame)
                if frame < frame_to_check - 1:
                    continue
                graph = context.evaluated_depsgraph_get()
                for obj, release in released.items():
                    if frame < release:
                        continue
                    evaluated = obj.evaluated_get(graph)
                    matrix = _matrix(evaluated)
                    points, triangles, rest = shards[obj]
                    prior = previous.get(obj, rest)
                    previous[obj] = matrix
                    if frame < frame_to_check or frame <= release or not evaluated.rigid_body.enabled:
                        continue
                    if not len(points) or not triangles:
                        continue
                    current, before = _transform(points, matrix), _transform(points, prior)
                    if np.max(np.linalg.norm(current - before, axis=1)) < motion:
                        continue
                    lo = np.minimum(current.min(0), before.min(0)) - margin
                    hi = np.maximum(current.max(0), before.max(0)) + margin
                    candidates = np.flatnonzero(np.all(lo <= bounds_hi, axis=1) & np.all(hi >= bounds_lo, axis=1))
                    if not len(candidates):
                        continue
                    tree = BVHTree.FromPolygons(current.tolist(), triangles, all_triangles=True)
                    for index in candidates:
                        target = pending_objects[index]
                        if (target not in found and frame+1 < released.get(target, end+1)
                                and _touches(current, before, tree, pending[target][0], margin)):
                            found.add(target)
                if found:
                    break
            if not found:
                break
            wake = frame + 1
            for obj in sorted(found, key=lambda obj: obj.name):
                if obj in released:
                    _hold_piece(obj, start)
                _release_hit(obj, start, wake)
                released[obj] = wake
                del pending[obj]
            frame_to_check = wake
    finally:
        with context.temp_override(point_cache=world.point_cache):
            if bpy.ops.ptcache.free_bake.poll():
                bpy.ops.ptcache.free_bake()
        world.enabled = enabled
        scene.frame_set(start)
        if original[0] != start or original[1] != 0:
            scene.frame_set(original[0], subframe=original[1])
