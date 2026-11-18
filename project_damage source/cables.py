# кабели внутри повреждения: маршруты, оболочка и свободные концы
from . import quality_ui
# SPDX-License-Identifier: GPL-3.0-or-later
"""Electrical cables in the wall (2026-10-26).

* Boolean Cables: a seventh Boolean Break type. Cable runs are laid out
  inside the object; where the breakers cut through them the cables are
  pulled out of the break, held by the solid that is left.
* Cable Damage: a Walls effect built on the Concrete Damage core, like Pipe
  Damage. The spalls are chased along the cable runs (or along your strokes,
  which then are the cable routes), deep enough to uncover the cables.
* Hanging Cables: where the damage sits on a horizontal surface (a ceiling,
  or a hole through a slab) the loose cables hang down out of the hole as
  long loops and dangling cut ends.

Cables are soft: every loose stretch is a short rope simulation (gravity,
fixed length, a little stiffness, kept out of the damaged solid), so a cable
spanning a hole sags and bulges out, a cut cable droops or hangs straight
down, and on a wall it drapes over the lower edge of the hole. A cut end has
its sheath stripped back, the insulated cores fan out and end in bare copper.
Every piece is its own closed mesh island in one output object
`<source>_project_damage_cables`, which follows the source.

Python only: no core rebuild needed. It reuses the pipe layout of pipes.py
(the Pipes update), Cable Damage drives the existing Concrete Damage core
(paint strokes), Boolean Cables the existing breaker cut."""
from . import output_collections
from . import ui_image_hints
from bisect import bisect_left
import copy
import math
import hashlib

import bpy
from . import material_controls
import numpy as np
from mathutils import Matrix, Vector

from . import live, ownership, pipes
from .pipes import (_rng, _norm, _cum, _interp, _smoothstep, _bvh, _mesh_bvh, _rmf, _simplify_index,
                    _samples, MeshBuffer, classify, _clean)

CABLES_STAMP = 'project_damage_cables_of'

                         
TYPE_ITEMS = (
    ('ROUND', 'Round Sheathed', 'Round grey or white cable (NYM, NM-B) with three insulated cores'),
    ('FLAT', 'Twin & Earth', 'Flat grey cable: brown and blue cores with a bare earth between them'),
    ('WIRES', 'Single Wires', 'Loose insulated wires (brown, blue, green-yellow, black, red), no sheath'),
    ('ARMORED', 'Armored', 'Cable in a spiral metal sheath (BX, MC): stiff, ragged metal at a cut'),
    ('DATA', 'Data', 'Thin blue network cable with four coloured cores'),
)
                                                                    
CORE_COLORS = (('Brown', (0.20, 0.08, 0.03)), ('Blue', (0.02, 0.10, 0.50)), ('GreenYellow', (0.42, 0.55, 0.03)),
               ('Black', (0.015, 0.015, 0.015)), ('Grey', (0.55, 0.55, 0.55)), ('Red', (0.55, 0.02, 0.02)),
               ('Orange', (0.75, 0.25, 0.02)), ('Green', (0.03, 0.35, 0.08)))
BROWN, BLUE, GY, BLACK, GREY, RED, ORANGE, GREEN = range(2, 10)
COPPER = 1


def _ring3(r, slots, share):
    return tuple((r * math.cos(a), r * math.sin(a), share, s, True)
                 for a, s in zip((math.pi / 2, math.pi * 7 / 6, math.pi * 11 / 6), slots))


# per type: default diameter (cm, the width of a flat cable), thickness / width,
                                                                        
                                                                          
                                                      
TYPES = {
    'ROUND': dict(d=1.1, aspect=1.0, square=2.0, stiff=0.45, strip=4.0, fray=0.5, helix=0.0,
                  color=(0.78, 0.78, 0.75), metal=0.0, rough=0.55,
                  cores=_ring3(0.45, (BROWN, BLUE, GY), 0.40)),
    'FLAT': dict(d=1.0, aspect=0.42, square=3.0, stiff=0.5, strip=3.5, fray=0.4, helix=0.0,
                 color=(0.58, 0.58, 0.57), metal=0.0, rough=0.5,
                 cores=((0.0, -0.56, 0.70, BROWN, True), (0.0, 0.56, 0.70, BLUE, True), (0.0, 0.0, 0.30, COPPER, False))),
    'WIRES': dict(d=0.35, aspect=1.0, square=2.0, stiff=0.25, strip=0.9, fray=0.15, helix=0.0,
                  color=(0.2, 0.08, 0.03), metal=0.0, rough=0.4,
                  cores=((0.0, 0.0, 0.52, COPPER, False),)),
    'ARMORED': dict(d=1.4, aspect=1.0, square=2.0, stiff=0.75, strip=5.0, fray=1.0, helix=1.0,
                    color=(0.6, 0.6, 0.58), metal=1.0, rough=0.35,
                    cores=_ring3(0.42, (BLACK, GREY, GREEN), 0.36)),
    'DATA': dict(d=0.6, aspect=1.0, square=2.0, stiff=0.3, strip=2.5, fray=0.3, helix=0.0,
                 color=(0.08, 0.22, 0.62), metal=0.0, rough=0.45,
                 cores=tuple((0.45 * math.cos(a), 0.45 * math.sin(a), 0.34, s, True)
                             for a, s in zip((0.4, 2.0, 3.5, 5.1), (ORANGE, GREEN, BLUE, BROWN)))),
}
WIRE_SLOTS = (BROWN, BLUE, GY, BLACK, RED, GREY)
SIDE_ITEMS = (
    ('AUTO', 'Auto', 'Walls: the + face. Slabs and ceilings: the face that points down, so the cables hang '
                     'out of a ceiling'),
    ('POS', '+ Face', 'Just behind the face on the positive side of the axis'),
    ('NEG', '- Face', 'Just behind the face on the negative side of the axis'),
    ('MID', 'Centre', 'In the middle of the wall'),
)
HANG_ITEMS = (
    ('OFF', 'Off', 'Loose cables sag and droop, and torn ends stay short, but nothing hangs far out of the hole'),
    ('HORIZONTAL', 'Horizontal', 'Where the damage is on a horizontal surface (a ceiling, or a hole through a slab) '
                                 'the loose cables hang down out of the hole'),
    ('ALWAYS', 'Always', 'Loose cables hang down out of every hole, on walls too'),
)

_INPUTS = {}                                                                             


                                                                             
                                                           

def _type_changed(self, context):
    t = TYPES.get(self.cable_type)
    if t is not None:
        self['cable_diameter'] = t['d']
        self['cable_stiffness'] = t['stiff']
        self['cable_strip'] = t['strip']
    live.touch_prop(self, context)


def _length(name, default, description, soft_max=100.0, minimum=0.0):
    return bpy.props.FloatProperty(name=name, default=default, min=minimum, soft_max=soft_max,
                                   update=live.touch_prop, description=description)


def _factor(name, default, description, maximum=1.0):
    return bpy.props.FloatProperty(name=name, default=default, min=0.0, max=maximum, subtype='FACTOR',
                                   update=live.touch_prop, description=description)


                                                                             
                                                                              
                                                                             
def draw_axes(layout, p):
    row = layout.row(align=True)
    row.label(text='Wall Normal')
    ui_image_hints.prop(row,p, 'cable_axis_auto', toggle=True)
    if not p.cable_axis_auto:
        row = layout.row(align=True)
        for k in 'xyz':
            row.prop(p, 'cable_axis_' + k, toggle=True)
        if pipes.axes_of(p, 'cable') == ():
            layout.label(text='Assign at least one axis', icon='INFO')


