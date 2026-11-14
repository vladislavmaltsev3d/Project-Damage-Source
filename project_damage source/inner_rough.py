# настройки неровных срезов, обновление осколков и простые тела для физики
from . import quality_ui
# SPDX-License-Identifier: GPL-3.0-or-later
"""Noisy Boolean Cutters. Shared noisy grids repartition internal boundaries;
plain physics proxies, exact inner-face masks and exterior shading are retained.
The previous displacement and tetrahedral-warp algorithms are no longer used.
"""
from . import ui_image_hints
import math

import bpy
import numpy as np
from mathutils import Vector, kdtree, noise


NORMAL_ATTR = '_pd_rough_n'
MAX_NEW_VERTS = 400000                                                                  
FAILED = [0]                                                      
HELD = 1000                                                                                        


_PENDING={}
_RUNNING=False


def _touch(self,context):
    from . import live
    if _RUNNING or live._APPLYING or live._HISTORY_RESTORING:return
    source=self.id_data
    if source is None or not source.project_damage.live_update:return
    group=source.project_damage_inner_rough
    kind=next((k for k in ('cellf','rbf','debris','pieces','fragments') if getattr(group,k)==self),None)
    if kind is None:return
    from . import background_compute as bg
    bg.cancel(source,method='rough_'+kind)
    import time
    _PENDING[(source.name,kind)]=(source.get('project_damage_uid'),time.monotonic())
    if not bpy.app.timers.is_registered(_tick):bpy.app.timers.register(_tick,first_interval=.2)


def _tick():
    global _RUNNING
    from . import live,debris_output,damage_physics
    if live._HISTORY_RESTORING:_PENDING.clear();return None
    import time
    for key,(uid,when) in list(_PENDING.items()):
        source=bpy.data.objects.get(key[0])
        if source is None or source.get('project_damage_uid')!=uid:_PENDING.pop(key,None);continue
        delay=max(.15,getattr(source.project_damage,'defer_ms',300)/1000.)
        if time.monotonic()-when<delay:continue
        _PENDING.pop(key,None)
        if not source.project_damage.live_update:continue
        _RUNNING=True
        try:
            if key[1]=='pieces':after_fracture(bpy.context,source,damage_physics.pieces_of(source))
            elif key[1]=='fragments':after_fragments(bpy.context,source,[o for o in bpy.data.objects if o.get('project_damage_bfrag_of')==uid])
            else:after_place(bpy.context,source,debris_output.objects_of(source,key[1]),key[1])
        except Exception as exc:print('[ProjectDamage] Noisy Cutters:',exc)
        finally:_RUNNING=False
    return .1 if _PENDING else None


def _clear_pending(*args):
    _PENDING.clear()
    if bpy.app.timers.is_registered(_tick):bpy.app.timers.unregister(_tick)


