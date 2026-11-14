# находим разницу исходной и повреждённой формы для Exact Matcher
"""Restore missing solid volume from the original and the selected damage cuts."""
import time

import bmesh
import bpy
import numpy as np

from . import background_compute, build, damage_physics as dp

SOLID_MODULES = ('edge', 'corner', 'surface', 'concrete', 'brick', 'lath', 'hit',
                 'bullet', 'hang', 'boolean', 'dbevel', 'erosion')
MODULE_TAG = 'project_damage_exact_modules'
SEPARATED_TAG = 'project_damage_exact_separated'
_NATIVE_CUTS = {
    'concrete': 'concrete', 'brick': 'brick', 'lath': 'lath', 'hit': 'hit',
    'bullet': 'bullet', 'hang': 'hang', 'damaged_bevel': 'dbevel', 'erosion': 'erosion',
    'wood': 'wood', 'scratch': 'scratch',
}


def enabled_modules(props):
    if props.all_off:
        return ()
    cuts = tuple(key for key in SOLID_MODULES if dp.module_enabled(props, key))
    return cuts + tuple(key for key in ('wood', 'scratch') if getattr(props, key + '_enable'))


def filter_params(params, modules, inputs):
    """Keep the same cutter settings, with unrelated stages removed from the build."""
    for block, key in _NATIVE_CUTS.items():
        if key not in modules:
            getattr(params, block).enable = False
    for block in ('deform', 'plaster', 'pits', 'metal', 'metal_tear', 'clay', 'fabric',
                  'ground', 'fissure', 'rusty', 'wallpaper', 'glass', 'wall_hole',
                  'blast', 'bevel', 'debris', 'rbf', 'cellf'):
        getattr(params, block).enable = False
    if not ('boolean' in modules or ('hang' in modules and params.hang.mode == 1)):
        params.breakers = []
    # Framing and Wooden Chips change the wall cut as well as spawning
    # inserts. Keep their carve. The inserted pieces are separate outputs.
    reinforcement = params.break_rebar
    reinforcement.enable = (reinforcement.enable and 'boolean' in modules
                            and reinforcement.material in {3, 4}
                            and not reinforcement.existing_damage)
    params.hang.bars_enable = False
    params.hang.cables_enable = False
    params.concrete.rebar_enable = False
    if 'edge' not in modules:
        params.amount = 0
        inputs.pop('chip_strokes', None)
        inputs.pop('chip_paint_radius', None)
    if 'corner' not in modules:
        params.corner_prob = 0
        params.corner_paint_only = False
    if 'surface' not in modules:
        params.surf_amount = 0
        params.surf_paint_only = False
    for name in ('fract1', 'fract2', 'fract3'):
        getattr(params, name).amount = 0
    params.fracture_layers = []
    params.fract_proximity = []
    params.fract_type = 0
    params.fract_paint_only = False
    params.fract_through = False
    params.fract_physics = False
    params.fract_shard_enable = False
    params.fract_tile_offset = 0
    params.build_coverage = 0
    params.build_corner_prob = 0
    params.build_paint_only = False
    params.deposit_amount = 0
    params.coating_enable = False
    params.rust_enable = False
    params.heat_enable = False


def _release(mesh):
    if mesh is not None and mesh.users == 0:
        bpy.data.meshes.remove(mesh)


def _close_and_orient(mesh):
    """Repair winding on the solid input while preserving nested cavity orientation."""
    bm = bmesh.new()
    try:
        bm.from_mesh(mesh)
        loose = [v for v in bm.verts if not v.link_faces]
        if loose:
            bmesh.ops.delete(bm, geom=loose, context='VERTS')
        if not bm.faces or not dp._closed(bm):
            return False
        if any(not edge.is_contiguous for edge in bm.edges):
            bmesh.ops.recalc_face_normals(bm, faces=list(bm.faces))
        if bm.calc_volume(signed=True) < 0:
            bmesh.ops.reverse_faces(bm, faces=list(bm.faces))
        bm.to_mesh(mesh)
        mesh.update()
        return True
    finally:
        bm.free()


