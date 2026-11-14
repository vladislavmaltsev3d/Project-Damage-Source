# общая основа новых генераторов: объект, материалы и обновление
# ProjectDamage damage generators: shared Blender side - GPL-3.0-or-later; see LICENSE.
#
# The five standalone damage generators (Torn Metal, Split Wood, Broken Tiles,
# Erosion, Blast Crater) share this module:
#   - a generator object is a Mesh object that carries its settings
#     (Object.project_damage_<kind>) and the stamp  obj['project_damage_gen'] = kind;
#     its mesh is rebuilt in place by the native core (core_loader.load())
#   - settings whose names match the core's parameter names go to the core as a
#     dict (no per-field binding code); a kind can add or convert values
#   - live update: a settings change, or a moved marker Empty (a child of the
#     generator with obj['pd_gen_marker']), rebuilds after a short pause
#   - the normal Project Damage hierarchy and Geometry tab own new instances
#     legacy standalone instances retain their existing settings
#   - Apply keeps the mesh as a plain object (stamp removed, settings kept)
#
# A generator module registers itself with register_kind(Kind(...)).
import time

import bpy
import numpy as np
from mathutils import Matrix

from . import core_loader

STAMP = 'project_damage_gen'        # on the generator object: the kind key
MARKER = 'pd_gen_marker'            # on a marker Empty: the marker type ('BLAST', ...)
SOURCE = 'pd_gen_source'            # on an erosion result: name of the source object

KINDS = {}                          # key -> Kind, in registration order
_pending = {}                       # object name -> time of the last change
_building = False


class Kind:
    """One generator type. build(context, obj, settings) -> (result dict, report text)."""

    def __init__(self, key, label, icon, prop, build, draw, materials, description='', defaults=None,
                 smooth_angle=35.0, add_object=None, markers=(), needs_source=False, owner_attrs=()):
        self.key, self.label, self.icon, self.prop = key, label, icon, prop
        self.build, self.draw, self.materials = build, draw, materials
        self.description = description
        self.defaults = defaults or {}
        self.smooth_angle = smooth_angle
        self.add_object = add_object            # optional custom "Add" (context) -> obj
        self.markers = markers                  # [(type, label, icon), ...]
        self.needs_source = needs_source


def register_kind(kind):
    KINDS[kind.key] = kind


def kind_of(obj):
    if obj is None or obj.type != 'MESH':
        return None
    return KINDS.get(obj.get(STAMP))


def settings_of(obj):
    k = kind_of(obj)
    from . import damage_generator_assets
    source = damage_generator_assets.owner(obj)
    return getattr(source or obj, k.prop) if k is not None else None


def generator_of(obj):
    """The generator for obj: obj itself, or the parent of a marker Empty."""
    if obj is None:
        return None
    if kind_of(obj) is not None:
        return obj
    if obj.get(MARKER) and obj.parent is not None and kind_of(obj.parent) is not None:
        return obj.parent
    from . import ownership, damage_generator_assets
    source=ownership.source_of(obj)
    if source is not None and source.type=='MESH':
        rows=source.project_damage_effects
        index=source.project_damage_effect_index
        if rows and index<len(rows) and rows[index].key in damage_generator_assets.KINDS:
            return damage_generator_assets.output(source,rows[index].key)
    return None


def core():
    c = core_loader.load()
    if c is None:
        raise RuntimeError('the native core is not loaded')
    return c


def require(c, fn):
    if not hasattr(c, fn):
        raise RuntimeError('the native core has no %s yet: install the updated core and restart Blender' % fn)
    return getattr(c, fn)


def resolution_property(reference):
    def get(self):
        return 100.0 * float(self.get('_resolution_reference', reference)) / max(float(self.resolution), 1e-9)
    def set(self, value):
        self.resolution = float(self.get('_resolution_reference', reference)) * 100.0 / max(float(value), 5.0)
    return bpy.props.FloatProperty(name='Resolution', default=100.0, subtype='PERCENTAGE',
        min=5.0, max=400.0, soft_min=5.0, soft_max=200.0, get=get, set=set,
        description='Geometry detail relative to the original resolution. 100% keeps the original density; 50% doubles the edge or voxel size for faster calculation; 200% halves it for finer geometry. Max Vertices can limit the final detail')