def _annotations(update):
    kw = {'update': update} if update else {}
    return {
        'smooth': bpy.props.BoolProperty(
            name='Auto Smooth Inner', default=False, **kw,
            description='Angle-limited smooth normals on inner faces only; preserve sharp folds and exterior normals'),
        'smooth_angle': bpy.props.FloatProperty(name='Inner Smooth Angle',default=math.radians(60),min=0,max=math.pi,subtype='ANGLE',**kw),
        'enable': bpy.props.BoolProperty(
            name='Noisy Cutters', default=False, **kw,
            description='Recut adjacent inner boundaries using shared noisy Boolean grids. Off keeps the base pieces; On makes complementary break surfaces. Exterior and wall contact remain bounded by the original solids'),
        'noise_detail': bpy.props.IntProperty(name='Noise Detail',default=2,min=0,max=8,**kw,description='Additional noise octaves. 0: broad smooth bumps; 8: fine layered roughness; higher detail needs higher Resolution'),
        'noise_roughness': bpy.props.FloatProperty(name='Noise Roughness',default=.75,min=0.,max=1.,**kw,description='Strength of small noise layers. 0: only broad bumps; 1: strong fine detail'),
        'form': bpy.props.EnumProperty(name='Form',items=(('MATCHED','Matching Fragments','One shared boundary: a bulge on one fragment is a matching recess on its neighbour'),('LEGACY','Legacy Rocky','Original per-face remeshing approach')),default='MATCHED',**kw),
        'resolution': bpy.props.FloatProperty(name='Resolution',default=25.,min=5.,soft_max=400.,subtype='PERCENTAGE',**kw,
            description='Mesh density: 100% uses Detail Size; 25% uses four times larger cells and much fewer polygons. Matching faces keep the same pattern'),
        'strength': bpy.props.FloatProperty(
            name='Strength (cm)', default=20.0, min=0.0, soft_max=20.0, **kw,
            description='How far the break faces are pushed. Follows Master Scale'),
        'scale': bpy.props.FloatProperty(
            name='Scale (cm)', default=1.0, min=0.5, soft_max=200.0, **kw,
            description='Size of the biggest bumps and ledges. Follows Master Scale'),
        'detail': bpy.props.FloatProperty(
            name='Detail Size (cm)', default=1.5, min=0.1, soft_max=10.0, **kw,
            description='Edge length of the remeshed break faces: smaller is finer and heavier. Follows Master '
                        'Scale (grows by itself when the pieces would get too heavy)'),
        'rocky': bpy.props.FloatProperty(
            name='Rocky', default=0.6, min=0.0, max=1.0, subtype='FACTOR', **kw,
            description='Zero: soft lumps. One: sharp ridges and ledges like broken rock'),
        'jagged': bpy.props.FloatProperty(
            name='Jagged Cracks', default=0.8, min=0.0, max=1.5, soft_max=1.0, subtype='FACTOR', **kw,
            description='How much the fracture lines zigzag across the outer surface. Zero keeps them straight'),
        'seed': bpy.props.IntProperty(name='Seed', default=0, min=0, **kw,
                                      description='Random pattern of the noisy cutter; the same seed is repeatable'),
        'proxy': bpy.props.BoolProperty(
            name='Physics Proxy', default=True, **kw,
            description='Keep each piece plain and low-poly for the simulation (shown as wire, not rendered) '
                        'and put the rough piece on it as a child that follows it. Hide the proxies with '
                        'the eye button and only the rough pieces show; they still simulate'),
    }


class ProjectDamageInnerRoughLive(bpy.types.PropertyGroup):
    """Settings for outputs an Update rebuilds (Cell Fracture, Rigid Body Fracture, Damage Debris)."""
    __annotations__ = _annotations(_touch)


class ProjectDamageInnerRough(bpy.types.PropertyGroup):
    """Settings for outputs an operator makes (Pieces for Physics, Separate Fragments)."""
    __annotations__ = _annotations(_touch)


class ProjectDamageInnerRoughSet(bpy.types.PropertyGroup):
    cellf: bpy.props.PointerProperty(type=ProjectDamageInnerRoughLive)
    rbf: bpy.props.PointerProperty(type=ProjectDamageInnerRoughLive)
    debris: bpy.props.PointerProperty(type=ProjectDamageInnerRoughLive)
    pieces: bpy.props.PointerProperty(type=ProjectDamageInnerRough)
    fragments: bpy.props.PointerProperty(type=ProjectDamageInnerRough)


def settings(source, kind):
    group = getattr(source, 'project_damage_inner_rough', None) if source is not None else None
    return getattr(group, kind, None) if group is not None else None


def _glass_cell(source,kind):
    return kind=='cellf' and source.project_damage.cellf_material=='1'


def active(s):
    return s is not None and (s.smooth or (s.enable and s.strength > 0.0))


def _bu(context, source, cm):
    """Local cm (following Master Scale) -> Blender units."""
    try:
        from .params import unit_factor, _clamp_scale
        g = _clamp_scale(source.project_damage.global_scale / 100.0)
        return cm * g / unit_factor(context)
    except Exception:
        return cm * 0.01


