# вырезаем нужную часть меша и закрываем края получившегося объёма
import bmesh
import numpy as np
from mathutils import Vector
from mathutils.geometry import delaunay_2d_cdt


def _inside(point, polygon):
    x, y = point
    odd = False
    a = polygon[-1]
    for b in polygon:
        if (a.y > y) != (b.y > y) and x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x:
            odd = not odd
        a = b
    return odd


def cap_boundaries(bm):
    edges = [edge for edge in bm.edges if edge.is_boundary]
    outgoing = {}
    for edge in edges:
        loop = edge.link_loops[0]
        a, b = loop.link_loop_next.vert, loop.vert
        if a in outgoing:
            return None
        outgoing[a] = b
    rings = []
    while outgoing:
        first = next(iter(outgoing))
        vertex = first
        ring = []
        while True:
            if vertex not in outgoing:
                return None
            ring.append(vertex)
            vertex = outgoing.pop(vertex)
            if vertex == first:
                break
        if len(ring) < 3:
            return None
        rings.append(ring)
    if not rings:
        return []
    scale = max((v.co.length for ring in rings for v in ring), default=1.0)
    epsilon = max(scale * 1e-8, 1e-9)
    groups = []
    for ring in rings:
        origin = ring[0].co
        normal = sum(((a.co-origin).cross(b.co-origin) for a, b in zip(ring, ring[1:]+ring[:1])), Vector())
        if normal.length < epsilon * epsilon:
            return None
        area = normal.length
        normal.normalize()
        group = next((g for g in groups if abs(g[1].dot(normal)) > 0.99999
                      and max(abs((v.co-g[0]).dot(g[1])) for v in ring) < epsilon * 10), None)
        if group is None:
            groups.append([origin.copy(), normal, area, [ring]])
        else:
            group[3].append(ring)
            if area > group[2]:
                group[1], group[2] = normal, area
    created = []
    for origin, normal, _, group_rings in groups:
        u = normal.orthogonal().normalized()
        v = normal.cross(u)
        vertices = [vertex for ring in group_rings for vertex in ring]
        coords = [Vector(((p.co-origin).dot(u), (p.co-origin).dot(v))) for p in vertices]
        faces = []
        polygons = []
        start = 0
        for ring in group_rings:
            face = list(range(start, start+len(ring)))
            faces.append(face)
            polygons.append([coords[i] for i in face])
            start += len(ring)
        output, _, triangles, ids, _, _ = delaunay_2d_cdt(coords, [], faces, 1, epsilon)
        for tri in triangles:
            centre = sum((output[i] for i in tri), Vector((0, 0))) / len(tri)
            if sum(_inside(centre, polygon) for polygon in polygons) % 2 == 0:
                continue
            if any(len(ids[i]) != 1 for i in tri):
                return None
            verts = [vertices[ids[i][0]] for i in tri]
            if (verts[1].co-verts[0].co).cross(verts[2].co-verts[0].co).dot(normal) < 0:
                verts.reverse()
            try:
                created.append(bm.faces.new(verts))
            except ValueError:
                return None
    return created


def close_box(bm, data, low, high):
    scale = max(float(np.linalg.norm(high-low)), 1e-6)
    epsilon = max(scale * 1e-7, 1e-8)
    created = []
    for axis in range(3):
        a, b = [i for i in range(3) if i != axis]
        for value, sign in ((low[axis], -1), (high[axis], 1)):
            vertices = [v for v in bm.verts if abs(v.co[axis]-value) < epsilon]
            coords = [Vector((v.co[a], v.co[b])) for v in vertices]
            lookup = {v: i for i, v in enumerate(vertices)}
            edges = [(lookup[e.verts[0]], lookup[e.verts[1]]) for e in bm.edges
                     if e.is_boundary and all(v in lookup for v in e.verts)]
            existing = [[Vector((v.co[a], v.co[b])) for v in face.verts] for face in bm.faces
                        if all(abs(v.co[axis]-value) < epsilon for v in face.verts)]
            start = len(coords)
            coords.extend(Vector(p) for p in ((low[a],low[b]), (high[a],low[b]),
                                               (high[a],high[b]), (low[a],high[b])))
            output, _, triangles, ids, _, _ = delaunay_2d_cdt(
                coords, edges, [list(range(start,start+4))], 1, epsilon * .05)
            points = {}
            for tri in triangles:
                centre = sum((output[i] for i in tri), Vector((0, 0))) / len(tri)
                if any(_inside(centre, poly) for poly in existing):
                    continue
                sample = Vector((0,0,0))
                sample[a], sample[b], sample[axis] = centre.x, centre.y, value-sign*epsilon*4
                if not data.contains(sample):
                    continue
                face_verts = []
                for index in tri:
                    if index not in points:
                        originals = [i for i in ids[index] if i < start]
                        if originals:
                            points[index] = vertices[originals[0]]
                        else:
                            point = Vector((0,0,0))
                            point[a],point[b],point[axis] = output[index].x,output[index].y,value
                            points[index] = bm.verts.new(point)
                    face_verts.append(points[index])
                normal = (face_verts[1].co-face_verts[0].co).cross(face_verts[2].co-face_verts[0].co)
                if normal[axis]*sign < 0:
                    face_verts.reverse()
                try:
                    created.append(bm.faces.new(face_verts))
                except ValueError:
                    return None
    return created
