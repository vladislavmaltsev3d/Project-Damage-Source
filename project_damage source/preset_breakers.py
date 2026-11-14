# подгоняем каттеры пресета под размеры выбранного объекта
# SPDX-License-Identifier: GPL-3.0-or-later
"""Boolean Break cutters for built-in presets, placed to fit the object.

A library preset with breakers does not carry fixed positions: it names the
cutter shapes and a placement rule, and the rule is resolved against the
object it is applied to. The object is first sorted into a shape class from
its bounding box and how much of that box it fills:

  wall          thin, upright (one side much thinner than the other two)
  slab          thin, lying flat (thin along Z)
  column        long and box-like (square pillar, beam)
  round_column  long and round (fills about pi/4 of its box)
  organic       fills little of its box (statues, sculpts, bodies)
  block         everything else (blocks, plinths, props)

The class picks the rule 'auto' means (front face of a wall, top of a slab,
sides of a column, the outermost parts of a statue, the corners of a block),
the size reference (a wall's height, a column's girth, a statue's full
size), and, for the Rebar fill, the reinforcement layout
(wall mesh, column cage, round cage, statue armature).

Cutters are ordinary Add Cutter Primitive objects (same stamp, so the
Boolean panel's Delete removes them), kept in one '<object> Preset Cutters'
collection per object and replaced on every preset apply.
"""
import math
import random

import bmesh
import bpy
from mathutils import Matrix, Vector

from . import boolean_break

STAMP = 'project_damage_preset_cutter'                                    

                                                         
REBAR_BY_CLASS = {'wall': '2', 'slab': '1', 'column': '3', 'round_column': '4', 'organic': '9', 'block': '1'}
AUTO_RULE = {'wall': 'front', 'slab': 'top', 'column': 'side', 'round_column': 'side',
             'organic': 'extremity', 'block': 'corner'}


def _world_mesh(src):
    bm = bmesh.new()
    bm.from_mesh(src.data)
    bm.transform(src.matrix_world)
    bm.normal_update()
    return bm


def analyse(src):
    """(shape class, info dict) of a mesh object, in world space."""
    bm = _world_mesh(src)
    lo = Vector([min(v.co[i] for v in bm.verts) for i in range(3)])
    hi = Vector([max(v.co[i] for v in bm.verts) for i in range(3)])
    dims = hi - lo
    order = sorted(range(3), key=lambda i: dims[i])                              
    d0, d1, d2 = (max(dims[i], 1e-6) for i in order)
    try:
        volume = abs(bm.calc_volume())
    except ValueError:
        volume = 0.0
    fill = volume / (d0 * d1 * d2) if volume > 0.0 else 1.0
    bm.free()
    if d0 / d2 < 0.3 and d0 / d1 < 0.4:
        shape = 'slab' if order[0] == 2 else 'wall'
    elif d2 / d1 > 1.8:
        shape = 'column' if fill >= 0.88 else 'round_column' if fill >= 0.62 else 'organic'
    elif fill < 0.6:
        shape = 'organic'
    else:
        shape = 'block'
                                                            
    ref = {'wall': d1, 'slab': d1, 'column': d1, 'round_column': d1}.get(shape, d2)
    return shape, dict(lo=lo, hi=hi, dims=dims, thin_axis=order[0], long_axis=order[2],
                       thickness=d0, ref=ref, fill=fill)


def _faces(bm, rule, info):
    """Faces a rule may place on, with their areas."""
    thin = info['thin_axis']
    centre = (info['lo'] + info['hi']) * 0.5
    out = []
    for f in bm.faces:
        n = f.normal
        c = f.calc_center_median()
        if rule == 'front':                                                                           
            ok = n[thin] < -0.7
        elif rule == 'back':
            ok = n[thin] > 0.7
        elif rule == 'top':
            ok = n.z > 0.7
        elif rule == 'bottom':
            ok = n.z < -0.7
        elif rule == 'side':                                            
            ok = abs(n.z) < 0.35 and info['lo'].z + 0.15 * info['dims'].z < c.z < info['lo'].z + 0.85 * info['dims'].z
        elif rule == 'upper':                                   
            ok = c.z > info['lo'].z + 0.66 * info['dims'].z
        elif rule == 'lower':
            ok = c.z < info['lo'].z + 0.33 * info['dims'].z and n.z > -0.7
        else:                               
            ok = True
        if ok:
                                                                             
                                                                             
            out.append((f, f.calc_area() * (1.0 + 2.0 * max(0.0, -n.y))))
    return out


