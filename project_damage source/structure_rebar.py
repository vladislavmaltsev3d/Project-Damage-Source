# используем профили арматуры на управляемой поверхности
from bisect import bisect_right
from collections import defaultdict
from itertools import accumulate, combinations

import numpy as np
from . import core_loader


def custom_paths(mesh):
    """Walk loose mesh edges once, continuing the straightest bars at a junction."""
    if not len(mesh.edges):
        return
    edge_ids = np.empty(len(mesh.loops), dtype=np.int32)
    mesh.loops.foreach_get('edge_index', edge_ids)
    face_edges = set(edge_ids.tolist())
    positions = [vertex.co.copy() for vertex in mesh.vertices]
    edges = []
    incident = defaultdict(list)
    for edge in mesh.edges:
        if edge.index in face_edges:
            continue
        a, b = edge.vertices
        direction = positions[b] - positions[a]
        if direction.length_squared <= 1e-16:
            continue
        direction.normalize()
        index = len(edges)
        edges.append((a, b))
        incident[a].append((index, direction))
        incident[b].append((index, -direction))

    continuation = {}
    for v, links in incident.items():
        if len(links) == 2:
            (a, _), (b, _) = links
            continuation[v, a] = b
            continuation[v, b] = a
        elif len(links) > 2:
            # An acute reversal is two branches, since a single bar would double back.
            pairs = sorted((u.dot(w), a, b) for (a, u), (b, w) in combinations(links, 2))
            for alignment, a, b in pairs:
                if alignment >= 0.0:
                    break
                if (v, a) not in continuation and (v, b) not in continuation:
                    continuation[v, a] = b
                    continuation[v, b] = a

    visited = set()

    def walk(vertex, index):
        path = [positions[vertex]]
        while index is not None and index not in visited:
            visited.add(index)
            a, b = edges[index]
            vertex = b if vertex == a else a
            path.append(positions[vertex])
            index = continuation.get((vertex, index))
        return path

    for index, (a, b) in enumerate(edges):
        if index in visited:
            continue
        if (a, index) not in continuation:
            yield walk(a, index)
        elif (b, index) not in continuation:
            yield walk(b, index)
    for index, (a, _) in enumerate(edges):
        if index not in visited:
            yield walk(a, index)


def path_runs(paths, damage, amount, bend, step):
    from .structure_surface import edge_normal, edge_runs, surviving_runs

    for path in paths:
        if len(path) == 2:
            for centers, normal, start_free, end_free in edge_runs((path,), None, damage, amount, bend, step):
                yield centers, [normal] * len(centers), start_free, end_free, False
            continue

        deltas = [b - a for a, b in zip(path, path[1:])]
        distances = list(accumulate((delta.length for delta in deltas), initial=0.0))
        directions = [delta.normalized() for delta in deltas]
        closed = path[-1] == path[0]
        normals = []
        for i in range(len(path) - int(closed)):
            before = directions[i - 1] if i or closed else directions[0]
            after = directions[min(i, len(directions) - 1)]
            tangent = before + after
            normal = edge_normal(tangent.normalized() if tangent.length_squared > 1e-12 else after)
            if normals and normal.dot(normals[-1]) < 0.0:
                normal.negate()
            normals.append(normal)
        if closed:
            normals.append(normals[0])
        if not damage.base.parts:
            yield path, normals, False, False, True
            continue

        def sample(distance):
            i = min(bisect_right(distances, distance) - 1, len(deltas) - 1)
            t = (distance - distances[i]) / (distances[i + 1] - distances[i])
            return path[i] + deltas[i] * t, normals[i].lerp(normals[i + 1], t).normalized()

        length = distances[-1]
        runs = list(surviving_runs(0.0, length, step, lambda t: damage.ratio(sample(t)[0]) >= amount,
                                   knots=distances[1:-1]))
        if closed and len(runs) > 1 and runs[0][0] == 0.0 and runs[-1][-1] == length:
            runs[0] = runs.pop() + runs[0][1:]
        for samples in runs:
            centers, run_normals = [], []
            for distance in samples:
                center, normal = sample(distance)
                ratio = damage.ratio(center)
                if ratio < 1.0:
                    center += normal * ((1.0 - ratio * ratio) ** 2 * bend)
                centers.append(center)
                run_normals.append(normal)
            yield centers, run_normals, samples[0] > 1e-8, samples[-1] < length - 1e-8, True


def generate(props,centers,normals,unit,start_free,end_free,key,*,custom_path=False):
    core=core_loader.load()
    if core is None or not hasattr(core,'build_rebar_surface_path'):
        raise RuntimeError('Restart Blender to load the updated rebar generator')
    params=core.BreakRebarParams()
    params.type=int(props.rebar_type)
    params.radius=props.radius*100.0
    params.rib_height=props.rebar_rib_height/100
    params.rib_pitch=props.rebar_rib_pitch
    params.twist_pitch=props.rebar_twist_pitch
    params.neck=props.rebar_neck/100
    params.wobble=props.rebar_wobble/100
    params.rebar_resolution=min(1.,props.resolution/100)
    params.detail=max(.05,props.resolution/100)
    params.seed=(props.seed+key*977)&0x7fffffff
    factor=100.0/unit
    args = (params, (np.asarray(centers) * factor).ravel().tolist(),
            np.asarray(normals).ravel().tolist(), start_free, end_free)
    if custom_path:
        try:
            result = core.build_rebar_surface_path(*args, custom_path=True)
        except TypeError as exc:
            raise RuntimeError('Restart Blender to load the updated Custom Rebars generator') from exc
    else:
        result = core.build_rebar_surface_path(*args)
    faces=[tuple(int(i) for i in (f[:3] if f[2]==f[3] else f)) for f in result['polys']]
    return np.asarray(result['points'])/factor,faces
