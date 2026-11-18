# отдельные кабели по кривым и рядом с объектами, включая их укладку
# ProjectDamage cable generator - GPL-3.0-or-later; see LICENSE.
#
# Cables Generator: a standalone generator (no damage source). The geometry
# and the rope simulation run in the native core (cable_generator.cpp); this
# module is the Blender side: settings on the owner object, sampling curves,
# gathering colliders and damage zones, the output mesh, live update,
# operators and the sidebar panel.
#   Owner    a Curve: cables along every spline (Bezier handles followed
#            exactly, Poly splines, NURBS by their control polygon). Every
#            control point is an anchor: the cables hang between them.
#            Point Radius scales the cables, Tilt rolls them.
#            An Empty: Proximity. Amount cables in Clumps are dropped within
#            Radius around it and fall onto the colliders.
#   Gravity  0 lays the cables exactly (Sag droops them between the curve
#            points); above 0 a rope simulation settles them: hanging between
#            the held points, lying on the Colliders collection / the floor.
#   Types    the five cable types of the Boolean Break cables (cables.py).
#   Ends     Capped, Stripped, Torn or a Connector (plug, RJ45, ferrule).
#   Damage   rat bites: holes through the sheath inside the objects of the
#            Damage Zones collection (or everywhere), cores and copper strands
#            showing through and sticking out.
#   Result   "<owner>_Cables", parented to the owner, rebuilt when the owner,
#            the colliders or the zones change (Live Update). Material slots
#            0 sheath, 1 copper, 2..9 core colours, 10 ties, 11 clips,
#            12 connector, 13 pins; UV map "UVMap" (U around, V along); face
#            attribute "cable_id" (-1 ties / clips); vertex group "PD_Cables".
import hashlib
import time

import bpy
import numpy as np
from mathutils import Matrix, Vector

from . import core_loader

STAMP = 'project_damage_cable_gen_of'                                    
ARRANGEMENT_ITEMS = (
    ('ROW', 'Side by Side', 'Cables of a clump next to each other, like a flat run on a wall or a tray', 0),
    ('BUNDLE', 'Bundle', 'Cables of a clump packed in a round bundle', 1),
    ('TWISTED', 'Twisted', 'A round bundle that turns around its centre (Twist turns per metre)', 2),
)
END_ITEMS = (
    ('CAP', 'Capped', 'The cable ends closed, cut clean', 0),
    ('STRIPPED', 'Stripped', 'Sheath cut back by Strip Length: the insulated cores fan out and end in bare copper', 1),
    ('TORN', 'Torn', 'Ragged sheath, cores of uneven length that splay and droop: a cable torn off', 2),
    ('CONNECTOR', 'Connector', 'The cable ends in a plug: a power plug with pins, an RJ45 plug for data cables, '
                               'a crimp ferrule for single wires', 3),
)
PIN_ITEMS = (
    ('POINTS', 'Control Points', 'Curve: held at every control point, the cables hang between them. '
                                 'Proximity: held at the start (hanging from the Empty)', 0),
    ('ENDS', 'Ends', 'Held at both ends only: the rest drapes over the colliders', 1),
    ('START', 'Start', 'Held at the first point only', 2),
    ('NONE', 'Nothing', 'Free: the cables drop and lie on the colliders / the floor', 3),
)
TYPE_KEYS = ('ROUND', 'FLAT', 'WIRES', 'ARMORED', 'DATA')
OWNER_TYPES = {'CURVE', 'EMPTY'}

_pending = {}                                      
_last = {}                                                                
_building = False


def _type_items():
    from .cables import TYPE_ITEMS
    return tuple((k, l, d, i) for i, (k, l, d) in enumerate(TYPE_ITEMS))


def _changed(self, context):
    if not _building:
        queue(self.id_data)


def _type_changed(self, context):
    from .cables import TYPES
    t = TYPES.get(self.cable_type)
    if t is not None:
        self['diameter'] = t['d']
        self['strip'] = t['strip']
        self['stiffness'] = t['stiff']
    _changed(self, context)


def _material_changed(self, context):
    result = self.result
    if result is not None and result.type == 'MESH' and result.get(STAMP):
        _assign_materials(result.data, self)


def _cm(name, default, description, soft_max=100.0, minimum=0.0):
    return bpy.props.FloatProperty(name=name, default=default, min=minimum, soft_max=soft_max, update=_changed,
                                   description=description)


def _dist(name, default, description, soft_max=10.0, minimum=0.0):
    return bpy.props.FloatProperty(name=name, default=default, min=minimum, soft_max=soft_max, subtype='DISTANCE',
                                   update=_changed, description=description)


def _factor(name, default, description, maximum=1.0):
    return bpy.props.FloatProperty(name=name, default=default, min=0.0, max=maximum, subtype='FACTOR',
                                   update=_changed, description=description)