def _sample(bm, faces, count, rng, spread, keep_in=None):
    """Area-weighted surface points kept at least `spread` apart (and, with
    keep_in = (lo, hi), inside that box)."""
    total = sum(a for _, a in faces)
    if total <= 0.0:
        return []
    picks = []
    for _ in range(count * 60):
        if len(picks) >= count:
            break
        t = rng.uniform(0.0, total)
        for f, a in faces:
            t -= a
            if t <= 0.0:
                break
        loops = f.loops
        tri = (loops[0].vert.co, loops[len(loops) // 2].vert.co, loops[-1].vert.co) if len(loops) > 3 else \
              tuple(l.vert.co for l in loops)
        u, v = rng.random(), rng.random()
        if u + v > 1.0:
            u, v = 1.0 - u, 1.0 - v
        p = tri[0] + (tri[1] - tri[0]) * u + (tri[2] - tri[0]) * v
                                                                                
        if keep_in is not None and not all(keep_in[0][i] <= p[i] <= keep_in[1][i] for i in range(3)):
            continue
        if all((p - q).length >= spread for q, _ in picks):
            picks.append((p.copy(), f.normal.copy()))
    return picks


def _extremities(bm, count, info, spread):
    """Outermost surface points (head, hands, raised knee): farthest from the
    vertical axis through the centre of mass, or above it, kept apart."""
    centre = (info['lo'] + info['hi']) * 0.5
    verts = sorted(bm.verts, key=lambda v: -((v.co - centre) * Vector((1.0, 1.0, 1.4))).length)
    picks = []
    for v in verts:
        if len(picks) >= count:
            break
        if v.co.z < info['lo'].z + 0.2 * info['dims'].z:                        
            continue
        if all((v.co - q).length >= spread for q, _ in picks):
            picks.append((v.co.copy(), v.normal.copy()))
    return picks


def _corners(bm, count, info, rng):
    lo, hi = info['lo'], info['hi']
    corners = [Vector((x, y, z)) for z in (hi.z, lo.z) for x in (lo.x, hi.x) for y in (lo.y, hi.y)]
                                                                 
    top = sorted(corners[:4], key=lambda c: (c.y, rng.random()))
    rest = sorted(corners[4:], key=lambda c: (c.y, rng.random()))
    picks = []
    for c in (top + rest)[:count]:
        v = min(bm.verts, key=lambda v: (v.co - c).length)
        picks.append((v.co.copy(), (v.co - (lo + hi) * 0.5).normalized()))
    return picks


def _clear(src):
    p = src.project_damage
    for obj in [o for o in bpy.data.objects if o.get(STAMP) == src.name]:
        mesh = obj.data if obj.type == 'MESH' else None
        bpy.data.objects.remove(obj)
        if mesh is not None and mesh.users == 0:
            bpy.data.meshes.remove(mesh)
    for i in reversed(range(len(p.breakers))):
        row = p.breakers[i]
        if row.obj is None and not getattr(row, 'from_collection', False):
            p.breakers.remove(i)
    p.breaker_index = 0


def clear(src):
    """Remove the cutters an earlier library preset made on src."""
    if any(o.get(STAMP) == src.name for o in bpy.data.objects):
        _clear(src)


def _collection(src):
    name = '%s Preset Cutters' % src.name
    col = bpy.data.collections.get(name)
    if col is None:
        col = bpy.data.collections.new(name)
        (src.users_collection[0] if src.users_collection else bpy.context.scene.collection).children.link(col)
    return col


def _add_cutter(src, col, shape, r, centre, normal, rng, stretch=(1.0, 1.0, 1.0)):
    mesh = boolean_break.primitive_mesh(shape, r, seed=rng.randrange(1, 10 ** 6))
    obj = bpy.data.objects.new('Preset Cutter %s' % shape.title(), mesh)
    mesh.name = obj.name
    col.objects.link(obj)
    if shape in {'PLANE', 'CYLINDER', 'CONE'}:
                                                                              
        rot = normal.to_track_quat('Z', 'Y').to_matrix().to_4x4()
    else:
        rot = Matrix.Rotation(rng.uniform(0, 2 * math.pi), 4, 'Z') @ Matrix.Rotation(rng.uniform(-0.5, 0.5), 4, 'X')
    scale = Matrix.Diagonal(Vector(stretch).to_4d())
    obj.matrix_world = Matrix.Translation(centre) @ rot @ scale
    obj[boolean_break.PRIMITIVE_KEY] = shape
    obj[STAMP] = src.name
    if shape == 'PLANE':
        obj.project_damage_breaker.custom_sheet = '0'
    boolean_break.link_cutter(src, obj)
    return obj


def place(src, spec, seed=0, *, replace_existing=True):
    """Make the cutters of a preset's breakers spec on src. Returns the shape class."""
    if replace_existing:
        _clear(src)
    shape, info = analyse(src)
    p = src.project_damage
    variant = spec.get('variant', 'SIMPLE')
    p.break_variant = variant
    if variant == 'REBAR':
        rebar = spec.get('rebar', 'auto')
        p.rebar_preset = REBAR_BY_CLASS[shape] if rebar == 'auto' else rebar
    rng = random.Random(seed + 7)
    col = _collection(src)
    bm = _world_mesh(src)
    try:
        for cut in spec.get('cutters', ()):
            rule = cut.get('rule', 'auto')
            if rule == 'auto':
                rule = AUTO_RULE[shape]
            if rule == 'front' and shape == 'slab':
                rule = 'top'
            r = max(cut.get('size', 0.15) * info['ref'], 1e-3)
            count = cut.get('count', 1)
            if cut.get('through'):                                                                 
                r = max(r, 0.75 * info['thickness'])
            if rule == 'slice':
                                                                          
                                                                                       
                axis = Vector((0.0, 0.0, 0.0))
                axis[info['long_axis']] = 1.0
                at = cut.get('at', 0.75)
                centre = info['lo'] + Vector(info['dims'][i] * (at if i == info['long_axis'] else 0.5) for i in range(3))
                                                                                              
                perp = axis.orthogonal().normalized()
                n = Matrix.Rotation(rng.uniform(0, 2 * math.pi), 3, axis) @ (Matrix.Rotation(cut.get('tilt', 0.35), 3, perp) @ axis)
                n.normalize()
                half = 0.75 * max(info['dims'])
                _add_cutter(src, col, 'PLANE', half, centre, n, rng)
                continue
            spread = cut.get('spread', 1.6) * r
            if rule == 'extremity':
                picks = _extremities(bm, count, info, spread)
            elif rule == 'corner':
                picks = _corners(bm, count, info, rng)
            else:
                faces = _faces(bm, rule, info) or _faces(bm, 'any', info)
                keep_in = None
                if rule in {'front', 'back', 'top', 'bottom'}:
                                                                               
                                                                                  
                    pad = Vector([min(1.2 * r, 0.45 * info['dims'][i]) for i in range(3)])
                    keep_in = (info['lo'] + pad, info['hi'] - pad)
                    free = 2 if rule in {'top', 'bottom'} else info['thin_axis']                        
                    keep_in[0][free] = info['lo'][free] - 1.0
                    keep_in[1][free] = info['hi'][free] + 1.0
                picks = _sample(bm, faces, count, rng, spread, keep_in)
            depth = cut.get('depth', 0.35)
            for point, normal in picks:
                rr = r * rng.uniform(0.8, 1.2) if count > 1 else r
                if cut.get('through') and shape in {'wall', 'slab'}:
                                                                           
                    centre = point.copy()
                    centre[info['thin_axis']] = (info['lo'][info['thin_axis']] + info['hi'][info['thin_axis']]) * 0.5
                else:
                    centre = point - normal * depth * rr
                _add_cutter(src, col, cut.get('shape', 'ROCK'), rr, centre, normal, rng,
                            cut.get('stretch', (1.0, 1.0, 1.0)))
    finally:
        bm.free()
    for key, value in spec.get('break_props', {}).items():
        setattr(p, key, value)
    return shape