PROPS = {
    'cable_damage_enable': bpy.props.BoolProperty(
        name='Cable Damage', default=False, update=live.touch_prop,
        description='Concrete Damage chased along electrical cables in the wall: the spalls uncover cables '
                    'that sag and hang out of the holes. Shares the Concrete Damage shape, crack and hole settings'),
    'cable_break_enable': bpy.props.BoolProperty(default=False, options={'HIDDEN'}, update=live.touch_prop,
                                                 description='internal: Boolean Break type Cables'),
    'cable_type': bpy.props.EnumProperty(name='Cable Type', items=TYPE_ITEMS, default='ROUND', update=_type_changed,
                                         description='Kind of cable. Choosing a type also sets its usual diameter, '
                                                     'stiffness and strip length'),
    'cable_material': bpy.props.PointerProperty(name='Cable Material', type=bpy.types.Material,
                                                update=live.touch_prop,
                                                description='Material for the cable sheath. Empty uses a built-in '
                                                            'material for the cable type'),
    'cable_layout': bpy.props.EnumProperty(name='Runs', items=pipes.LAYOUT_ITEMS, default='BOTH',
                                           update=live.touch_prop, description='Direction of the cable runs'),
    'cable_count': bpy.props.IntProperty(name='Runs per Direction', default=2, min=1, max=40, update=live.touch_prop,
                                         description='Number of cable runs for each direction, spread over the wall'),
    'cable_run_gap': _length(
        'Distance Between Cables (cm)', 0.0,
        'Minimum clear space between parallel cable runs at placement. '
        'Zero uses the original automatic spacing. Bundle Gap sets the space within a run'),
    'cable_bundle': bpy.props.IntProperty(name='Cables per Run', default=3, min=1, max=12, update=live.touch_prop,
                                          description='Cables laid side by side in one run'),
    'cable_bundle_gap': _length('Bundle Gap (cm)', 3.0, 'Space between the cables of one run', 10.0),
    'cable_diameter': _length('Diameter (cm)', 1.1, 'Outer diameter of a cable (the width of a flat cable)',
                              5.0, 0.05),
    'cable_cover': _length('Deepness (cm)', 1.0, 'Plaster or concrete in front of the cables', 20.0),
    'cable_axis': bpy.props.EnumProperty(name='Wall Normal', items=pipes.AXIS_ITEMS, default='AUTO',
                                         update=live.touch_prop),
    **pipes.axis_props('cable'),
    'cable_side': bpy.props.EnumProperty(name='Side', items=SIDE_ITEMS, default='AUTO', update=live.touch_prop,
                                         description='Which face the cables run behind'),
    'cable_seed': bpy.props.IntProperty(name='Seed', default=0, min=0, soft_max=100, update=live.touch_prop),
    'cable_turns': _factor('Turns', 0.3, 'Chance that a run turns 90 degrees'),
    'cable_embed': _length('Embed (cm)', 6.0, 'How far a cable piece continues into the wall that holds it', 50.0),
    'cable_cut': _factor('Cut', 0.4, 'Chance that a cable crossing a hole is torn through'),
    'cable_slack': bpy.props.FloatProperty(name='Slack', default=0.15, min=0.0, soft_max=2.0, update=live.touch_prop,
                                           description='Loose cable pulled out of the wall, as a share of the hole '
                                                       'width: more slack sags and bulges further out'),
    'cable_stiffness': _factor('Stiffness', 0.45, 'Soft cables follow gravity in a smooth curve; stiff ones keep '
                                                  'straighter. Choosing a type sets its usual value'),
    'cable_strip': _length('Strip Length (cm)', 4.0, 'Sheath stripped back at a cut end, showing the cores', 30.0),
    'cable_splay': _factor('Splay', 0.5, 'How far the cores fan out of a cut end'),
    'cable_fray': _factor('Frayed Sheath', 0.5, 'Torn, ragged edge where the sheath ends'),
    'cable_sides': bpy.props.IntProperty(name='Sides', default=10, min=5, max=32, update=live.touch_prop,
                                         description='Resolution around a cable'),
                            
    'cable_hang': bpy.props.EnumProperty(name='Hanging Cables', items=HANG_ITEMS, default='ALWAYS',
                                         update=live.touch_prop,
                                         description='Cables hanging down out of the hole: on horizontal surfaces '
                                                     '(ceilings, holes through a slab), everywhere, or never'),
    'cable_hang_length': _length('Hang Length (cm)', 35.0, 'How far the cables hang below the hole', 300.0),
    'cable_hang_ends': _factor('Loose Ends', 0.5, 'Share of the hanging cables that are torn through and dangle '
                                                  'as loose ends. The others hang as loops'),
    'cable_hang_sway': _factor('Sway', 0.3, 'Hanging cables drift sideways a little, each its own way'),
    'cable_hang_tilt': bpy.props.FloatProperty(name='Max Tilt', default=math.radians(35.0), min=0.0,
                                               max=math.radians(89.0), subtype='ANGLE', update=live.touch_prop,
                                               description='How far a surface may tilt from horizontal and still '
                                                           'count as a ceiling or floor'),
                         
    'cable_chase_amount': bpy.props.IntProperty(name='Amount', default=3, min=0, max=200, update=live.touch_prop,
                                                description='Number of damaged stretches along the cables'),
    'cable_chase_length_min': _length('Length Min (cm)', 20.0, 'Shortest damaged stretch', 400.0),
    'cable_chase_length_max': _length('Length Max (cm)', 55.0, 'Longest damaged stretch', 400.0),
    'cable_chase_width': bpy.props.FloatProperty(name='Width', default=3.0, min=0.5, soft_max=8.0,
                                                 update=live.touch_prop,
                                                 description='Width of the damage, as a multiple of the cable run '
                                                             '(all its cables) width'),
    'cable_chase_dig': bpy.props.FloatProperty(name='Dig Depth', default=1.2, min=0.0, soft_max=3.0,
                                               update=live.touch_prop,
                                               description='How deep the damage goes past the cover, as a share of '
                                                           'the cable diameter. Above 0.5 the cables come loose'),
}


                                                                             
                                          

class Settings:
    """Also carries the field names the pipe layout of pipes.py reads."""
    def __init__(self, p, gscale, source=None, points=None, polys=None):
        t = TYPES.get(p.cable_type, TYPES['ROUND'])
        self.type, self.t = p.cable_type, t
        self.Dw = max(p.cable_diameter, 0.05) * gscale
        self.Dt = self.Dw * t['aspect']
        self.hw, self.ht = 0.5 * self.Dw, 0.5 * self.Dt
        self.D = self.Dw                                                       
        self.R = self.ht                                                        
        self.cover = max(p.cable_cover, 0.0) * gscale
        self.bundle = max(1, int(p.cable_bundle))
        self.gap = max(p.cable_bundle_gap, 0.0) * gscale
        self.run_gap = p.cable_run_gap * gscale
        self.half_width = 0.5 * (self.bundle * self.Dw + (self.bundle - 1) * self.gap)
        self.embed = max(p.cable_embed, 0.0) * gscale
        self.joint = 0.0
        self.cut, self.slack, self.stiff = p.cable_cut, max(p.cable_slack, 0.0), p.cable_stiffness
        self.strip = max(p.cable_strip, 0.0) * gscale
        self.splay, self.fray = p.cable_splay, p.cable_fray
        self.sides = int(p.cable_sides)
        self.seed = int(p.cable_seed)
        self.layout, self.count = p.cable_layout, int(p.cable_count)
        self.axes = pipes.axes_of(p, 'cable')
        self.axis = 'XYZ'[self.axes[0]] if self.axes else _resolve_axis('AUTO', points, polys)
        self.side = _resolve_side(p.cable_side, self.axis, source, points)
        self.elbows = p.cable_turns
        self.hang = p.cable_hang
        self.hang_len = max(p.cable_hang_length, 0.0) * gscale
        self.hang_ends, self.sway = p.cable_hang_ends, p.cable_hang_sway
        self.hang_cos = math.cos(min(max(p.cable_hang_tilt, 0.0), math.radians(89.0)))
        self.chase_amount = int(p.cable_chase_amount)
        lo, hi = sorted((p.cable_chase_length_min, p.cable_chase_length_max))
        self.chase_len = (lo * gscale, hi * gscale)
        self.chase_width = p.cable_chase_width
        self.chase_dig = p.cable_chase_dig


def _scale(source):
    """The object's scale. The cables are laid out and built with it applied,
    so a wall whose Scale was never applied gets round cables and the right
    wall normal."""
    s = np.abs(np.array(source.matrix_world.to_scale()[:], dtype=np.float64))
    return np.maximum(s, 1e-6)


def _down(source):
    """World down in the object's axes, rotation only (the scale is applied)."""
    d = source.matrix_world.to_quaternion().to_matrix().inverted() @ Vector((0.0, 0.0, -1.0))
    return np.array(d.normalized()[:] if d.length > 1e-9 else (0.0, 0.0, -1.0))


def _resolve_axis(axis, points, polys):
    """Auto wall normal: the axis the most face area points along (the big
    faces of a wall, slab or ceiling, in whatever direction it faces); the
    thinnest side breaks a tie (a cube)."""
    if axis != 'AUTO' or points is None or len(points) == 0:
        return axis
    pts = np.asarray(points, dtype=np.float64)
    ext = pts.max(axis=0) - pts.min(axis=0)
    thin = int(np.argmin(ext))
    if polys is not None and len(polys):
        f = np.asarray(polys)
        tris = [f[:, :3]]
        quad = f[:, 2] != f[:, 3]
        if quad.any():
            tris.append(f[quad][:, [0, 2, 3]])
        t = np.vstack(tris)
        av = np.cross(pts[t[:, 1]] - pts[t[:, 0]], pts[t[:, 2]] - pts[t[:, 0]])
        score = np.abs(av).sum(axis=0)
        order = np.argsort(score)[::-1]
        if score[order[0]] > 1.15 * score[order[1]]:
            return 'XYZ'[int(order[0])]
        if thin in order[:2]:
            return 'XYZ'[thin]
        return 'XYZ'[int(order[0])]
    return 'XYZ'[thin]