DETAIL_STAMP = 'project_damage_rough_detail'                               
DETAIL_COLLECTION = 'project_damage_rough_detail_collection'


def _inner_indices(obj, materials, mesh=None):
    from .material_uv import base_material

    requested = {base_material(mat) for mat in materials if mat is not None}
    mats = mesh.materials if mesh is not None else [slot.material for slot in obj.material_slots]
    return {i for i, mat in enumerate(mats) if mat is not None and base_material(mat) in requested}


def _corner_normals(mesh):
    out = np.empty(len(mesh.loops) * 3, dtype=np.float32)
    mesh.corner_normals.foreach_get('vector', out)
    return out


def _inner_area(obj, idx):
    me = obj.data
    if not idx or not len(me.polygons):
        return 0.0
    mi = np.empty(len(me.polygons), dtype=np.int32)
    me.polygons.foreach_get('material_index', mi)
    ar = np.empty(len(me.polygons), dtype=np.float64)
    me.polygons.foreach_get('area', ar)
    sx, sy, sz = obj.matrix_world.to_scale()
    s2 = (abs(sx * sy) + abs(sy * sz) + abs(sx * sz)) / 3.0
    return float(ar[np.isin(mi, list(idx))].sum()) * s2


def _contact_tree(contact):
    """World BVH of the CONTACT objects' surfaces, and a distance tolerance."""
    from mathutils.bvhtree import BVHTree
    verts, polys, size = [], [], 0.0
    for obj in contact:
        if obj is None or obj.type != 'MESH':
            continue
        mw = obj.matrix_world
        base = len(verts)
        verts.extend(mw @ v.co for v in obj.data.vertices)
        polys.extend([base + i for i in p.vertices] for p in obj.data.polygons)
        size = max(size, max(obj.dimensions) if len(obj.data.vertices) else 0.0)
    if not polys:
        return None, 0.0
    return BVHTree.FromPolygons(verts, polys), max(size * 2e-4, 1e-6)


def _kd(points):
    tree = kdtree.KDTree(max(1, len(points)))
    for i, co in enumerate(points):
        tree.insert(co, i)
    tree.balance()
    return tree


def _surface_normals(faces):
    out = []
    for f in faces:
        if f.normal.length > 1e-9 and all(f.normal.angle(m, 0.0) > 0.35 for m in out):
            out.append(f.normal.copy())
    return out


def _detail_collection(context, obj):
    """The '<pieces>_Detail' collection next to the piece's own collection."""
    home = obj.users_collection[0] if obj.users_collection else context.scene.collection
    coll = next((c for c in bpy.data.collections if c.get(DETAIL_COLLECTION) == home.name), None)
    if coll is None:
        coll = bpy.data.collections.new(home.name + '_Detail')
        coll[DETAIL_COLLECTION] = home.name
        parent = next((c for c in _all_collections(context.scene.collection) if home.name in c.children), None)
        (parent or context.scene.collection).children.link(coll)
    return coll


def _all_collections(root):
    yield root
    for child in root.children:
        yield from _all_collections(child)


def detail_of(obj):
    return next((c for c in obj.children if c.get(DETAIL_STAMP)), None)


def _set_proxy(obj, on):
    """The plain piece that is simulated: wire, not rendered, under its rough copy."""
    if on:
        if not obj.get('project_damage_rough_proxy'):
            obj['project_damage_rough_proxy'] = obj.display_type
        obj.display_type = 'WIRE'
        obj.hide_render = True
    elif 'project_damage_rough_proxy' in obj:
        obj.display_type = obj['project_damage_rough_proxy'] or 'TEXTURED'
        obj.hide_render = False
        del obj['project_damage_rough_proxy']


def _remove_object(obj):
    mesh = obj.data if obj.type == 'MESH' else None
    bpy.data.objects.remove(obj, do_unlink=True)
    if mesh is not None and mesh.users == 0:
        bpy.data.meshes.remove(mesh)


