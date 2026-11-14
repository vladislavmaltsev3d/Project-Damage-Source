# подготавливаем открытые поверхности к восстановлению объёма
"""On-demand Fill for open inputs whose temporary solid is hidden after generation."""
import time

import bpy
import bmesh
import numpy as np

from . import damage_physics as dp, build, background_compute as bg, effect_groups


def _surface_plan(context, source):
    from . import input_prep, boolean_break
    from .params import unit_factor
    bodies = [build.result_of(source), *boolean_break.fragments_of(source)]
    if all(obj is None or dp._mesh_closed(obj.data) for obj in bodies):
        return None
    prepared = input_prep.plan(context, source, unit_factor(context))
    if prepared is None or not np.any(prepared[1].helper):
        return None
    return prepared


def _solid_body(context, source, *, fill_modules=None):
    capture, reports = bg.immediate(build._rebuild_steps(
        context, source, capture_solid=True, fill_modules=fill_modules))
    if capture is None:
        message = '; '.join(text for levels, text in reports if 'ERROR' in levels or 'WARNING' in levels)
        raise ValueError(message or 'could not reconstruct the prepared damage volume')
    out, prep, factor, points, polygons = capture
    if fill_modules is not None and not out.get('input_valid'):
        raise ValueError('the damage input could not be prepared as a closed volume')
    if fill_modules is not None and out.get('passthrough'):
        raise ValueError('the active effects remove no volume')
    if not len(out.get('polys', ())):
        if fill_modules is not None:
            return None
        raise ValueError('the prepared damage result has no faces')
    mesh = build._result_mesh('_pd_fill_solid', out['points'] / factor, out['polys'], None)
    try:
        ids = np.asarray(out.get('effect_ids', ()), np.int32)
        if len(ids) == len(mesh.polygons):
            mesh.attributes.new(effect_groups.ATTRIBUTE, 'INT', 'FACE').data.foreach_set('value', ids)
        p = source.project_damage
        from . import displacement_shader
        smooth_effects = any(getattr(p, displacement_shader._field(key, 'enable'))
                             and getattr(p, displacement_shader._field(key, 'smooth')) > 0
                             for key in displacement_shader.EFFECTS)
        smooth_boolean = (fill_modules is None or bool({'boolean', 'hang'}.intersection(fill_modules))) and p.break_smooth_inside > 0
        if smooth_boolean or smooth_effects:
            visible = prep.finish(out) if prep is not None else out
            surface = build._result_mesh('_pd_fill_surface', visible['points'] / factor, visible['polys'], None)
            try:
                effect_groups.tag_mesh(surface, visible, p)
                hanging = p.hang_enable and p.hang_mode == '1'
                shader_pin = p.ui_boolean_enable and p.break_shader_displacement
                if (p.ui_boolean_enable or hanging) and smooth_boolean:
                    from . import break_surface
                    pin = (p.hang_shader_pin_distance if hanging and p.hang_shader_pin_enable and not shader_pin
                           else p.break_displacement_pin)
                    bg.immediate(break_surface.finish_cut_surface_steps(
                        surface, source.matrix_world, factor, pin, p.break_smooth_inside,
                        write_mask=False, rim_damage=False))
                bg.immediate(displacement_shader.finish_effects(source, surface, factor, points, polygons))
                keep = np.ones(len(out['polys']), bool)
                if prep is not None and prep.helper.any() and not out.get('passthrough'):
                    base = np.asarray(out['base_poly'])
                    traced = base >= 0
                    keep[traced] = ~prep.helper[base[traced]]
                used = np.unique(np.asarray(out['polys'])[keep])
                coords = np.asarray(out['points'] / factor, np.float32).copy()
                moved = np.empty((len(surface.vertices), 3), np.float32)
                surface.vertices.foreach_get('co', moved.ravel())
                if prep is None or not prep.helper.any() or out.get('passthrough'):
                    used = np.arange(len(coords))
                if len(used) != len(moved):
                    raise ValueError('prepared surface mapping changed; update the damage first')
                coords[used] = moved
                mesh.vertices.foreach_set('co', coords.ravel())
                mesh.update()
            finally:
                bpy.data.meshes.remove(surface)
        if not dp._mesh_closed(mesh):
            raise ValueError('the prepared damage volume is not closed')
        return dp._temp_object(context, '_pd_fill_solid', mesh, source)
    except Exception:
        if not mesh.users:
            bpy.data.meshes.remove(mesh)
        raise