def _resolve_side(side, axis, source, points):
    """Auto: + face on walls; on a slab, the face that points down (a ceiling)."""
    if side != 'AUTO':
        return side
    if source is None or points is None or len(points) == 0:
        return 'POS'
    _lo, _hi, a, _up, _h = pipes._axes(np.asarray(points), axis)
    d = source.matrix_world.to_3x3() @ Vector(np.eye(3)[a])
    if d.length < 1e-9:
        return 'POS'
    d.normalize()
    if abs(d.z) > 0.7:
        return 'NEG' if d.z > 0 else 'POS'
    return 'POS'


                                                                             
                                                           

def _sway(S, rng, side, down, hang):
    """(direction, amount, waves, phase) of the sideways drift of a cable."""
    d = _norm(np.cross(_norm(down), side) * rng.uniform(-1.0, 1.0) + side * rng.uniform(-1.0, 1.0))
    amount = S.sway * rng.uniform(0.3, 1.0) * (1.0 if hang else 0.4)
    return (d, amount, rng.uniform(0.5, 1.6), rng.uniform(0.0, 6.283))


class Rope:
    """A loose stretch: points (with pinned anchors), rest lengths, simulated together."""
    def __init__(self, X, pinned, radius, force):
        self.X = np.ascontiguousarray(X, dtype=np.float64)
        self.pinned = np.asarray(pinned, dtype=bool)
        seg = np.linalg.norm(np.diff(self.X, axis=0), axis=1)
        self.rest = seg
        self.radius = radius
        self.force = force                                            
        self.hang = False


class Piece:
    def __init__(self, start_free=False):
        self.chunks = []                                                             
        self.start_free = start_free
        self.end_free = False
        self.slot = 0                              


def _rope_len_points(length, ds):
    n = max(3, int(math.ceil(length / max(ds, 1e-6))))
    return n, length / n


def _free_rope(S, A, Aback, T, nout, down, length, hang, rng, side):
    """Free (cut) end anchored at A (Aback one step behind it, inside the solid)."""
    ds_goal = max(0.5 * S.Dw, length / 300.0)
    n, ds = _rope_len_points(length, ds_goal)
    s = np.arange(1, n + 1) * ds
    out_dir = _norm(down) if hang else _norm(nout * 0.9 + down * 0.6)
    r0 = min(length * 0.3, max(2.0 * S.Dw, S.cover + 2.0 * S.Dt))
    lead = np.minimum(s, r0)
    pts = A + T * lead[:, None] * 0.6 + nout * lead[:, None] * 0.6 + out_dir * np.maximum(s - r0, 0.0)[:, None]
    X = np.vstack((Aback, A, pts))
    pinned = np.zeros(len(X), dtype=bool)
    pinned[:2] = True
    rope = Rope(X, pinned, max(S.hw, S.ht), _sway(S, rng, side, down, hang))
    rope.rest[1:] = ds
    rope.hang = hang
    return rope


def _loop_rope(S, A, Aback, B, Bfwd, nout, down, length, hang, rng, side):
    """Loose stretch held at both ends: a sagging loop of `length`."""
    chord = B - A
    L = float(np.linalg.norm(chord))
    ds_goal = max(0.5 * S.Dw, length / 300.0)
    n, ds = _rope_len_points(max(length, L * 1.001), ds_goal)
    t = np.linspace(0.0, 1.0, n + 1)[1:-1]
    o = _norm(down) if hang else _norm(nout + down * 0.5)
    base = A + chord * t[:, None]
    bump = (np.sin(math.pi * t) ** 0.9)[:, None]

    def total(h):
        pts = np.vstack((A, base + o * (h * bump), B))
        return np.linalg.norm(np.diff(pts, axis=0), axis=1).sum()
    lo, hi = 0.0, max(length, 1e-6)
    for _ in range(40):
        mid = 0.5 * (lo + hi)
        if total(mid) < length:
            lo = mid
        else:
            hi = mid
    pts = base + o * (lo * bump)
    X = np.vstack((Aback, A, pts, B, Bfwd))
    pinned = np.zeros(len(X), dtype=bool)
    pinned[:2] = True
    pinned[-2:] = True
    rope = Rope(X, pinned, max(S.hw, S.ht), _sway(S, rng, side, down, hang))
    rope.hang = hang
    seg = rope.rest.copy()
    seg[1:-1] = ds
    rope.rest = seg
    return rope


def _free_len(S, span, extra, hang, rng):
    """A torn end: hanging, it drops about Hang Length below the hole; else it
    is the cut-off stretch with a little slack pulled out of the wall."""
    if hang:
        return min(span, 2.0 * S.Dw) + extra
                                                                        
    return min(span, 12.0 * S.Dw + 0.25 * span) * (1.0 + 0.5 * S.slack * rng.uniform(0.5, 1.5)) + extra


def _hangs(S, q, n, down, res_bvh):
    if S.hang == 'OFF':
        return False
    if S.hang == 'ALWAYS':
        return True
    c = float(n @ down)
    if c >= S.hang_cos:
        return True                                                        
    if c <= -S.hang_cos and res_bvh is not None:
                                                                 
        loc, _n, _i, _d = res_bvh.ray_cast(Vector(q), Vector(down), S.hang_len * 0.5 + S.Dw)
        return loc is None
    return False


def plan_cable(S, path, cum, nout, iv, rng, down, res_bvh, ropes):
    """Turn the intervals of one cable into pieces (fixed stretches and ropes)."""
    n = len(iv)
    regions = []
    i = 0
    while i < n:
        if not iv[i][3]:
            i += 1
            continue
        j = i
        while j + 1 < n and (iv[j + 1][3] or (iv[j + 1][2] == 'S' and j + 2 < n and iv[j + 2][3]
                                               and iv[j + 1][1] - iv[j + 1][0] < 2.0 * S.embed + S.Dw)):
            j += 1
        regions.append((i, j))
        i = j + 1
    D = S.Dw
    tang = _norm(np.gradient(path, axis=0))
    side_all = _norm(np.cross(nout, tang))

    def at(u):
        return _interp(np.array([u]), cum, path)[0]

    def nat(u):
        return _norm(_interp(np.array([u]), cum, nout))[0]

    def tat(u):
        return _norm(_interp(np.array([u]), cum, tang))[0]

    def sat(u):
        return _norm(_interp(np.array([u]), cum, side_all))[0]

    pieces = []
    visible_all = []
    for i, j in regions:
        held = any(iv[k][2] == 'S' for k in range(i, j + 1)) or (i > 0 and iv[i - 1][2] == 'S') \
            or (j + 1 < n and iv[j + 1][2] == 'S')
        if not held:
            continue                                       
        visible = [(iv[k][0], iv[k][1]) for k in range(i, j + 1) if iv[k][3]]
        if sum(x1 - x0 for x0, x1 in visible) < 0.5 * S.Dw:
            continue                                                                   
        visible_all.extend(visible)
        segs = []
        if i > 0 and iv[i - 1][2] == 'S':
            u0 = iv[i][0] - min(S.embed * rng.uniform(0.6, 1.4), iv[i - 1][1] - iv[i - 1][0])
            segs.append([u0, iv[i][0], 'F'])
        for k in range(i, j + 1):
            kind = 'E' if iv[k][2] == 'E' else 'F'
            if segs and segs[-1][2] == kind:
                segs[-1][1] = iv[k][1]
            else:
                segs.append([iv[k][0], iv[k][1], kind])
        if j + 1 < n and iv[j + 1][2] == 'S':
            u1 = iv[j][1] + min(S.embed * rng.uniform(0.6, 1.4), iv[j + 1][1] - iv[j + 1][0])
            if segs[-1][2] == 'F':
                segs[-1][1] = u1
            else:
                segs.append([iv[j][1], u1, 'F'])
        step = max(0.5 * D, 1e-3)
        cur = None
        for k, (x0, x1, kind) in enumerate(segs):
            if kind == 'F':
                if cur is None:
                    cur = Piece(start_free=False)
                us = _samples(x0, x1, max(0.6 * D, 0.2), max(2.0 * D, 1.0), visible, D)
                cur.chunks.append(('fixed', _interp(us, cum, path), _norm(_interp(us, cum, nout)), us))
                continue
            hl = k > 0 and segs[k - 1][2] == 'F'
            hr = k + 1 < len(segs) and segs[k + 1][2] == 'F'
            L = x1 - x0
            mid = 0.5 * (x0 + x1)
            nm = nat(mid)
            hang = _hangs(S, at(mid), nm, down, res_bvh)
            side = sat(mid)
            extra_free = (S.hang_len * rng.uniform(0.5, 1.15) + S.cover) if hang else D * rng.uniform(1.0, 3.0)
            if hl and hr:
                cut = rng.random() < (S.hang_ends if hang else S.cut)
                if cut and L > 1.5 * D:
                    xc = x0 + L * rng.uniform(0.3, 0.7)
                    r1 = _free_rope(S, at(x0), at(x0 - step), tat(x0), nat(x0), down,
                                    _free_len(S, xc - x0, extra_free, hang, rng), hang, rng, side)
                    ropes.append(r1)
                    cur.chunks.append(('rope', r1, False))
                    cur.end_free = True
                    pieces.append(cur)
                    extra2 = (S.hang_len * rng.uniform(0.5, 1.15) + S.cover) if hang else D * rng.uniform(1.0, 3.0)
                    r2 = _free_rope(S, at(x1), at(x1 + step), -tat(x1), nat(x1), down,
                                    _free_len(S, x1 - xc, extra2, hang, rng), hang, rng, side)
                    ropes.append(r2)
                    cur = Piece(start_free=True)
                    cur.chunks.append(('rope', r2, True))
                else:
                    if hang:
                        h = S.hang_len * rng.uniform(0.45, 1.0)
                        length = max(2.0 * math.hypot(0.5 * L, h), L * (1.0 + S.slack))
                    else:
                        length = L * (1.0 + S.slack * rng.uniform(0.5, 1.5))
                    r = _loop_rope(S, at(x0), at(x0 - step), at(x1), at(x1 + step), nm, down, length, hang, rng, side)
                    ropes.append(r)
                    cur.chunks.append(('rope', r, False))
            elif hl:
                r = _free_rope(S, at(x0), at(x0 - step), tat(x0), nat(x0), down,
                               _free_len(S, L, extra_free, hang, rng), hang, rng, side)
                ropes.append(r)
                cur.chunks.append(('rope', r, False))
                cur.end_free = True
                pieces.append(cur)
                cur = None
            elif hr:
                if cur is not None and cur.chunks:
                    pieces.append(cur)
                r = _free_rope(S, at(x1), at(x1 + step), -tat(x1), nat(x1), down,
                               _free_len(S, L, extra_free, hang, rng), hang, rng, side)
                ropes.append(r)
                cur = Piece(start_free=True)
                cur.chunks.append(('rope', r, True))
            else:
                if cur is not None and cur.chunks:
                    pieces.append(cur)
                cur = None
        if cur is not None and cur.chunks:
            pieces.append(cur)
    for pc in pieces:
        pc.visible = visible_all
    return pieces


                                                                             
                                                             