# здесь хранятся значения, которые сохраняются вместе с объектом
class ProjectDamageCableGenSettings(bpy.types.PropertyGroup):
    enabled: bpy.props.BoolProperty(default=False, options={'HIDDEN'}, update=lambda self, context: _invalidate_owner_index())
    result: bpy.props.PointerProperty(type=bpy.types.Object, options={'HIDDEN'})
    live_update: bpy.props.BoolProperty(name='Live Update', default=True, update=_changed,
                                        description='Rebuild the cables while you edit or move the curve, the proximity Empty, '
                                                    'the colliders or the damage zones')
    seed: bpy.props.IntProperty(name='Seed', default=0, min=0, update=_changed)
                   
    count: bpy.props.IntProperty(name='Amount', default=1, min=1, max=256, soft_max=32, update=_changed,
                                 description='Number of cables (per curve spline, or per proximity Empty)')
    clumps: bpy.props.IntProperty(name='Clumps', default=1, min=1, max=64, soft_max=12, update=_changed,
                                  description='The cables are split into this many separate groups. 1 keeps them '
                                              'all together')
    noise: _factor('Noisiness', 0.0, 'Curve: the clumps and the loose cables meander. Proximity: the dropped cables '
                                     'bend and coil')
    gravity: bpy.props.FloatProperty(name='Gravity', default=0.0, min=0.0, max=10.0, soft_max=10.0, update=_changed,
                                     description='0 lays the cables exactly along the curve. Above 0 a rope simulation '
                                                 'settles them: they hang between the held points and lie on the '
                                                 'colliders. 1 is earth gravity, up to 10 for heavy, fast settling '
                                                 'cables')
    master_scale: bpy.props.FloatProperty(name='Scale', default=1.0, min=0.01, soft_min=0.1, soft_max=10.0,
                                          update=_changed,
                                          description='Scales every cable size together (diameter, gaps, strip '
                                                      'length, ties, connectors, holes)')
    radius: _dist('Radius', 0.0, 'Curve: how far the clumps spread around the curve. Proximity: the cables are dropped '
                                 'within this distance of the Empty', 5.0)
    diameter: _cm('Diameter (cm)', 1.1, 'Outer diameter of a cable (the width of a flat cable)', 5.0, 0.01)
           
    cable_type: bpy.props.EnumProperty(name='Cable Type', items=_type_items(), default='ROUND', update=_type_changed,
                                       description='Kind of cable. Choosing a type also sets its usual diameter, '
                                                   'stiffness and strip length')
    arrangement: bpy.props.EnumProperty(name='Arrangement', items=ARRANGEMENT_ITEMS, default='BUNDLE',
                                        update=_changed, description='How the cables of one clump lie together')
    gap: _cm('Gap (cm)', 0.1, 'Space between neighbouring cables of a clump', 10.0)
    roll: bpy.props.FloatProperty(name='Roll', default=0.0, soft_min=-3.14159, soft_max=3.14159, subtype='ANGLE',
                                  update=_changed,
                                  description='Turns the cables around the curve. Side by Side lies across the curve '
                                              'horizontally; 90 degrees stacks the run upwards, flat against a wall')
    twist: bpy.props.FloatProperty(name='Twist', default=2.0, min=0.0, soft_max=20.0, update=_changed,
                                   description='Twisted: turns of the bundle per metre')
    noise_length: _cm('Noise Length (cm)', 40.0, 'Length of one bend of the Noisiness', 300.0, 1.0)
             
    length_min: _dist('Length Min', 1.0, 'Shortest cable dropped in proximity of the Empty', 20.0, 0.01)
    length_max: _dist('Length Max', 2.0, 'Longest cable dropped in proximity of the Empty', 20.0, 0.01)
                        
    sag: _factor('Sag', 0.0, 'Without gravity: the cables droop between the control points, as a share of the '
                             'distance between two points. Vertical stretches do not sag')
    sag_random: _factor('Sag Random', 0.3, 'Every stretch between two points sags a little differently')
    spread: _factor('Spread', 0.3, 'Without gravity: loose cables (no ties) each sag their own way')
                                                       
    diameter_random: _factor('Diameter Random', 0.0, 'Thinner cables mixed in: each cable gets its own thickness, '
                                                     'down to 30% of Diameter')
    fan_out: _dist('Fan Out', 0.0, 'At both ends the cables of a clump spread this far apart, each its own way, '
                                   'like wiring splaying out of a bundle', 1.0)
    coils: _factor('Coils', 0.0, 'Share of the cables with a loop of spare cable hanging from them. With Gravity '
                                 'the loops hang and tangle')
    coil_radius: _dist('Coil Radius', 0.12, 'Size of the hanging loops', 1.0, 0.005)
    coil_turns: bpy.props.IntProperty(name='Coil Turns', default=2, min=1, max=20, update=_changed,
                                      description='Turns of a loop of spare cable')
    dangle: _factor('Dangling Ends', 0.0, 'With Gravity: share of the cables that came loose at one end and hang '
                                          'down from the last point that still holds them')
    dangle_length: _dist('Dangle Length', 1.0, 'How much longer a loose cable is, so it hangs down (to the floor)',
                         5.0, 0.01)
                
    colliders: bpy.props.PointerProperty(name='Colliders', type=bpy.types.Collection, update=_changed,
                                         description='Objects in this collection are solid for the cables: they lie '
                                                     'on them, drape over edges and hang off them')
    use_floor: bpy.props.BoolProperty(name='Floor', default=False, update=_changed,
                                      description='An endless floor at Floor Height the cables cannot fall through')
    floor_height: bpy.props.FloatProperty(name='Floor Height', default=0.0, subtype='DISTANCE', update=_changed,
                                          description='Height of the floor (world Z)')
    pin: bpy.props.EnumProperty(name='Hold', items=PIN_ITEMS, default='POINTS', update=_changed,
                                description='Where the simulated cables are held')
    slack: bpy.props.FloatProperty(name='Slack', default=0.06, min=0.0, soft_max=0.5, subtype='FACTOR',
                                   update=_changed,
                                   description='Extra cable between two held points (share of their distance): more '
                                               'slack hangs lower. Hanging curves: the cables sag from point to point')
    stiffness: _factor('Stiffness', 0.45, 'Soft cables bend in tight curves and coil; stiff ones keep straighter. '
                                          'Choosing a type sets its usual value')
    friction: _factor('Friction', 0.6, 'How much the cables grip the colliders and each other')
    steps: bpy.props.IntProperty(name='Simulation Steps', default=240, min=10, max=5000, soft_max=1200,
                                 update=_changed,
                                 description='How long the cables may fall and settle (60 steps per second). The '
                                             'simulation stops early once everything is at rest')
          
    end_start: bpy.props.EnumProperty(name='Start', items=END_ITEMS, default='CAP', update=_changed,
                                      description='The cable end at the first point')
    end_end: bpy.props.EnumProperty(name='End', items=END_ITEMS, default='STRIPPED', update=_changed,
                                    description='The cable end at the last point')
    strip: _cm('Strip Length (cm)', 4.0, 'Sheath stripped back at a Stripped or Torn end, showing the cores', 30.0)
    splay: _factor('Splay', 0.4, 'How far the cores fan out of a stripped or torn end')
    fray: _factor('Frayed Sheath', 0.5, 'Torn ends: how ragged the edge of the sheath is')
                
    damage_zones: bpy.props.PointerProperty(name='Damage Zones', type=bpy.types.Collection, update=_changed,
                                            description='Empties (sphere size = display size x scale) or objects '
                                                        '(their bounding sphere) in this collection mark where the '
                                                        'cables are bitten through')
    damage_everywhere: bpy.props.BoolProperty(name='Everywhere', default=False, update=_changed,
                                              description='Bite holes along the whole length, not only in the '
                                                          'damage zones')
    damage_amount: bpy.props.FloatProperty(name='Holes per Metre', default=0.0, min=0.0, soft_max=40.0,
                                           update=_changed,
                                           description='How many holes are bitten into a metre of cable inside a '
                                                       'damage zone (0 = no damage)')
    damage_size: bpy.props.FloatProperty(name='Hole Size', default=1.2, min=0.2, soft_max=4.0, update=_changed,
                                         description='Length of a hole, in cable diameters (also how far it reaches '
                                                     'around the cable)')
    strands: _factor('Copper Strands', 0.6, 'How many copper strands of a bitten core stick out of the hole')
    strand_length: bpy.props.FloatProperty(name='Strand Length', default=1.0, min=0.0, soft_max=4.0, update=_changed,
                                           description='How far the strands stick out, in cable diameters')
                    
    ties: bpy.props.BoolProperty(name='Cable Ties', default=False, update=_changed,
                                 description='Bands around every clump every Tie Spacing. A tied clump stays '
                                             'together (it falls and hangs as one)')
    tie_spacing: _cm('Tie Spacing (cm)', 25.0, 'Distance between two cable ties', 200.0, 1.0)
    tie_width: _cm('Tie Width (cm)', 0.4, 'Width of a cable tie', 3.0, 0.05)
    clips: bpy.props.BoolProperty(name='Clips', default=False, update=_changed,
                                  description='A clamp holding every clump at every control point (curves; not at '
                                              'stripped, torn or connector ends)')
             
    sides: bpy.props.IntProperty(name='Sides', default=10, min=4, max=48, update=_changed,
                                 description='Resolution around a cable (16 or more where holes are bitten)')
    segment: _cm('Segment Length (cm)', 1.0, 'Resolution along the cables: one ring of vertices every so far', 20.0,
                 0.05)
    use_radius: bpy.props.BoolProperty(name='Use Point Radius', default=True, update=_changed,
                                       description='The radius of the curve points (Alt S in Edit Mode) scales the '
                                                   'cables')
    use_tilt: bpy.props.BoolProperty(name='Use Tilt', default=True, update=_changed,
                                     description='The tilt of the curve points (Ctrl T in Edit Mode) rolls the cables '
                                                 'around the curve')
    uv_scale: bpy.props.FloatProperty(name='UV Scale', default=1.0, min=0.001, soft_max=20.0, update=_changed,
                                      description='Tiling of the cable texture along the cables')
               
    sheath_material: bpy.props.PointerProperty(name='Sheath', type=bpy.types.Material, update=_material_changed,
                                               description='Material for the cable sheath. Empty uses the built-in '
                                                           'material of the cable type')
    tie_material: bpy.props.PointerProperty(name='Ties', type=bpy.types.Material, update=_material_changed,
                                            description='Material for the cable ties. Empty uses black nylon')
    clip_material: bpy.props.PointerProperty(name='Clips', type=bpy.types.Material, update=_material_changed,
                                             description='Material for the clips. Empty uses white plastic')
    connector_material: bpy.props.PointerProperty(name='Connector', type=bpy.types.Material,
                                                  update=_material_changed,
                                                  description='Material for the plug bodies. Empty uses grey plastic')
    pin_material: bpy.props.PointerProperty(name='Pins', type=bpy.types.Material, update=_material_changed,
                                            description='Material for the plug pins and contacts. Empty uses brass')


                                                                             
                                                                  

