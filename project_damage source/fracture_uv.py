# сохраняем внешние UV и разворачиваем новые внутренние грани
"""UV transfer at fracture seams and a separate map for inside faces."""
import numpy as np

NAME = 'ProjectDamage_InsideFaces'
OWNER = 'project_damage_inside_uv_layer'


def layer_name(mesh):
    name = mesh.get(OWNER)
    return name if isinstance(name, str) and mesh.uv_layers.get(name) is not None else None


def copy_flags(mesh, active, render):
    if active and mesh.uv_layers.get(active) is not None:
        mesh.uv_layers.active = mesh.uv_layers[active]
    if render and mesh.uv_layers.get(render) is not None:
        mesh.uv_layers[render].active_render = True


def flags(mesh):
    active = mesh.uv_layers.active
    return active.name if active else None, next((layer.name for layer in mesh.uv_layers if layer.active_render), None)


def capture(mesh, *, by_polygon=False):
    """Read all authored maps in loop-triangle order, preserving seams."""
    mesh.calc_loop_triangles()
    triangles = np.empty((len(mesh.loop_triangles), 3), np.int32)
    mesh.loop_triangles.foreach_get('loops', triangles.ravel())
    indices = np.empty(len(mesh.loops), np.int32)
    mesh.loops.foreach_get('vertex_index', indices)
    points = np.empty((len(mesh.vertices), 3), np.float32)
    mesh.vertices.foreach_get('co', points.ravel())
    named = {}
    inside = layer_name(mesh)
    for layer in mesh.uv_layers:
        if layer.name == inside:
            continue
        coords = np.empty((len(indices), 2), np.float32)
        layer.uv.foreach_get('vector', coords.ravel())
        named[layer.name] = coords[triangles]
    polygons = None
    if by_polygon:
        polygons = np.empty(len(triangles), np.int32)
        mesh.loop_triangles.foreach_get('polygon_index', polygons)
    return points[indices[triangles]], named, flags(mesh), polygons, len(mesh.polygons)


# сохраняем UV внешних граней при создании осколков
def transfer(mesh, source_faces, captures):
    """Interpolate retained face corners against their known source triangles."""
    from .result_transfer import barycentric

    names = dict.fromkeys(name for _, maps, _, _, _ in captures for name in maps)
    if not names:
        return
    totals = np.empty(len(mesh.polygons), np.int32)
    mesh.polygons.foreach_get('loop_total', totals)
    source = np.repeat(source_faces, totals)
    indices = np.empty(len(mesh.loops), np.int32)
    mesh.loops.foreach_get('vertex_index', indices)
    points = np.empty((len(mesh.vertices), 3), np.float32)
    mesh.vertices.foreach_get('co', points.ravel())
    values = {name: np.zeros((len(indices), 2), np.float32) for name in names}
    offset = 0
    for triangles, maps, _, polygon_indices, poly_count in captures:
        local = source - offset
        count = len(triangles) if polygon_indices is None else poly_count
        retained = np.flatnonzero((local >= 0) & (local < count))
        if len(retained) and maps:
            ids = local[retained]
            sample = points[indices[retained]].astype(np.float64)
            if polygon_indices is None:
                weights = barycentric(sample, triangles[ids].astype(np.float64))
            else:
                starts = np.searchsorted(polygon_indices, ids)
                ends = np.searchsorted(polygon_indices, ids, side='right') - 1
                first = triangles[starts].astype(np.float64)
                last = triangles[ends].astype(np.float64)
                w0 = barycentric(sample, first)
                w1 = barycentric(sample, last)
                d0 = np.sum((np.einsum('ij,ijk->ik', w0, first) - sample) ** 2, axis=1)
                d1 = np.sum((np.einsum('ij,ijk->ik', w1, last) - sample) ** 2, axis=1)
                use_last = d1 < d0
                ids = np.where(use_last, ends, starts)
                weights = np.where(use_last[:, None], w1, w0)
            for name, uv in maps.items():
                values[name][retained] = np.einsum('ij,ijk->ik', weights, uv[ids])
        offset += count
    for name, coords in values.items():
        layer = mesh.uv_layers.get(name) or mesh.uv_layers.new(name=name, do_init=False)
        layer.uv.foreach_set('vector', coords.ravel())
    copy_flags(mesh, *captures[0][2])


def with_details(pieces):
    from . import inner_rough

    for obj in pieces:
        yield obj
        detail = inner_rough.detail_of(obj)
        if detail is not None:
            yield detail


def project(obj, mask, scale=2.0, *, native=None):
    """Write only inside-face corners to an owned map. Leave existing layers intact."""
    mesh = obj.data
    if not mask.any():
        return None
    active, render = flags(mesh)
    old_name = layer_name(mesh)
    layer = mesh.uv_layers.get(old_name) if old_name else None
    if layer is None:
        layer = mesh.uv_layers.new(name=NAME, do_init=False)
        mesh[OWNER] = layer.name
    totals = np.empty(len(mesh.polygons), np.int32)
    mesh.polygons.foreach_get('loop_total', totals)
    loop_mask = np.repeat(mask, totals)
    uv = np.empty((len(mesh.loops), 2), np.float32)
    layer.uv.foreach_get('vector', uv.ravel())
    if native is None:
        points = np.empty((len(mesh.vertices), 3), np.float32)
        mesh.vertices.foreach_get('co', points.ravel())
        indices = np.empty(len(mesh.loops), np.int32)
        mesh.loops.foreach_get('vertex_index', indices)
        normals = np.empty((len(mesh.polygons), 3), np.float32)
        mesh.polygons.foreach_get('normal', normals.ravel())
        axes = np.repeat(np.argmax(np.abs(normals), axis=1), totals)[loop_mask]
        uv[loop_mask] = np.take_along_axis(points[indices[loop_mask]],
                                         np.array(((1, 2), (0, 2), (0, 1)))[axes], axis=1) / scale
    else:
        uv[loop_mask] = native[loop_mask]
    layer.uv.foreach_set('vector', uv.ravel())
    copy_flags(mesh, active, render)
    return layer.name