def params_dict(c, defaults_fn, s, skip=(), convert=None):
    """Settings -> core dict: every core parameter that has a same-named property."""
    d = {}
    for key in require(c, defaults_fn)().keys():
        if key in skip or not hasattr(s, key):
            continue
        v = getattr(s, key)
        if isinstance(v, bool):
            v = int(v)
        elif isinstance(v, str):
            continue
        d[key] = v
    if convert is not None:
        convert(d, s)
    if 'resolution' in d:
        from . import preview_quality
        source = s.id_data
        if getattr(source, 'project_damage', None) is not None:
            d['resolution'] /= max(.1, preview_quality.percent(source.project_damage) / 100.0)
    return d


# ---------------------------------------------------------------------------
# mesh output

def _attr_kind(name):
    return 'FLOAT'


def mesh_from_result(r, name):
    pts = np.asarray(r['points'], dtype=np.float64).reshape(-1, 3)
    fv = np.asarray(r['face_verts'], dtype=np.int32).ravel()
    fs = np.asarray(r['face_sizes'], dtype=np.int32).ravel()
    mesh = bpy.data.meshes.new(name)
    mesh.vertices.add(len(pts))
    mesh.vertices.foreach_set('co', pts.astype(np.float32).ravel())
    if len(fs):
        starts = np.zeros(len(fs), dtype=np.int32)
        starts[1:] = np.cumsum(fs)[:-1]
        mesh.loops.add(len(fv))
        mesh.loops.foreach_set('vertex_index', fv)
        mesh.polygons.add(len(fs))
        mesh.polygons.foreach_set('loop_start', starts)
        try:
            mesh.polygons.foreach_set('loop_total', fs)   # Blender < 4.0
        except (AttributeError, RuntimeError, TypeError):
            pass
    mesh.update(calc_edges=True)
    if len(fs):
        uv = mesh.uv_layers.new(name=r.get('_uv_name') or 'UVMap')
        uv.data.foreach_set('uv', np.asarray(r['loop_uvs'], dtype=np.float32).ravel())
        part = np.asarray(r.get('face_part', np.zeros(0)), dtype=np.int32).ravel()
        if len(part) == len(fs) and (part >= 0).any():
            a = mesh.attributes.new('pd_part', 'INT', 'FACE')
            a.data.foreach_set('value', part)
    for aname, values in r.get('attrs', {}).items():
        vals = np.asarray(values, dtype=np.float32).ravel()
        if len(vals) != len(pts):
            continue
        a = mesh.attributes.new(aname, 'FLOAT', 'POINT')
        a.data.foreach_set('value', vals)
    return mesh


def _apply_materials(obj, mesh, kind, face_mat, fixed=None):
    """Keep the object's own materials when it has them; fill empty slots with the kind's defaults.
    fixed: the full list to use instead (a generator working on another object's mesh)."""
    if fixed is not None:
        for m in fixed:
            mesh.materials.append(m)
        mesh.polygons.foreach_set('material_index', face_mat)
        return
    old = [m for m in (obj.data.materials if obj.data is not None else [])]
    defaults = kind.materials()
    n = max(len(defaults), int(face_mat.max()) + 1 if len(face_mat) else 0)
    for i in range(n):
        m = old[i] if i < len(old) and old[i] is not None else (defaults[i] if i < len(defaults) else None)
        mesh.materials.append(m)
    mesh.polygons.foreach_set('material_index', face_mat)


def _shade(mesh, angle_deg):
    n = len(mesh.polygons)
    if not n:
        return
    try:
        mesh.shade_smooth()
    except AttributeError:
        mesh.polygons.foreach_set('use_smooth', np.ones(n, dtype=bool))
    try:
        mesh.set_sharp_from_angle(angle=np.radians(angle_deg))
    except (AttributeError, TypeError):
        pass
    # edges between different materials stay sharp (torn edge walls, grout)
    mi = np.zeros(n, dtype=np.int32)
    mesh.polygons.foreach_get('material_index', mi)
    if len(np.unique(mi)) < 2:
        return
    ne = len(mesh.edges)
    nl = len(mesh.loops)
    loop_edge = np.zeros(nl, dtype=np.int32)
    mesh.loops.foreach_get('edge_index', loop_edge)
    totals = np.zeros(n, dtype=np.int32)
    mesh.polygons.foreach_get('loop_total', totals)
    loop_mat = np.repeat(mi, totals)
    lo = np.full(ne, np.iinfo(np.int32).max, dtype=np.int32)
    hi = np.full(ne, -1, dtype=np.int32)
    np.minimum.at(lo, loop_edge, loop_mat)
    np.maximum.at(hi, loop_edge, loop_mat)
    sharp = np.zeros(ne, dtype=bool)
    attr = mesh.attributes.get('sharp_edge')
    if attr is not None and attr.domain == 'EDGE':
        attr.data.foreach_get('value', sharp)
    sharp |= (hi >= 0) & (lo != hi)
    attr = mesh.attributes.get('sharp_edge') or mesh.attributes.new('sharp_edge', 'BOOLEAN', 'EDGE')
    attr.data.foreach_set('value', sharp)