def _bezier(p0, p1, p2, p3, t):
    u = 1.0 - t
    return ((u * u * u)[:, None] * p0 + (3 * u * u * t)[:, None] * p1 + (3 * u * t * t)[:, None] * p2
            + (t * t * t)[:, None] * p3)


def sample_splines(obj, steps=24):
    """The curve's splines as (rows, cyclic) in world space."""
    mw = np.array(obj.matrix_world, dtype=np.float64)
    out = []
    for spline in obj.data.splines:
        rows = []
        if spline.type == 'BEZIER':
            pts = list(spline.bezier_points)
            n = len(pts)
            if n < 2:
                continue
            segs = n if spline.use_cyclic_u else n - 1
            for i in range(segs):
                a, b = pts[i], pts[(i + 1) % n]
                t = np.linspace(0.0, 1.0, steps, endpoint=False)
                P = _bezier(np.array(a.co), np.array(a.handle_right), np.array(b.handle_left), np.array(b.co), t)
                seg = np.zeros((steps, 6))
                seg[:, :3] = P
                seg[:, 3] = a.tilt + (b.tilt - a.tilt) * t
                seg[:, 4] = a.radius + (b.radius - a.radius) * t
                seg[0, 5] = 1.0
                rows.append(seg)
            if not spline.use_cyclic_u:
                last = pts[-1]
                rows.append(np.array([[*last.co, last.tilt, last.radius, 1.0]]))
        else:
            pts = list(spline.points)
            if len(pts) < 2:
                continue
            rows.append(np.array([[p.co[0], p.co[1], p.co[2], p.tilt, p.radius, 1.0] for p in pts]))
        R = np.vstack(rows)
        R[:, :3] = R[:, :3] @ mw[:3, :3].T + mw[:3, 3]
        keep = np.r_[True, np.linalg.norm(np.diff(R[:, :3], axis=0), axis=1) > 1e-9]
        R = R[keep]
        cyclic = bool(spline.use_cyclic_u) and len(R) >= 3
        if cyclic and np.linalg.norm(R[0, :3] - R[-1, :3]) < 1e-9:
            R = R[:-1]
        if len(R) >= 2 and np.isfinite(R).all():
            out.append((np.ascontiguousarray(R), cyclic))
    return out


def _collection_objects(coll, skip=()):
    if coll is None:
        return []
    return [o for o in coll.all_objects if o not in skip]


def collider_mesh(context, s, skip=()):
    """World-space triangles of every object in the Colliders collection."""
    deps = context.evaluated_depsgraph_get()
    pts, tris, base = [], [], 0
    for obj in _collection_objects(s.colliders, skip):
        if obj.type not in {'MESH', 'CURVE', 'SURFACE', 'FONT', 'META'}:
            continue
        ev = obj.evaluated_get(deps)
        try:
            me = ev.to_mesh()
        except RuntimeError:
            continue
        if me is None:
            continue
        try:
            me.calc_loop_triangles()
            n = len(me.vertices)
            if n == 0 or len(me.loop_triangles) == 0:
                continue
            co = np.empty(n * 3, dtype=np.float64)
            me.vertices.foreach_get('co', co)
            t = np.empty(len(me.loop_triangles) * 3, dtype=np.int32)
            me.loop_triangles.foreach_get('vertices', t)
        finally:
            ev.to_mesh_clear()
        mw = np.array(ev.matrix_world, dtype=np.float64)
        co = co.reshape(-1, 3) @ mw[:3, :3].T + mw[:3, 3]
        pts.append(co)
        tris.append(t.reshape(-1, 3) + base)
        base += n
    if not pts:
        return np.zeros((0, 3)), np.zeros((0, 3), dtype=np.int32)
    return np.ascontiguousarray(np.vstack(pts)), np.ascontiguousarray(np.vstack(tris).astype(np.int32))