def simulate(S, ropes, down, res_bvh, steps=90, iters=10):
    if not ropes:
        return
    sizes = [len(r.X) for r in ropes]
    offs = np.concatenate(([0], np.cumsum(sizes)[:-1]))
    X = np.vstack([r.X for r in ropes])
    free = ~np.concatenate([r.pinned for r in ropes])
    seg_i, seg_j, rest, par = [], [], [], []
    tri = []
    rad = np.empty(len(X))
    force = np.zeros_like(X)
    grav = np.zeros(len(X))
    for r, o, m in zip(ropes, offs, sizes):
        k = np.arange(m - 1)
        seg_i.append(o + k)
        seg_j.append(o + k + 1)
        rest.append(r.rest)
        par.append(k % 2)
        tri.append(np.stack((o + np.arange(m - 2), o + np.arange(1, m - 1), o + np.arange(2, m)), axis=1))
        rad[o:o + m] = r.radius
                                                                             
        g = (0.05 if r.hang else 0.02) * float(np.median(r.rest))
        grav[o:o + m] = g
        d, amount, waves, phase = r.force
        t = np.linspace(0.0, 1.0, m)
        force[o:o + m] = d * (g * amount * (0.5 + np.sin(2.0 * math.pi * waves * t + phase)))[:, None]
    seg_i, seg_j = np.concatenate(seg_i), np.concatenate(seg_j)
    rest, par = np.concatenate(rest), np.concatenate(par)
    tri = np.vstack(tri)
    tri = tri[free[tri[:, 1]]]
    w = free.astype(np.float64)
    groups = []
    for p_ in (0, 1):
        sel = (par == p_) & ((w[seg_i] + w[seg_j]) > 0)
        groups.append((seg_i[sel], seg_j[sel], rest[sel]))
    G = (down[None, :] * grav[:, None] + force)[free]
    stiff = 0.01 + 0.12 * float(np.clip(S.stiff, 0.0, 1.0)) ** 2
    Xp = X.copy()
    fidx = np.flatnonzero(free)

    def collide():
        if res_bvh is None:
            return
        for q in fidx:
            p = X[q]
            loc, nrm, _i, _d = res_bvh.find_nearest(Vector(p), rad[q] + 2.0 * grav[q] * 20.0 + 1e-4)
            if loc is None:
                continue
            l = np.array(loc[:])
            nn = np.array(nrm[:])
            if (p - l) @ nn < rad[q]:
                X[q] = l + nn * (rad[q] * 1.02)
                Xp[q] = X[q] * 0.5 + Xp[q] * 0.5

    for step in range(steps):
        V = (X - Xp) * 0.9
        Xp = X.copy()
        X[free] += V[free] + G
        for _ in range(iters):
            for i, j, r0 in groups:
                d = X[j] - X[i]
                L = np.linalg.norm(d, axis=1)
                wi, wj = w[i], w[j]
                k = (L - r0) / np.maximum(L, 1e-12) / np.maximum(wi + wj, 1e-12)
                corr = d * k[:, None]
                X[i] += corr * wi[:, None]
                X[j] -= corr * wj[:, None]
        if len(tri):
            a, b, c = tri[:, 0], tri[:, 1], tri[:, 2]
            X[b] += stiff * (0.5 * (X[a] + X[c]) - X[b])
        if step % 6 == 5 or step == steps - 1:
            collide()
                                                     
    for _ in range(iters):
        for i, j, r0 in groups:
            d = X[j] - X[i]
            L = np.linalg.norm(d, axis=1)
            wi, wj = w[i], w[j]
            k = (L - r0) / np.maximum(L, 1e-12) / np.maximum(wi + wj, 1e-12)
            corr = d * k[:, None]
            X[i] += corr * wi[:, None]
            X[j] -= corr * wj[:, None]
    for r, o, m in zip(ropes, offs, sizes):
        r.X = X[o:o + m].copy()


                                                                             
          

def _section(K, square):
    phi = np.arange(K) * (2.0 * math.pi / K)
    c, s = np.cos(phi), np.sin(phi)
    e = 2.0 / max(square, 2.0)
    return np.sign(c) * np.abs(c) ** e, np.sign(s) * np.abs(s) ** e, phi


def _frames(C, n0):
    T, N, B = _rmf(C, n0)
    return T, N, B


def _tube(buf, C, N, B, ha, hb, K, square, mat, part, expo, rmod=None, pull0=None, pull1=None,
          *, shell=None, buried=None):
    """Closed tube along C (J, 3) with half sizes ha (along N) and hb (along B)."""
    J = len(C)
    if J < 2:
        return False
    ca, cb, _phi = _section(K, square)
    ha = np.broadcast_to(ha, (J,))
    hb = np.broadcast_to(hb, (J,))
    ra = ha[:, None] * ca[None, :]
    rb = hb[:, None] * cb[None, :]
    if rmod is not None:
        ra = ra * rmod
        rb = rb * rmod
    rings = C[:, None, :] + ra[..., None] * N[:, None, :] + rb[..., None] * B[:, None, :]
    if shell is not None:
        centers = C[:, None, :]
        rings = centers + (rings - centers) * pipes._hidden_ring_scales(shell, C, rings, buried)
    T0 = _norm(C[1] - C[0])
    T1 = _norm(C[-1] - C[-2])
    c0, c1 = C[0].copy(), C[-1].copy()
    if pull0 is not None:
        rings[0] += T0 * pull0[:, None]
        c0 = c0 + T0 * float(pull0.mean())
    if pull1 is not None:
        rings[-1] -= T1 * pull1[:, None]
        c1 = c1 - T1 * float(pull1.mean())
    verts = np.vstack((rings.reshape(-1, 3), c0, c1))
    faces = []
    for j in range(J - 1):
        for k in range(K):
            k1 = (k + 1) % K
            faces.append((j * K + k, j * K + k1, (j + 1) * K + k1, (j + 1) * K + k))
    e0, e1 = J * K, J * K + 1
    last = (J - 1) * K
    for k in range(K):
        k1 = (k + 1) % K
        faces.append((e0, k1, k))
        faces.append((e1, last + k, last + k1))
    ex = np.broadcast_to(np.asarray(expo, dtype=np.float32), (J,))
    buf.add(verts, faces, mat, True, part, np.concatenate((np.repeat(ex, K), [ex[0], ex[-1]])))
    return True


def _cut_at(s, arr, a, b):
    """Rows of `arr` sampled at arc lengths s, clipped to [a, b] (both ends interpolated)."""
    keep = (s > a + 1e-9) & (s < b - 1e-9)
    sa = np.concatenate(([a], s[keep], [b]))
    return sa, _interp(sa, s, arr)