def _difference(context, source, base, operands):
    solvers = ('MANIFOLD', 'EXACT') if dp._manifold_solver() else ('EXACT',)
    messages = []
    for solver in solvers:
        trial = base.copy()
        try:
            difference, _ = dp._boolean(context, trial, source, operands, solver=solver)
        except (ValueError, RuntimeError) as exc:
            _release(trial)
            messages.append(str(exc))
            continue
        if not difference.polygons or dp._mesh_closed(difference):
            return difference
        _release(difference)
        messages.append(solver + ' returned an open difference')
    raise ValueError(messages[-1] if messages else 'could not calculate a closed difference')


def _components(mesh, minimum, thickness):
    bm = bmesh.new()
    try:
        bm.from_mesh(mesh)
        parts = dp._solid_islands(bm)
        accepted = []
        for ids in parts:
            vertices = {v for i in ids for v in bm.faces[i].verts}
            origin = next(iter(vertices)).co
            volume = 0.0
            area = 0.0
            for i in ids:
                face = bm.faces[i]
                area += face.calc_area()
                points = [v.co - origin for v in face.verts]
                volume += sum(points[0].dot(points[j].cross(points[j+1])) / 6
                              for j in range(1, len(points)-1))
            if volume > minimum and 2 * volume >= thickness * area:
                accepted.append(ids)
        return accepted
    finally:
        bm.free()


def _span(data, faces):
    return data.hi[faces].max(axis=0) - data.lo[faces].min(axis=0)