def damage_zones(s, skip=()):
    """Spheres (x, y, z, r) of the Damage Zones collection."""
    zones = []
    for obj in _collection_objects(s.damage_zones, skip):
        mw = obj.matrix_world
        scale = max(abs(v) for v in mw.to_scale())
        if obj.type == 'EMPTY':
            r = obj.empty_display_size * scale
            c = mw.translation
        else:
            corners = [mw @ Vector(b) for b in obj.bound_box]
            c = sum(corners, Vector()) / 8.0
            r = max((v - c).length for v in corners)
        zones.append((c.x, c.y, c.z, max(r, 1e-6)))
    return np.ascontiguousarray(np.array(zones, dtype=np.float64).reshape(-1, 4))


                                                                             
                                 

def _assign_materials(mesh, s):
    from .cables import CORE_COLORS, _type_material
    from .pipes import _plain_material
    mats = [s.sheath_material or _type_material(s.cable_type),
            _plain_material('ProjectDamage_Cable_Copper', (0.80, 0.38, 0.20), 0.3, 1.0)]
    mats += [_plain_material('ProjectDamage_Cable_' + label, c, 0.45) for label, c in CORE_COLORS]
    mats.append(s.tie_material or _plain_material('ProjectDamage_Cable_Tie', (0.02, 0.02, 0.02), 0.4))
    mats.append(s.clip_material or _plain_material('ProjectDamage_Cable_Clip', (0.85, 0.85, 0.82), 0.5))
    mats.append(s.connector_material or _plain_material('ProjectDamage_Cable_Connector', (0.12, 0.12, 0.13), 0.45))
    mats.append(s.pin_material or _plain_material('ProjectDamage_Cable_Pins', (0.78, 0.60, 0.25), 0.25, 1.0))
    if len(mesh.materials) != len(mats):
        mesh.materials.clear()
        for m in mats:
            mesh.materials.append(m)
    else:
        for i, m in enumerate(mats):
            if mesh.materials[i] != m:
                mesh.materials[i] = m


def result_of(owner):
    r = owner.project_damage_cable_gen.result
    if r is not None and r.type == 'MESH' and r.get(STAMP) == owner.name and r.parent == owner:
        return r
    return next((o for o in owner.children if o.type == 'MESH' and o.get(STAMP) == owner.name), None)


def owner_of(obj):
    """The cable owner of obj: obj itself (a Curve or an Empty), or the owner of a cable result."""
    if obj is None:
        return None
    if obj.type in OWNER_TYPES:
        return obj
    if obj.type == 'MESH' and obj.get(STAMP) and obj.parent is not None and obj.parent.type in OWNER_TYPES:
        return obj.parent
    return None


curve_of = owner_of


def params_for(core, s, scene):
    p = core.CableGenParams()
    unit = scene.unit_settings.scale_length or 1.0
    metre = 1.0 / unit                                                   
    cm = 0.01 * s.master_scale * metre                                         
    p.seed = s.seed
    p.type = TYPE_KEYS.index(s.cable_type) if s.cable_type in TYPE_KEYS else 0
    p.diameter = max(s.diameter, 0.01) * cm
    p.count = s.count
    p.clumps = min(s.clumps, s.count)
    p.radius = s.radius
    p.arrangement = {'ROW': 0, 'BUNDLE': 1, 'TWISTED': 2}[s.arrangement]
    p.gap = s.gap * cm
    p.twist = s.twist / metre / max(s.master_scale, 1e-6)
    p.length_min, p.length_max = s.length_min, s.length_max
    p.noise = s.noise
    p.noise_length = max(s.noise_length, 1.0) * cm
    p.sag, p.sag_random, p.spread = s.sag, s.sag_random, s.spread
    p.diameter_random, p.fan_out, p.coils = s.diameter_random, s.fan_out, s.coils
    p.coil_radius, p.coil_turns = s.coil_radius, float(s.coil_turns)
    p.dangle, p.dangle_length = s.dangle, s.dangle_length
    p.gravity = s.gravity
    p.metre = metre
    p.slack, p.stiffness, p.friction = s.slack, s.stiffness, s.friction
    p.pin = {'POINTS': 0, 'ENDS': 1, 'START': 2, 'NONE': 3}[s.pin]
    p.steps = s.steps
    p.use_floor = int(s.use_floor)
    p.floor_height = s.floor_height
    ends = {'CAP': 0, 'STRIPPED': 1, 'TORN': 2, 'CONNECTOR': 3}
    p.end_start, p.end_end = ends[s.end_start], ends[s.end_end]
    p.strip = s.strip * cm
    p.splay, p.fray = s.splay, s.fray
    p.damage = s.damage_amount / metre
    p.damage_size = s.damage_size
    p.strands, p.strand_length = s.strands, s.strand_length
    p.damage_everywhere = int(s.damage_everywhere)
    p.tie_spacing = s.tie_spacing * cm if s.ties else 0.0
    p.tie_width = s.tie_width * cm
    p.clips = int(s.clips)
    p.sides = s.sides
    p.segment = max(s.segment, 0.05) * cm
    p.use_radius = int(s.use_radius)
    p.use_tilt = 1                                        
    p.uv_scale = s.uv_scale
    p.down_x, p.down_y, p.down_z = 0.0, 0.0, -1.0
    return p