def cleanup(source, keep=()):
    """Removes rough copies whose plain piece is gone (or not in KEEP when
    given) and turns orphaned proxies back into plain pieces."""
    uid = source.get('project_damage_uid') if source is not None else None
    keep = {o.as_pointer() for o in keep}
    for obj in list(bpy.data.objects):
        if not obj.get(DETAIL_STAMP) or (uid and obj.get(DETAIL_STAMP) != uid):
            continue
        if obj.parent is None or (keep and obj.parent.as_pointer() in keep):
            if obj.parent is not None:
                _set_proxy(obj.parent, False)
            _remove_object(obj)


def _make_detail(context, source, obj):
    """Rough copy of OBJ as its child, same place; OBJ stays the physics proxy."""
    det = detail_of(obj)
    mesh = obj.data.copy()
    mesh.name = obj.data.name + '_detail'
    if det is None:
        det = bpy.data.objects.new(obj.name + '_detail', mesh)
        _detail_collection(context, obj).objects.link(det)
    else:
        old = det.data
        det.data = mesh
        if old.users == 0:
            bpy.data.meshes.remove(old)
        det.name = obj.name + '_detail'
    det[DETAIL_STAMP] = (source.get('project_damage_uid') if source is not None else None) or obj.name
    det.parent = obj
    det.matrix_parent_inverse.identity()
    det.matrix_basis.identity()
    det.hide_render = False
    det.hide_viewport = False
    obj.hide_set(False);det.hide_set(False)
    for collection in det.users_collection:
        collection.hide_viewport=False;collection.hide_render=False
    if context.space_data and context.space_data.type=='VIEW_3D' and context.space_data.local_view:
        det.local_view_set(context.space_data,True)
    _set_proxy(obj, True)
    return det


def _inner_face_mask(mesh,values,idx):
    allowed=np.isin(values,list(idx))
    tag=mesh.attributes.get('project_damage_physics_inner')
    if tag is not None and tag.domain=='FACE' and tag.data_type=='BOOLEAN':
        mask=np.empty(len(mesh.polygons),bool);tag.data.foreach_get('value',mask)
        return allowed & mask
    provenance=next((mesh.attributes.get(n) for n in ('project_damage_debris_base_poly','project_damage_base_poly') if mesh.attributes.get(n) is not None),None)
    if provenance is not None and provenance.domain=='FACE' and provenance.data_type=='INT':
        base=np.empty(len(mesh.polygons),np.int32);provenance.data.foreach_get('value',base);allowed &= base<0
    return allowed


# применяем общие неровные срезы к выбранным частям
def process(context,source,objects,s,materials,contact=(),proxies=None):
    if not active(s) or not materials:return 0
    if s.enable and s.strength>0:
        from . import inner_cutters
        return inner_cutters.process(context,source,objects,s,materials,contact,proxies)
    _plain(objects)
    changed=0
    for obj in objects:
        if obj is None or obj.type!='MESH':continue
        values=np.empty(len(obj.data.polygons),np.int32);obj.data.polygons.foreach_get('material_index',values)
        mask=_inner_face_mask(obj.data,values,_inner_indices(obj,materials))
        if mask.any():auto_smooth_inner(obj.data,mask,s.smooth_angle);changed+=1
    return changed


