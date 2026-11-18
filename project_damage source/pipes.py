# трубы внутри повреждения: профиль, стыки, изгибы и обломанные концы
from . import quality_ui
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pipes in the wall (2026-10-26).

* Boolean Pipes: a sixth Boolean Break type. Pipe runs are laid out inside
  the object; where the breakers cut through them the pipes show in the
  break, held by the solid that is left.
* Pipe Damage: a Walls effect built on the Concrete Damage core. The spalls
  are chased along the pipe runs (or along your strokes, which then are the
  pipe routes), deep enough to uncover the pipes.

The pipes are hollow (outer and inner wall, the wall thickness shows at a
broken end). Where a pipe leaves the concrete at the edge of a hole it can
bow, snap with a gap, bend outward from the hole edge, get dented, and end in
a jagged or torn rim. Conduit carries wires that hang out of a broken end or
sag across a gap. Every piece is its own closed mesh island in one output
object `<source>_project_damage_pipes`, which follows the source.

Python only: no core rebuild needed. Pipe Damage drives the existing
Concrete Damage core (paint strokes), Boolean Pipes the existing breaker cut."""
from . import output_collections
from .blender_compat import enable_nodes
from . import ui_image_hints
import math

import bpy
from . import material_controls
import numpy as np
from mathutils import Matrix, Vector
from mathutils.bvhtree import BVHTree

from . import live, ownership

PIPES_STAMP = 'project_damage_pipes_of'

                         
TYPE_ITEMS = (
    ('PVC', 'PVC', 'Plastic water or drain pipe: brittle, snaps into a jagged shard edge'),
    ('COPPER', 'Copper', 'Thin copper water pipe: bends easily, torn and squashed ends'),
    ('STEEL', 'Steel', 'Galvanised steel pipe with threaded couplings: stiff, bends little'),
    ('CAST_IRON', 'Cast Iron', 'Thick rusty soil pipe with socket hubs: breaks with ragged ends'),
    ('CONDUIT', 'Conduit', 'Corrugated electrical conduit with wires inside: sags and tears'),
)
# per type: default diameter / wall (cm), joint spacing (cm), how much it bends,
                                                                        
                                               
TYPES = {
    'PVC': dict(d=5.0, wall=0.32, joints=100.0, bend=0.3, jag=1.0, dent=0.0, gap=1.6, flare=0.0, corr=0.0,
                joint=(1.13, 1.6), wires=False, sag=0.0, color=(0.82, 0.82, 0.80), metal=0.0, rough=0.42),
    'COPPER': dict(d=2.2, wall=0.1, joints=80.0, bend=1.0, jag=0.35, dent=0.7, gap=0.7, flare=0.18, corr=0.0,
                   joint=(1.09, 1.1), wires=False, sag=0.0, color=(0.80, 0.40, 0.22), metal=1.0, rough=0.38),
    'STEEL': dict(d=3.4, wall=0.33, joints=120.0, bend=0.45, jag=0.3, dent=0.35, gap=0.9, flare=0.08, corr=0.0,
                  joint=(1.2, 1.25), wires=False, sag=0.0, color=(0.52, 0.53, 0.55), metal=1.0, rough=0.5),
    'CAST_IRON': dict(d=10.0, wall=0.6, joints=150.0, bend=0.12, jag=0.9, dent=0.0, gap=1.2, flare=0.0, corr=0.0,
                      joint=(1.22, 0.9), wires=False, sag=0.0, color=(0.16, 0.09, 0.06), metal=0.55, rough=0.85),
    'CONDUIT': dict(d=2.5, wall=0.12, joints=0.0, bend=0.85, jag=0.6, dent=0.2, gap=0.8, flare=0.0, corr=1.0,
                    joint=(1.0, 1.0), wires=True, sag=1.0, color=(0.36, 0.37, 0.39), metal=0.0, rough=0.6),
}
LAYOUT_ITEMS = (
    ('H', 'Horizontal', 'Runs along the wall'),
    ('V', 'Vertical', 'Risers from floor to ceiling'),
    ('BOTH', 'Both', 'Horizontal runs and risers'),
)
AXIS_ITEMS = (
    ('AUTO', 'Auto', 'The thinnest side of the object is the wall normal'),
    ('X', 'X', ''), ('Y', 'Y', ''), ('Z', 'Z', ''),
)
AXES_ITEMS = (('X', 'X', 'Faces across X', 1), ('Y', 'Y', 'Faces across Y', 2), ('Z', 'Z', 'Faces across Z', 4))
_BITS = {'X': 1, 'Y': 2, 'Z': 4}


def _axes_bits(self, prefix):
    """Zero in legacy scenes means to read the single axis; -1 means empty."""
    bits = int(self.get(prefix + '_axes_bits', 0))
    if bits < 0:
        return 0
    if bits:
        return bits
    old = getattr(self, prefix + '_axis', 'AUTO')
    return _BITS.get(old, _BITS['Y'])


def axis_props(prefix):
    """Wall Normal with several axes (shared with Cables): <prefix>_axis_auto
    over the old <prefix>_axis enum, <prefix>_axes (ENUM_FLAG) and the toggles
    <prefix>_axis_x/_y/_z, all over the hidden <prefix>_axes_bits."""
    def get_auto(self):
        return getattr(self, prefix + '_axis', 'AUTO') == 'AUTO'

    def set_auto(self, value):
        if value:
            if not int(self.get(prefix + '_axes_bits', 0)):
                self[prefix + '_axes_bits'] = _axes_bits(self, prefix)                        
            setattr(self, prefix + '_axis', 'AUTO')
        else:
            bits = _axes_bits(self, prefix)
            setattr(self, prefix + '_axis', next((k for k in 'XYZ' if bits & _BITS[k]), 'Y'))

    def get_axes(self):
        return _axes_bits(self, prefix)

    def set_axes(self, value):
        bits = int(value) & 7
        self[prefix + '_axes_bits'] = bits or -1
        if bits and not get_auto(self):
            setattr(self, prefix + '_axis', next(k for k in 'XYZ' if bits & _BITS[k]))

    def toggle(k):
        def get(self):
            return bool(_axes_bits(self, prefix) & _BITS[k])

        def set(self, value):
            bits = _axes_bits(self, prefix)
            bits = (bits | _BITS[k]) if value else (bits & ~_BITS[k])
            set_axes(self, bits)
        return get, set
    out = {
        prefix + '_axes_bits': bpy.props.IntProperty(default=0, min=-1, max=7, options={'HIDDEN'}),
        prefix + '_axis_auto': bpy.props.BoolProperty(
            name='Auto', get=get_auto, set=set_auto, update=live.touch_prop,
            description='Pick the wall normal by itself (each break or painted area takes its own face). '
                        'Off: choose the axes'),
        prefix + '_axes': bpy.props.EnumProperty(
            name='Axes', items=AXES_ITEMS, options={'ENUM_FLAG'}, get=get_axes, set=set_axes,
            update=live.touch_prop, description='The axes whose faces get runs'),
    }
    for k in 'XYZ':
        g, st = toggle(k)
        out['%s_axis_%s' % (prefix, k.lower())] = bpy.props.BoolProperty(
            name=k, get=g, set=st, update=live.touch_prop,
            description='Runs behind the faces across %s (click to turn on or off; several can be on)' % k)
    return out


def axes_of(p, prefix='pipe'):
    """None for Auto, else the selected local axis indices, e.g. (0, 1)."""
    if getattr(p, prefix + '_axis', 'AUTO') == 'AUTO':
        return None
    bits = _axes_bits(p, prefix)
    return tuple(i for i, k in enumerate('XYZ') if bits & _BITS[k])


def draw_axes(layout, p, prefix='pipe'):
    """Wall Normal: Auto, and the X Y Z toggles only when Auto is off."""
    row = layout.row(align=True)
    ui_image_hints.prop(row,p, prefix + '_axis_auto', text='Auto Wall Normal', toggle=True)
    if not getattr(p, prefix + '_axis_auto'):
        for k in 'xyz':
            row.prop(p, '%s_axis_%s' % (prefix, k), toggle=True)
        if not _axes_bits(p, prefix):
            layout.label(text='Assign at least one axis', icon='INFO')


def _allowed(axis):
    """'AUTO' -> None; a letter or a tuple of indices -> the allowed indices."""
    if axis is None or axis == 'AUTO':
        return None
    if isinstance(axis, str):
        return ('XYZ'.index(axis),)
    return tuple(axis) or None


def _pick_axis(n, allowed):
    """The face axis for normal `n`; None when no chosen axis fits (a face
    across another axis is skipped, as in Cables)."""
    if allowed is None:
        return int(np.argmax(np.abs(n)))
    a = max(allowed, key=lambda k: abs(n[k]))
    return a if abs(n[a]) >= 0.3 else None


SIDE_ITEMS = (
    ('POS', '+ Face', 'Just behind the face on the positive side of the axis'),
    ('NEG', '- Face', 'Just behind the face on the negative side of the axis'),
    ('MID', 'Centre', 'In the middle of the wall'),
)
WIRE_COLORS = ((0.30, 0.13, 0.05), (0.03, 0.12, 0.55), (0.55, 0.55, 0.03), (0.02, 0.02, 0.02), (0.6, 0.6, 0.6))
_RAY_DIRS = tuple(Vector(v).normalized() for v in ((0.48, 0.62, 0.62), (-0.71, 0.31, 0.63), (0.27, -0.83, 0.49)))
EPS = 1e-4

_INPUTS = {}                                                                             


                                                                             
                                                           

def _type_changed(self, context):
    t = TYPES.get(self.pipe_type)
    if t is not None:
        self['pipe_diameter'] = t['d']
        self['pipe_wall'] = t['wall']
        self['pipe_joint_spacing'] = t['joints']
        self['pipe_wires'] = t['wires']
    live.touch_prop(self, context)


def _length(name, default, description, soft_max=100.0, minimum=0.0):
    return bpy.props.FloatProperty(name=name, default=default, min=minimum, soft_max=soft_max,
                                   update=live.touch_prop, description=description)


def _factor(name, default, description, maximum=1.0):
    return bpy.props.FloatProperty(name=name, default=default, min=0.0, max=maximum, subtype='FACTOR',
                                   update=live.touch_prop, description=description)


PROPS = {
    'pipe_damage_enable': bpy.props.BoolProperty(
        name='Pipe Damage', default=False, update=live.touch_prop,
        description='Concrete Damage chased along pipes in the wall: the spalls uncover hollow pipes '
                    'instead of rebar. Shares the Concrete Damage shape, crack and hole settings'),
    'pipe_break_enable': bpy.props.BoolProperty(default=False, options={'HIDDEN'}, update=live.touch_prop,
                                                description='internal: Boolean Break type Pipes'),
    'pipe_type': bpy.props.EnumProperty(name='Pipe Type', items=TYPE_ITEMS, default='COPPER', update=_type_changed,
                                        description='Material and behaviour of the pipes. Choosing a type also '
                                                    'sets its usual diameter, wall and joint spacing'),
    'pipe_material': bpy.props.PointerProperty(name='Pipe Material', type=bpy.types.Material,
                                               update=live.touch_prop,
                                               description='Material for the pipes. Empty uses a built-in '
                                                           'material for the pipe type'),
    'pipe_layout': bpy.props.EnumProperty(name='Runs', items=LAYOUT_ITEMS, default='BOTH', update=live.touch_prop,
                                          description='Direction of the pipe runs in the wall'),
    'pipe_count': bpy.props.IntProperty(name='Runs per Direction', default=2, min=1, max=40, update=live.touch_prop,
                                        description='Number of pipe runs for each direction, spread over the wall'),
    'pipe_bundle': bpy.props.IntProperty(name='Pipes per Run', default=2, min=1, max=6, update=live.touch_prop,
                                         description='Pipes laid side by side in one run (hot and cold water, '
                                                     'several conduits)'),
    'pipe_bundle_gap': _length('Bundle Gap (cm)', 1.5, 'Space between the pipes of one run', 20.0),
    'pipe_diameter': _length('Diameter (cm)', 2.2, 'Outer diameter of a pipe', 30.0, 0.05),
    'pipe_wall': _length('Wall (cm)', 0.1, 'Wall thickness of the hollow pipe, seen at a broken end', 3.0, 0.005),
    'pipe_cover': _length('Deepness (cm)', 1.5, 'Concrete in front of the pipes, from the surface to the pipe', 20.0),
    'pipe_axis': bpy.props.EnumProperty(name='Wall Normal', items=AXIS_ITEMS, default='AUTO', update=live.touch_prop),
    'pipe_side': bpy.props.EnumProperty(name='Side', items=SIDE_ITEMS, default='POS', update=live.touch_prop,
                                        description='Which face the pipes run behind'),
    'pipe_seed': bpy.props.IntProperty(name='Seed', default=0, min=0, soft_max=100, update=live.touch_prop),
    'pipe_elbows': _factor('Elbows', 0.3, 'Chance that a run turns 90 degrees with an elbow'),
    'pipe_joint_spacing': _length('Joint Spacing (cm)', 80.0, 'Couplings, sockets or hubs along the pipe. '
                                  '0 = no joints', 400.0),
    'pipe_embed': _length('Embed (cm)', 6.0, 'How far a pipe piece continues into the concrete that holds it', 50.0),
    'pipe_broken': _factor('Broken', 0.5, 'Chance that a pipe crossing a hole is snapped, with a gap'),
    'pipe_bend': _factor('Bend', 0.5, 'How far broken and free ends bend out of the hole, starting at the hole edge'),
    'pipe_jagged': _factor('Jagged Ends', 0.6, 'Ragged, torn rim at broken ends. PVC and cast iron break more '
                           'jagged than copper and steel'),
    'pipe_dent': _factor('Dents', 0.4, 'Squashed spots on metal pipes and flattened broken ends'),
    'pipe_bow': _factor('Bow', 0.3, 'Pipes spanning a hole bow outward (conduit also sags)'),
    'pipe_wires': bpy.props.BoolProperty(name='Wires', default=False, update=live.touch_prop,
                                         description='Wires inside the pipe that hang out of broken ends'),
    'pipe_wire_count': bpy.props.IntProperty(name='Wire Count', default=3, min=1, max=5, update=live.touch_prop),
    'pipe_sides': bpy.props.IntProperty(name='Sides', default=16, min=6, max=48, update=live.touch_prop,
                                        description='Resolution around the pipe'),
                        
    'pipe_chase_amount': bpy.props.IntProperty(name='Amount', default=3, min=0, max=200, update=live.touch_prop,
                                               description='Number of damaged stretches along the pipes'),
    'pipe_chase_length_min': _length('Length Min (cm)', 25.0, 'Shortest damaged stretch', 400.0),
    'pipe_chase_length_max': _length('Length Max (cm)', 70.0, 'Longest damaged stretch', 400.0),
    'pipe_chase_width': bpy.props.FloatProperty(name='Width', default=2.4, min=0.5, soft_max=6.0,
                                                update=live.touch_prop,
                                                description='Width of the damage, as a multiple of the pipe run '
                                                            '(all its pipes) width'),
    'pipe_chase_dig': bpy.props.FloatProperty(name='Dig Depth', default=0.9, min=0.0, soft_max=2.0,
                                              update=live.touch_prop,
                                              description='How deep the damage goes past the cover, as a share of '
                                                          'the pipe diameter. 1 uncovers the whole front half'),
}
PROPS.update(axis_props('pipe'))
PROPS['pipe_spawn_center']=bpy.props.BoolProperty(name='Spawn in Center',get=lambda p:p.pipe_side=='MID',set=lambda p,v:setattr(p,'pipe_side','MID' if v else 'POS'),description='Off follows each painted face; on places pipes in the middle of the wall')


                                                                             
               

def _rng(*key):
    h = 0x9E3779B97F4A7C15
    for k in key:
        h = (h ^ (int(k) & 0xFFFFFFFF)) * 0x100000001B3 & 0xFFFFFFFFFFFFFFFF
    return np.random.default_rng(h)


def _norm(v):
    n = np.linalg.norm(v, axis=-1, keepdims=True)
    return v / np.maximum(n, 1e-12)


def _cum(path):
    seg = np.linalg.norm(np.diff(path, axis=0), axis=1)
    return np.concatenate(([0.0], np.cumsum(seg)))


def _interp(s, cum, values):
    return np.stack([np.interp(s, cum, values[:, i]) for i in range(values.shape[1])], axis=-1)


def _smoothstep(x):
    x = np.clip(x, 0.0, 1.0)
    return x * x * (3.0 - 2.0 * x)


def _rotate(v, axis, angle):
    """Rodrigues: rotate rows of v about unit `axis` by `angle` (per row)."""
    angle = np.asarray(angle, dtype=np.float64)[..., None]
    c, s = np.cos(angle), np.sin(angle)
    return v * c + np.cross(axis, v) * s + axis * (v @ axis)[..., None] * (1.0 - c)


def _simplify(path, tol):
    """Douglas-Peucker; keeps both ends."""
    if len(path) < 3:
        return path
    keep = np.zeros(len(path), dtype=bool)
    keep[0] = keep[-1] = True
    stack = [(0, len(path) - 1)]
    while stack:
        i, j = stack.pop()
        if j <= i + 1:
            continue
        a, b = path[i], path[j]
        ab = b - a
        L = np.linalg.norm(ab)
        seg = path[i + 1:j] - a
        if L < 1e-12:
            d = np.linalg.norm(seg, axis=1)
        else:
            d = np.linalg.norm(np.cross(seg, ab / L), axis=1)
        k = int(np.argmax(d))
        if d[k] > tol:
            keep[i + 1 + k] = True
            stack.append((i, i + 1 + k))
            stack.append((i + 1 + k, j))
    return path[keep]


def _faces_of(polys):
    return [tuple(int(v) for v in row[:3]) if row[2] == row[3] else tuple(int(v) for v in row) for row in polys]


def _bvh(points, polys, guide=False):
    if points is None or len(points) == 0 or polys is None or len(polys) == 0:
        return None
    faces = _faces_of(polys)
    tree = BVHTree.FromPolygons([Vector(v) for v in np.asarray(points, dtype=np.float64)], faces, all_triangles=False)
    from . import surface_guidance
    return surface_guidance.wrap_bvh(tree, points, faces) if guide else tree


def _mesh_bvh(mesh, matrix, factor, guide=False):
    n = len(mesh.vertices)
    if n == 0 or len(mesh.polygons) == 0:
        return None
    co = np.empty(n * 3, dtype=np.float64)
    mesh.vertices.foreach_get('co', co)
    co = co.reshape(-1, 3)
    m = np.array(matrix, dtype=np.float64)
    co = (co @ m[:3, :3].T + m[:3, 3]) * factor
    starts = np.empty(len(mesh.polygons), dtype=np.int32)
    totals = np.empty(len(mesh.polygons), dtype=np.int32)
    mesh.polygons.foreach_get('loop_start', starts)
    mesh.polygons.foreach_get('loop_total', totals)
    loops = np.empty(len(mesh.loops), dtype=np.int32)
    mesh.loops.foreach_get('vertex_index', loops)
    faces = [tuple(loops[s:s + t]) for s, t in zip(starts.tolist(), totals.tolist())]
    tree = BVHTree.FromPolygons([Vector(v) for v in co], faces, all_triangles=False)
    from . import surface_guidance
    return surface_guidance.wrap_bvh(tree, co, faces) if guide else tree


def _inside(bvh, q):
    """Majority of three ray parities: robust next to edges and seams."""
    if bvh is None:
        return False
    votes = 0
    for d in _RAY_DIRS:
        o = Vector(q)
        hits = 0
        for _ in range(256):
            loc, _n, _i, dist = bvh.ray_cast(o, d)
            if loc is None:
                break
            hits += 1
            o = loc + d * EPS
        votes += hits & 1
    return votes >= 2


def _crossings(path, cum, bvh):
    """Arc-length positions where the polyline crosses the mesh surface."""
    out = []
    if bvh is None:
        return out
    for i in range(len(path) - 1):
        a = Vector(path[i])
        d = Vector(path[i + 1]) - a
        L = d.length
        if L < 1e-9:
            continue
        d /= L
        t = 0.0
        for _ in range(512):
            loc, _n, _i, dist = bvh.ray_cast(a + d * t, d, L - t)
            if loc is None:
                break
            t = t + dist
            out.append(cum[i] + (cum[i + 1] - cum[i]) * t / L)
            t += EPS
            if t >= L:
                break
    return out


                                                                             
                                                     

class Settings:
    def __init__(self, p, gscale):
        t = TYPES.get(p.pipe_type, TYPES['COPPER'])
        self.type = p.pipe_type
        self.t = t
        self.D = max(p.pipe_diameter, 0.05) * gscale
        self.R = self.D * 0.5
        self.wall = min(max(p.pipe_wall * gscale, self.R * 0.03), self.R * 0.85)
        self.cover = max(p.pipe_cover, 0.0) * gscale
        self.bundle = max(1, int(p.pipe_bundle))
        self.gap = max(p.pipe_bundle_gap, 0.0) * gscale
        self.half_width = 0.5 * (self.bundle * self.D + (self.bundle - 1) * self.gap)
        self.embed = max(p.pipe_embed, 0.0) * gscale
        self.joint = max(p.pipe_joint_spacing, 0.0) * gscale
        self.broken, self.bend, self.jag = p.pipe_broken, p.pipe_bend, p.pipe_jagged
        self.dent, self.bow = p.pipe_dent, p.pipe_bow
        self.wires = bool(p.pipe_wires)
        self.wire_count = int(p.pipe_wire_count)
        self.sides = int(p.pipe_sides)
        self.seed = int(p.pipe_seed)
        self.layout, self.count = p.pipe_layout, int(p.pipe_count)
        self.axes = axes_of(p, 'pipe')
        self.axis = 'XYZ'[self.axes[0]] if self.axes else 'AUTO'
        self.side = p.pipe_side
        self.elbows = p.pipe_elbows
        self.chase_amount = int(p.pipe_chase_amount)
        lo, hi = sorted((p.pipe_chase_length_min, p.pipe_chase_length_max))
        self.chase_len = (lo * gscale, hi * gscale)
        self.chase_width = p.pipe_chase_width
        self.chase_dig = p.pipe_chase_dig


class Run:
    """One pipe route (bundle centre): a dense polyline with an outward normal per point."""
    def __init__(self, path, nout, index):
        self.path = np.ascontiguousarray(path, dtype=np.float64)
        self.nout = _norm(np.ascontiguousarray(nout, dtype=np.float64))
        self.index = index
        self.cum = _cum(self.path)


def _axes(points, axis, mw=None):
    """Wall normal `a`, up `up` and along `h` as local axis indices. With the
    object's matrix `mw`, Auto measures the sides as they are in the scene
    (unapplied scale counts) and up is the local axis closest to world Z, so a
    wall facing X or Y, rotated or scaled, is found without setting it."""
    lo, hi = points.min(axis=0), points.max(axis=0)
    ext = hi - lo
    m = None
    if mw is not None:
        m = np.array([list(r) for r in mw.to_3x3()], dtype=np.float64)
        scale = np.linalg.norm(m, axis=0)
        if np.all(scale > 1e-12):
            ext = ext * scale
            m = m / scale
        else:
            m = None
    if axis == 'AUTO':
        a = int(np.argmin(ext))
                                                                       
        near = [k for k in range(3) if ext[k] <= ext[a] * 1.02]
        if m is not None and len(near) > 1:
            a = min(near, key=lambda k: abs(m[2, k]))
    else:
        a = 'XYZ'.index(axis)
    others = [k for k in range(3) if k != a]
    if m is not None:
        up = max(others, key=lambda k: (abs(m[2, k]), k == 2))
    else:
        up = 2 if a != 2 else 1
    h = 3 - a - up
    return lo, hi, a, up, h


def _fillet_path(corners, radii, step):
    """Polyline through `corners` with arcs of `radii` at the inner corners."""
    pts = [corners[0]]
    for k in range(1, len(corners) - 1):
        a, c, b = corners[k - 1], corners[k], corners[k + 1]
        d1 = _norm(c - a)
        d2 = _norm(b - c)
        r = radii[k - 1]
        cosang = float(np.clip(d1 @ d2, -1.0, 1.0))
        turn = math.acos(cosang)
        if turn < 1e-3 or r <= 0:
            pts.append(c)
            continue
        tlen = r * math.tan(turn * 0.5)
        tlen = min(tlen, 0.45 * np.linalg.norm(c - a), 0.45 * np.linalg.norm(b - c))
        r = tlen / math.tan(turn * 0.5)
        t1 = c - d1 * tlen
        t2 = c + d2 * tlen
        bis = _norm((t1 + t2) * 0.5 - c)
        o = c + bis * (r / math.cos(turn * 0.5))
        u0 = t1 - o
        u1 = t2 - o
        n = max(3, int(math.ceil(turn * r / max(step, 1e-6))) + 1)
        for i in range(n):
            f = i / (n - 1)
                                                     
            ang = turn * f
            w0 = math.sin(turn - ang) / math.sin(turn)
            w1 = math.sin(ang) / math.sin(turn)
            pts.append(o + u0 * w0 + u1 * w1)
    pts.append(corners[-1])
    return np.asarray(pts, dtype=np.float64)


def _densify(path, step):
    out = [path[0]]
    for a, b in zip(path[:-1], path[1:]):
        L = np.linalg.norm(b - a)
        n = max(1, int(math.ceil(L / step)))
        for i in range(1, n + 1):
            out.append(a + (b - a) * (i / n))
    return np.asarray(out, dtype=np.float64)


def paint_areas(strokes, points, axis, mw=None):
    """Painted areas as boxes (lo, hi) of their stroke points, and the face
    they were painted on ('POS' or 'NEG' along the wall normal)."""
    strokes = [np.asarray(st, dtype=np.float64) for st in strokes if len(st)]
    if not strokes:
        return [], None
    lo, hi, a, _up, _h = _axes(points, axis, mw)
    mid = 0.5 * (lo[a] + hi[a])
    areas = [(st.min(axis=0), st.max(axis=0)) for st in strokes]
    allp = np.vstack(strokes)
    side = 'POS' if np.median(allp[:, a]) >= mid else 'NEG'
    return areas, side


def paint_faces(strokes, src_bvh, axis='AUTO'):
    """Painted areas sorted by the face they were painted on: {(a, sign):
    [(lo, hi), ...]} with `a` the local axis of the face normal and sign +1/-1.
    Every stroke point takes the normal of the nearest surface, so one painting
    can cover several faces (or one stroke can go round a corner). With a fixed
    `axis` ('X', 'Y', 'Z') only the side is taken from the painting."""
    faces = {}
    if src_bvh is None:
        return faces
    fixed = _allowed(axis)
    for st in strokes:
        st = np.asarray(st, dtype=np.float64)
        if len(st) == 0:
            continue
        keys = []
        for q in st:
            loc, n, _i, _d = src_bvh.find_nearest(Vector(q))
            if loc is None:
                keys.append(None)
                continue
            n = np.asarray(n[:])
            a = _pick_axis(n, fixed)
            keys.append(None if a is None else (a, 1.0 if n[a] >= 0 else -1.0))
        for key in sorted({k for k in keys if k is not None}):
            pts = st[[k == key for k in keys]]
            if len(pts) < 2 and len(st) > 2:
                continue                                         
            faces.setdefault(key, []).append((pts.min(axis=0), pts.max(axis=0)))
    return faces


def _area_positions(S, areas, ai, lo, hi, margin, rng):
    """Run positions across `ai` inside the painted areas, Runs per Direction
    each, without two runs closer than a bundle apart."""
    pos = []
    for alo, ahi in areas:
        a0 = max(alo[ai], lo[ai] + margin)
        a1 = min(ahi[ai], hi[ai] - margin)
        if a1 < a0:
            a0 = a1 = min(max(0.5 * (alo[ai] + ahi[ai]), lo[ai] + margin), hi[ai] - margin)
        n = max(1, S.count)
        for i in range(n):
            pos.append(a0 + (a1 - a0) * (i + 0.5 + rng.uniform(-0.3, 0.3)) / n)
    pos.sort()
    keep = []
    gap = 2.0 * S.half_width + S.D
    for v in pos:
        if not keep or v - keep[-1] >= gap:
            keep.append(v)
    return keep


def auto_runs(S, points, src_bvh, areas=None, side=None):
    """Runs across the whole object. With `areas` (paint_areas) the runs pass
    through the painted areas, on the face they were painted on."""
    lo, hi, a, up, h = _axes(points, S.axis, getattr(S, 'mw', None))
    ea, eu, eh = np.eye(3)[a], np.eye(3)[up], np.eye(3)[h]
    mid = 0.5 * (lo[a] + hi[a])
    if side is None or S.side == 'MID':
        side = S.side
    sign = -1.0 if side == 'NEG' else 1.0
    nout = ea * sign
    depth = S.cover + S.R
    if side == 'MID':
        plane = mid
    elif side == 'NEG':
        plane = lo[a] + depth
    else:
        plane = hi[a] - depth
    pad = 2.0 + S.D
    rng = _rng(S.seed, 101)
    dirs = []
    if S.layout in {'H', 'BOTH'}:
        dirs.append((eh, eu, h, up))
    if S.layout in {'V', 'BOTH'}:
        dirs.append((eu, eh, up, h))
    runs = []
    margin = S.half_width + S.D
    for d, across, di, ai in dirs:
        span = (hi[ai] - margin) - (lo[ai] + margin)
        where = _area_positions(S, areas, ai, lo, hi, margin, rng) if areas else range(S.count)
        for pos in where:
            if not areas:
                                                                 
                if span <= 0:
                    pos = 0.5 * (lo[ai] + hi[ai])
                else:
                    pos = lo[ai] + margin + span * (pos + 0.5 + rng.uniform(-0.3, 0.3)) / S.count
            start = np.zeros(3)
            start[a] = plane
            start[ai] = pos
            start[di] = lo[di] - pad
            end = start.copy()
            end[di] = hi[di] + pad
            corners = [start, end]
            radii = []
            if rng.random() < S.elbows:
                f = rng.uniform(0.25, 0.75)
                c = start + (end - start) * f
                if areas:
                                                                      
                    hit = [ar for ar in areas if ar[0][ai] - margin <= pos <= ar[1][ai] + margin] or areas
                    alo, ahi = hit[int(rng.integers(len(hit)))]
                    c[di] = rng.uniform(alo[di], ahi[di])
                turn = across * (1.0 if rng.random() < 0.5 else -1.0)
                stop = c.copy()
                stop[ai] = (hi[ai] + pad) if turn[ai] > 0 else (lo[ai] - pad)
                corners = [start, c, stop]
                radii = [2.0 * S.D + S.half_width]
            path = _fillet_path(corners, radii, max(S.D * 0.25, 0.5))
            path = _densify(path, max(S.D, 2.0))
            if side != 'MID' and src_bvh is not None:
                path = _follow_surface(path, nout, ea, a, mid, sign, depth, src_bvh)
            runs.append(Run(path, np.tile(nout, (len(path), 1)), len(runs)))
    return runs


def _follow_surface(path, nout, ea, a, mid, sign, depth, bvh):
    """Keep the run at Deepness behind an uneven or curved face."""
    out = path.copy()
    target = np.full(len(path), np.nan)
    for i, q in enumerate(path):
        o = q.copy()
        o[a] = mid
        loc, _n, _i, _d = bvh.ray_cast(Vector(o), Vector(nout))
        if loc is not None:
            target[i] = loc[a] - sign * depth
    ok = np.isfinite(target)
    if not ok.any():
        return out
    idx = np.arange(len(path))
    target = np.interp(idx, idx[ok], target[ok])
    k = 5
    kern = np.ones(k) / k
    padded = np.pad(target, (k // 2, k // 2), mode='edge')
    target = np.convolve(padded, kern, mode='valid')
    target = np.maximum(target, mid) if sign > 0 else np.minimum(target, mid)
    out[:, a] = target
    return out


def drawn_runs(S, strokes, src_bvh):
    """Pipe Damage drawn: every stroke (core units, on the surface) is a route."""
    runs = []
    if src_bvh is None:
        return runs
    depth = S.cover + S.R
    for stroke in strokes:
        if len(stroke) < 2:
            continue
        path = _densify(np.asarray(stroke, dtype=np.float64), max(S.D * 0.5, 1.0))
        qs, ns = [], []
        for v in path:
            loc, n, _i, _d = src_bvh.find_nearest(Vector(v))
            if loc is None:
                continue
            qs.append(loc[:])
            ns.append(n[:])
        if len(qs) < 2:
            continue
        qs, ns = np.asarray(qs), _norm(np.asarray(ns))
        k = 7
        if len(ns) > k:
            padded = np.pad(ns, ((k // 2, k // 2), (0, 0)), mode='edge')
            ns = _norm(np.stack([np.convolve(padded[:, i], np.ones(k) / k, mode='valid') for i in range(3)], -1))
        centre = qs - ns * depth
        ext = S.embed + 2.0 * S.D
        t0 = _norm(centre[0] - centre[1])
        t1 = _norm(centre[-1] - centre[-2])
        centre = np.vstack((centre[0] + t0 * ext, centre, centre[-1] + t1 * ext))
        ns = np.vstack((ns[0], ns, ns[-1]))
        runs.append(Run(centre, ns, len(runs)))
    return runs


def pipe_paths(S, run):
    """The pipes of one run: (path, nout) each, offset side by side."""
    tang = np.gradient(run.path, axis=0)
    side = _norm(np.cross(run.nout, _norm(tang)))
    out = []
    for j in range(S.bundle):
        o = (j - (S.bundle - 1) * 0.5) * (S.D + S.gap)
        out.append((run.path + side * o, run.nout))
    return out


                                                                             
                                                         

def chase_strokes(S, runs, src_bvh):
    """Open strokes on the surface, above stretches of the pipe runs."""
    strokes = []
    if not runs or src_bvh is None or S.chase_amount <= 0:
        return strokes
    rng = _rng(S.seed, 202)
    reach = S.cover + S.R + S.D * 4.0 + 5.0
    usable = []
    for run in runs:
                                                        
        hits = []
        for s, q, n in zip(run.cum, run.path, run.nout):
            loc, _n, _i, dist = src_bvh.ray_cast(Vector(q), Vector(n), reach)
            hits.append(loc is not None)
        hits = np.asarray(hits)
        if hits.any():
            idx = np.flatnonzero(hits)
            usable.append((run, run.cum[idx[0]], run.cum[idx[-1]]))
    if not usable:
        return strokes
    weights = np.array([max(b - a, 1e-6) for _r, a, b in usable])
    weights /= weights.sum()
    step = max(S.D * 0.5, 1.0)
    for _ in range(S.chase_amount):
        run, a, b = usable[int(rng.choice(len(usable), p=weights))]
        L = min(rng.uniform(*S.chase_len), b - a)
        if L <= step:
            continue
        c = rng.uniform(a + L * 0.5, b - L * 0.5)
        s = np.arange(c - L * 0.5, c + L * 0.5 + 1e-9, step)
        qs = _interp(s, run.cum, run.path)
        ns = _norm(_interp(s, run.cum, run.nout))
        pts = []
        for q, n in zip(qs, ns):
            loc, _n, _i, _d = src_bvh.ray_cast(Vector(q), Vector(n), reach)
            if loc is not None:
                pts.append(loc[:])
            elif len(pts) >= 2:
                break
        if len(pts) >= 2:
            strokes.append(np.ascontiguousarray(pts, dtype=np.float64))
    return strokes


                                                                             
                                                             

def classify(path, cum, nout, R, src_bvh, res_bvh, *, inside_source=False):
    """Intervals [(x0, x1, C, vis)] along the pipe. C: 'S' centre held by the
    damaged solid, 'E' centre in removed volume, 'O' outside the object.
    vis: some of the pipe (centre or front) lies in removed volume."""
    front = path + nout * (0.85 * R)
    xs = set()
    for line in (path, front):
        for bvh in (src_bvh, res_bvh):
            xs.update(_crossings(line, cum, bvh))
    tolerance = max(EPS, 4.0 * float(np.spacing(np.float32(np.abs(front).max()))))
    edges = [0.0]
    for x in sorted(xs):
        if edges[-1] + tolerance < x < cum[-1] - tolerance:
            edges.append(x)
    edges.append(cum[-1])
    out = []
    for x0, x1 in zip(edges[:-1], edges[1:]):
        if x1 - x0 < 1e-6:
            continue
        m = 0.5 * (x0 + x1)
        c = _interp(np.array([m]), cum, path)[0]
        f = _interp(np.array([m]), cum, front)[0]
        c_src, c_res = _inside(src_bvh, c), _inside(res_bvh, c)
        f_src, f_res = _inside(src_bvh, f), _inside(res_bvh, f)
        C = 'S' if c_res else ('E' if c_src else 'O')
        if inside_source and not c_src:
            C, vis = 'O', False
        else:
            vis = (C == 'E') or (f_src and not f_res)
        if out and out[-1][2] == C and out[-1][3] == vis:
            out[-1][1] = x1
        else:
            out.append([x0, x1, C, vis])
    return out


def _clean(iv, D):
    """Drop slivers: a tiny exposed nick, a thin concrete bridge."""
    changed = True
    while changed and len(iv) > 2:
        changed = False
        for i in range(1, len(iv) - 1):
            x0, x1, C, vis = iv[i]
            if x1 - x0 >= 0.25 * D:
                continue
            l, r = iv[i - 1], iv[i + 1]
            if l[2] == r[2] and l[3] == r[3]:
                l[1] = r[1]
                del iv[i:i + 2]
                changed = True
                break
    return iv


                                                                             
        

class Piece:
    def __init__(self, u0, u1, start_free, end_free):
        self.u0, self.u1 = u0, u1
        self.start_free, self.end_free = start_free, end_free
        self.bends = []                                                         
        self.bows = []                      
        self.dents = []                  
        self.visible = []             


def plan_pieces(S, iv, rng):
    """Turn intervals into pieces with their damage."""
    D = S.D
    t = S.t
    n = len(iv)
    regions = []
    i = 0
    while i < n:
        if not iv[i][3]:
            i += 1
            continue
        j = i
        while j + 1 < n and (iv[j + 1][3] or (iv[j + 1][2] == 'S' and j + 2 < n and iv[j + 2][3]
                                               and iv[j + 1][1] - iv[j + 1][0] < 2.0 * S.embed + D)):
            j += 1
        regions.append((i, j))
        i = j + 1
    pieces = []
    for i, j in regions:
        held = any(iv[k][2] == 'S' for k in range(i, j + 1)) or (i > 0 and iv[i - 1][2] == 'S') \
            or (j + 1 < n and iv[j + 1][2] == 'S')
        if not held:
            continue                                       
        trim = lambda: D * rng.uniform(0.3, 1.2)
               
        if i > 0 and iv[i - 1][2] == 'S':
            u0 = iv[i][0] - min(S.embed * rng.uniform(0.6, 1.4), iv[i - 1][1] - iv[i - 1][0])
            start_free = False
        else:
            start_free = iv[i][2] == 'E'
            u0 = iv[i][0] + (trim() if start_free else 0.0)
        if j + 1 < n and iv[j + 1][2] == 'S':
            u1 = iv[j][1] + min(S.embed * rng.uniform(0.6, 1.4), iv[j + 1][1] - iv[j + 1][0])
            end_free = False
        else:
            end_free = iv[j][2] == 'E'
            u1 = iv[j][1] - (trim() if end_free else 0.0)
        if u1 - u0 < 0.5 * D:
            continue
                                                 
        spans = []
        k = i
        while k <= j:
            if iv[k][2] != 'E':
                k += 1
                continue
            m = k
            while m + 1 <= j and iv[m + 1][2] == 'E':
                m += 1
            left = iv[k - 1][2] if k > 0 else 'O'
            right = iv[m + 1][2] if m + 1 < n else 'O'
            spans.append((max(iv[k][0], u0), min(iv[m][1], u1), left == 'S', right == 'S'))
            k = m + 1
        cuts = []
        piece = Piece(u0, u1, start_free, end_free)
        piece.visible = [(iv[k][0], iv[k][1]) for k in range(i, j + 1) if iv[k][3]]
        for a, b, hl, hr in spans:
            L = b - a
            if L <= 0:
                continue
            ang = math.radians(75.0) * S.bend * t['bend'] * rng.uniform(0.5, 1.0)
            mix = rng.uniform(-0.6, 0.6)
            if hl and hr:
                if L > 2.2 * D and rng.random() < S.broken:
                    x = rng.uniform(a + 0.3 * L, b - 0.3 * L)
                    g = D * t['gap'] * rng.uniform(0.6, 1.4)
                    cuts.append((x - g * 0.5, x + g * 0.5, a, b, ang, mix,
                                 math.radians(75.0) * S.bend * t['bend'] * rng.uniform(0.5, 1.0),
                                 rng.uniform(-0.6, 0.6)))
                else:
                    piece.bows.append((a, b, S.bow * min(0.12 * L, 1.5 * D) * (0.5 + 0.5 * t['bend'])))
                    if t['dent'] > 0 and rng.random() < S.dent:
                        piece.dents.append((rng.uniform(a + 0.2 * L, b - 0.2 * L), S.dent * t['dent']))
            elif hl and end_free and abs(b - u1) < max(D * 1.3, 1e-3) + S.D * 1.2:
                piece.bends.append((a, 1, ang, mix))
            elif hr and start_free and abs(a - u0) < max(D * 1.3, 1e-3) + S.D * 1.2:
                piece.bends.append((b, -1, ang, mix))
        if not cuts:
            pieces.append(piece)
            continue
                                                    
        cuts.sort()
        bounds = [(u0, start_free)]
        tail_bends = []
        for c0, c1, a, b, ang0, mix0, ang1, mix1 in cuts:
            bounds.append((c0, True))
            bounds.append((c1, True))
            tail_bends.append(((a, 1, ang0, mix0), (b, -1, ang1, mix1)))
        bounds.append((u1, end_free))
        for q in range(len(bounds) // 2):
            (s0, f0), (s1, f1) = bounds[2 * q], bounds[2 * q + 1]
            if s1 - s0 < 0.5 * D:
                continue
            sub = Piece(s0, s1, f0, f1)
            sub.visible = piece.visible
            sub.bows = [bw for bw in piece.bows if s0 <= bw[0] and bw[1] <= s1]
            sub.dents = [dn for dn in piece.dents if s0 <= dn[0] <= s1]
            sub.bends = [bd for bd in piece.bends if s0 <= bd[0] <= s1]
            if q > 0:
                sub.bends.append(tail_bends[q - 1][1])
            if q < len(tail_bends):
                sub.bends.append(tail_bends[q][0])
            sub.gap_before = q > 0
            sub.gap_after = q < len(tail_bends)
            pieces.append(sub)
    return pieces


                                                                             
          

def _rmf(C, n0):
    """Rotation-minimising frames (double reflection)."""
    T = np.gradient(C, axis=0)
    T = _norm(T)
    N = np.zeros_like(C)
    n = n0 - T[0] * (n0 @ T[0])
    if np.linalg.norm(n) < 1e-9:
        n = np.cross(T[0], [0.0, 0.0, 1.0])
        if np.linalg.norm(n) < 1e-9:
            n = np.cross(T[0], [1.0, 0.0, 0.0])
    N[0] = n / np.linalg.norm(n)
    for i in range(len(C) - 1):
        v1 = C[i + 1] - C[i]
        c1 = v1 @ v1
        if c1 < 1e-18:
            N[i + 1] = N[i]
            continue
        rL = N[i] - (2.0 / c1) * (v1 @ N[i]) * v1
        tL = T[i] - (2.0 / c1) * (v1 @ T[i]) * v1
        v2 = T[i + 1] - tL
        c2 = v2 @ v2
        N[i + 1] = rL - (2.0 / c2) * (v2 @ rL) * v2 if c2 > 1e-18 else rL
        N[i + 1] = N[i + 1] / max(np.linalg.norm(N[i + 1]), 1e-12)
    B = np.cross(T, N)
    return T, N, B


def _bend(C, nout, u, pivot, direction, angle, mix, down, sag):
    """Bend the part of the centreline past `pivot` (towards the free end)."""
    if angle <= 1e-4:
        return C, nout
    if direction < 0:
        Cr, nr = _bend(C[::-1].copy(), nout[::-1].copy(), -u[::-1], -pivot, 1, angle, mix, down, sag)
        return Cr[::-1].copy(), nr[::-1].copy()
    jp = int(np.searchsorted(u, pivot))
    if jp >= len(u) - 1:
        return C, nout
    jp = max(jp, 0)
    T = _norm(C[min(jp + 1, len(C) - 1)] - C[jp])
    n = nout[jp] - T * (nout[jp] @ T)
    n = n / max(np.linalg.norm(n), 1e-12)
    side = np.cross(T, n)
    bdir = n * math.cos(mix) + side * math.sin(mix)
    if sag > 0:
        g = down - T * (down @ T)
        if np.linalg.norm(g) > 1e-6:
            bdir = _norm(bdir * (1.0 - sag) + _norm(g) * sag)
    axis = np.cross(T, bdir)
    if np.linalg.norm(axis) < 1e-9:
        return C, nout
    axis = axis / np.linalg.norm(axis)
    Lb = max(u[-1] - pivot, 1e-6)
    v = np.diff(C[jp:], axis=0)
    um = 0.5 * (u[jp:-1] + u[jp + 1:]) - pivot
    theta = angle * _smoothstep(um / (0.75 * Lb))
    v = _rotate(v, axis, theta)
    out = C.copy()
    out[jp + 1:] = C[jp] + np.cumsum(v, axis=0)
    nn = nout.copy()
    nn[jp + 1:] = _rotate(nout[jp + 1:], axis, theta)
    return out, nn


class MeshBuffer:
    def __init__(self):
        self.verts = []
        self.faces = []
        self.mat = []
        self.smooth = []
        self.part = []
        self.expo = []
        self.nv = 0
        self.restructure_recipes = []

    def add(self, verts, faces, mat, smooth, part, expo):
        verts = np.asarray(verts, dtype=np.float64).reshape(-1, 3)
        self.verts.append(verts)
        self.faces.extend([tuple(int(i) + self.nv for i in f) for f in faces])
        self.mat.extend([mat] * len(faces) if np.isscalar(mat) else list(mat))
        self.smooth.extend([smooth] * len(faces) if np.isscalar(smooth) else list(smooth))
        self.part.extend([part] * len(faces))
        self.expo.append(np.broadcast_to(np.asarray(expo, dtype=np.float32), (len(verts),)).copy())
        self.nv += len(verts)


def _samples(a, b, fine, coarse, visible, D):
    """Fine rings where the pipe shows, coarse ones inside the concrete."""
    spans = sorted((max(a, x0 - D), min(b, x1 + D)) for x0, x1 in visible if x1 + D > a and x0 - D < b)
    out, cur = [], a
    for x0, x1 in spans:
        if x1 <= cur:
            continue
        x0 = max(x0, cur)
        if x0 > cur:
            out.append(np.linspace(cur, x0, max(2, int(math.ceil((x0 - cur) / coarse)) + 1))[:-1])
        out.append(np.linspace(x0, x1, max(2, int(math.ceil((x1 - x0) / fine)) + 1))[:-1])
        cur = x1
    if b > cur:
        out.append(np.linspace(cur, b, max(2, int(math.ceil((b - cur) / coarse)) + 1))[:-1])
    out.append(np.array([b]))
    u = np.concatenate(out)
    return u[np.r_[True, np.diff(u) > 1e-9]]


def _shown(u, visible, D):
    """1 where the pipe shows (and a diameter around it), 0 deep in the concrete."""
    w = np.zeros_like(u)
    for x0, x1 in visible:
        d = np.maximum(x0 - u, u - x1)
        w = np.maximum(w, 1.0 - _smoothstep(d / D))
    return w


def _buried_samples(u, visible):
    buried = np.isfinite(u)
    for start, end in visible:
        buried &= (u < start) | (u > end)
    return buried


def _hidden_ring_scales(shell, centers, rings, buried):
    """Fit hidden cross sections to the original shell at their attachment points."""
    scales = np.ones(rings.shape[:2])
    for index in np.flatnonzero(buried):
        center = Vector(centers[index])
        radial = rings[index] - centers[index]
        lengths = np.linalg.norm(radial, axis=1)
        if shell.find_nearest(center, float(lengths.max()) + EPS)[0] is None:
            continue
        for k, (offset, length) in enumerate(zip(radial, lengths)):
            if length <= EPS:
                continue
            hit, _, _, distance = shell.ray_cast(center, Vector(offset / length), float(length) + EPS)
            if hit is not None:
                scales[index, k] = np.clip((distance - EPS) / length, .001, 1.)
    return scales[..., None]


def _radius_profile(S, ug, piece_end_dist, free_flare, shown=1.0):
    """Outer radius factor along the pipe (joints, corrugation, flared ends)."""
    t = S.t
    f = np.ones_like(ug)
    if S.joint > 0 and t['joint'][0] > 1.0:
        L = t['joint'][1] * S.D
        local = np.mod(ug - S._joint_phase, S.joint)
        ramp = 0.12 * L
        bump = np.clip(np.minimum(local, L - local) / max(ramp, 1e-6), 0.0, 1.0) * (local < L)
        f += (t['joint'][0] - 1.0) * bump
    if t['corr'] > 0:
        pitch = 0.36 * S.D
        f += 0.07 * t['corr'] * shown * np.sin(2.0 * math.pi * ug / pitch)
    if free_flare > 0:
        f += free_flare * (1.0 - _smoothstep(piece_end_dist / (0.8 * S.D)))
    return f


def build_piece(S, buf, part_id, path, cum, nout, piece, rng, down, *, shell=None):
    recipe_state = rng.bit_generator.state
    D, R = S.D, S.R
    t = S.t
    K = S.sides
    corr = t['corr'] > 0
    h = (0.36 * D / 4.0) if corr else D * 0.2
    h /= max(.2, float(getattr(S, 'surface_resolution', 1.0)))
    jag_amt = S.jag * t['jag'] * 0.75 * D
                                                     
    def rim():
        phi = np.arange(K) * (2.0 * math.pi / K)
        w = np.zeros(K)
        for m in (2, 3, 5, 7):
            w += rng.uniform(0.3, 1.0) / m * np.sin(m * phi + rng.uniform(0, 6.283))
        w = (w - w.min()) / max(w.max() - w.min(), 1e-9)
        if t['jag'] >= 0.9:                         
            w = w ** 0.6
            teeth = rng.random(K) < 0.25
            w[teeth] = np.minimum(1.0, w[teeth] + 0.5)
        return w * jag_amt
    pull0 = rim() if piece.start_free else np.zeros(K)
    pull1 = rim() if piece.end_free else np.zeros(K)
    a = piece.u0 + (pull0.max() + 0.02 * D if piece.start_free else 0.0)
    b = piece.u1 - (pull1.max() + 0.02 * D if piece.end_free else 0.0)
    if b - a < 0.2 * D:
        return False
    u = _samples(a, b, h, max(D * 0.6, h), piece.visible, D)
                                          
    lo_u = piece.u0 + pull0.min() if piece.start_free else a
    hi_u = piece.u1 - pull1.min() if piece.end_free else b
    ext = np.concatenate(([lo_u] if lo_u < a - 1e-9 else [], u, [hi_u] if hi_u > b + 1e-9 else []))
    C = _interp(ext, cum, path)
    nn = _norm(_interp(ext, cum, nout))
                                                           
    for x0, x1, amp in piece.bows:
        w = np.where((ext > x0) & (ext < x1), np.sin(math.pi * (ext - x0) / max(x1 - x0, 1e-9)), 0.0)
        C = C + nn * (amp * w)[:, None]
        if t['sag'] > 0:
            C = C + down * (amp * 1.5 * t['sag'] * w)[:, None]
    for pivot, direction, ang, mix in piece.bends:
        C, nn = _bend(C, nn, ext, pivot, direction, ang, mix, down, t['sag'] * 0.7)
    T, N, B = _rmf(C, nn[0])
                           
    dist_end = np.minimum(np.where(piece.start_free, ext - piece.u0, 1e9),
                          np.where(piece.end_free, piece.u1 - ext, 1e9))
    flare = t['flare'] * S.jag
    prof = _radius_profile(S, ext, dist_end, flare, _shown(ext, piece.visible, D))
    ro = R * prof
    ri = np.maximum(ro - S.wall * (prof if corr else 1.0), R * 0.1)
                                            
    sx = np.ones_like(ext)
    sy = np.ones_like(ext)
    for x, amt in piece.dents:
        g = np.exp(-((ext - x) / (1.2 * D)) ** 2)
        sx -= 0.45 * amt * g
        sy += 0.2 * amt * g
    if t['dent'] > 0:
        for free, uu in ((piece.start_free, piece.u0), (piece.end_free, piece.u1)):
            if free and rng.random() < S.dent:
                g = np.exp(-((ext - uu) / (0.9 * D)) ** 2)
                amt = S.dent * t['dent'] * rng.uniform(0.4, 1.0)
                sx -= 0.5 * amt * g
                sy += 0.25 * amt * g
    phi = np.arange(K) * (2.0 * math.pi / K)
    cphi, sphi = np.cos(phi), np.sin(phi)

    def ring(idx, radius):
        return (C[idx][:, None, :] + radius[idx][:, None, None] *
                (sx[idx][:, None, None] * cphi[None, :, None] * N[idx][:, None, :] +
                 sy[idx][:, None, None] * sphi[None, :, None] * B[idx][:, None, :]))

    core = np.arange(len(ext))
    if piece.start_free and lo_u < a - 1e-9:
        core = core[1:]
    if piece.end_free and hi_u > b + 1e-9:
        core = core[:-1]
    outer = ring(core, ro)
    inner = ring(core, ri)
    if shell is not None:
        centers = C[core, None, :]
        scales = _hidden_ring_scales(shell, C[core], outer, _buried_samples(ext[core], piece.visible))
        outer = centers + (outer - centers) * scales
        inner = centers + (inner - centers) * scales

    def rim_ring(us, radius_scale):
                                                            
        out = np.empty((K, 3))
        for k in range(K):
            c = _interp(np.array([us[k]]), ext, C)[0]
            nk = _norm(_interp(np.array([us[k]]), ext, N))[0]
            bk = _norm(_interp(np.array([us[k]]), ext, B))[0]
            r = np.interp(us[k], ext, radius_scale)
            out[k] = c + r * (np.interp(us[k], ext, sx) * cphi[k] * nk + np.interp(us[k], ext, sy) * sphi[k] * bk)
        return out

    rings_o = list(outer)
    rings_i = list(inner)
    if piece.start_free:
        us = piece.u0 + pull0
        rings_o.insert(0, rim_ring(us, ro))
        rings_i.insert(0, rim_ring(us, ri))
    if piece.end_free:
        us = piece.u1 - pull1
        rings_o.append(rim_ring(us, ro))
        rings_i.append(rim_ring(us, ri))
    J = len(rings_o)
    vo = np.asarray(rings_o).reshape(-1, 3)
    vi = np.asarray(rings_i).reshape(-1, 3)
    verts = np.vstack((vo, vi))
    off = J * K
    faces, mats, smooth = [], [], []
    for j in range(J - 1):
        for k in range(K):
            k1 = (k + 1) % K
            faces.append((j * K + k, j * K + k1, (j + 1) * K + k1, (j + 1) * K + k))
            mats.append(0)
            smooth.append(True)
            faces.append((off + j * K + k, off + (j + 1) * K + k, off + (j + 1) * K + k1, off + j * K + k1))
            mats.append(1)
            smooth.append(True)
    last = (J - 1) * K
    for k in range(K):
        k1 = (k + 1) % K
        faces.append((k, off + k, off + k1, k1))
        mats.append(0)
        smooth.append(False)
        faces.append((last + k, last + k1, off + last + k1, off + last + k))
        mats.append(0)
        smooth.append(False)
                                                             
    ring_u = np.concatenate((([piece.u0] if piece.start_free else []), ext[core], ([piece.u1] if piece.end_free else [])))
    vis = np.zeros(len(ring_u), dtype=np.float32)
    for x0, x1 in piece.visible:
        vis[(ring_u >= x0 - 0.5 * D) & (ring_u <= x1 + 0.5 * D)] = 1.0
    expo = np.concatenate((np.repeat(vis, K), np.repeat(vis, K)))
    buf.add(verts, faces, mats, smooth, part_id, expo)
    piece._curve = (ext, C, N, B, ri)
    from . import tube_restructure
    tube_restructure.record(buf,C,N[0],piece.start_free,piece.end_free,0,part_id,
                            recipe_state,joint_phase=S._joint_phase)
    return True


def _tube(buf, path, radius, sides, mat, part_id):
    """Closed solid tube (wires)."""
    if len(path) < 2:
        return
    T, N, B = _rmf(path, np.array([0.0, 0.0, 1.0]) if abs(path[-1][2] - path[0][2]) < 1e9 else None)
    phi = np.arange(sides) * (2.0 * math.pi / sides)
    rings = path[:, None, :] + radius * (np.cos(phi)[None, :, None] * N[:, None, :] +
                                         np.sin(phi)[None, :, None] * B[:, None, :])
    J = len(path)
    verts = np.vstack((rings.reshape(-1, 3), path[0], path[-1]))
    faces = []
    for j in range(J - 1):
        for k in range(sides):
            k1 = (k + 1) % sides
            faces.append((j * sides + k, j * sides + k1, (j + 1) * sides + k1, (j + 1) * sides + k))
    c0, c1 = J * sides, J * sides + 1
    last = (J - 1) * sides
    for k in range(sides):
        k1 = (k + 1) % sides
        faces.append((c0, k1, k))
        faces.append((c1, last + k, last + k1))
    buf.add(verts, faces, mat, True, part_id, 1.0)


def build_wires(S, buf, part_ids, pieces, rng, down):
    """Wires through the pieces of one region; they sag across gaps and hang
    out of free ends."""
    curves = [p for p in pieces if getattr(p, '_curve', None) is not None]
    if not curves:
        return
    count = S.wire_count
    D = S.D
    for w in range(count):
        ang = 2.0 * math.pi * w / count + rng.uniform(-0.3, 0.3)
        segs = []
        for p in curves:
            ext, C, N, B, ri = p._curve
            rad = 0.42 * ri[:, None]
            pts = C + rad * (math.cos(ang) * N + math.sin(ang) * B)
            segs.append((p, pts))
        path = []
        for q, (p, pts) in enumerate(segs):
            if q == 0 and p.start_free:
                L = D * rng.uniform(1.0, 3.5)
                tdir = _norm(pts[0] - pts[min(3, len(pts) - 1)])
                tt = np.linspace(1.0, 0.0, 8)[:-1]
                path.extend(pts[0] + tdir * (L * tt[:, None]) + down * (0.5 * L * tt ** 2)[:, None])
            path.extend(pts)
            nxt = segs[q + 1][1] if q + 1 < len(segs) else None
            if nxt is not None and getattr(p, 'gap_after', False):
                p0, p3 = pts[-1], nxt[0]
                t0 = _norm(pts[-1] - pts[max(len(pts) - 4, 0)])
                t3 = _norm(nxt[min(3, len(nxt) - 1)] - nxt[0])
                L = np.linalg.norm(p3 - p0) + D
                p1 = p0 + t0 * L * 0.4 + down * (0.6 * L)
                p2 = p3 - t3 * L * 0.4 + down * (0.6 * L)
                tt = np.linspace(0.0, 1.0, 14)[1:-1, None]
                bez = ((1 - tt) ** 3) * p0 + 3 * ((1 - tt) ** 2) * tt * p1 + 3 * (1 - tt) * tt ** 2 * p2 + tt ** 3 * p3
                path.extend(bez)
            elif q == len(segs) - 1 and p.end_free:
                L = D * rng.uniform(1.0, 3.5)
                tdir = _norm(pts[-1] - pts[max(len(pts) - 4, 0)])
                tt = np.linspace(0.0, 1.0, 8)[1:]
                path.extend(pts[-1] + tdir * (L * tt[:, None]) + down * (0.5 * L * tt ** 2)[:, None])
        path = np.asarray(path)
        if len(path) < 2:
            continue
        keep = np.r_[True, np.linalg.norm(np.diff(path, axis=0), axis=1) > 1e-6]
        path = path[keep]
        wr = max(0.2 * float(np.min(curves[0]._curve[4])), 0.03)
        _tube(buf, path, wr, 6, 2 + (w % len(WIRE_COLORS)), part_ids)


def build_pipes(S, runs, src_bvh, res_bvh, down, *, boolean=False):
    buf = MeshBuffer()
    part = 0
    counts = dict(pieces=0, broken=0)
    for run in runs:
        for j, (path, nout) in enumerate(pipe_paths(S, run)):
            simple_idx = _simplify_index(path, 1e-3)
            spath, snout = path[simple_idx], nout[simple_idx]
            cum = _cum(spath)
            if cum[-1] <= 0:
                continue
            iv = _clean(classify(spath, cum, snout, S.R, src_bvh, res_bvh, inside_source=boolean), S.D)
            rng = _rng(S.seed, 303, run.index, j)
            S._joint_phase = rng.uniform(0.0, max(S.joint, 1.0))
            pieces = plan_pieces(S, iv, rng)
            built = []
            for piece in pieces:
                if build_piece(S, buf, part, spath, cum, snout, piece, rng, down,
                               shell=src_bvh if boolean else None):
                    built.append(piece)
                    counts['pieces'] += 1
                    counts['broken'] += int(piece.start_free) + int(piece.end_free)
                    part += 1
            if S.wires and built:
                                                                           
                groups = []
                for piece in built:
                    if groups and groups[-1][-1].visible is piece.visible:
                        groups[-1].append(piece)
                    else:
                        groups.append([piece])
                for g in groups:
                    build_wires(S, buf, part, g, rng, down)
                    part += 1
    return buf, counts


def _simplify_index(path, tol):
    """Indices kept by Douglas-Peucker (so the normals can follow)."""
    if len(path) < 3:
        return np.arange(len(path))
    keep = np.zeros(len(path), dtype=bool)
    keep[0] = keep[-1] = True
    stack = [(0, len(path) - 1)]
    while stack:
        i, j = stack.pop()
        if j <= i + 1:
            continue
        a, b = path[i], path[j]
        ab = b - a
        L = np.linalg.norm(ab)
        seg = path[i + 1:j] - a
        d = np.linalg.norm(seg, axis=1) if L < 1e-12 else np.linalg.norm(np.cross(seg, ab / L), axis=1)
        k = int(np.argmax(d))
        if d[k] > tol:
            keep[i + 1 + k] = True
            stack.append((i, i + 1 + k))
            stack.append((i + 1 + k, j))
    return np.flatnonzero(keep)


                                                                             
                             

def _principled(mat):
    enable_nodes(mat)
    return mat.node_tree.nodes.get('Principled BSDF')


def _type_material(kind):
    from .pipe_material_library import load,PIPE_NAMES
    material=load(PIPE_NAMES[kind])
    if material is not None:return material
    t = TYPES[kind]
    name = 'ProjectDamage_Pipe_' + kind.title().replace('_', '')
    mat = bpy.data.materials.get(name)
    if mat is not None:
        return mat
    mat = bpy.data.materials.new(name)
    mat.diffuse_color = t['color'] + (1.0,)
    bsdf = _principled(mat)
    if bsdf is not None:
        bsdf.inputs['Base Color'].default_value = t['color'] + (1.0,)
        bsdf.inputs['Metallic'].default_value = t['metal']
        bsdf.inputs['Roughness'].default_value = t['rough']
        nt = mat.node_tree
        if kind in {'CAST_IRON', 'STEEL', 'COPPER'}:
                                                                      
            noise = nt.nodes.new('ShaderNodeTexNoise')
            noise.location = (-900, 300)
            noise.inputs['Scale'].default_value = 18.0
            ramp = nt.nodes.new('ShaderNodeValToRGB')
            ramp.location = (-650, 300)
            ramp.color_ramp.elements[0].position = 0.58
            ramp.color_ramp.elements[1].position = 0.8
            mix = nt.nodes.new('ShaderNodeMix')
            mix.data_type = 'RGBA'
            mix.location = (-350, 300)
            mix.inputs[6].default_value = t['color'] + (1.0,)
            mix.inputs[7].default_value = {'CAST_IRON': (0.33, 0.12, 0.04, 1.0), 'STEEL': (0.38, 0.2, 0.08, 1.0),
                                           'COPPER': (0.42, 0.24, 0.13, 1.0)}[kind]
            nt.links.new(noise.outputs['Fac'], ramp.inputs['Fac'])
            nt.links.new(ramp.outputs['Color'], mix.inputs[0])
            nt.links.new(mix.outputs[2], bsdf.inputs['Base Color'])
    return mat


def _plain_material(name, color, rough=0.6, metal=0.0):
    mat = bpy.data.materials.get(name)
    if mat is None:
        mat = bpy.data.materials.new(name)
        mat.diffuse_color = color + (1.0,)
        bsdf = _principled(mat)
        if bsdf is not None:
            bsdf.inputs['Base Color'].default_value = color + (1.0,)
            bsdf.inputs['Roughness'].default_value = rough
            bsdf.inputs['Metallic'].default_value = metal
    return mat


def pipes_of(source, instance=0):
    from . import wall_stack
    return next(wall_stack.output_objects(source, PIPES_STAMP, instance), None)


def _remove(source, instance=0):
    obj = pipes_of(source, instance)
    if obj is not None:
        mesh = obj.data
        bpy.data.objects.remove(obj, do_unlink=True)
        if mesh is not None and mesh.users == 0:
            bpy.data.meshes.remove(mesh)


def _mesh_from(buf, name, factor, p):
    verts = np.vstack(buf.verts) / factor
    faces = buf.faces
    sizes = np.fromiter((len(f) for f in faces), dtype=np.int32, count=len(faces))
    loops = np.fromiter((i for f in faces for i in f), dtype=np.int32, count=int(sizes.sum()))
    starts = np.zeros(len(faces), dtype=np.int32)
    starts[1:] = np.cumsum(sizes)[:-1]
    mesh = bpy.data.meshes.new(name)
    mesh.vertices.add(len(verts))
    mesh.vertices.foreach_set('co', verts.astype(np.float32).ravel())
    mesh.loops.add(len(loops))
    mesh.loops.foreach_set('vertex_index', loops)
    mesh.polygons.add(len(faces))
    mesh.polygons.foreach_set('loop_start', starts)
    try:
        mesh.polygons.foreach_set('loop_total', sizes)                  
    except (AttributeError, RuntimeError, TypeError):
        pass
    mesh.update(calc_edges=True)
    mesh.validate(clean_customdata=False)
    mesh.polygons.foreach_set('use_smooth', np.asarray(buf.smooth, dtype=bool))
    mesh.polygons.foreach_set('material_index', np.asarray(buf.mat, dtype=np.int32))
    a = mesh.attributes.new('pipe_id', 'INT', 'FACE')
    a.data.foreach_set('value', np.asarray(buf.part, dtype=np.int32))
    e = mesh.attributes.new('pipe_exposed', 'FLOAT', 'POINT')
    e.data.foreach_set('value', np.concatenate(buf.expo).astype(np.float32))
    from .tube_restructure import write_recipes
    write_recipes(mesh,buf,factor)
    from .material_controls import automatic_default
    mesh.materials.append(automatic_default(p,'pipe_material',p.pipe_type,_type_material))
    mesh.materials.append(_plain_material('ProjectDamage_Pipe_Inside', (0.05, 0.045, 0.04), 0.9))
    for i, c in enumerate(WIRE_COLORS):
        mesh.materials.append(_plain_material('ProjectDamage_Wire_%d' % (i + 1), c, 0.45))
    return mesh


def _place(context, source, mesh, instance=0):
    obj = pipes_of(source, instance)
    name = source.name + '_project_damage_pipes' + (f' #{instance}' if instance else '')
    if obj is None:
        obj = bpy.data.objects.new(name, mesh)
    else:
        old = obj.data
        obj.data = mesh
        if old is not None and old.users == 0:
            bpy.data.meshes.remove(old)
    obj.name = name
    mesh.name = name
    obj[PIPES_STAMP] = source.get('project_damage_uid')
    if instance:
        from .wall_stack import INSTANCE
        obj[INSTANCE] = instance
    ownership.bind(source, obj)
    output_collections.link_object(context,source,obj)
    obj.matrix_world = source.matrix_world
    return obj


                                                                             
             

def boolean_wanted(p):
    return bool(getattr(p, 'pipe_break_enable', False) and p.ui_boolean_enable and not p.rebar_enable
                and not p.all_off)


def damage_wanted(p):
    return bool(getattr(p, 'pipe_damage_enable', False) and not p.all_off)


def wanted(p):
    return (boolean_wanted(p) or damage_wanted(p)) and axes_of(p) != ()


def _gscale(p):
    from .params import _clamp_scale
    return _clamp_scale(p.global_scale / 100.0)


def _scene_scale(source):
    """The object's scale per local axis. The pipes are laid out and built with
    it applied (scene proportions), so an object with unapplied scale gets
    round pipes at the real cover, and its thinnest side is the real one."""
    m = np.array([list(r) for r in source.matrix_world.to_3x3()], dtype=np.float64)
    sc = np.linalg.norm(m, axis=0)
    return sc if np.all(sc > 1e-9) else np.ones(3)


def _rotation(source):
    return source.matrix_world.to_3x3().normalized()


def _down(source, scaled=False):
    """World down in mesh space, or with `scaled` in scene-scaled space (the rotation alone)."""
    m = _rotation(source).transposed() if scaled else source.matrix_world.to_3x3().inverted_safe()
    d = m @ Vector((0.0, 0.0, -1.0))
    return np.array(d.normalized()[:] if d.length > 1e-9 else (0.0, 0.0, -1.0))


def _drawn(context, source, factor, sc=1.0, *, props=None):
    from . import paint
    p = source.project_damage if props is None else props
    return [np.ascontiguousarray(path * factor * sc, dtype=np.float64)
            for path in paint.read_paths(context, source, p.concrete_drawing)]


def face_runs(S, points, src_bvh, faces):
    """Runs through the areas of every face in `faces` (paint_faces or
    break_faces), each face with its own normal and side."""
    import copy
    runs = []
    for k, ((a, sign), areas) in enumerate(sorted(faces.items())):
        Sf = copy.copy(S)
        Sf.axis = 'XYZ'[a]
        Sf.seed = S.seed + k
        for run in auto_runs(Sf, points, src_bvh, areas, 'POS' if sign > 0 else 'NEG'):
            run.index = len(runs)
            runs.append(run)
    return runs


def break_faces(S, res_points, src_bvh, axis='AUTO', limit=20000):
    """The broken areas of a Boolean Break, sorted by face like paint_faces:
    the result's points that are not on the source surface (the walls of the
    breaks) take the face nearest to them, and are grouped into areas."""
    faces = {}
    if src_bvh is None or res_points is None or len(res_points) == 0:
        return faces
    pts = np.asarray(res_points, dtype=np.float64)
    if len(pts) > limit:
        pts = pts[_rng(S.seed, 303).choice(len(pts), limit, replace=False)]
    eps = max(0.5, S.D * 0.1)
    fixed = _allowed(axis)
    keyed = {}
    for q in pts:
        loc, n, _i, d = src_bvh.find_nearest(Vector(q))
        if loc is None or d < eps:
            continue                                                         
        n = np.asarray(n[:])
        a = _pick_axis(n, fixed)
        if a is None:
            continue
        keyed.setdefault((a, 1.0 if n[a] >= 0 else -1.0), []).append(np.asarray(loc[:]))
    cell = max(S.D * 3.0, 10.0)
    for key, ps in keyed.items():
        ps = np.asarray(ps)
                                                                  
        cells = {}
        for i, c in enumerate(map(tuple, np.floor(ps / cell).astype(np.int64))):
            cells.setdefault(c, []).append(i)
        seen = set()
        for c0 in cells:
            if c0 in seen:
                continue
            stack, idx = [c0], []
            seen.add(c0)
            while stack:
                c = stack.pop()
                idx += cells[c]
                for dx in (-1, 0, 1):
                    for dy in (-1, 0, 1):
                        for dz in (-1, 0, 1):
                            nb = (c[0] + dx, c[1] + dy, c[2] + dz)
                            if nb in cells and nb not in seen:
                                seen.add(nb)
                                stack.append(nb)
            if len(idx) >= 3:
                sub = ps[idx]
                faces.setdefault(key, []).append((sub.min(axis=0), sub.max(axis=0)))
    return faces


def _res_points(res, source, factor, sc):
    """The result's vertices and face centres in the source's scene-scaled space."""
    mesh = res.data
    m = np.array(Matrix.Diagonal((sc[0], sc[1], sc[2], 1.0)) @ source.matrix_world.inverted() @ res.matrix_world)
    co = np.empty(len(mesh.vertices) * 3)
    mesh.vertices.foreach_get('co', co)
    co = co.reshape(-1, 3)
    cen = np.empty(len(mesh.polygons) * 3)
    mesh.polygons.foreach_get('center', cen)
    co = np.vstack((co, cen.reshape(-1, 3)))
    return (co @ m[:3, :3].T + m[:3, 3]) * factor


def runs_for(context, source, S, points, src_bvh, factor, res=None, polys=None, res_bvh=None, *, props=None):
    p = source.project_damage if props is None else props
                                                                                
    S.mw = _rotation(source)
    if damage_wanted(p):
                                                                              
                                                                            
        faces = paint_faces(_drawn(context, source, factor, _scene_scale(source), props=p), src_bvh, S.axes or 'AUTO')
        return face_runs(S, points, src_bvh, faces)
    if res is not None and boolean_wanted(p) and not damage_wanted(p) and S.side != 'MID':
                                                                           
                                                                         
        faces = break_faces(S, _res_points(res, source, factor, _scene_scale(source)), src_bvh, S.axes or 'AUTO')
        if not faces and polys is not None:
            from .cables import damage_areas
            faces = {('XYZ'.index(axis), 1.0 if side == 'POS' else -1.0):
                     [(lo, hi) for lo, hi, _ in areas]
                     for axis, side, areas in damage_areas(S, points, polys, res_bvh, S.axes)}
        if faces:
            return face_runs(S, points, src_bvh, faces)
    if S.axes and len(S.axes) > 1:
                                                                         
        sign = -1.0 if S.side == 'NEG' else 1.0
        return face_runs(S, points, src_bvh, {(a, sign): None for a in S.axes})
    return auto_runs(S, points, src_bvh)


# готовим данные этого эффекта перед расчётом геометрии
def before_core(context, source, params, points, polys, factor, concrete_strokes, reports, *, props=None):
    """build.rebuild_object, before the fingerprint and the core call. Pipe
    Damage takes over the Concrete Damage block: paint strokes along the pipes,
    no rebar, deep enough to reach them. Returns the concrete strokes."""
    p = source.project_damage if props is None else props
    uid = source.get('project_damage_uid')
    if not wanted(p):
        _INPUTS.pop(uid, None)
        return concrete_strokes
    _INPUTS[uid] = (np.array(points, dtype=np.float64, copy=True), np.array(polys, copy=True))
    if not damage_wanted(p):
        return concrete_strokes
    block = getattr(params, 'concrete', None)
    if block is None:
        reports.append(({'WARNING'}, 'Pipe Damage needs the Concrete Damage core'))
        return concrete_strokes
    S = Settings(p, _gscale(p))
    sc = _scene_scale(source)
    pts = np.asarray(points, dtype=np.float64) * sc
    src_bvh = _bvh(pts, polys, guide=True)
    strokes = _drawn(context, source, factor, props=p)
    if not strokes:
        reports.append(({'INFO'}, 'Pipe Damage: draw the damaged area to create pipes'))
                                                                               
    _lo, _hi, a, up, h = _axes(pts, S.axis, _rotation(source))
    s_n = sc[a]
    s_w = 0.5 * (sc[up] + sc[h])
    block.enable = True
    block.mode = 1
    block.rebar_enable = False
    dig = (S.cover + S.D * max(S.chase_dig, 0.0)) / s_n
    block.depth_min = max(dig * 0.85, 0.05)
    block.depth_max = max(dig * 1.1 + 0.2 / s_n, block.depth_min + 0.05)
    return strokes


def after_build(context, source, res, reports=None, *, body_has_damage=True, props=None, instance=0, damage=True, cut_body=None):
    """Keep the pipes uncovered by the complete damaged body, before splitting it."""
    p = source.project_damage if props is None else props
    boolean = boolean_wanted(p) and not damage_wanted(p)
    from .generated_geometry import enabled
    if (not damage and not boolean) or not enabled(p, 'boolean' if boolean else 'pipe'):
        _remove(source, instance)
        return None
    if res is None or res.type != 'MESH' or not wanted(p) or (boolean and not body_has_damage):
        _remove(source, instance)
        return None
    from . import incremental
    existing=pipes_of(source, instance)
    cache_key = 'pipes' if not instance else ('pipes', instance)
    if not instance and incremental.reuse_output(context,source,res,cache_key,existing):return existing
    from .params import unit_factor
    factor = unit_factor(context)
    uid = source.get('project_damage_uid')
    sc = _scene_scale(source)
    unit = Matrix.Diagonal((sc[0], sc[1], sc[2], 1.0))
    stored = _INPUTS.get(uid)
    polys = None
    if stored is not None:
        points, polys = stored
        points = points * sc
        src_bvh = _bvh(points, polys, guide=True)
    else:
        mesh = source.data
        co = np.empty(len(mesh.vertices) * 3)
        mesh.vertices.foreach_get('co', co)
        points = co.reshape(-1, 3) * factor * sc
        src_bvh = _mesh_bvh(mesh, unit, factor, guide=True)
    if src_bvh is None or len(points) == 0:
        _remove(source, instance)
        return None
    res_bvh = (_bvh(np.asarray(cut_body['points']) * sc, cut_body['polys']) if cut_body is not None else
               _mesh_bvh(res.data, unit @ source.matrix_world.inverted() @ res.matrix_world, factor))
    scale=_gscale(p)
    if not damage_wanted(p):scale*=p.boolean_pipe_scale/100.
    S = Settings(p, scale)
    if not damage_wanted(p):
        from .boolean_ui_controls import tube_settings
        tube_settings(S,p,'pipe')
    runs = [] if boolean and S.axes == () else runs_for(
        context, source, S, points, src_bvh, factor, res, polys, res_bvh, props=p)
    buf, counts = build_pipes(S, runs, src_bvh, res_bvh, _down(source, scaled=True), boolean=boolean)
    if not buf.faces:
        _remove(source, instance)
        if reports is not None:
            reports.append(({'INFO'}, 'Pipes: the damage does not reach the pipes yet'))
        return None
    from . import material_uv,material_controls,effect_groups
    uv_snapshots=material_uv.capture(source)
    obj = _place(context, source, _mesh_from(buf, source.name + '_project_damage_pipes', factor * sc, p), instance)
    material_controls.apply_shared_output_material(source)
    material_uv.after_build(source,uv_snapshots)
    key='pipes' if damage_wanted(p) else 'boolean'
    materials={mat for mat in obj.data.materials if mat is not None}
    effect_groups._replace(obj,{key:effect_groups.material_vertices(obj.data,materials)},
                           {'pipes':'Pipe Damage','boolean':'Boolean Break'})
    obj.hide_render = False
    if reports is not None:
        reports.append(({'INFO'}, 'Pipes: %d pieces, %d broken ends' % (counts['pieces'], counts['broken'])))
    if not instance:
        incremental.remember_output(context,source,res,cache_key,obj)
    return obj


def follow_source(source):
    uid = source.get('project_damage_uid')
    if uid:
        for obj in bpy.data.objects:
            if obj.get(PIPES_STAMP) == uid:
                obj.matrix_world = source.matrix_world


                                                                             
    

def _section(layout, title):
    from . import effect_layout
    return effect_layout.section(layout, title)


def draw_pipe_settings(layout, p, damage=False):
    from .pipes import disclosure as disclosure_box
    from .effect_layout import section as material_controls_box
    from . import generated_geometry
    if damage and not generated_geometry.draw(layout, p, 'pipe'): return
    box=material_controls_box(layout,'Pipes')
    material_controls.inline(box,p,'pipe_material',text='Pipes Material')
    col=disclosure_box(box,'pipe_axis_open','Placement Logic')
    if col is not None:
        draw_axes(col,p,'pipe')
        col.prop(p,'pipe_spawn_center',text='Spawn in Center',toggle=True)
    col=box.column(align=True)
    ui_image_hints.prop(col,p,'pipe_type')
    col.prop(p,'pipe_diameter')
    col.prop(p,'pipe_wall')
    col.prop(p,'pipe_cover')
    col.prop(p,'pipe_sides',text='Resolution')
    ui_image_hints.prop(col,p,'pipe_layout')
    col.prop(p,'pipe_count',text='Amount')
    col.prop(p,'pipe_bundle',text='Pipes per Run')
    col.prop(p,'pipe_bundle_gap')
    col.prop(p,'pipe_elbows')
    col.prop(p,'pipe_joint_spacing',text='Clamps')
    col.prop(p,'pipe_seed')
    col.prop(p,'pipe_wires',toggle=True)
    if p.pipe_wires:col.prop(p,'pipe_wire_count')
    col=disclosure_box(box,'pipe_break_open','Breakage')
    if col is not None:
        col.prop(p,'pipe_broken')
        col.prop(p,'pipe_bend')
        col.prop(p,'pipe_jagged')
        col.prop(p,'pipe_dent')
        col.prop(p,'pipe_bow')
        col.prop(p,'pipe_embed')


_ICONS = set()


# показываем настройки выбранного эффекта
def draw_settings(layout, context):
    from . import paint,recording_tools
    from .pipes import disclosure as disclosure_box
    from .effect_layout import section
    src=ownership.context_source(context)
    if src is None:return
    p=src.project_damage
    box=section(layout,'Damage along the Pipes')
    paint.drawing_row(box,p,'CONCRETE')
    col=box.column(align=True)
    col.prop(p,'concrete_paint_radius')
    col.prop(p,'concrete_paint_snap')
    ui_image_hints.prop(col,p,'concrete_paint_close')
    col.prop(p,'concrete_seed')
    quality_ui.draw(col, p,'concrete_detail')
    col.prop(p,'pipe_chase_dig')
    draw_pipe_settings(layout,p,damage=True)
    box=section(layout,'Exposed Concrete')
    material_controls.inline(box,p,'surface_damage_material',text='Exposed Concrete')
    col=box.column(align=True)
    col.prop(p,'concrete_wall_soft')
    col.prop(p,'concrete_jagged')
    col.prop(p,'concrete_outline_scale')
    col.prop(p,'concrete_terraces')
    col.prop(p,'concrete_terrace_skin')
    col.prop(p,'concrete_terrace_inset')
    col.prop(p,'concrete_thickness_cap')
    col=disclosure_box(box,'pipe_noise_open','Surface Noise')
    if col is not None:
        row=col.row(align=True)
        row.operator('project_damage.concrete_noise_preset',text='Concrete').preset='CONCRETE'
        row.operator('project_damage.concrete_noise_preset',text='Soft Concrete').preset='SOFT'
        col.prop(p,'concrete_aggregate_size')
        quality_ui.draw(col, p,'concrete_surface_detail')
        col.prop(p,'concrete_surface_distortion')
        col.prop(p,'concrete_surface_roughness')
        col.prop(p,'concrete_surface_strength')
    col=recording_tools.toggle_box(layout,p,'concrete_crack_enable','Cracks')
    if col is not None:
        material_controls.inline(col,p,'fracture_l1_material',text='Cracks')
        col.prop(p,'concrete_crack_amount')
        col.prop(p,'concrete_crack_length_min')
        col.prop(p,'concrete_crack_length_max')
        col.prop(p,'concrete_crack_width')
        col.prop(p,'concrete_crack_depth')
    from . import through_holes
    col=recording_tools.toggle_box(layout,p,'concrete_hole_enable','Break Through')
    if col is not None and through_holes.draw_controls(col, p, 'concrete'):
        col.prop(p,'concrete_hole_chance')
        col.prop(p,'concrete_hole_size')
        col.prop(p,'concrete_hole_jagged')


# подключаем свойства и действия этого модуля к Blender
def register():
    _ICONS.update(bpy.types.UILayout.bl_rna.functions['prop'].parameters['icon'].enum_items.keys())
    if PIPES_STAMP not in ownership.STAMPS:
        ownership.STAMPS = ownership.STAMPS + (PIPES_STAMP,)


# убираем подключения модуля при выключении аддона
def unregister():
    _INPUTS.clear()


def disclosure(layout,key,title):
    box=_section(layout,title)
    wm=bpy.context.window_manager
    row=box.row(align=True)
    row.prop(wm,'project_damage_'+key,text='Settings',emboss=False,icon='TRIA_DOWN' if getattr(wm,'project_damage_'+key) else 'TRIA_RIGHT')
    return box.column(align=True) if getattr(wm,'project_damage_'+key) else None