def _mesh_from(r, name):
    pts = np.asarray(r['points'], dtype=np.float64)
    fv = np.asarray(r['face_verts'], dtype=np.int32).ravel()
    fs = np.asarray(r['face_sizes'], dtype=np.int32).ravel()
    mat = np.asarray(r['face_mat'], dtype=np.int32).ravel()
    cid = np.asarray(r['face_cable'], dtype=np.int32).ravel()
    starts = np.zeros(len(fs), dtype=np.int32)
    if len(fs):
        starts[1:] = np.cumsum(fs)[:-1]
    mesh = bpy.data.meshes.new(name)
    mesh.vertices.add(len(pts))
    mesh.vertices.foreach_set('co', pts.astype(np.float32).ravel())
    mesh.loops.add(len(fv))
    mesh.loops.foreach_set('vertex_index', fv)
    mesh.polygons.add(len(fs))
    mesh.polygons.foreach_set('loop_start', starts)
    try:
        mesh.polygons.foreach_set('loop_total', fs)                  
    except (AttributeError, RuntimeError, TypeError):
        pass
    mesh.update(calc_edges=True)
    uv = mesh.uv_layers.new(name='UVMap')
    uv.data.foreach_set('uv', np.asarray(r['loop_uvs'], dtype=np.float32).ravel())
                                             
    mesh.polygons.foreach_set('use_smooth', (fs == 4) & ((mat < 10) | (mat >= 12)))
    a = mesh.attributes.new('cable_id', 'INT', 'FACE')
    a.data.foreach_set('value', cid)
    return mesh


def _settings_key(s):
    skip = {'rna_type', 'result', 'enabled', 'live_update', 'sheath_material', 'tie_material', 'clip_material',
            'connector_material', 'pin_material'}
    vals = []
    for prop in s.bl_rna.properties:
        if prop.identifier in skip:
            continue
        v = getattr(s, prop.identifier)
        vals.append((prop.identifier, v.name if isinstance(v, bpy.types.ID) else
                     tuple(v) if hasattr(v, '__len__') and not isinstance(v, str) else v))
    return repr(vals).encode()


def build(context, owner, reports=None, force=True):
    """(Re)build the cables of one owner (Curve or Empty); returns the result object or None.
    force=False skips the build when nothing it reads has changed (live update)."""
    global _building
    s = owner.project_damage_cable_gen
    core = core_loader.load()
    if not hasattr(core, 'build_cable_curves') or not hasattr(core.CableGenParams(), 'gravity'):
        raise RuntimeError('the native core has no cable generator yet: install the updated core and restart Blender')
    old = result_of(owner)
    skip = {owner} | ({old} if old is not None else set())
    if owner.type == 'CURVE':
                                                                   
        splines = sample_splines(owner)
        for rows, _cyclic in splines:
            rows[:, 3] = (rows[:, 3] if s.use_tilt else 0.0) + s.roll
        spawns = np.zeros((0, 3))
    else:
        splines = []
        spawns = np.ascontiguousarray(np.array([owner.matrix_world.translation[:]], dtype=np.float64))
    p = params_for(core, s, context.scene)
    if s.gravity > 0.0:
        cpts, ctris = collider_mesh(context, s, skip)
    else:
        cpts, ctris = np.zeros((0, 3)), np.zeros((0, 3), dtype=np.int32)
    zones = damage_zones(s, skip) if s.damage_amount > 0.0 and not s.damage_everywhere else np.zeros((0, 4))
    h = hashlib.sha1(_settings_key(s))
    for a in [r for r, _c in splines] + [np.asarray([c for _r, c in splines]), spawns, cpts, ctris, zones]:
        h.update(np.ascontiguousarray(a).tobytes())
    h.update(repr(context.scene.unit_settings.scale_length).encode())
    digest = h.digest()
    if not force and old is not None and _last.get(owner.name) == digest:
        return old
    _building = True
    try:
        if not splines and not len(spawns):
            if old is not None:
                old.data.clear_geometry()
                _last[owner.name] = digest
            return old
        r = core.build_cable_curves([a for a, _c in splines], [int(c) for _a, c in splines], p,
                                    spawns, cpts, ctris, zones)
        name = owner.name + '_Cables'
        mesh = _mesh_from(r, name)
        _assign_materials(mesh, s)
                                                                          
        mesh.polygons.foreach_set('material_index', np.asarray(r['face_mat'], dtype=np.int32).ravel())
        obj = old
        if obj is None:
            obj = bpy.data.objects.new(name, mesh)
            for coll in owner.users_collection or (context.scene.collection,):
                coll.objects.link(obj)
        else:
            previous = obj.data
            obj.data = mesh
            if previous is not None and previous.users == 0:
                bpy.data.meshes.remove(previous)
            mesh.name = name
        obj[STAMP] = owner.name
                                                                                     
        obj.parent = owner
        obj.matrix_parent_inverse = owner.matrix_world.inverted_safe()
        obj.matrix_basis = Matrix.Identity(4)
        group = obj.vertex_groups.get('PD_Cables') or obj.vertex_groups.new(name='PD_Cables')
        group.add(range(len(mesh.vertices)), 1.0, 'REPLACE')
        s.result = obj
        s.enabled = True
        _last[owner.name] = digest
        if reports is not None:
            text = '%d cables' % r['cables']
            for key in ('connectors', 'holes', 'ties', 'clips'):
                if r[key]:
                    text += ', %d %s' % (r[key], key)
            if r['particles']:
                text += ', simulated %d steps' % r['sim_steps']
            reports.append(text + ', %d vertices' % len(mesh.vertices))
        return obj
    finally:
        _building = False


                                                                             
             

def _live():
    try:
        from . import live
        return live
    except ImportError:
        return None


def queue(owner, force=False):
    if owner is None or owner.type not in OWNER_TYPES:
        return
    s = owner.project_damage_cable_gen
    if not s.enabled or not (s.live_update or force):
        return
    live = _live()
    if live is not None and getattr(live, '_HISTORY_RESTORING', False):
        return
    _, already_forced = _pending.get(owner.name, (0.0, False))
    _pending[owner.name] = time.monotonic(), force or already_forced
    if not bpy.app.timers.is_registered(_tick):
        bpy.app.timers.register(_tick, first_interval=0.05)


def _tick():
    live = _live()
    if live is not None and getattr(live, '_HISTORY_RESTORING', False):
        _pending.clear()
        return None
    for name, (stamp, force) in list(_pending.items()):
        if time.monotonic() - stamp < 0.04:
            continue
        _pending.pop(name, None)
        owner = bpy.data.objects.get(name)
        if owner is None or owner.type not in OWNER_TYPES:
            continue
        settings = owner.project_damage_cable_gen
        if not settings.enabled or not (settings.live_update or force):
            continue
        try:
            build(bpy.context, owner, force=False)
        except Exception as exc:
            print('ProjectDamage Cables:', exc)
    return 0.05 if _pending else None