def auto_smooth_inner(me,inner,angle,outer_normals=None):
    """Use Blender's split-normal fans, preserving all exterior loop normals."""
    inner=np.asarray(inner,bool)
    if not inner.any():return
    old=_corner_normals(me).reshape(-1,3) if outer_normals is None else np.asarray(outer_normals,np.float32).copy()
    totals=np.empty(len(me.polygons),np.int32);me.polygons.foreach_get('loop_total',totals)
    loops=np.repeat(np.arange(len(me.polygons)),totals);affected=inner[loops]
    flags=np.empty(len(me.polygons),bool);me.polygons.foreach_get('use_smooth',flags);flags[inner]=True;me.polygons.foreach_set('use_smooth',flags)
    edge_ids=np.empty(len(me.loops),np.int32);me.loops.foreach_get('edge_index',edge_ids)
    faces=[[] for _ in me.edges]
    for li,ei in enumerate(edge_ids):faces[int(ei)].append(int(loops[li]))
    attr=me.attributes.get('sharp_edge') or me.attributes.new('sharp_edge','BOOLEAN','EDGE')
    sharp=np.empty(len(me.edges),bool);attr.data.foreach_get('value',sharp)
    normals=np.asarray([f.normal[:] for f in me.polygons]);cosine=math.cos(angle)
    for i,linked in enumerate(faces):
        if not any(inner[f] for f in linked):continue
        sharp[i]=(len(linked)!=2 or not all(inner[f] for f in linked) or normals[linked[0]].dot(normals[linked[1]])<cosine)
    attr.data.foreach_set('value',sharp)
    me.normals_split_custom_set(np.zeros_like(old));me.update()
    computed=_corner_normals(me).reshape(-1,3);old[affected]=computed[affected]
    me.normals_split_custom_set(old);me.update()

def _shade(me, idx, smooth, custom, face_mask=None,angle=math.radians(60)):
    npoly = len(me.polygons)
    mi = np.empty(npoly, dtype=np.int32)
    me.polygons.foreach_get('material_index', mi)
    inner = np.isin(mi, list(idx)) if face_mask is None else np.asarray(face_mask,dtype=bool)
    if smooth:
        stored=None
        if custom:
            attr=me.attributes.get(NORMAL_ATTR)
            if attr is not None:
                stored=np.empty((len(me.loops),3),np.float32);attr.data.foreach_get('vector',stored.ravel());me.attributes.remove(attr)
        auto_smooth_inner(me,inner,angle,stored)
        return
    if custom:
        nl = len(me.loops)
        attr = me.attributes.get(NORMAL_ATTR)
        stored = np.empty(nl * 3, dtype=np.float32)
        attr.data.foreach_get('vector', stored)
        me.attributes.remove(attr)
        stored = stored.reshape(-1, 3)
        loop_vert = np.empty(nl, dtype=np.int32)
        me.loops.foreach_get('vertex_index', loop_vert)
        start = np.empty(npoly, dtype=np.int32)
        total = np.empty(npoly, dtype=np.int32)
        me.polygons.foreach_get('loop_start', start)
        me.polygons.foreach_get('loop_total', total)
        loop_poly = np.repeat(np.arange(npoly), total)
        fn = np.empty(npoly * 3, dtype=np.float32)
        me.polygons.foreach_get('normal', fn)
        fn = fn.reshape(-1, 3)
        out = stored.copy()
        lin = inner[loop_poly]
        if smooth:
            ar = np.empty(npoly, dtype=np.float32)
            me.polygons.foreach_get('area', ar)
            acc = np.zeros((len(me.vertices), 3), dtype=np.float64)
            np.add.at(acc, loop_vert[lin], (fn * ar[:, None])[loop_poly[lin]])
            ln = np.linalg.norm(acc, axis=1, keepdims=True)
            vn = acc / np.maximum(ln, 1e-20)
            out[lin] = vn[loop_vert[lin]]
        else:
            out[lin] = fn[loop_poly[lin]]
        bad = ~np.isfinite(out).all(axis=1) | (np.linalg.norm(out, axis=1) < 1e-6)
        out[bad] = fn[loop_poly[bad]]
        flags=np.empty(npoly,dtype=bool);me.polygons.foreach_get('use_smooth',flags);flags[inner]=bool(smooth)
        me.polygons.foreach_set('use_smooth',flags)
        me.normals_split_custom_set(out.tolist())
        return
    use = np.empty(npoly, dtype=bool)
    me.polygons.foreach_get('use_smooth', use)
    use[inner] = bool(smooth)
    me.polygons.foreach_set('use_smooth', use)
    if smooth:
                                                          
        ne = len(me.edges)
        a = me.attributes.get('sharp_edge') or me.attributes.new('sharp_edge', 'BOOLEAN', 'EDGE')
        sharp = np.zeros(ne, dtype=bool)
        a.data.foreach_get('value', sharp)
        nl = len(me.loops)
        loop_edge = np.empty(nl, dtype=np.int32)
        me.loops.foreach_get('edge_index', loop_edge)
        total = np.empty(npoly, dtype=np.int32)
        me.polygons.foreach_get('loop_total', total)
        loop_poly = np.repeat(np.arange(npoly), total)
        has_in = np.zeros(ne, dtype=bool)
        has_out = np.zeros(ne, dtype=bool)
        has_in[loop_edge[inner[loop_poly]]] = True
        has_out[loop_edge[~inner[loop_poly]]] = True
        sharp |= has_in & has_out
        a.data.foreach_set('value', sharp)


                                                                             
              