def write_result(obj, kind, r):
    name = obj.data.name if obj.data is not None else obj.name
    mesh = r.get('_mesh') or mesh_from_result(r, name + '_new')
    face_mat = np.asarray(r['face_mat'], dtype=np.int32).ravel()
    _apply_materials(obj, mesh, kind, face_mat, r.get('_materials'))
    if not r.get('_preserve_shading'):_shade(mesh, kind.smooth_angle)
    old = obj.data
    obj.data = mesh
    if old is not None and old.users == 0:
        bpy.data.meshes.remove(old)
    mesh.name = name
    return mesh


def rebuild(context, obj, reports=None):
    global _building
    kind = kind_of(obj)
    if kind is None:
        return None
    s = settings_of(obj)
    _building = True
    try:
        t0 = time.perf_counter()
        r, text = kind.build(context, obj, s)
        if r is None:
            return None
        mesh = write_result(obj, kind, r)
        ms = (time.perf_counter() - t0) * 1000.0
        s.report = '%s, %d verts, %.0f ms' % (text, len(mesh.vertices), ms) if text else '%d verts' % len(mesh.vertices)
        if reports is not None:
            reports.append(s.report)
        return obj
    finally:
        _building = False


# ---------------------------------------------------------------------------
# live update

def settings_changed(self, context):
    """update= callback of every generator setting."""
    obj = self.id_data
    if _building or not isinstance(obj, bpy.types.Object):return
    from . import damage_generator_assets, live
    if live._APPLYING or live._HISTORY_RESTORING:return
    for key, kind_key in damage_generator_assets.KINDS.items():
        if getattr(obj,KINDS[kind_key].prop)==self and getattr(obj.project_damage,key+'_enable',False):
            live._mark_dirty(obj);return
    if kind_of(obj) is not None:
        queue(obj, scene=context.scene)


def queue(obj, *, scene=None):
    from . import damage_generator_assets, live
    if live._APPLYING or live._HISTORY_RESTORING:
        return
    source = damage_generator_assets.owner(obj)
    if source is not None:
        key = obj['project_damage_generator_key']
        props = source.project_damage
        if props.live_update and not props.all_off and getattr(props, key + '_enable'):
            live._mark_dirty(source, scene=scene)
        return
    if not settings_of(obj).live_update:
        return
    _pending[obj.name] = time.monotonic()
    if not bpy.app.timers.is_registered(_tick):
        bpy.app.timers.register(_tick, first_interval=0.12)


def _tick():
    for name, stamp in list(_pending.items()):
        if time.monotonic() - stamp < 0.1:
            continue
        _pending.pop(name, None)
        obj = bpy.data.objects.get(name)
        if obj is None or kind_of(obj) is None or not getattr(settings_of(obj), 'live_update', True):
            continue
        if obj.mode != 'OBJECT':
            _pending[name] = time.monotonic()
            continue
        try:
            rebuild(bpy.context, obj)
        except Exception as exc:
            print('ProjectDamage generator %s: %s' % (name, exc))
    return 0.1 if _pending else None