def _without_surface_bridges(context, source, base, body, raw, data, parts, minimum):
    """Retry coincident surfaces when one fill runs across the whole input."""
    from .fill_v2 import MeshData

    co = np.empty((len(base.vertices), 3), np.float32)
    base.vertices.foreach_get('co', co.ravel())
    extent = np.ptp(co, axis=0)
    largest = max(parts, key=len)
    if not np.all(_span(data, largest) >= 0.9 * extent):
        return None

    # One mesh-scale float tolerance clears thin coplanar bridges without
    # rounding off the visible damage. The original exterior stays fixed.
    tolerance = float(np.linalg.norm(extent)) * 2e-7
    grown = dp._grown(context, body, tolerance)
    alternate = None
    try:
        for material in body.data.materials:
            grown.data.materials.append(material)
        alternate = _difference(context, source, base, [grown])
        revised = _components(alternate, minimum, 0.0)
        if len(revised) < len(parts) + max(1, len(parts)//4):
            return None
        before, after = dp._mass(raw)[0], dp._mass(alternate)[0]
        if before <= 0 or abs(after - before) > before * 0.005:
            return None
        new_data = MeshData(alternate)
        if np.all(_span(new_data, max(revised, key=len)) >= 0.9 * extent):
            return None
        result = alternate
        alternate = None
        return result, new_data, revised
    except (ValueError, RuntimeError):
        return None
    finally:
        dp._remove(grown)
        _release(alternate)


def _legacy_operands(context, source, module, label, temps):
    """The two original commands still honor their fill-depth and socket rules."""
    ignored = {'project_damage_bfrag_of'}
    if label == 'Fill V2':
        ignored.update(('project_damage_rebar_of', 'project_damage_pipes_of', 'project_damage_cables_of'))
    uid = dp._uid(source)
    for obj in dp._outputs_inside(source):
        if any(obj.get(key) == uid for key in ignored):
            continue
        operand = dp._grown(context, obj, 0.0)
        if dp._mesh_closed(operand.data):
            temps.append(operand)
        else:
            dp._remove(operand)
    depth = dp._auto_depth(context, source, module, dp.settings(source))
    if depth is not None:
        inner = dp._inner_body(context, source, depth)
        if inner is not None:
            temps.append(inner)


def _fill(context, source, modules, *, legacy=None):
    from . import fill_surface, fill_v2
    from .params import unit_factor

    start = time.perf_counter()
    label = legacy or 'Exact Matcher'
    base = difference = None
    temps, pending, created = [], [], []
    try:
        body = fill_surface._solid_body(context, source, fill_modules=modules)
        if body is not None:
            temps.append(body)
        base = dp._fill_base(context, source, solid_damage=True)
        if not _close_and_orient(base):
            raise ValueError('the original mesh could not be prepared as a closed volume')
        if not len(base.materials):
            base.materials.append(None)
        base.materials.append(dp._inner_material(source))
        if legacy:
            _legacy_operands(context, source, modules[0], legacy, temps)
        for operand in temps:
            mesh = operand.data
            for name, kind, domain in (('_pd_fill_outer', 'INT', 'FACE'),
                                       ('_pd_fill_outer_normal', 'FLOAT_VECTOR', 'CORNER')):
                mesh.attributes.new(name, kind, domain)
            mesh.materials.clear()
            mesh.materials.append(base.materials[-1])
        difference = _difference(context, source, base, temps) if temps else base.copy()
        factor = unit_factor(context)
        # Only discard numerical dust in Exact Matcher. The two legacy
        # commands retain the user's minimum volume and thin-crack settings.
        minimum = 1e-6 / factor ** 3
        thickness = 0.0
        if legacy:
            p, s = source.project_damage, dp.settings(source)
            scale = dp._gscale(p) / factor
            minimum = max(minimum, p.debris_min_volume * scale ** 3)
            thickness = 0.0 if s.fill_cracks else p.debris_min_thickness * scale
            if modules[0] == 'dbevel':
                minimum, thickness = min(minimum, 1e-10), 0.0
            if modules[0] == 'hit':
                thickness = min(thickness, 0.3 * max(0.0, p.hit_layer_thickness) * scale)
            if modules[0] == 'bullet':
                minimum *= 0.2
                thickness *= 0.5
        parts = _components(difference, minimum, thickness)
        if not parts:
            raise ValueError('no closed removed volume was found')
        data = fill_v2.MeshData(difference)
        cleared_bridges = False
        if not legacy and body is not None:
            corrected = _without_surface_bridges(context, source, base, body, difference, data, parts, minimum)
            if corrected is not None:
                _release(difference)
                difference, data, parts = corrected
                cleared_bridges = True
        if legacy:
            pending = [data.extract(ids, '_pd_recovered_fill') for ids in parts]
        else:
            pending = [data.extract([i for ids in parts for i in ids], '_pd_exact_fill')]
        if not all(dp._mesh_closed(mesh) for mesh in pending):
            raise ValueError('the difference contains an open island')
        if legacy:
            previous = dp.fills_of(source, modules[0])
        else:
            previous = [o for o in dp.fills_of(source) if o.get(MODULE_TAG) or o.get(dp.FILL_KIND) in modules]
        coll = dp._collection(context, source, dp.FILL_COLLECTION, '_DamageFill')
        prefix = '_FillExact' if not legacy else '_FillV2' if legacy == 'Fill V2' else '_Fill'
        for mesh in pending:
            obj, volume = dp._make_object(source.name + prefix + '_%03d' % (len(created) + 1), mesh, coll, source)
            created.append(obj)
            obj[dp.FILL_STAMP] = dp._uid(source)
            obj[dp.FILL_KIND] = modules[0] if legacy else 'exact'
            if legacy == 'Fill V2':
                obj['project_damage_fill_v2'] = True
            if not legacy:
                obj[MODULE_TAG] = list(modules)
        for obj in previous:
            dp._remove(obj)
        status = '%s: %d closed volume(s) in %.2f s' % (label, len(parts), time.perf_counter() - start)
        if legacy:
            status += ' (recovered from the isolated cut)'
        if cleared_bridges:
            status += ' (removed coincident surface bridges)'
        return created, [({'INFO'}, status)]
    except (ValueError, RuntimeError) as exc:
        for obj in created:
            dp._remove(obj)
        return [], [({'WARNING'}, '%s: %s. Previous fills were kept' % (label, exc))]
    finally:
        for obj in temps:
            dp._remove(obj)
        for mesh in (base, difference, *pending):
            try:
                _release(mesh)
            except ReferenceError:
                pass


# сравниваем исходный объём с повреждённым результатом
@dp._operation(weld=False)
def fill(context, source):
    reason = dp.fill_block_reason(source)
    if reason:
        return [], [({'WARNING'}, reason)]
    if build.result_of(source) is None:
        return [], [({'WARNING'}, 'Exact Matcher: build the damage first')]
    modules = enabled_modules(source.project_damage)
    if not modules:
        return [], [({'WARNING'}, 'Exact Matcher: no supported solid damage effects are active')]
    return _fill(context, source, modules)


def combined_fill(fills):
    exact = [obj for obj in fills if obj.type == 'MESH' and obj.get(MODULE_TAG)]
    if len(exact) == 1 and not exact[0].get(SEPARATED_TAG):
        return exact[0]
    return None


@dp._operation(weld=False)
def separate(context, source, module):
    from .fill_v2 import MeshData

    combined = combined_fill(dp.fills_of(source, module))
    if combined is None:
        return [], [({'WARNING'}, 'Run Exact Matcher to create a combined fill first')]

    bm = bmesh.new()
    try:
        bm.from_mesh(combined.data)
        if not bm.faces or not dp._closed(bm):
            return [], [({'WARNING'}, 'The Exact Matcher fill must be a closed mesh')]
        parts = dp._solid_islands(bm)
    finally:
        bm.free()
    if len(parts) == 1:
        combined[SEPARATED_TAG] = True
        return [combined], [({'INFO'}, 'The Exact Matcher fill already contains one fragment')]

    data = MeshData(combined.data)
    normals = np.empty((len(combined.data.loops), 3), np.float32)
    combined.data.corner_normals.foreach_get('vector', normals.ravel())
    meshes, created = [], []
    matrix = combined.matrix_world.copy()
    collections = list(combined.users_collection)
    if not collections:
        collections = [dp._collection(context, source, dp.FILL_COLLECTION, '_DamageFill')]
    hidden = combined.hide_get()
    name = combined.name
    try:
        for ids in parts:
            mesh = data.extract(ids, '_pd_exact_fragment')
            meshes.append(mesh)
            if not dp._mesh_closed(mesh):
                raise ValueError('a separated fragment would be open')
            loops = np.concatenate([np.arange(data.start[i], data.start[i] + data.total[i]) for i in ids])
            mesh.normals_split_custom_set(normals[loops])
        for i, mesh in enumerate(meshes, 1):
            obj, _ = dp._make_object(f'{name}_Fragment_{i:03d}', mesh, collections[0], source,
                                     local_matrix=matrix, apply_inner=False)
            created.append(obj)
            for collection in collections[1:]:
                collection.objects.link(obj)
            obj.data.name = obj.name
            obj[dp.FILL_STAMP] = dp._uid(source)
            obj[dp.FILL_KIND] = 'exact'
            obj[MODULE_TAG] = list(combined[MODULE_TAG])
            obj[SEPARATED_TAG] = True
            obj.hide_viewport = combined.hide_viewport
            obj.hide_render = combined.hide_render
            if obj.name in context.view_layer.objects:
                obj.hide_set(hidden)
    except (ValueError, RuntimeError) as exc:
        for obj in created:
            dp._remove(obj)
        for mesh in meshes:
            try:
                _release(mesh)
            except ReferenceError:
                pass
        return [], [({'WARNING'}, 'Separate Fragments: %s. The combined fill was kept' % exc)]

    if combined.name in context.view_layer.objects:
        combined.select_set(False, view_layer=context.view_layer)
    dp._remove(combined)
    context.view_layer.update()
    return created, [({'INFO'}, 'Separated Exact Matcher fill into %d objects' % len(created))]


def recover(context, source, module, label):
    if module not in enabled_modules(source.project_damage):
        return None
    objects, reports = _fill(context, source, (module,), legacy=label)
    return (objects, reports) if objects else None