def _plain(objects):
    """Back to plain pieces: no rough child, normal display."""
    for obj in objects:
        det = detail_of(obj)
        if det is not None:
            _remove_object(det)
        saved=obj.get('project_damage_noisify_original')
        if saved is not None:
            del obj['project_damage_noisify_original']
            if obj.data.get('project_damage_noisify_form') in {'MATCHED','CUTTERS'}:
                previous=obj.data;obj.data=saved
                if previous.users==0:bpy.data.meshes.remove(previous)
            elif saved.users==0:bpy.data.meshes.remove(saved)
        _set_proxy(obj, False)


def after_place(context, source, outputs, kind):
    """debris_output.place_debris: Cell Fracture, Rigid Body Fracture, Damage Debris."""
    cleanup(None)
    s = settings(source, kind)
    if _glass_cell(source,kind) or not active(s) or not outputs:
        _plain(outputs)
        return
    from . import debris_output
    _, _, _, prop, default = debris_output.KINDS[kind]
    material = getattr(source.project_damage, prop, None) or bpy.data.materials.get(default)
    process(context, source, outputs, s, {material})


# обрабатываем новые осколки заполнения, не двигая контакт со стеной
def after_fracture(context, source, pieces):
    """damage_physics.fracture: Pieces for Physics of every generator (the
    fill objects' inner material: their back faces and every break face)."""
    cleanup(None)
    s = settings(source, 'pieces')
    if not active(s) or not pieces:
        _plain(pieces)
        return
    from . import damage_physics
    fills = {name for o in pieces for name in o.get('project_damage_piece_fills',[o.get('project_damage_piece_fill')]) if name}
    contact = [bpy.data.objects.get(n) for n in fills if n]
    process(context, source, pieces, s, {damage_physics._inner_material(source)}, contact)


def after_fragments(context, src, pieces):
    """separate_fragments: Through Fractures (concrete, hit, glass). The inner
    faces are the faces whose material the source itself does not use."""
    from . import ownership
    source = ownership.source_of(src) if src is not None else None
    source = source or src
    cleanup(None)
    s = settings(source, 'fragments')
    if not active(s) or not pieces:
        _plain(pieces)
        return
    from . import damage_physics
    selected=damage_physics._inner_material(source)
    mats={selected} if selected is not None else set()
    mats.update(sl.material for o in pieces for sl in o.material_slots if sl.material is not None and 'inner' in sl.material.name.lower())
    process(context, source, pieces, s, mats)