def _dependencies(owner):
    s = owner.project_damage_cable_gen
    deps = {owner}
    if owner.data is not None:
        deps.add(owner.data)
    for coll, needed in ((s.colliders, s.gravity > 0.0), (s.damage_zones, s.damage_amount > 0.0)):
        if coll is not None and needed:
            deps.add(coll)
            for o in coll.all_objects:
                deps.add(o)
                if o.data is not None:
                    deps.add(o.data)
    return deps


def _invalidate_owner_index():
    from . import live
    live.invalidate_index()


@bpy.app.handlers.persistent
def _depsgraph_changed(scene, depsgraph):
    from . import live, live_input_state
    if _building:
        return
    updates=[u for u in depsgraph.updates if u.is_updated_geometry or u.is_updated_transform]
    if not updates:return
    for owner in live._scan().cables:
        if owner.name not in scene.objects or owner.type not in OWNER_TYPES:continue
        s=owner.project_damage_cable_gen
        if not s.enabled or not s.live_update:continue
        deps=_dependencies(owner)
        relevant=set()
        for update in updates:
            obj=update.id.original
            if obj==s.result or obj not in deps:continue
            if isinstance(obj,bpy.types.Object):relevant.add(obj)
            elif isinstance(obj,(bpy.types.Mesh,bpy.types.Curve)):
                relevant.update(ob for ob in deps if isinstance(ob,bpy.types.Object) and ob.data==obj)
            elif isinstance(obj,bpy.types.Collection):queue(owner)
        for obj in relevant:
            if obj == owner and owner.type == 'EMPTY':
                # Proximity simulation starts at the Empty's world position.
                if any(u.is_updated_transform and u.id.original == owner for u in updates):
                    queue(owner)
                continue
            if obj==owner and owner.mode=='OBJECT':
                if any(u.is_updated_transform and u.id.original==owner for u in updates):continue
                if not any(u.is_updated_geometry and u.id.original in {owner,owner.data} for u in updates):continue
            if live_input_state.changed(obj,('cables',owner.session_uid),local=obj==owner):queue(owner)


@bpy.app.handlers.persistent
def _file_loaded(*_args):
    _pending.clear()
    _last.clear()


                                                                             
           

def _target(context):
    return owner_of(context.active_object)


def _enable(owner, **settings):
    global _building
    _building = True
    try:
        s = owner.project_damage_cable_gen
        for k, v in settings.items():
            setattr(s, k, v)
        s.enabled = True
    finally:
        _building = False


def _select_only(context, obj):
    for o in context.selected_objects:
        o.select_set(False)
    obj.select_set(True)
    context.view_layer.objects.active = obj


def _run(op, context, owners):
    reports = []
    try:
        for o in owners:
            build(context, o, reports)
    except Exception as exc:
        op.report({'ERROR'}, 'Cables: ' + str(exc))
        return {'CANCELLED'}
    op.report({'INFO'}, 'Cables: ' + '; '.join(reports))
    return {'FINISHED'}


# действие «Cable Curve»
class PROJECT_DAMAGE_OT_cable_gen_add(bpy.types.Operator):
    bl_idname = 'project_damage.cable_gen_add'
    bl_label = 'Cable Curve'
    bl_description = 'Add a new Bezier curve at the 3D cursor with cables along it. Edit the curve to lay the cables'
    bl_options = {'REGISTER', 'UNDO'}

    length: bpy.props.FloatProperty(name='Length', default=2.0, min=0.01, subtype='DISTANCE',
                                    description='Distance between the first and the last point')
    sag: bpy.props.FloatProperty(name='Sag', default=0.08, min=0.0, max=1.0, subtype='FACTOR')

    @classmethod
    def poll(cls, context):
        return context.mode == 'OBJECT'

    def execute(self, context):
        data = bpy.data.curves.new('Cable', 'CURVE')
        data.dimensions = '3D'
        sp = data.splines.new('BEZIER')
        sp.bezier_points.add(2)
        half = 0.5 * self.length
        for bp, x in zip(sp.bezier_points, (-half, 0.0, half)):
            bp.co = (x, 0.0, 0.0)
            bp.handle_left_type = bp.handle_right_type = 'AUTO'
        obj = bpy.data.objects.new('Cable', data)
        from . import quality_defaults
        quality_defaults.standalone(obj,'cable_curve')
        context.collection.objects.link(obj)
        obj.location = context.scene.cursor.location
        _select_only(context, obj)
        _enable(obj, sag=self.sag)
        context.view_layer.update()
        return _run(self, context, [obj])


# действие «Proximity»
class PROJECT_DAMAGE_OT_cable_gen_add_spawner(bpy.types.Operator):
    bl_idname = 'project_damage.cable_gen_add_spawner'
    bl_label = 'Proximity'
    bl_description = ('Proximity cables: add an Empty above the 3D cursor that drops cables in its proximity. They fall '
                      'onto the assigned colliders. Enable Floor for an additional floor at the cursor height')
    bl_options = {'REGISTER', 'UNDO'}

    height: bpy.props.FloatProperty(name='Drop Height', default=0.5, min=0.0, subtype='DISTANCE')

    @classmethod
    def poll(cls, context):
        return context.mode == 'OBJECT'

    def execute(self, context):
        obj = bpy.data.objects.new('Cable Proximity', None)
        from . import quality_defaults
        quality_defaults.standalone(obj,'cable_proximity')
        obj.empty_display_type = 'SPHERE'
        obj.empty_display_size = 0.3
        context.collection.objects.link(obj)
        cursor = context.scene.cursor.location.copy()
        obj.location = cursor + Vector((0.0, 0.0, self.height))
        _select_only(context, obj)
        _enable(obj, count=8, clumps=3, radius=0.3, noise=0.5, gravity=1.0, pin='NONE',
                floor_height=cursor.z, end_start='CONNECTOR', end_end='TORN')
        context.view_layer.update()
        return _run(self, context, [obj])


# действие «Generate Cables»
class PROJECT_DAMAGE_OT_cable_gen_build(bpy.types.Operator):
    bl_idname = 'project_damage.cable_gen_build'
    bl_label = 'Generate Cables'
    bl_description = 'Lay cables along the selected curves / drop them from the selected empties (or rebuild them)'
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return _target(context) is not None

    def execute(self, context):
        owners = {owner_of(o) for o in context.selected_objects} | {_target(context)}
        owners = sorted((o for o in owners if o is not None and o.library is None), key=lambda o: o.name)
        for o in owners:
            if not o.project_damage_cable_gen.enabled:
                if o.type == 'EMPTY':
                    _enable(o, gravity=1.0, pin='NONE')
                else:
                    _enable(o)
        return _run(self, context, owners)