@bpy.app.handlers.persistent
def _depsgraph_changed(scene, depsgraph):
    from . import damage_generator_assets, live, live_input_state, metal_tear_gen
    if _building or live._APPLYING or live._HISTORY_RESTORING:
        return
    gens = set()
    blast_gens = set()
    for update in depsgraph.updates:
        obj = update.id.original
        if not isinstance(obj, bpy.types.Object):
            continue
        marker = obj.get(MARKER)
        parent = kind_of(obj.parent) if marker else None
        if marker == 'BLAST' and parent is metal_tear_gen.KIND:
            blast_gens.add(obj.parent)
            continue
        if not (update.is_updated_transform or update.is_updated_geometry):
            continue
        if update.is_updated_transform and parent is not None and live_input_state.changed(obj, 'standalone'):
            gens.add(obj.parent)
        if update.is_updated_geometry and not (update.is_updated_transform and obj.mode=='OBJECT'):
            if kind_of(obj) is metal_tear_gen.KIND:
                blast_gens.add(obj)
            for name in _source_users.get(obj.name, ()):
                generator = bpy.data.objects.get(name)
                if generator is not None and generator != obj and damage_generator_assets.owner(generator) is None:
                    gens.add(generator)
    gens.update(obj for obj in blast_gens if metal_tear_gen.blast_inputs_changed(obj))
    for obj in gens:
        queue(obj, scene=scene)


_source_users = {}   # source object name -> generator names (filled by kinds that read a source)


@bpy.app.handlers.persistent
def _file_loaded(*_args):
    from . import metal_tear_gen
    _pending.clear()
    _source_users.clear()
    metal_tear_gen._blast_inputs.clear()
    for obj in bpy.data.objects:
        kind = kind_of(obj)
        if kind is None:continue
        settings = settings_of(obj)
        if kind is metal_tear_gen.KIND:
            metal_tear_gen.remember_blast_inputs(obj, metal_tear_gen.blast_rows(obj, settings))
        source = getattr(settings, 'source', None)
        if source is not None:_source_users.setdefault(source.name,set()).add(obj.name)


# ---------------------------------------------------------------------------
# markers

# ---------------------------------------------------------------------------
# generators that work on another object's mesh ("On an Object")

def source_poll(self, obj):
    return obj.type == 'MESH' and kind_of(obj) is None and obj != self.id_data


def source_mesh(context, src):
    """The evaluated mesh of src in its own space with its scale applied, as the
    arrays of the core's *_mesh calls, plus its active UV map name and materials."""
    if src is None:
        raise RuntimeError('pick the object to damage (Object)')
    dg = context.evaluated_depsgraph_get()
    ev = src.evaluated_get(dg)
    me = ev.to_mesh()
    try:
        nv, nf, nl = len(me.vertices), len(me.polygons), len(me.loops)
        if nf == 0:
            raise RuntimeError('%s has no faces' % src.name)
        pts = np.zeros(nv * 3, dtype=np.float64)
        me.vertices.foreach_get('co', pts)
        sizes = np.zeros(nf, dtype=np.int32)
        me.polygons.foreach_get('loop_total', sizes)
        verts = np.zeros(nl, dtype=np.int32)
        me.loops.foreach_get('vertex_index', verts)
        mats = np.zeros(nf, dtype=np.int32)
        me.polygons.foreach_get('material_index', mats)
        smooth = np.ones(nf, dtype=bool)
        try:
            me.polygons.foreach_get('use_smooth', smooth)
        except (AttributeError, RuntimeError, TypeError):
            pass
        uv = np.zeros((0, 2), dtype=np.float64)
        uv_name = None
        layer = me.uv_layers.active
        if layer is not None:
            uv_name = layer.name
            buf = np.zeros(nl * 2, dtype=np.float32)
            layer.data.foreach_get('uv', buf)
            uv = buf.astype(np.float64).reshape(-1, 2)
    finally:
        ev.to_mesh_clear()
    scale = np.array(src.matrix_world.to_scale(), dtype=np.float64)
    pts = pts.reshape(-1, 3) * scale
    materials = [sl.material for sl in src.material_slots]
    return dict(points=np.ascontiguousarray(pts), face_sizes=sizes, face_verts=verts, face_mat=mats,
                loop_uvs=np.ascontiguousarray(uv), face_smooth=smooth.astype(np.int32), uv_name=uv_name,
                materials=materials, n_mats=max(len(materials), 1))


def track_source(obj, src, hide):
    """Rebuild obj when src changes; hide src (the result takes its place)."""
    from . import damage_generator_assets
    if damage_generator_assets.owner(obj) is not None:return
    for users in _source_users.values():
        users.discard(obj.name)
    if src is None:
        if SOURCE in obj:
            del obj[SOURCE]
        return
    obj[SOURCE] = src.name
    _source_users.setdefault(src.name, set()).add(obj.name)
    hide = bool(hide)
    if src.hide_render != hide:
        src.hide_render = hide
    try:
        if src.hide_get() != hide:
            src.hide_set(hide)
    except RuntimeError:   # not in the view layer
        pass