def try_fill(context, source, module, *, v2=False, all_damage=False):
    """Return None for the existing closed-input path; otherwise commit only valid fills."""
    label = 'Fill V2' if v2 else 'Fill Damage'
    try:
        if _surface_plan(context, source) is None:
            return None
    except ValueError as exc:
        return [], [({'WARNING'}, '%s: %s; previous fills were kept' % (label, exc))]
    from . import live, ownership
    if source.get('project_damage_uid') in live._dirty:
        return [], [({'WARNING'}, label + ': press Update before filling this open surface; previous fills were kept')]
    started = time.perf_counter()
    p = source.project_damage
    s = dp.settings(source)
    from .params import unit_factor
    factor = unit_factor(context)
    g = dp._gscale(p)
    minimum = max(1e-9, p.debris_min_volume * g ** 3) / factor ** 3
    thickness = 0.0 if s.fill_cracks else p.debris_min_thickness * g / factor
    if module == 'dbevel':
        thickness = 0.0
        minimum = min(minimum, 1e-10)
    if module == 'hit':
        thickness = min(thickness, 0.3 * max(0.0, p.hit_layer_thickness) * g / factor)
    if module == 'bullet':
        minimum *= 0.2
        thickness *= 0.5
    temps, pending, created, reports = [], [], [], []
    base = result = None
    committed = False
    try:
        body = _solid_body(context, source)
        temps.append(body)
        base = dp._fill_base(context, source, triangulate=False)
        if not dp._mesh_closed(base):
            raise ValueError('the prepared source volume is not closed')
        if not len(base.materials):
            base.materials.append(None)
        base.materials.append(dp._inner_material(source))
        from .fill_v2 import MeshData, valid_piece
        data = MeshData(base)
        epsilon = max(1e-7, float(np.linalg.norm(np.ptp(data.co, axis=0))) * 2e-6)
        grown = dp._grown(context, body, epsilon)
        temps.append(grown)
        operands = [grown]
        ignored = ('project_damage_bfrag_of',)
        if v2:
            ignored += ('project_damage_rebar_of', 'project_damage_pipes_of', 'project_damage_cables_of')
        inside = [obj for obj in dp._outputs_inside(source)
                  if not any(obj.get(stamp) == dp._uid(source) for stamp in ignored)]
        for obj in inside:
            operand = dp._grown(context, obj, epsilon / max(1e-9, min(obj.matrix_world.to_scale())))
            temps.append(operand)
            operands.append(operand)
        depth = None if all_damage else dp._auto_depth(context, source, module, s)
        if depth is not None:
            inner = dp._inner_body(context, source, depth)
            if inner is None:
                reports.append(({'WARNING'}, label + ': this open source cannot support Surface Layer depth; filled Whole Cut'))
            else:
                temps.append(inner)
                operands.append(inner)
        for operand in operands:
            mesh = operand.data
            for name in ('_pd_fill_outer', '_pd_fill_outer_normal'):
                attr = mesh.attributes.get(name)
                if attr is not None:
                    mesh.attributes.remove(attr)
            mesh.attributes.new('_pd_fill_outer', 'INT', 'FACE')
            mesh.attributes.new('_pd_fill_outer_normal', 'FLOAT_VECTOR', 'CORNER')
        result, solver = dp._boolean(context, base, source, operands)
        base = None
        if solver != 'MANIFOLD':
            raise ValueError('one of the inner objects is not a closed solid')
        bm = bmesh.new()
        try:
            bm.from_mesh(result)
            wanted = (lambda ids: True) if all_damage else dp._effect_parts(bm, module)
            parts = [ids for ids in dp._solid_islands(bm) if wanted(ids)]
        finally:
            bm.free()
        data = MeshData(result)
        outer = next((values.ravel() for name, domain, kind, field, values in data.attrs
                      if name == '_pd_fill_outer'), None)
        for ids in parts:
            if outer is None or not outer[ids].any():
                continue
            piece = data.extract(ids, '_pd_surface_fill')
            if valid_piece(piece, minimum, thickness):
                pending.append(piece)
            else:
                bpy.data.meshes.remove(piece)
        if not pending:
            raise ValueError('no closed damage volumes above the minimum size')
        previous = dp.fills_of(source, None if all_damage else module)
        coll = dp._collection(context, source, dp.FILL_COLLECTION, '_DamageFill')
        for mesh in pending:
            obj, volume = dp._make_object(source.name + ('_FillV2_' if v2 else '_Fill_') + '%03d' % (len(created) + 1),
                                         mesh, coll, source)
            created.append(obj)
            obj[dp.FILL_STAMP] = dp._uid(source)
            obj[dp.FILL_KIND] = module
            ownership.bind(source, obj)
            if v2:
                obj['project_damage_fill_v2'] = True
        for obj in previous:
            dp._remove(obj)
        committed = True
        reports.append(({'INFO'}, '%s: %d fills from the prepared surface volume in %.2f s' % (
            label, len(created), time.perf_counter() - started)))
        return created, reports
    except (ValueError, RuntimeError) as exc:
        return [], reports + [({'WARNING'}, '%s: %s; previous fills were kept' % (label, exc))]
    finally:
        if not committed:
            for obj in created:
                dp._remove(obj)
        for obj in temps:
            dp._remove(obj)
        for mesh in [base, result, *pending]:
            try:
                if mesh is not None and mesh.users == 0:
                    bpy.data.meshes.remove(mesh)
            except ReferenceError:
                pass