# собираем видимые настройки этого раздела
def draw(layout, source, kind):
    if _glass_cell(source,kind):
        return
    s = settings(source, kind)
    if s is None:
        return
    from . import material_controls
    box = layout.box()
    heading = box.row(align=True)
    material_controls.centered_heading(heading, 'Noisy Break Cutters', icon='MOD_DISPLACE')
    ui_image_hints.button(heading, 'inner_form')
    row = box.row(align=True)
    row.prop(s, 'smooth', toggle=True, icon='MOD_SMOOTH')
    row.prop(s, 'enable', toggle=True, icon='MOD_NOISE')
    if s.smooth:box.prop(s,'smooth_angle')
    if s.enable:
        box.label(text='Shared noisy Boolean cutters', icon='MOD_BOOLEAN')
        col = box.column(align=True)
        ui_image_hints.prop(col,s,'resolution')
        col.prop(s, 'strength')
        col.prop(s, 'scale')
        quality_ui.draw(col, s, 'detail')
        quality_ui.draw(col, s, 'noise_detail', text='Noise Detail')
        col.prop(s, 'noise_roughness', text='Noise Roughness')
        col.prop(s, 'seed')
        row = box.row(align=True)
        ui_image_hints.prop(row,s, 'proxy', toggle=True, icon='PHYSICS')
        if s.proxy:
            row.operator('project_damage.inner_rough_proxies', text='', icon='HIDE_OFF').show = True
            row.operator('project_damage.inner_rough_proxies', text='', icon='HIDE_ON').show = False
    op=box.operator('project_damage.inner_rough_refresh',text='Show / Update Noisy Cutters',icon='FILE_REFRESH');op.kind=kind
    op=box.operator('project_damage.inner_rough_refresh',text='Hide Noisy Cutters',icon='HIDE_ON');op.kind=kind;op.show=False
    if kind in {'pieces', 'fragments'} and active(s):
        box.label(text='Show / Update applies these settings to existing pieces', icon='INFO')


# действие «Roughen Selected»
class PROJECT_DAMAGE_OT_inner_rough_selected(bpy.types.Operator):
    """Rough Break Faces / Shade Smooth Inner on the selected meshes, with the Pieces for
    Physics settings of the active source. Runs again on top of an earlier result"""
    bl_idname = 'project_damage.inner_rough_selected'
    bl_label = 'Roughen Selected'
    bl_options = {'REGISTER', 'UNDO'}

    material: bpy.props.StringProperty(name='Inner Material', default='',
                                       description='Material of the break faces. Empty: every material '
                                                   'whose name contains Inner')

    @classmethod
    def poll(cls, context):
        return context.mode == 'OBJECT' and any(o.type == 'MESH' for o in context.selected_objects)

    def execute(self, context):
        from . import ownership
        source = ownership.context_source(context) or context.active_object
        s = settings(source, 'pieces')
        if not active(s):
            self.report({'WARNING'}, 'Turn on Rough Break Faces or Shade Smooth Inner first')
            return {'CANCELLED'}
        objs = [o for o in context.selected_objects if o.type == 'MESH']
        mats = {sl.material for o in objs for sl in o.material_slots if sl.material is not None}
        if self.material:
            mats = {m for m in mats if m.name == self.material}
        else:
            mats = {m for m in mats if 'inner' in m.name.lower()}
        n = process(context, source, objs, s, mats)
        self.report({'INFO'}, 'Break Faces: %d objects' % n)
        return {'FINISHED'}


# действие «Physics Proxies»
class PROJECT_DAMAGE_OT_inner_rough_proxies(bpy.types.Operator):
    """Show or hide the plain physics proxies under the rough pieces (hidden ones still simulate)"""
    bl_idname = 'project_damage.inner_rough_proxies'
    bl_label = 'Physics Proxies'
    bl_options = {'REGISTER', 'UNDO'}

    show: bpy.props.BoolProperty(name='Show', default=False)

    def execute(self, context):
        from . import ownership
        source = ownership.context_source(context)
        uid = source.get('project_damage_uid') if source is not None else None
        n = 0
        for det in bpy.data.objects:
            if not det.get(DETAIL_STAMP) or det.parent is None:
                continue
            if uid and det.get(DETAIL_STAMP) != uid:
                continue
            if det.parent.name in context.view_layer.objects:
                det.parent.hide_set(not self.show)
                n += 1
        self.report({'INFO'}, '%s %d physics proxies' % ('Showing' if self.show else 'Hid', n))
        return {'FINISHED'}