# действие «Apply Cables»
class PROJECT_DAMAGE_OT_cable_gen_apply(bpy.types.Operator):
    bl_idname = 'project_damage.cable_gen_apply'
    bl_label = 'Apply Cables'
    bl_description = 'Keep the cables as a plain mesh: unparented, no longer rebuilt'
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        c = _target(context)
        return c is not None and result_of(c) is not None

    def execute(self, context):
        from . import mesh_finalization
        c = _target(context)
        obj = result_of(c)
        mesh_finalization.remove_unused_slots(obj)
        world = obj.matrix_world.copy()
        obj.parent = None
        obj.matrix_world = world
        del obj[STAMP]
        s = c.project_damage_cable_gen
        s.result = None
        s.enabled = False
        _pending.pop(c.name, None)
        return {'FINISHED'}


# действие «Remove Cables»
class PROJECT_DAMAGE_OT_cable_gen_remove(bpy.types.Operator):
    bl_idname = 'project_damage.cable_gen_remove'
    bl_label = 'Remove Cables'
    bl_description = 'Delete the generated cables (the curve / empty stays)'
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        c = _target(context)
        return c is not None and c.project_damage_cable_gen.enabled

    def execute(self, context):
        c = _target(context)
        obj = result_of(c)
        if obj is not None:
            mesh = obj.data
            bpy.data.objects.remove(obj, do_unlink=True)
            if mesh is not None and mesh.users == 0:
                bpy.data.meshes.remove(mesh)
        s = c.project_damage_cable_gen
        s.result = None
        s.enabled = False
        _pending.pop(c.name, None)
        if context.view_layer.objects.active is None and c.name in context.view_layer.objects:
            context.view_layer.objects.active = c
        return {'FINISHED'}


# действие «Use Selected»
class PROJECT_DAMAGE_OT_cable_gen_collection(bpy.types.Operator):
    bl_idname = 'project_damage.cable_gen_collection'
    bl_label = 'Use Selected'
    bl_description = 'Put the other selected objects into the Colliders / Damage Zones collection of these cables'
    bl_options = {'REGISTER', 'UNDO'}

    kind: bpy.props.EnumProperty(items=(('COLLIDERS', 'Colliders', ''), ('ZONES', 'Damage Zones', '')))

    @classmethod
    def poll(cls, context):
        return _target(context) is not None

    def execute(self, context):
        owner = _target(context)
        s = owner.project_damage_cable_gen
        prop = 'colliders' if self.kind == 'COLLIDERS' else 'damage_zones'
        coll = getattr(s, prop)
        if coll is None:
            coll = bpy.data.collections.new(owner.name + ('_Colliders' if prop == 'colliders' else '_DamageZones'))
            context.scene.collection.children.link(coll)
        result = result_of(owner)
        added = 0
        for o in context.selected_objects:
            if o in (owner, result) or o.name in coll.objects:
                continue
            coll.objects.link(o)
            added += 1
        setattr(s, prop, coll)
        self.report({'INFO'}, 'Cables: %d objects added to %s' % (added, coll.name))
        return {'FINISHED'}


# действие «Add Damage Zone»
class PROJECT_DAMAGE_OT_cable_gen_add_zone(bpy.types.Operator):
    bl_idname = 'project_damage.cable_gen_add_zone'
    bl_label = 'Add Damage Zone'
    bl_description = 'Add a sphere Empty at the 3D cursor to the Damage Zones collection: cables inside it get bitten'
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return context.mode == 'OBJECT' and _target(context) is not None

    def execute(self, context):
        owner = _target(context)
        s = owner.project_damage_cable_gen
        coll = s.damage_zones
        if coll is None:
            coll = bpy.data.collections.new(owner.name + '_DamageZones')
            context.scene.collection.children.link(coll)
        zone = bpy.data.objects.new('Damage Zone', None)
        zone.empty_display_type = 'SPHERE'
        zone.empty_display_size = 0.2
        zone.location = context.scene.cursor.location
        coll.objects.link(zone)
        _enable(owner, damage_zones=coll, **({'damage_amount': 15.0} if s.damage_amount <= 0.0 else {}))
        queue(owner, force=True)
        return {'FINISHED'}


                                                                             
    

def _material_row(layout, s, name):
    try:
        from .material_library import draw_property
        draw_property(layout, s, name, text=s.bl_rna.properties[name].name)
    except Exception:
        layout.prop(s, name)


def _box(layout, title, icon):
    box = layout.box()
    box.label(text=title, icon=icon)
    return box, box.column(align=True)


def _source_row(layout):
    row = layout.row(align=True)
    row.operator('project_damage.cable_gen_add', text='Cable Curve', icon='OUTLINER_DATA_CURVE')
    row.operator('project_damage.cable_gen_add_spawner', text='Proximity', icon='EMPTY_DATA')