def _fray(S, rng, K):
    amt = S.fray * S.t['fray'] * 0.6 * S.Dw
    if amt <= 0:
        return np.zeros(K)
    phi = np.arange(K) * (2.0 * math.pi / K)
    w = np.zeros(K)
    for m in (2, 3, 5):
        w += rng.uniform(0.3, 1.0) / m * np.sin(m * phi + rng.uniform(0, 6.283))
    w = (w - w.min()) / max(w.max() - w.min(), 1e-9)
    if S.t['helix'] > 0:
        teeth = rng.random(K) < 0.3
        w[teeth] = np.minimum(1.0, w[teeth] + 0.6)
    return w * amt


def _cores(S, buf, part, C, N, B, s, s_sheath, rng, down):
    """Cores coming out of a stripped end at the END of C (arc length s)."""
    t = S.t
    s_end = s[-1]
    strip = s_end - s_sheath
    if strip <= 1e-6:
        return part
    start = max(s_sheath - 0.8 * S.Dt, s[0])
    for (a, b, rshare, slot, insulated) in t['cores']:
        rc = rshare * S.ht
        trim = rng.uniform(0.0, 0.35) * strip
        e = s_end - trim
        if e - start < 2.0 * rc:
            continue
        step = max(0.5 * rc, 0.02)
        us = np.linspace(start, e, max(3, int(math.ceil((e - start) / step)) + 1))
        Cc = _interp(us, s, C)
        Nc = _norm(_interp(us, s, N))
        Bc = _norm(_interp(us, s, B))
        f = _smoothstep((us - s_sheath) / max(strip, 1e-6))
        spread = 1.0 + S.splay * 2.2 * f
        pos = Cc + (a * S.ht * spread)[:, None] * Nc + (b * S.hw * spread)[:, None] * Bc
        if a == 0 and b == 0:
            jitter = _norm(Nc * rng.uniform(-1, 1) + Bc * rng.uniform(-1, 1))
            pos = pos + jitter * (S.splay * 0.6 * S.ht * f)[:, None]
        pos = pos + down * (S.splay * 0.25 * strip * f ** 2)[:, None]
        T, Nk, Bk = _rmf(pos, Nc[0])
        sk = _cum(pos)
        bare = min(rng.uniform(0.25, 0.6) * strip, max(sk[-1] - 2.0 * rc, 0.0)) if insulated else sk[-1]
        mat = slot if insulated else COPPER
        K = max(6, S.sides // 2 + 2)
        if insulated:
            cut = sk[-1] - bare
            sa, Pa = _cut_at(sk, pos, sk[0], cut)
            if len(Pa) >= 2 and cut > sk[0] + 1e-6:
                Ta, Na, Ba = _rmf(Pa, Nc[0])
                _tube(buf, Pa, Na, Ba, rc, rc, K, 2.0, mat, part, 1.0)
                part += 1
            sb, Pb = _cut_at(sk, pos, max(cut - 0.5 * rc, sk[0]), sk[-1])
            if len(Pb) >= 2:
                Tb, Nb, Bb = _rmf(Pb, Nc[0])
                _tube(buf, Pb, Nb, Bb, rc * 0.55, rc * 0.55, K, 2.0, COPPER, part, 1.0)
                part += 1
        else:
            _tube(buf, pos, Nk, Bk, rc, rc, K, 2.0, COPPER, part, 1.0)
            part += 1
    return part


def build_piece(S, buf, part, piece, rng, down, *, shell=None):
    """One continuous cable piece: sheath (stripped at free ends) and its cores."""
    recipe_state = rng.bit_generator.state
    first_part = part
    pts, nrm, us = [], [], []
    for ch in piece.chunks:
        if ch[0] == 'fixed':
            _k, P, Nn, u = ch
            pts.append(P)
            nrm.append(Nn)
            us.append(u)
        else:
            _k, rope, rev = ch
            X = rope.X
                                                                     
            if rope.pinned[-1] and rope.pinned[-2]:
                X = X[1:-1]
            else:
                X = X[1:]
            if rev:
                X = X[::-1]
            pts.append(X)
            nrm.append(np.full_like(X, np.nan))
            us.append(np.full(len(X), np.nan))
    C = np.vstack(pts)
    Nn = np.vstack(nrm)
    U = np.concatenate(us)
    keep = np.r_[True, np.linalg.norm(np.diff(C, axis=0), axis=1) > 1e-6]
    C, Nn, U = C[keep], Nn[keep], U[keep]
    if len(C) < 2:
        return part
    ok = np.isfinite(Nn[:, 0])
    n0 = Nn[np.flatnonzero(ok)[0]] if ok.any() else np.array([0.0, 0.0, 1.0])
    T, N, B = _frames(C, n0)
    s = _cum(C)
    total = s[-1]
    if total < 0.5 * S.Dw:
        return part
                                                                                
    vis = np.ones(len(C), dtype=np.float32)
    for q in np.flatnonzero(np.isfinite(U)):
        vis[q] = 1.0 if any(x0 - 0.5 * S.Dw <= U[q] <= x1 + 0.5 * S.Dw for x0, x1 in piece.visible) else 0.0
    strip0 = strip1 = 0.0
    if piece.start_free:
        strip0 = min(S.strip * rng.uniform(0.6, 1.3), 0.45 * total)
    if piece.end_free:
        strip1 = min(S.strip * rng.uniform(0.6, 1.3), 0.45 * total)
    if total - strip0 - strip1 < S.Dw:
        strip0 = strip1 = 0.0
    a, b = strip0, total - strip1
    sa, Cs = _cut_at(s, C, a, b)
    Ns = _norm(_interp(sa, s, N))
    Bs = _norm(_interp(sa, s, B))
                                           
    Ts = _norm(np.gradient(Cs, axis=0))
    Ns = _norm(Ns - Ts * np.sum(Ns * Ts, axis=1)[:, None])
    Bs = np.cross(Ts, Ns)
    vs = np.interp(sa, s, vis)
    K = S.sides
    rmod = None
    if S.t['helix'] > 0:
        phi = _section(K, 2.0)[2]
        pitch = 0.45 * S.Dw
        rmod = 1.0 + 0.07 * np.sin(2.0 * math.pi * sa[:, None] / pitch - phi[None, :])
    seg_last = float(np.linalg.norm(Cs[-1] - Cs[-2])) if len(Cs) > 1 else S.Dw
    seg_first = float(np.linalg.norm(Cs[1] - Cs[0])) if len(Cs) > 1 else S.Dw
    pull0 = np.minimum(_fray(S, rng, K), 0.6 * seg_first) if piece.start_free else None
    pull1 = np.minimum(_fray(S, rng, K), 0.6 * seg_last) if piece.end_free else None
    slot = piece.slot
    buried = np.interp(sa, s, pipes._buried_samples(U, piece.visible)) >= 1.0 if shell is not None else None
    if _tube(buf, Cs, Ns, Bs, S.ht, S.hw, K, S.t['square'], slot, part, vs, rmod, pull0, pull1,
             shell=shell, buried=buried):
        part += 1
    if piece.end_free and strip1 > 0:
        part = _cores(S, buf, part, C, N, B, s, b, rng, down)
    if piece.start_free and strip0 > 0:
        Cr = C[::-1].copy()
        sr = _cum(Cr)
        _Tr, Nr, Br = _rmf(Cr, N[-1])
        part = _cores(S, buf, part, Cr, Nr, Br, sr, total - a, rng, down)
    from . import tube_restructure
    tube_restructure.record(buf,C,N[0],piece.start_free,piece.end_free,piece.slot,first_part,recipe_state)
    return part


def build_cables(S, runs, src_bvh, res_bvh, down, *, boolean=False):
    """Plan every cable, simulate all loose stretches together, then build."""
    buf = MeshBuffer()
    ropes, plans = [], []
    counts = dict(pieces=0, cut=0)
    for run in runs:
        for j, (path, nout) in enumerate(pipes.pipe_paths(S, run)):
            idx = _simplify_index(path, 1e-3)
            spath, snout = path[idx], nout[idx]
            cum = _cum(spath)
            if cum[-1] <= 0:
                continue
            iv = _clean(classify(spath, cum, snout, S.R, src_bvh, res_bvh, inside_source=boolean), S.Dw)
            rng = _rng(S.seed, 404, run.index, j)
            pieces = plan_cable(S, spath, cum, snout, iv, rng, down, res_bvh, ropes)
            slot = WIRE_SLOTS[(run.index * 7 + j) % len(WIRE_SLOTS)] if S.type == 'WIRES' else 0
            for pc in pieces:
                pc.slot = slot
            plans.append((pieces, rng))
            counts['cut'] += sum(int(pc.start_free) + int(pc.end_free) for pc in pieces)
    simulate(S, ropes, down, res_bvh)
    part = 0
    for pieces, rng in plans:
        for pc in pieces:
            p0 = part
            part = build_piece(S, buf, part, pc, rng, down, shell=src_bvh if boolean else None)
            counts['pieces'] += int(part > p0)
    return buf, counts


                                                                             
                             

def _type_material(kind):
    if kind not in {'WIRES','DATA'}:
        from .pipe_material_library import load
        material=load('Black Cable Material')
        if material is not None:return material
    t = TYPES[kind]
    name = 'ProjectDamage_Cable_' + {'ROUND': 'Round', 'FLAT': 'TwinEarth', 'WIRES': 'Wire', 'ARMORED': 'Armored',
                                     'DATA': 'Data'}[kind]
    return pipes._plain_material(name, t['color'], t['rough'], t['metal'])


def cables_of(source, instance=0):
    from . import wall_stack
    return next(wall_stack.output_objects(source, CABLES_STAMP, instance), None)


def _remove(source, instance=0):
    obj = cables_of(source, instance)
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
    a = mesh.attributes.new('cable_id', 'INT', 'FACE')
    a.data.foreach_set('value', np.asarray(buf.part, dtype=np.int32))
    e = mesh.attributes.new('cable_exposed', 'FLOAT', 'POINT')
    e.data.foreach_set('value', np.concatenate(buf.expo).astype(np.float32))
    from .tube_restructure import write_recipes
    write_recipes(mesh,buf,factor)
    from .material_controls import automatic_default
    mesh.materials.append(automatic_default(p,'cable_material',p.cable_type,_type_material))
    mesh.materials.append(pipes._plain_material('ProjectDamage_Cable_Copper', (0.80, 0.38, 0.20), 0.3, 1.0))
    for label, c in CORE_COLORS:
        mesh.materials.append(pipes._plain_material('ProjectDamage_Cable_' + label, c, 0.45))
    return mesh


def _place(context, source, mesh, instance=0):
    obj = cables_of(source, instance)
    name = source.name + '_project_damage_cables' + (f' #{instance}' if instance else '')
    if obj is None:
        obj = bpy.data.objects.new(name, mesh)
    else:
        old = obj.data
        obj.data = mesh
        if old is not None and old.users == 0:
            bpy.data.meshes.remove(old)
    obj.name = name
    mesh.name = name
    obj[CABLES_STAMP] = source.get('project_damage_uid')
    if instance:
        from .wall_stack import INSTANCE
        obj[INSTANCE] = instance
    ownership.bind(source, obj)
    output_collections.link_object(context,source,obj)
    obj.matrix_world = source.matrix_world
    return obj


                                                                             
             

def boolean_wanted(p):
    return bool(getattr(p, 'cable_break_enable', False) and p.ui_boolean_enable and not p.rebar_enable
                and not p.all_off)


def damage_wanted(p):
    return bool(getattr(p, 'cable_damage_enable', False) and not p.all_off)


def wanted(p):
    return (boolean_wanted(p) or damage_wanted(p)) and pipes.axes_of(p, 'cable') != ()


def _drawn(context, source, factor, *, props=None):
    from . import paint
    p = source.project_damage if props is None else props
    s = _scale(source)
    return [np.ascontiguousarray(path * factor * s, dtype=np.float64)
            for path in paint.read_paths(context, source, p.concrete_drawing)]


def paint_areas(S, strokes, points, radius, src_bvh=None, axes=None):
    """Cable Damage, Paint: the painted damage, sorted by the face it was
    painted on: every stroke point takes the direction of the face nearest to
    it (local X, Y or Z, + or -), so a stroke over an edge splits between the
    two faces. With `axes` (chosen by hand) only those axes are used: the one
    closest to the face normal. Strokes on one face that overlap make one
    area. Returns [(axis, side, [(lo, hi, stroke points), ...]), ...]."""
    points = np.asarray(points, dtype=np.float64)
    mid = 0.5 * (points.min(0) + points.max(0))
    allowed = tuple(axes) if axes else (0, 1, 2)
    step = max(0.5 * radius, 0.5 * S.Dw, 0.5)
    groups = {}
    for st in strokes:
        st = np.asarray(st, dtype=np.float64)
        if len(st) == 0:
            continue
        if len(st) > 1:
            st = _densify(st, step)
        labels = []
        for q in st:
            n = None
            if src_bvh is not None:
                loc, n, _i, _d = src_bvh.find_nearest(Vector(q))
            if n is not None:
                k = max(allowed, key=lambda i: abs(n[i]))
                if axes and abs(n[k]) < 0.3:
                    labels.append(None)                                                    
                    continue
                sgn = 1 if n[k] >= 0 else -1
            else:
                k = allowed[0] if axes else int(np.argmin(np.ptp(points, axis=0)))
                sgn = 1 if q[k] >= mid[k] else -1
            labels.append((k, sgn))
        i = 0
        while i < len(st):
            j = i
            while j + 1 < len(st) and labels[j + 1] == labels[i]:
                j += 1
            if labels[i] is not None:
                groups.setdefault(labels[i], []).append(st[i:j + 1])
            i = j + 1
    out = []
    for (a, sign), pieces in sorted(groups.items()):
        up = 2 if a != 2 else 1
        h = 3 - a - up
        boxes = [[q.min(0) - radius, q.max(0) + radius, [q]] for q in pieces]
        merged = True
        while merged:
            merged = False
            for i in range(len(boxes)):
                for j in range(i + 1, len(boxes)):
                    A, B = boxes[i], boxes[j]
                    if all(A[0][k] <= B[1][k] and B[0][k] <= A[1][k] for k in (up, h)):
                        A[0], A[1] = np.minimum(A[0], B[0]), np.maximum(A[1], B[1])
                        A[2] += B[2]
                        del boxes[j]
                        merged = True
                        break
                if merged:
                    break
        out.append(('XYZ'[a], 'POS' if sign > 0 else 'NEG', [(bx[0], bx[1], np.vstack(bx[2])) for bx in boxes]))
    return out


def damage_areas(S, points, polys, res_bvh, axes=None, max_samples=40000):
    """Boolean Cables, Wall Normal Auto: the broken areas of the surface,
    sorted by face like paint_areas. The source surface is sampled; a sample
    the damaged result no longer reaches was broken away. Each face direction
    (local X, Y or Z, + or -) gets its own areas, so every break on every
    face gets cables, the way Boolean Rebar fills every break with rebar.
    With `axes` (chosen by hand) only those axes are used: the one closest to
    the face normal."""
    points = np.asarray(points, dtype=np.float64)
    tris = []
    for poly in polys:
        poly = [int(i) for i in poly if int(i) >= 0]
        for k in range(1, len(poly) - 1):
            tris.append((poly[0], poly[k], poly[k + 1]))
    if not tris or res_bvh is None:
        return []
    T = points[np.asarray(tris)]
    cr = np.cross(T[:, 1] - T[:, 0], T[:, 2] - T[:, 0])
    area = 0.5 * np.linalg.norm(cr, axis=1)
    keep = area > 1e-9
    T, cr, area = T[keep], cr[keep], area[keep]
    if not len(T):
        return []
    step = max(math.sqrt(area.sum() / max_samples), 0.5 * S.D, 0.5)
    nrm = cr / np.linalg.norm(cr, axis=1)[:, None]
    allowed = np.array(axes if axes else (0, 1, 2))
    ax = allowed[np.argmax(np.abs(nrm[:, allowed]), axis=1)]
    nk = nrm[np.arange(len(nrm)), ax]
    sg = np.where(nk >= 0, 1, -1)
    off = np.abs(nk) < 0.3                                                            
                                                                             
    rng = _rng(S.seed, 105)
    ns = np.maximum(1, np.ceil(np.sqrt(2.0 * area) / step).astype(int))
    small = ns == 1
    lab_all = np.where(off, -1, ax * 2 + (sg > 0))
    qs, labels = [T[small].mean(axis=1)], [lab_all[small]]
    for t in np.nonzero(~small)[0]:
        n = int(ns[t])
        i, j = np.meshgrid(np.arange(n), np.arange(n), indexing='ij')
        m = (i + j) < n
        u = (i[m] + 1.0 / 3.0) / n
        v = (j[m] + 1.0 / 3.0) / n
        ok = u + v <= 1.0
        u, v = u[ok], v[ok]
        if not len(u):
            u, v = np.array([1.0 / 3.0]), np.array([1.0 / 3.0])
        q = T[t, 0] + (T[t, 1] - T[t, 0]) * u[:, None] + (T[t, 2] - T[t, 0]) * v[:, None]
        qs.append(q)
        labels.append(np.full(len(q), lab_all[t]))
    Q = np.vstack(qs)
    L = np.concatenate(labels)
    if len(Q) > 4 * max_samples:
        pick = rng.choice(len(Q), 4 * max_samples, replace=False)
        Q, L = Q[pick], L[pick]
    tol = max(0.3, 0.25 * S.cover, 0.25 * S.D)
    broken = np.zeros(len(Q), dtype=bool)
    for k, q in enumerate(Q):
        loc, _n, _i, d = res_bvh.find_nearest(Vector(q), 2.0 * tol)
        broken[k] = loc is None
    out = []
    cell = 2.5 * step
    for lab in sorted(set(L[broken].tolist()) - {-1}):
        a, sign = lab // 2, (1 if lab % 2 else -1)
        up = 2 if a != 2 else 1
        h = 3 - a - up
        P = Q[broken & (L == lab)]
        if len(P) < 3:
            continue
                                                               
        keys = np.floor(P[:, [up, h]] / cell).astype(np.int64)
        cells = {}
        for idx, key in enumerate(map(tuple, keys)):
            cells.setdefault(key, []).append(idx)
        seen, areas = set(), []
        for start in cells:
            if start in seen:
                continue
            stack, members = [start], []
            seen.add(start)
            while stack:
                c = stack.pop()
                members += cells[c]
                for du in (-1, 0, 1):
                    for dh in (-1, 0, 1):
                        nb = (c[0] + du, c[1] + dh)
                        if nb in cells and nb not in seen:
                            seen.add(nb)
                            stack.append(nb)
            pts = P[members]
            if len(pts) < 3:
                continue
            areas.append((pts.min(0), pts.max(0), pts))
        if areas:
            out.append(('XYZ'[a], 'POS' if sign > 0 else 'NEG', areas))
    return out


def _densify(path, step):
    out = [path[0]]
    for q0, q1 in zip(path[:-1], path[1:]):
        n = max(1, int(math.ceil(np.linalg.norm(q1 - q0) / step)))
        out.extend(q0 + (q1 - q0) * (np.arange(1, n + 1)[:, None] / n))
    return np.asarray(out, dtype=np.float64)


def _spaced_positions(lo, hi, limit, spacing, rng, occupied):
    """Spread the available runs across the stroke while reserving the minimum gap."""
    span = hi - lo
    count = min(limit, int(span / spacing) + 1)
    slack = max(0.0, span - (count - 1) * spacing)
    minimum = spacing - 1e-9 * spacing
    for i in range(count):
        pos = lo + i * spacing + slack * (i + 0.5 + rng.uniform(-0.3, 0.3)) / count
        index = bisect_left(occupied, pos)
        if index and pos - occupied[index - 1] < minimum:
            continue
        if index < len(occupied) and occupied[index] - pos < minimum:
            continue
        occupied.insert(index, pos)
        yield pos


def area_runs(S, points, src_bvh, areas, first=0, key=0):
    """Cable runs through the painted areas: Runs per Direction in each area,
    placed where the strokes are, running on through the whole wall. Run
    numbers start at `first`; `key` varies the random layout per face."""
    lo, hi, a, up, h = pipes._axes(np.asarray(points), S.axis)
    ea, eu, eh = np.eye(3)[a], np.eye(3)[up], np.eye(3)[h]
    mid = 0.5 * (lo[a] + hi[a])
    sign = -1.0 if S.side == 'NEG' else 1.0
    nout = ea * sign
    depth = S.cover + S.R
    plane = mid if S.side == 'MID' else (lo[a] + depth if S.side == 'NEG' else hi[a] - depth)
    pad = 2.0 + S.D
    rng = _rng(S.seed, 103, key)
    dirs = []
    if S.layout in {'H', 'BOTH'}:
        dirs.append((eu, h, up))
    if S.layout in {'V', 'BOTH'}:
        dirs.append((eh, up, h))
    spacing = 2.0 * S.half_width + (S.run_gap or S.D)
    occupied = {ai: [] for _, _, ai in dirs}
    runs = []
    for blo, bhi, pts in areas:
        for across, di, ai in dirs:
                                                                              
            v0, v1 = float(pts[:, ai].min()), float(pts[:, ai].max())
            if S.run_gap:
                placed = list(_spaced_positions(v0, v1, S.count, spacing, rng, occupied[ai]))
            else:
                n = max(1, min(S.count, int((v1 - v0) // spacing) + 1))
                cand = sorted(v0 + (v1 - v0) * min(max((i + 0.5 + rng.uniform(-0.3, 0.3)) / n, 0.0), 1.0)
                              for i in range(n))
                placed = []
                for pos in cand:
                    if all(abs(pos - q) >= spacing for q in placed):
                        placed.append(pos)
            for pos in placed:
                start = np.zeros(3)
                start[a] = plane
                start[ai] = pos
                start[di] = lo[di] - pad
                end = start.copy()
                end[di] = hi[di] + pad
                corners, radii = [start, end], []
                if rng.random() < S.elbows:
                    c = start.copy()
                    c[di] = float(np.quantile(pts[:, di], rng.uniform(0.25, 0.75)))
                    up_turn = rng.random() < 0.5
                    stop = c.copy()
                    stop[ai] = (hi[ai] + pad) if up_turn else (lo[ai] - pad)
                    corners = [start, c, stop] if rng.random() < 0.5 else [end, c, stop]
                    radii = [2.0 * S.D + S.half_width]
                path = pipes._fillet_path(corners, radii, max(S.D * 0.25, 0.5))
                path = _densify(path, max(S.D, 2.0))
                if S.side != 'MID' and src_bvh is not None:
                    path = pipes._follow_surface(path, nout, ea, a, mid, sign, depth, src_bvh)
                runs.append(pipes.Run(path, np.tile(nout, (len(path), 1)), first + len(runs)))
    return runs


def _paint_radius(p, source):
    """The Concrete Paint Radius, in the object's scaled space (core units)."""
    sc = _scale(source)
    return max(p.concrete_paint_radius, 0.01) * pipes._gscale(p) * float(np.mean(sc))


def _face_runs(S, points, src_bvh, groups, side=None):
    """Runs for the areas of every face; `side` (chosen by hand) replaces the
    face each group was found on."""
    if S.run_gap and side is not None:
        by_axis = {}
        for axis, _, areas in groups:
            by_axis.setdefault(axis, []).extend(areas)
        groups = [(axis, side, areas) for axis, areas in by_axis.items()]
    runs = []
    for key, (axis, found, areas) in enumerate(groups):
        Sg = copy.copy(S)
        Sg.axis = axis
        Sg.side = side or found
        runs += area_runs(Sg, points, src_bvh, areas, len(runs), key)
    return runs


def _mesh_polys(mesh):
    return [tuple(pl.vertices) for pl in mesh.polygons]


def _whole_wall_runs(S, points, src_bvh):
    if not S.run_gap:
        return pipes.auto_runs(S, points, src_bvh)
    lo, hi, _, _, _ = pipes._axes(points, S.axis)
    inset = np.minimum(0.5 * (hi - lo), S.half_width + S.D)
    area = (lo, hi, np.vstack((lo + inset, hi - inset)))
    return area_runs(S, points, src_bvh, [area])


def runs_for(context, source, S, points, src_bvh, factor, polys=None, res_bvh=None, *, props=None):
    p = source.project_damage if props is None else props
    axes = S.axes
    side = p.cable_side if axes is not None and p.cable_side != 'AUTO' else None
    if damage_wanted(p):
                                                                                  
        groups = paint_areas(S, _drawn(context, source, factor, props=p), points, _paint_radius(p, source), src_bvh, axes)
        return _face_runs(S, points, src_bvh, groups, side)
    if boolean_wanted(p) and polys is not None and res_bvh is not None:
                                                                                 
        return _face_runs(S, points, src_bvh, damage_areas(S, points, polys, res_bvh, axes), side)
    if axes is None:
        return _whole_wall_runs(S, points, src_bvh)
    runs = []
    for k, i in enumerate(axes):                                             
        Sg = copy.copy(S)
        Sg.axis = 'XYZ'[i]
        Sg.side = _resolve_side(p.cable_side, Sg.axis, source, points)
        Sg.seed = S.seed + k
        for run in _whole_wall_runs(Sg, points, src_bvh):
            run.index = len(runs)
            runs.append(run)
    return runs


# готовим данные этого эффекта перед расчётом геометрии
def before_core(context, source, params, points, polys, factor, concrete_strokes, reports, *, props=None):
    """build.rebuild_object, before the fingerprint and the core call. Cable
    Damage takes over the Concrete Damage block: paint strokes along the
    cables, no rebar, deep enough to reach them. Returns the concrete strokes."""
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
        reports.append(({'WARNING'}, 'Cable Damage needs the Concrete Damage core'))
        return concrete_strokes
    sc = _scale(source)
    points_s = np.asarray(points, dtype=np.float64) * sc
    S = Settings(p, pipes._gscale(p), source, points_s, polys)
    src_bvh = _bvh(points_s, polys, guide=True)
    strokes = _drawn(context, source, factor, props=p)
    if not strokes:
        reports.append(({'INFO'}, 'Cable Damage: draw the damaged area to create cables'))
                                               
    strokes = [np.ascontiguousarray(st / sc, dtype=np.float64) for st in strokes]
    a = 'XYZ'.index(S.axis) if S.axis in 'XYZ' else int(np.argmin(np.ptp(points_s, axis=0)))
    s_depth = sc[a]
    if S.axes is None or (S.axes is not None and len(S.axes) > 1):
        s_depth = float(sc.min())                                                                   
    s_across = float(np.mean(np.delete(sc, a)))
    block.enable = True
    block.mode = 1
    block.rebar_enable = False
    dig = S.cover + S.Dt * max(S.chase_dig, 0.0)
    block.depth_min = max(dig * 0.85, 0.05) / s_depth
    block.depth_max = max(dig * 1.1 + 0.2, dig * 0.85 + 0.05) / s_depth
    return strokes


def _uv_topology(mesh):
    """Rope positions may move; UV corners remain valid on identical connectivity."""
    digest=hashlib.sha256()
    for data,field in ((mesh.loops,'vertex_index'),(mesh.polygons,'loop_start'),(mesh.polygons,'loop_total')):
        values=np.empty(len(data),np.int32);data.foreach_get(field,values);digest.update(values.tobytes())
    ids=mesh.attributes.get('cable_id')
    if ids is not None:
        values=np.empty(len(ids.data),np.int32);ids.data.foreach_get('value',values);digest.update(values.tobytes())
    return digest.digest()


def after_build(context, source, res, reports=None, *, body_has_damage=True, props=None, instance=0, damage=True, cut_body=None):
    """Keep the cables uncovered by the complete damaged body, before splitting it."""
    p = source.project_damage if props is None else props
    boolean = boolean_wanted(p) and not damage_wanted(p)
    from .generated_geometry import enabled
    if (not damage and not boolean) or not enabled(p, 'boolean' if boolean else 'cable'):
        _remove(source, instance)
        return None
    if res is None or res.type != 'MESH' or not wanted(p) or (boolean and not body_has_damage):
        _remove(source, instance)
        return None
    from . import incremental
    existing=cables_of(source, instance)
    cache_key = 'cables' if not instance else ('cables', instance)
    if not instance and incremental.reuse_output(context,source,res,cache_key,existing):return existing
    from .params import unit_factor
    factor = unit_factor(context)
    uid = source.get('project_damage_uid')
    stored = _INPUTS.get(uid)
    sc = _scale(source)
    scale_m = Matrix.Diagonal(Vector(tuple(sc) + (1.0,)))
    polys = None
    if stored is not None:
        points, polys = stored
        points = np.asarray(points, dtype=np.float64) * sc
        src_bvh = _bvh(points, polys, guide=True)
    else:
        mesh = source.data
        co = np.empty(len(mesh.vertices) * 3)
        mesh.vertices.foreach_get('co', co)
        points = co.reshape(-1, 3) * factor * sc
        src_bvh = _mesh_bvh(mesh, scale_m, factor, guide=True)
    if src_bvh is None or len(points) == 0:
        _remove(source, instance)
        return None
    res_bvh = (_bvh(np.asarray(cut_body['points']) * sc, cut_body['polys']) if cut_body is not None else
               _mesh_bvh(res.data, scale_m @ source.matrix_world.inverted() @ res.matrix_world, factor))
    scale=pipes._gscale(p)
    if not damage_wanted(p):scale*=p.boolean_cable_scale/100.
    S = Settings(p, scale, source, points, polys)
    if not damage_wanted(p):
        from .boolean_ui_controls import tube_settings
        tube_settings(S,p,'cable')
    if polys is None:
        polys = _mesh_polys(source.data)
    runs = [] if boolean and S.axes == () else runs_for(
        context, source, S, points, src_bvh, factor, polys, res_bvh, props=p)
    buf, counts = build_cables(S, runs, src_bvh, res_bvh, _down(source), boolean=boolean)
    if not buf.faces:
        _remove(source, instance)
        if reports is not None:
            reports.append(({'INFO'}, 'Cables: the damage does not reach the cables yet'))
        return None
    from . import material_uv,material_controls,effect_groups
    uv_snapshots=material_uv.capture(source)
    previous=cables_of(source, instance)
    rope_uv=None
    if previous is not None and material_uv.has_uv(previous):
        values=np.empty(len(previous.data.loops)*2,np.float32)
        previous.data.uv_layers[material_uv.UV_NAME].data.foreach_get('uv',values)
        rope_uv=(_uv_topology(previous.data),values)
    obj = _place(context, source, _mesh_from(buf, source.name + '_project_damage_cables', factor * sc, p), instance)
    if rope_uv is not None and rope_uv[0]==_uv_topology(obj.data):
        layer=obj.data.uv_layers.get(material_uv.UV_NAME) or obj.data.uv_layers.new(name=material_uv.UV_NAME)
        layer.data.foreach_set('uv',rope_uv[1])
    material_controls.apply_shared_output_material(source)
    material_uv.after_build(source,uv_snapshots)
    key='cables' if damage_wanted(p) else 'boolean'
    materials={mat for mat in obj.data.materials if mat is not None}
    effect_groups._replace(obj,{key:effect_groups.material_vertices(obj.data,materials)},
                           {'cables':'Cable Damage','boolean':'Boolean Break'})
    obj.hide_render = False
    if reports is not None:
        reports.append(({'INFO'}, 'Cables: %d pieces, %d cut ends' % (counts['pieces'], counts['cut'])))
    if not instance:
        incremental.remember_output(context,source,res,cache_key,obj)
    return obj


def follow_source(source):
    uid = source.get('project_damage_uid')
    if uid:
        for obj in bpy.data.objects:
            if obj.get(CABLES_STAMP) == uid:
                obj.matrix_world = source.matrix_world


                                                                             
    

def _section_box(layout, title):
    from . import effect_layout
    return effect_layout.section(layout, title)


def draw_cable_settings(layout, p, damage=False):
    from .pipes import disclosure as disclosure_box
    from .effect_layout import section as material_controls_box
    from . import generated_geometry
    if damage and not generated_geometry.draw(layout, p, 'cable'): return
    box=material_controls_box(layout,'Cables')
    material_controls.inline(box,p,'cable_material',text='Cables Material')
    col=disclosure_box(box,'cable_axis_open','Placement Logic')
    if col is not None:
        draw_axes(col,p)
        col.prop(p,'cable_side')
    col=box.column(align=True)
    ui_image_hints.prop(col,p,'cable_type')
    col.prop(p,'cable_diameter')
    col.prop(p,'cable_cover')
    col.prop(p,'cable_sides',text='Resolution')
    ui_image_hints.prop(col,p,'cable_layout')
    col.prop(p,'cable_count',text='Amount')
    col.prop(p,'cable_run_gap')
    col.prop(p,'cable_bundle')
    col.prop(p,'cable_bundle_gap')
    col.prop(p,'cable_turns')
    col.prop(p,'cable_seed')
    col=disclosure_box(box,'cable_break_open','Breakage')
    if col is not None:
        col.prop(p,'cable_cut')
        ui_image_hints.prop(col,p,'cable_slack')
        col.prop(p,'cable_stiffness')
        col.prop(p,'cable_embed')
        col.prop(p,'cable_strip')
        col.prop(p,'cable_splay')
        col.prop(p,'cable_fray')
    hang=material_controls_box(box,'Hanging Cables');ui_image_hints.prop(hang,p,'cable_hang',expand=True)
    if p.cable_hang!='OFF':
        hang.prop(p,'cable_hang_length')
        hang.prop(p,'cable_hang_ends')
        hang.prop(p,'cable_hang_sway')
        if p.cable_hang=='HORIZONTAL':hang.prop(p,'cable_hang_tilt')


# показываем настройки выбранного эффекта
def draw_settings(layout, context):
    from . import paint,recording_tools
    from .pipes import disclosure as disclosure_box
    from .effect_layout import section
    src=ownership.context_source(context)
    if src is None:return
    p=src.project_damage
    box=section(layout,'Damage along the Cables')
    paint.drawing_row(box,p,'CONCRETE')
    col=box.column(align=True)
    col.prop(p,'concrete_paint_radius')
    col.prop(p,'concrete_paint_snap')
    ui_image_hints.prop(col,p,'concrete_paint_close')
    col.prop(p,'concrete_seed')
    quality_ui.draw(col, p,'concrete_detail')
    col.prop(p,'cable_chase_dig')
    draw_cable_settings(layout,p,damage=True)
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
    col=disclosure_box(box,'cable_noise_open','Surface Noise')
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
    if CABLES_STAMP not in ownership.STAMPS:
        ownership.STAMPS = ownership.STAMPS + (CABLES_STAMP,)


# убираем подключения модуля при выключении аддона
def unregister():
    _INPUTS.clear()