# действие «Update Rough Break Faces»
class PROJECT_DAMAGE_OT_inner_rough_refresh(bpy.types.Operator):
    bl_idname='project_damage.inner_rough_refresh'
    bl_label='Update Rough Break Faces'
    bl_options={'REGISTER','UNDO'}
    kind:bpy.props.StringProperty(default='pieces')
    show:bpy.props.BoolProperty(default=True)
    def execute(self,context):
        from . import ownership,damage_physics
        source=ownership.context_source(context)
        if source is None:return {'CANCELLED'}
        if _glass_cell(source,self.kind):return {'CANCELLED'}
        s=settings(source,self.kind)
        uid=source.get('project_damage_uid')
        stamps={'cellf':'project_damage_cellf_of','rbf':'project_damage_rbf_of','debris':'project_damage_debris_of','pieces':damage_physics.PIECE_STAMP,'fragments':'project_damage_bfrag_of'}
        stamp=stamps.get(self.kind)
        objects=[o for o in bpy.data.objects if stamp and uid and o.type=='MESH' and o.get(stamp)==uid and not o.get(DETAIL_STAMP)]
        generated=False
        if not objects and self.show and self.kind=='pieces':
            fills=damage_physics.fills_of(source)
            selected=[o for o in context.selected_objects if o in fills]
            fills=selected or fills
            if fills:
                from . import live
                previous=live._APPLYING;live._APPLYING=True
                try:s.enable=True
                finally:live._APPLYING=previous
                objects,reports=damage_physics.fracture(context,source,fills[0].get(damage_physics.FILL_KIND,'boolean'),fills=fills)
                for flags,message in reports:self.report(flags,message)
                generated=True
        if not objects:
            self.report({'WARNING'},'Create fragments first');return {'CANCELLED'}
        if not self.show:
            from . import live
            previous=live._APPLYING;live._APPLYING=True
            try:s.enable=False
            finally:live._APPLYING=previous
            from . import background_compute as bg
            bg.cancel(source,method='rough_'+self.kind)
            _PENDING.pop((source.name,self.kind),None);_plain(objects)
            for ob in objects:ob.hide_set(False)
            return {'FINISHED'}
        from . import live
        previous=live._APPLYING;live._APPLYING=True
        try:s.enable=True
        finally:live._APPLYING=previous
        _PENDING.pop((source.name,self.kind),None)
        if not generated:
            if self.kind in {'cellf','rbf','debris'}:after_place(context,source,objects,self.kind)
            elif self.kind=='pieces':after_fracture(context,source,objects)
            else:after_fragments(context,source,objects)
        for ob in objects:
            det=detail_of(ob)
            if det:det.hide_set(False);det.hide_render=False
        return {'FINISHED'}

_CLASSES = (PROJECT_DAMAGE_OT_inner_rough_refresh,ProjectDamageInnerRoughLive, ProjectDamageInnerRough, ProjectDamageInnerRoughSet,
            PROJECT_DAMAGE_OT_inner_rough_selected, PROJECT_DAMAGE_OT_inner_rough_proxies)


# подключаем свойства и действия этого модуля к Blender
def register():
    for handlers in (bpy.app.handlers.undo_pre,bpy.app.handlers.load_pre):
        if _clear_pending not in handlers:handlers.append(_clear_pending)
    for cls in _CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.Object.project_damage_inner_rough = bpy.props.PointerProperty(type=ProjectDamageInnerRoughSet)


# убираем подключения модуля при выключении аддона
def unregister():
    _clear_pending()
    for handlers in (bpy.app.handlers.undo_pre,bpy.app.handlers.load_pre):
        if _clear_pending in handlers:handlers.remove(_clear_pending)
    if hasattr(bpy.types.Object, 'project_damage_inner_rough'):
        del bpy.types.Object.project_damage_inner_rough
    for cls in reversed(_CLASSES):
        try:
            bpy.utils.unregister_class(cls)
        except RuntimeError:
            pass