# показываем настройки выбранного эффекта
def draw_settings(layout, context, show_sources=True):
    from . import effect_layout
    owner = _target(context)
    if show_sources:_source_row(layout)
    if owner is None:
        layout.label(text='Or select a curve / an empty', icon='INFO')
        return
    s = owner.project_damage_cable_gen
    spawner = owner.type == 'EMPTY'
    if not s.enabled:
        layout.operator('project_damage.cable_gen_build',
                        text='Proximity Cables from this Empty' if spawner else 'Cables from this Curve',
                        icon='EMPTY_DATA' if spawner else 'OUTLINER_DATA_CURVE')
        return
    effect_layout.standalone_update(layout, s, 'project_damage.cable_gen_build')
    row = layout.row(align=True)
    row.operator('project_damage.cable_gen_apply', text='', icon='CHECKMARK')
    row.operator('project_damage.cable_gen_remove', text='', icon='TRASH')

    box, col = _box(layout, 'Main', 'OUTLINER_DATA_CURVE')
    col.prop(s, 'cable_type', text='')
    col.separator(factor=0.5)
    for name in ('count', 'clumps', 'noise', 'gravity', 'master_scale', 'radius', 'diameter'):
        col.prop(s, name)
    col.separator(factor=0.5)
    col.prop(s, 'seed')

    box, col = _box(layout, 'Clumps', 'MOD_ARRAY')
    col.prop(s, 'arrangement', text='')
    col.prop(s, 'gap')
    if s.arrangement == 'TWISTED':
        col.prop(s, 'twist')
    if not spawner:
        col.prop(s, 'roll')
    if s.noise > 0.0:
        col.prop(s, 'noise_length')
    if spawner:
        col.separator(factor=0.5)
        col.prop(s, 'length_min')
        col.prop(s, 'length_max')

    box, col = _box(layout, 'Variation', 'FORCE_TURBULENCE')
    loose = s.count == 1 or not s.ties
    col.active = loose
    col.prop(s, 'diameter_random')
    col.prop(s, 'fan_out')
    col.prop(s, 'coils')
    if s.coils > 0.0:
        col.prop(s, 'coil_radius')
        col.prop(s, 'coil_turns')
    sub = col.column(align=True)
    sub.active = loose and s.gravity > 0.0
    sub.prop(s, 'dangle')
    if s.dangle > 0.0:
        sub.prop(s, 'dangle_length')
    if not loose:
        box.label(text='Tied clumps stay neat', icon='INFO')

    if s.gravity > 0.0:
        box, col = _box(layout, 'Physics', 'PHYSICS')
        row = col.row(align=True)
        row.prop(s, 'colliders', text='')
        row.operator('project_damage.cable_gen_collection', text='', icon='ADD').kind = 'COLLIDERS'
        row = col.row(align=True)
        row.prop(s, 'use_floor', toggle=True)
        sub = row.row(align=True)
        sub.active = s.use_floor
        sub.prop(s, 'floor_height', text='')
        col.separator(factor=0.5)
        col.prop(s, 'pin', text='Hold')
        for name in ('slack', 'stiffness', 'friction', 'steps'):
            col.prop(s, name)
    elif not spawner:
        box, col = _box(layout, 'Hanging', 'FORCE_CURVE')
        col.prop(s, 'sag')
        sub = col.column(align=True)
        sub.active = s.sag > 0.0
        sub.prop(s, 'sag_random')
        if s.count > 1 and not s.ties:
            col.prop(s, 'spread')

    box, col = _box(layout, 'Ends', 'SNAP_VERTEX')
    row = col.row(align=True)
    row.prop(s, 'end_start', text='')
    row.prop(s, 'end_end', text='')
    ends = {s.end_start, s.end_end}
    if ends & {'STRIPPED', 'TORN'}:
        col.prop(s, 'strip')
        col.prop(s, 'splay')
        if 'TORN' in ends:
            col.prop(s, 'fray')

    box, col = _box(layout, 'Rat Damage', 'MOD_DECIM')
    col.prop(s, 'damage_amount')
    if s.damage_amount > 0.0:
        row = col.row(align=True)
        row.prop(s, 'damage_everywhere', toggle=True)
        if not s.damage_everywhere:
            row = col.row(align=True)
            row.prop(s, 'damage_zones', text='')
            row.operator('project_damage.cable_gen_add_zone', text='', icon='ADD')
            row.operator('project_damage.cable_gen_collection', text='', icon='RESTRICT_SELECT_OFF').kind = 'ZONES'
        for name in ('damage_size', 'strands', 'strand_length'):
            col.prop(s, name)

    box, col = _box(layout, 'Ties & Clips', 'LINKED')
    row = col.row(align=True)
    row.prop(s, 'ties', toggle=True)
    if not spawner:
        row.prop(s, 'clips', toggle=True)
    if s.ties:
        col.prop(s, 'tie_spacing')
        col.prop(s, 'tie_width')

    box, col = _box(layout, 'Resolution', 'MOD_SUBSURF')
    for name in ('sides', 'segment', 'uv_scale'):
        col.prop(s, name)
    if not spawner:
        row = col.row(align=True)
        row.prop(s, 'use_radius', toggle=True, text='Radius')
        row.prop(s, 'use_tilt', toggle=True, text='Tilt')

    box, col = _box(layout, 'Materials', 'MATERIAL')
    _material_row(col, s, 'sheath_material')
    if s.ties:
        _material_row(col, s, 'tie_material')
    if s.clips and not spawner:
        _material_row(col, s, 'clip_material')
    if 'CONNECTOR' in ends:
        _material_row(col, s, 'connector_material')
        _material_row(col, s, 'pin_material')


class PROJECT_DAMAGE_PT_cable_gen(bpy.types.Panel):
    bl_label = 'Cables Generator'
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "⚒ Project Damage"
    bl_options = {'DEFAULT_CLOSED'}

    def draw(self, context):
        draw_settings(self.layout, context)


def menu_add_curve(self, context):
    self.layout.operator('project_damage.cable_gen_add', text='Cable (Project Damage)', icon='OUTLINER_DATA_CURVE')


_CLASSES = (ProjectDamageCableGenSettings, PROJECT_DAMAGE_OT_cable_gen_add, PROJECT_DAMAGE_OT_cable_gen_add_spawner,
            PROJECT_DAMAGE_OT_cable_gen_build, PROJECT_DAMAGE_OT_cable_gen_apply, PROJECT_DAMAGE_OT_cable_gen_remove,
            PROJECT_DAMAGE_OT_cable_gen_collection, PROJECT_DAMAGE_OT_cable_gen_add_zone)
_MENUS = (('VIEW3D_MT_curve_add', menu_add_curve),)


# подключаем свойства и действия этого модуля к Blender
def register():
    for cls in _CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.Object.project_damage_cable_gen = bpy.props.PointerProperty(type=ProjectDamageCableGenSettings)
    for menu, fn in _MENUS:
        getattr(bpy.types, menu).append(fn)
    if _depsgraph_changed not in bpy.app.handlers.depsgraph_update_post:
        bpy.app.handlers.depsgraph_update_post.append(_depsgraph_changed)
    if _file_loaded not in bpy.app.handlers.load_post:
        bpy.app.handlers.load_post.append(_file_loaded)


# убираем подключения модуля при выключении аддона
def unregister():
    _pending.clear()
    if bpy.app.timers.is_registered(_tick):
        bpy.app.timers.unregister(_tick)
    for handlers, fn in ((bpy.app.handlers.depsgraph_update_post, _depsgraph_changed),
                         (bpy.app.handlers.load_post, _file_loaded)):
        while fn in handlers:
            handlers.remove(fn)
    for menu, fn in _MENUS:
        getattr(bpy.types, menu).remove(fn)
    if hasattr(bpy.types.Object, 'project_damage_cable_gen'):
        del bpy.types.Object.project_damage_cable_gen
    for cls in reversed(_CLASSES):
        if cls.is_registered:
            bpy.utils.unregister_class(cls)