def add_on_object(context, kind, suffix):
    """Add for a generator with an "On an Object" mode: with a mesh selected, the
    result sits where it is and takes its place. Returns (obj, source or None)."""
    src = context.active_object
    use = src is not None and src.type == 'MESH' and kind_of(src) is None and src.select_get()
    name = (src.name + ' ' + suffix) if use else kind.label
    obj = bpy.data.objects.new(name, bpy.data.meshes.new(name))
    (src.users_collection[0] if use and src.users_collection else context.collection).objects.link(obj)
    if use:
        loc, rot, _sc = src.matrix_world.decompose()
        obj.matrix_world = Matrix.LocRotScale(loc, rot, None)
    else:
        obj.location = context.scene.cursor.location
    obj[STAMP] = kind.key
    return obj, (src if use else None)


def marker_rows(obj, mtype):
    """Marker Empties of a generator: (local matrix, empty) for each child with obj[MARKER] == mtype."""
    inv = obj.matrix_world.inverted_safe()
    out = []
    for ch in obj.children:
        if ch.type == 'EMPTY' and ch.get(MARKER) == mtype:
            out.append((inv @ ch.matrix_world, ch))
    out.sort(key=lambda t: t[1].name)
    return out


def add_marker(context, obj, mtype, location_local, size, display='SPHERE', name=None, **props):
    e = bpy.data.objects.new(name or '%s_%s' % (obj.name, mtype.title()), None)
    e.empty_display_type = display
    e.empty_display_size = size
    e[MARKER] = mtype
    for k, v in props.items():
        e[k] = v
    for coll in obj.users_collection or (context.scene.collection,):
        coll.objects.link(e)
    e.parent = obj
    from . import damage_generator_assets
    if damage_generator_assets.owner(obj) is not None:e.project_damage_og_object=damage_generator_assets.owner(obj)
    e.matrix_parent_inverse = Matrix.Identity(4)
    e.location = location_local
    return e


# ---------------------------------------------------------------------------
# materials

def node_material(name, build_fn):
    """A material made once by name; build_fn(nodes, links, out_node) fills an empty node tree."""
    m = bpy.data.materials.get(name)
    if m is not None:
        return m
    m = bpy.data.materials.new(name)
    try:
        if not m.use_nodes:
            m.use_nodes = True
    except AttributeError:
        pass
    nt = m.node_tree
    if nt is None:
        return m
    nt.nodes.clear()
    out = nt.nodes.new('ShaderNodeOutputMaterial')
    out.location = (600, 0)
    try:
        build_fn(nt.nodes, nt.links, out)
    except Exception as exc:   # a node API change must not break the generator
        print('ProjectDamage generator material %s: %s' % (name, exc))
    return m


def principled(nodes, links, out, color, metallic=0.0, roughness=0.5):
    b = nodes.new('ShaderNodeBsdfPrincipled')
    b.location = (300, 0)
    b.inputs['Base Color'].default_value = (*color, 1.0)
    b.inputs['Metallic'].default_value = metallic
    b.inputs['Roughness'].default_value = roughness
    links.new(b.outputs[0], out.inputs['Surface'])
    return b


def attr_node(nodes, name, x=-600, y=0):
    a = nodes.new('ShaderNodeAttribute')
    a.attribute_name = name
    a.location = (x, y)
    return a


def mix_color(nodes, links, fac_socket, a, b, x=0, y=0):
    """Mix two colours (a at 0, b at 1); a/b are RGB tuples or sockets. Returns the colour socket."""
    m = nodes.new('ShaderNodeMix')
    m.data_type = 'RGBA'
    m.location = (x, y)
    if isinstance(fac_socket, (int, float)):
        m.inputs[0].default_value = fac_socket
    else:
        links.new(fac_socket, m.inputs[0])
    for idx, v in ((6, a), (7, b)):
        if isinstance(v, tuple):
            m.inputs[idx].default_value = (*v, 1.0)
        else:
            links.new(v, m.inputs[idx])
    return m.outputs[2]


def map_range(nodes, links, socket, a, b, x=-400, y=0):
    n = nodes.new('ShaderNodeMapRange')
    n.location = (x, y)
    links.new(socket, n.inputs[0])
    n.inputs[1].default_value = a
    n.inputs[2].default_value = b
    return n.outputs[0]


def noise_tex(nodes, scale, x=-800, y=0, detail=4.0):
    n = nodes.new('ShaderNodeTexNoise')
    n.location = (x, y)
    n.inputs['Scale'].default_value = scale
    try:
        n.inputs['Detail'].default_value = detail
    except KeyError:
        pass
    return n


def math_node(nodes, links, op, a, b=None, x=-200, y=0):
    n = nodes.new('ShaderNodeMath')
    n.operation = op
    n.location = (x, y)
    for i, v in enumerate((a, b)):
        if v is None:
            continue
        if isinstance(v, (int, float)):
            n.inputs[i].default_value = v
        else:
            links.new(v, n.inputs[i])
    return n.outputs[0]


# ---------------------------------------------------------------------------
# operators

def _kind_items(self, context):
    from .damage_generator_assets import RETIRED_KINDS
    return [(k.key, k.label, k.description, k.icon, i) for i, k in enumerate(KINDS.values()) if k.key not in RETIRED_KINDS]


def _select_only(context, obj):
    for o in context.selected_objects:
        o.select_set(False)
    obj.select_set(True)
    context.view_layer.objects.active = obj


# действие «Add Damage Generator»
class PROJECT_DAMAGE_OT_gen_add(bpy.types.Operator):
    bl_idname = 'project_damage.gen_add'
    bl_label = 'Add Damage Generator'
    bl_description = 'Add a new generator object at the 3D cursor'
    bl_options = {'REGISTER', 'UNDO'}

    kind: bpy.props.EnumProperty(name='Generator', items=_kind_items)

    @classmethod
    def description(cls, context, props):
        k = KINDS.get(props.kind)
        return k.description if k is not None else cls.bl_description

    def execute(self, context):
        from . import damage_generator_assets,ownership,build
        if self.kind in damage_generator_assets.RETIRED_KINDS:return {'CANCELLED'}
        source=ownership.source_of(context.active_object)
        if source is not None and build.result_of(source) is not None:
            key=next((key for key,value in damage_generator_assets.KINDS.items() if value==self.kind),None)
            if key is None:return {'CANCELLED'}
            with context.temp_override(project_damage_import_source=source):
                return bpy.ops.project_damage.effect_action(effect=key,action='ADD')
        kind=KINDS.get(self.kind)
        if kind is None:return {'CANCELLED'}
        global _building
        old=_building;_building=True;obj=None
        try:
            with context.temp_override(object=None,active_object=None,selected_objects=[],selected_editable_objects=[]):
                obj=kind.add_object(self,context)
            s=settings_of(obj)
            s['_resolution_reference']=s.resolution
            entry=context.preferences.addons.get(__package__)
            if entry and getattr(entry.preferences,'low_details_mode',False):s.resolution_quality=20.0
            rebuild(context,obj)
            _select_only(context,obj)
            return {'FINISHED'}
        except Exception as exc:
            if obj is not None:
                mesh=obj.data;bpy.data.objects.remove(obj,do_unlink=True)
                if mesh.users==0:bpy.data.meshes.remove(mesh)
            self.report({'ERROR'},str(exc));return {'CANCELLED'}
        finally:_building=old


# действие «Update Generator»
class PROJECT_DAMAGE_OT_gen_build(bpy.types.Operator):
    bl_idname = 'project_damage.gen_build'
    bl_label = 'Update Generator'
    bl_description = 'Rebuild the selected damage generators'
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return generator_of(context.active_object) is not None

    def execute(self, context):
        gens = {generator_of(o) for o in context.selected_objects} | {generator_of(context.active_object)}
        reports = []
        try:
            for g in sorted((g for g in gens if g is not None), key=lambda o: o.name):
                rebuild(context, g, reports)
        except Exception as exc:
            self.report({'ERROR'}, str(exc))
            return {'CANCELLED'}
        self.report({'INFO'}, '; '.join(reports))
        return {'FINISHED'}


# действие «New Seed»
class PROJECT_DAMAGE_OT_gen_seed(bpy.types.Operator):
    bl_idname = 'project_damage.gen_seed'
    bl_label = 'New Seed'
    bl_description = 'Pick a new random seed for the active generator'
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return generator_of(context.active_object) is not None

    def execute(self, context):
        g = generator_of(context.active_object)
        s = settings_of(g)
        s.seed = int.from_bytes(np.random.default_rng().bytes(3), 'little')
        rebuild(context, g)
        return {'FINISHED'}


# действие «Apply Generator»
class PROJECT_DAMAGE_OT_gen_apply(bpy.types.Operator):
    bl_idname = 'project_damage.gen_apply'
    bl_label = 'Apply Generator'
    bl_description = ('Keep the mesh as a plain object: it is no longer rebuilt, marker Empties are removed '
                      '(the settings stay on the object)')
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return generator_of(context.active_object) is not None

    def execute(self, context):
        g = generator_of(context.active_object)
        from . import damage_generator_assets as A
        from . import mesh_finalization
        source=A.owner(g)
        if source is not None:
            try:A.bake_selected(context,source,g['project_damage_generator_key'])
            except Exception as exc:
                self.report({'ERROR'},str(exc));return {'CANCELLED'}
            return {'FINISHED'}
        for ch in list(g.children):
            if ch.get(MARKER):
                bpy.data.objects.remove(ch, do_unlink=True)
        _pending.pop(g.name, None)
        if SOURCE in g:
            del g[SOURCE]
        del g[STAMP]
        mesh_finalization.remove_unused_slots(g)
        _select_only(context, g)
        return {'FINISHED'}


# действие «Add Marker»
class PROJECT_DAMAGE_OT_gen_add_marker(bpy.types.Operator):
    bl_idname = 'project_damage.gen_add_marker'
    bl_label = 'Add Marker'
    bl_description = 'Add a marker Empty to the active generator; move and scale it to place the damage'
    bl_options = {'REGISTER', 'UNDO'}

    marker: bpy.props.StringProperty()

    @classmethod
    def poll(cls, context):
        return generator_of(context.active_object) is not None

    def execute(self, context):
        g = generator_of(context.active_object)
        kind = kind_of(g)
        fn = getattr(kind, 'add_marker', None)
        if fn is None:
            return {'CANCELLED'}
        e = fn(context, g, self.marker)
        if e is None:
            return {'CANCELLED'}
        _select_only(context, e)
        rebuild(context, g)
        return {'FINISHED'}


# ---------------------------------------------------------------------------
# UI

def box(layout, title, icon='NONE', s=None, toggle=None):
    b = layout.box()
    row = b.row(align=True)
    if s is not None and toggle:
        row.prop(s, toggle, text='')
    row.alignment = 'CENTER'
    row.label(text=title, icon=icon)
    col = b.column(align=True)
    if s is not None and toggle:
        col.active = bool(getattr(s, toggle))
    return col



# показываем настройки выбранного эффекта
def draw_settings(layout, context, g):
    if g is None:return
    kind = kind_of(g)
    s = settings_of(g)
    from . import material_controls
    material_controls.centered_heading(layout, kind.label, icon=kind.icon)
    row = layout.row(align=True)
    row.operator('project_damage.gen_build', text='Update', icon='FILE_REFRESH')
    row.prop(s, 'live_update', text='', icon='FILE_REFRESH')
    row.operator('project_damage.gen_seed', text='', icon='SHADERFX')
    row.operator('project_damage.gen_apply', text='Apply', icon='CHECKMARK')
    if s.report:layout.label(text=s.report)
    kind.draw(layout, s, context, g)
    from .material_library import draw_property
    materials=box(layout,'Materials','MATERIAL')
    labels=('Stone Surface','Fresh Break','Aggregate','Mortar') if g.get(STAMP)=='EROSION' else ()
    for i,slot in enumerate(g.material_slots):draw_property(materials,slot,'material',text=labels[i] if i<len(labels) else slot.name or 'Material')


def _menu_mesh_add(self, context):
    pass


_CLASSES = (PROJECT_DAMAGE_OT_gen_add, PROJECT_DAMAGE_OT_gen_build, PROJECT_DAMAGE_OT_gen_seed, PROJECT_DAMAGE_OT_gen_apply,
            PROJECT_DAMAGE_OT_gen_add_marker,)


# подключаем свойства и действия этого модуля к Blender
def register():
    for cls in _CLASSES:
        bpy.utils.register_class(cls)
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
    try:
        bpy.types.VIEW3D_MT_mesh_add.remove(_menu_mesh_add)
    except Exception:
        pass
    for cls in reversed(_CLASSES):
        if cls.is_registered:
            bpy.utils.unregister_class(cls)
