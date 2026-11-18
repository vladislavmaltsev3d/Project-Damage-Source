# восстанавливаем повреждённые объёмы, делим их и настраиваем физику
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pieces for Physics on wall damage: Fill Damage and Fracture.

Concrete Hit, Concrete Damage (V2), Lath Damage, Lath and Plaster V2, Riven
Lath, Stud Framing and Brick Damage cut a hole into the source. For a
rigid-body shot of the wall breaking, two buttons work on that hole:

* Fill Damage: one closed object per damaged area that fills the hole
  exactly and looks like the undamaged wall (the source's materials and UVs
  on its front, the inner material on the faces that touch the damage). It
  is the source minus the damage result (a Boolean, Manifold solver when
  Blender has it), cut around the laths, studs, bricks, mortar and rebar so
  it never overlaps them. Lath effects fill only the plaster coat in front
  of the laths (Fill Depth: Auto).
* Fracture: breaks every fill object into pieces for a simulation, by
  Fracture Type: the native core's Cell Fracture (Concrete, Rock, Soil, Wood,
  Glass) run straight on the fill mesh, or Flat Cells. Auto: Brick Damage
  fills break along the bricks and the mortar joints of the wall's own brick
  lattice (the render in front breaks into plates); Concrete Hit fills break
  as concrete, finer at the impacts; the others as concrete.

Edge, Corner and Surface Damage and classic Fracture (2026-10-26) get the same
Fill Damage and Fracture, limited to their own damage by the core's effect
stamp. Bead, Corner Piles and Deposits add material instead of cutting it:
Detach makes one closed object per lump (Remove from Wall switches the
generator off), and Fracture breaks them (Auto: Soil, crumbly).

Bullet Impacts and Hanging Rebar Holes (2026-10-26) get the same Fill Damage
and Fracture, limited to their own holes by the effect stamp. Bullet fills
break by the bullet material (Auto: metal Rock, wood Wood, concrete
Concrete) at their own, finer Piece Size. Hanging Rebar fills are cut around
the bars and cables like every fill, so the chunks thread onto them.

Concrete Hit also gets Separate Fragments (the impact plates come apart as
loose islands, then Separate to Pieces makes one object of each) and Fill
Hole (Fill Damage and Fracture in one step).

Every piece is closed, has its origin at its centre of mass and no parent.
Quick Physics adds rigid bodies when asked. The source, its result and the
other outputs are never rebuilt or edited. Pure Python on top of the existing core: no core rebuild needed."""
from . import output_collections
from .blender_compat import enable_nodes
from . import ui_image_hints
import math
import random
from functools import wraps

import bmesh
from . import guide_timing
import bpy
import numpy as np
from mathutils import Matrix, Vector

from . import build, live, ownership, physics_workflow

FILL_STAMP = 'project_damage_fill_of'
FILL_KIND = 'project_damage_fill_kind'
FILL_COLLECTION = 'project_damage_fill_collection'
PIECE_STAMP = 'project_damage_fill_piece_of'
PIECE_COLLECTION = 'project_damage_fill_piece_collection'
SPLIT_STAMP = 'project_damage_split_piece_of'
INNER_DEFAULT = 'ProjectDamage_FillInner'
MODULES = ('hit', 'concrete', 'lath', 'brick', 'edge', 'corner', 'surface', 'fracture', 'bead', 'piles', 'deposits',
           'bullet', 'hang', 'boolean', 'dbevel', 'erosion')
LABELS = {'hit': 'Concrete Hit', 'concrete': 'Concrete Damage', 'lath': 'Lath', 'brick': 'Brick Damage',
          'edge': 'Edge Damage', 'corner': 'Corner Damage', 'surface': 'Surface Damage', 'fracture': 'Fracture',
          'bead': 'Bead', 'piles': 'Corner Piles', 'deposits': 'Deposits',
          'bullet': 'Bullet Impacts', 'hang': 'Hanging Rebar Holes',
          'boolean': 'Boolean Break', 'dbevel': 'Damaged Bevel', 'erosion': 'Voxel Erosion'}





EFFECT_IDS = {'edge': (1,), 'corner': (2,), 'surface': (3,), 'fracture': (4,),
              'bead': (5,), 'piles': (28,), 'deposits': (29,),

              'bullet': (15,), 'hang': (26, 23),
              'boolean': (23,), 'dbevel': (21,), 'erosion': (32,)}
ADDITIVE = ('bead', 'piles', 'deposits')
ENABLE_PROP = {'edge': 'chips_enable', 'corner': 'corner_enable', 'surface': 'surf_enable',
               'fracture': 'fract_enable', 'bead': 'build_enable', 'piles': 'pile_enable',
               'deposits': 'deposit_enable', 'bullet': 'bullet_enable', 'hang': 'hang_enable',
               'boolean': 'ui_boolean_enable', 'dbevel': 'dbevel_enable', 'erosion': 'erosion_enable'}


_OPERATION_DEPTH = 0


def _operation(fn=None, *, weld=True):
    """Explicit operators finish atomically; never leave jobs using temporary meshes."""
    if fn is None:
        return lambda operation: _operation(operation, weld=weld)

    @wraps(fn)
    def run(context, source, *args, **kwargs):
        global _OPERATION_DEPTH
        from . import background_compute
        preview = kwargs.get('live_preview', False)
        if not preview:
            background_compute.cancel(source)
        previous = live._APPLYING
        live._APPLYING = True
        _OPERATION_DEPTH += 1
        try:
            from . import effect_groups, boolean_break
            from contextlib import ExitStack


            with background_compute.synchronous(), ExitStack() as stack:
                if weld and not preview:
                    for body in [build.result_of(source), *boolean_break.fragments_of(source)]:
                        stack.enter_context(effect_groups.welded(body))
                return fn(context, source, *args, **kwargs)
        finally:
            _OPERATION_DEPTH -= 1
            try:
                if _OPERATION_DEPTH == 0:
                    _drop_work_scene()
            finally:
                live._APPLYING = previous
    return run





def _touch(self, context):
    live.touch_prop(self, context)


def _inner_update(self,context):
    source=self.id_data
    if source is None:return
    for obj in fills_of(source)+pieces_of(source):_apply_inner(obj,source)


def _inner_material_update(self, context):
    from . import material_update
    source = self.id_data
    material = self.inner_material
    if source is None or material is None or live._APPLYING or live._HISTORY_RESTORING:
        return
    from . import fracture_uv
    with material_update.shader_edit(context):
        for obj in fracture_uv.with_details(fills_of(source) + pieces_of(source)):
            uv_name = fracture_uv.layer_name(obj.data)
            outer = material_update.face_values(obj.data, '_pd_fill_outer')
            if outer is not None:
                material_update.assign(obj, material, outer == 0, uv_name=uv_name)
                continue
            inner = material_update.face_values(obj.data, 'project_damage_physics_inner')
            if inner is not None:
                material_update.assign(obj, material, inner.astype(bool, copy=False), uv_name=uv_name)
            else:
                _apply_inner(obj, source)
    material_update.expect_rna_notification(source)


def _apply_inner(obj,source):
    mesh=obj.data
    if not len(mesh.polygons):return
    attr=mesh.attributes.get('project_damage_physics_inner')
    idx=np.empty(len(mesh.polygons),np.int32);mesh.polygons.foreach_get('material_index',idx)
    original=[slot.material for slot in source.material_slots]
    materials=list(mesh.materials)
    interior_slots={i for i,mat in enumerate(materials) if mat is not None and mat not in original}
    mask=np.isin(idx,list(interior_slots))
    if materials[:len(original)]==original:
        mask |= idx>=max(1,len(original))
    provenance=mesh.attributes.get('_pd_fill_outer')
    if provenance is not None:
        outer=np.empty(len(mesh.polygons),np.int32)
        provenance.data.foreach_get('value',outer)
        mask=outer==0
    elif attr is not None:
        old=np.empty(len(mesh.polygons),bool);attr.data.foreach_get('value',old);mask=old
    if attr is None:attr=mesh.attributes.new('project_damage_physics_inner','BOOLEAN','FACE')
    attr.data.foreach_set('value',mask)
    s=settings(source);mat=s.inner_material
    if mat is not None:
        from . import fracture_uv, material_uv
        uv_name = fracture_uv.layer_name(mesh)
        if uv_name:
            mat = material_uv.for_object(mat, obj, uv_name)
        slot=next((i for i,m in enumerate(mesh.materials) if m==mat),None)
        if slot is None:slot=len(mesh.materials);mesh.materials.append(mat)
        idx[mask]=slot;mesh.polygons.foreach_set('material_index',idx)
    if s.smooth_inner:
        from .inner_rough import auto_smooth_inner
        auto_smooth_inner(mesh,mask,s.inner_smooth_angle)
        _restore_fill_normals(mesh,mask)
        return
    normals=np.array([n.vector[:] for n in mesh.corner_normals],dtype=np.float32)
    for face in mesh.polygons:
        if mask[face.index]:
            face.use_smooth=s.smooth_inner
            normals[face.loop_start:face.loop_start+face.loop_total]=0. if s.smooth_inner else face.normal[:]
    mesh.normals_split_custom_set(normals);mesh.update()
    _restore_fill_normals(mesh,mask)


def _restore_fill_normals(mesh, inner):
    """Exterior shading is the source's field, not an average with cut walls.

    The Boolean and BMesh interpolate this CORNER field together with the UVs.
    Consequently seams stay split and the surface needs no extra vertices.
    """
    attr=mesh.attributes.get('_pd_fill_outer_normal')
    if attr is None or len(attr.data)!=len(mesh.loops):return
    saved=np.empty((len(mesh.loops),3),np.float32);attr.data.foreach_get('vector',saved.ravel())
    normals=np.empty_like(saved);mesh.corner_normals.foreach_get('vector',normals.ravel())
    counts=np.empty(len(mesh.polygons),np.int32);mesh.polygons.foreach_get('loop_total',counts)
    use=np.repeat(~inner,counts)
    lengths=np.linalg.norm(saved,axis=1)
    use &= np.isfinite(saved).all(axis=1)&(lengths>1e-12)
    normals[use]=saved[use]/lengths[use,None]
    mesh.normals_split_custom_set(normals)
    mesh.update()


def _fill_base(context, source, triangulate=True, *, solid_damage=False):
    """Match the damage input geometry and carry its exterior corner normals."""
    from . import input_prep
    use_modifiers=input_prep.settings(source).use_modifiers
    base=_object_mesh_local(source,source) if use_modifiers else source.data.copy()
    normals=np.array([n.vector[:] for n in base.corner_normals],dtype=np.float32)
    if not use_modifiers and source.modifiers:


        reference=_object_mesh_local(source,source)
        try:
            if len(reference.vertices)==len(base.vertices) and len(reference.loops)==len(base.loops):
                a=np.empty(len(base.vertices)*3);b=np.empty_like(a)
                base.vertices.foreach_get('co',a);reference.vertices.foreach_get('co',b)
                ia=np.empty(len(base.loops),np.int32);ib=np.empty_like(ia)
                base.loops.foreach_get('vertex_index',ia);reference.loops.foreach_get('vertex_index',ib)
                if np.allclose(a,b,rtol=0,atol=1e-7) and np.array_equal(ia,ib):
                    reference.corner_normals.foreach_get('vector',normals.ravel())
        finally:bpy.data.meshes.remove(reference)
    for name in ('_pd_fill_outer','_pd_fill_outer_normal','project_damage_physics_inner'):
        attr=base.attributes.get(name)
        if attr is not None:base.attributes.remove(attr)
    base.attributes.new('_pd_fill_outer','INT','FACE').data.foreach_set('value',np.ones(len(base.polygons),np.int32))
    base.attributes.new('_pd_fill_outer_normal','FLOAT_VECTOR','CORNER').data.foreach_set('vector',normals.ravel())
    from .params import unit_factor
    factor=unit_factor(context)
    plan = input_prep.plan(context, source, factor, solid_damage=solid_damage)
    if plan is not None:
        _,prep=plan
        base=_prepared_fill_mesh(base,prep,factor)
    if not triangulate:return base


    bm=bmesh.new();bm.from_mesh(base)
    bmesh.ops.triangulate(bm,faces=[f for f in bm.faces if len(f.verts)>3],quad_method='FIXED',ngon_method='EAR_CLIP')
    bm.to_mesh(base);bm.free();base.update()
    return base


def _prepared_fill_mesh(base,prep,factor):
    """Reuse the generator's weld/cap/shell plan, including exterior UV/normals.
    Executed only by Fill; it adds no work to normal generator updates."""
    from .fill_v2 import MeshData
    data=MeshData(base)
    tri=prep.polys[:,2]==prep.polys[:,3]
    keep=np.ones(prep.polys.shape,bool);keep[tri,3]=False
    faces=[tuple(row[:3] if t else row) for row,t in zip(prep.polys,tri)]
    mesh=bpy.data.meshes.new('_pd_fill_prepared')
    mesh.from_pydata(prep.points/factor,[],faces);mesh.update()
    for mat in data.materials:mesh.materials.append(mat)
    face_map=np.maximum(prep.poly_src,0)
    loop_map=prep.corner_loop[keep];valid_loop=loop_map>=0
    mesh.polygons.foreach_set('material_index',data.material[face_map])
    mesh.polygons.foreach_set('use_smooth',data.smooth[face_map])
    for name,values in data.uv.items():
        uv=np.zeros((len(loop_map),2),np.float32);uv[valid_loop]=values[loop_map[valid_loop]]
        mesh.uv_layers.new(name=name).uv.foreach_set('vector',uv.ravel())
    if data.uv_active:mesh.uv_layers.active=mesh.uv_layers.get(data.uv_active)
    if data.uv_render:mesh.uv_layers[data.uv_render].active_render=True
    for name,domain,kind,field,values in data.attrs:
        if domain=='FACE':
            mapped=values[face_map].copy();mapped[prep.helper]=0
        elif domain=='POINT':mapped=values[prep.point_src]
        else:
            mapped=np.zeros((len(loop_map),values.shape[1]),values.dtype)
            mapped[valid_loop]=values[loop_map[valid_loop]]
        attr=mesh.attributes.get(name) or mesh.attributes.new(name,kind,domain)
        attr.data.foreach_set(field,mapped.ravel())
    if base.users==0:bpy.data.meshes.remove(base)
    return mesh


def fill_block_reason(source):
    """Detached bodies can fill their original cut only at their rest poses.
    Compare source-local matrices so moving the complete source is harmless.
    Old scenes retain the original fragment COM, so need no regeneration."""
    from . import boolean_break
    if source is None:return ''
    inv=source.matrix_world.inverted_safe()
    fragments=boolean_break.fragments_of(source)
    result=build.result_of(source)
    if fragments and result is not None and not np.allclose(np.asarray(inv@result.matrix_world),np.eye(4),rtol=0,atol=1e-6):
        return 'Fill unavailable: the main damage body was moved. Return it to its original position.'
    for obj in fragments:
        rest=obj.get('project_damage_bfrag_rest')
        if rest is not None and len(rest)==16:
            expected=Matrix([rest[i:i+4] for i in range(0,16,4)])
        else:
            com=obj.get('project_damage_bfrag_com')
            if com is None:return 'Fill unavailable: fragment rest position is unknown. Update the generator first.'
            expected=Matrix.Translation(Vector(com))
        if not np.allclose(np.asarray(inv@obj.matrix_world),np.asarray(expected),rtol=0,atol=1e-6):
            return 'Fill unavailable: '+obj.name+' was moved. Return separated fragments to their original positions.'
    return ''


def fracture_targets(context,source,module,fragments_only=False):
    """One Fracture button: selected detached bodies take precedence over fills."""
    from .boolean_break import FRAGMENT_STAMP
    uid=_uid(source)
    fragments=[o for o in context.selected_objects if o.type=='MESH' and o.get(FRAGMENT_STAMP)==uid]
    if module=='boolean' and fragments:return fragments
    if fragments_only:raise RuntimeError('Select at least one separated Boolean fragment')
    chosen=[o for o in context.selected_objects if o.type=='MESH' and o.get(FILL_STAMP)==uid]
    return chosen or None


def _triangulate_warped_faces(bm):
    """Resolve only non-planar faces; planar n-gons stay compact."""
    warped=[]
    for face in bm.faces:
        if len(face.verts)<4:continue
        origin=face.verts[0].co
        span=max((v.co-origin).length for v in face.verts)
        if max(abs((v.co-origin).dot(face.normal)) for v in face.verts)>max(1e-7,span*1e-6):
            warped.append(face)
    if warped:bmesh.ops.triangulate(bm,faces=warped,quad_method='FIXED',ngon_method='EAR_CLIP')


# здесь хранятся значения, которые сохраняются вместе с объектом
class ProjectDamagePhysicsSettings(bpy.types.PropertyGroup):
    inner_material:bpy.props.PointerProperty(name="Inside Faces",type=bpy.types.Material,update=_inner_material_update)
    smooth_inner:bpy.props.BoolProperty(name="Auto Smooth Inside Faces",default=False,update=_inner_update)
    inner_smooth_angle:bpy.props.FloatProperty(name='Inside Smooth Angle',default=math.radians(60),min=0,max=math.pi,subtype='ANGLE',update=_inner_update)
    fill_depth: bpy.props.EnumProperty(
        name='Fill Depth', default='AUTO',
        items=(('AUTO', 'Auto', 'Lath effects and Stud Framing: the plaster coat in front of the laths or studs. '
                'Concrete, Concrete Hit and Brick Damage: the whole cut'),
               ('WHOLE', 'Whole Cut', 'Everything the damage removed, around the laths, studs, bricks and rebar'),
               ('SURFACE', 'Surface Layer', 'Only the layer under the surface, Surface Depth deep')),
        description='How deep the fill reaches into the damage')
    surface_depth: bpy.props.FloatProperty(
        name='Surface Depth (cm)', default=2.0, min=0.01, soft_max=20.0,
        description='Surface Layer: thickness of the filled layer. Follows Master Scale')
    fill_cracks: bpy.props.BoolProperty(
        name='Fill Lone Cracks', default=False,
        description='Cracks that run out of a damaged area are always filled with it. On: cracks '
                    'that touch no damaged area get thin fill objects of their own too. Off: they stay '
                    'visible on the filled wall (anything thinner than Damage Debris Min Thickness is dropped)')
    piece_size: bpy.props.FloatProperty(
        name='Piece Size (cm)', default=12.0, min=0.5, soft_max=100.0,
        description='Typical size of a concrete, plaster or render piece. Follows Master Scale')
    amount_budget:bpy.props.IntProperty(name='Amount Budget',default=250,min=1,soft_max=2000,
        description='Maximum fragment count for one Fracture operation. Limits size-based counts before generation; excess disconnected regions are kept together rather than discarded')
    seed: bpy.props.IntProperty(name='Seed', default=0, min=0, description='Variation of the fracture')
    brick_mortar: bpy.props.EnumProperty(
        name='Mortar', default='ATTACH',
        items=(('ATTACH', 'With Bricks', 'Each brick keeps half of the joints around it'),
               ('SEPARATE', 'Separate Pieces', 'The bricks and the mortar joints break apart: '
                'every brick also drops its bed joint and its head joint as two thin pieces')),
        description='Brick Damage: what happens to the mortar joints')
    brick_snap: bpy.props.FloatProperty(
        name='Snapped Bricks', default=0.15, min=0.0, max=1.0,
        description='Brick Damage: chance that a brick breaks in two')
    hide_fill: bpy.props.BoolProperty(
        name='Hide Fill', default=True, description='Hide the fill objects once they are fractured')

    hit_separate: bpy.props.BoolProperty(
        name='Separate Fragments', default=False, update=_touch,
        description='Concrete Hit: the cracks around the crater cut through, so the impact plates come '
                    'apart as loose pieces (Piece Gap, Remove Chunks, Min Piece)')
    hit_fill_shards: bpy.props.IntProperty(
        name='Shards', default=40, min=2, max=600, soft_max=200,
        description='Concrete Hit (Fill Hole and Fracture): number of shards in one fill object')
    hit_fill_center: bpy.props.FloatProperty(
        name='Denser at Centre', default=0.7, min=0.0, max=1.0,
        description='Zero spreads the shards evenly; one packs small shards tightly around the impact centre')
    material: bpy.props.EnumProperty(
        name='Fracture Type', default='AUTO',
        items=(('AUTO', 'Auto', 'Brick Damage: along the bricks and joints. Concrete Hit: concrete, finer at the '
                'impacts. Concrete, lath, stud framing and Hanging Rebar Holes: concrete. Bullet Impacts: '
                'by the bullet material (metal Rock, wood Wood, concrete Concrete)'),
               ('CONCRETE', 'Concrete', 'Cell Fracture Concrete: chunky pieces with rough break faces'),
               ('ROCK', 'Rock', 'Cell Fracture Rock: angular blocks'),
               ('SOIL', 'Soil', 'Cell Fracture Soil: big and small clods and crumbs'),
               ('WOOD', 'Wood', 'Cell Fracture Wood: long splinters along the grain'),
               ('GLASS', 'Glass', 'Cell Fracture Glass: shards with radial and ring cracks'),
               ('FLAT', 'Flat Cells', 'Plain Voronoi cells with flat faces')),
        description='How the fill objects break (the Cell Fracture materials come from the native core)')
    grain: bpy.props.EnumProperty(
        name='Grain', default='Z',
        items=(('X', 'X', 'Fibres along the object X axis'), ('Y', 'Y', 'Fibres along the object Y axis'),
               ('Z', 'Z', 'Fibres along the object Z axis (vertical studs and boards)')),
        description='Wood: direction of the fibres')

    cut_mode: bpy.props.EnumProperty(
        name='Fracture', default='AUTO',
        items=(('AUTO', 'Automatic + Cuts', 'Generate cells, then apply the knife drawings and any assigned Cut Plane. Guidance adds seeds first'),
               ('DRAWN', 'Cuts Only', 'Split using the assigned Cut Plane and knife drawings. Guidance adds local cells if assigned'),
               ('BOTH', 'Automatic + Cuts', 'Generate cells, then apply the knife drawings and any assigned Cut Plane')),
        description='What breaks the fill objects')
    cut_drawing: bpy.props.PointerProperty(
        name='Fracture Lines', type=bpy.types.Object, poll=lambda self, obj: obj.type in {'GREASEPENCIL', 'GPENCIL', 'CURVE'},
        description='Grease Pencil (or Curve) drawing whose strokes are fracture lines. Each stroke cuts straight '
                    'through the wall, so it does not matter which surface it lies on')
    cut_gap: bpy.props.FloatProperty(
        name='Line Gap (cm)', default=0.05, min=0.001, soft_max=2.0,
        description='Width of a drawn fracture line. Follows Master Scale')

    detach_remove: bpy.props.BoolProperty(
        name='Remove from Wall', default=True,
        description='Bead, Corner Piles and Deposits: once detached, switch the generator off on the wall and '
                    'update it, so only the loose objects remain (Undo brings it back)')

    bullet_piece_size: bpy.props.FloatProperty(
        name='Piece Size (cm)', default=1.0, min=0.05, soft_max=10.0,
        description='Bullet Impacts: typical size of a piece of a crater or bullet hole. Follows Master Scale')


def settings(obj):
    return obj.project_damage_physics





def _gscale(props):
    try:
        from .params import _clamp_scale
        return _clamp_scale(props.global_scale / 100.0)
    except Exception:
        return 1.0


def _cm(context, source, value):
    """Local cm (following Master Scale) -> Blender units."""
    from .params import unit_factor
    return value * _gscale(source.project_damage) / unit_factor(context)


def module_enabled(p, module):
    if module == 'hit':
        return bool(p.hit_enable or p.hit_through_enable)
    if module == 'concrete':
        return bool(p.concrete_enable or p.concrete_v2_enable or getattr(p, 'pipe_damage_enable', False)
                    or getattr(p, 'cable_damage_enable', False))
    if module == 'lath':
        return bool(p.lath_enable or p.lath_v2_enable or getattr(p, 'lath_riven_enable', False)
                    or getattr(p, 'lath_framing_enable', False))
    if module == 'brick':
        return bool(p.brick_enable or getattr(p, 'brick_v2_enable', False))
    if module in ENABLE_PROP:
        if module=='fracture':return bool(p.fract_enable or p.fract_through_enable)
        return bool(getattr(p, ENABLE_PROP[module], False))
    return False


def _uid(source):
    return source.get('project_damage_uid')


def fills_of(source, module=None):
    """The fill objects of a source; with `module`, only that effect's (the
    wall effects share theirs, as before the worn effects had any)."""
    uid = _uid(source)
    out = [o for o in bpy.data.objects if uid and o.get(FILL_STAMP) == uid]
    if module is None:
        return out
    exact = [o for o in out if module in o.get('project_damage_exact_modules', ())]
    if exact:
        return exact
    return [o for o in out if not o.get('project_damage_exact_modules') and
            (o.get(FILL_KIND) == module or
             module not in EFFECT_IDS and o.get(FILL_KIND) not in EFFECT_IDS)]


def pieces_of(source):
    uid = _uid(source)
    return [o for o in bpy.data.objects if uid and o.get(PIECE_STAMP) == uid]


def _collections(root):
    yield root
    for child in root.children:
        yield from _collections(child)


def _collection(context, source, stamp, suffix):
    uid = _uid(source)
    coll = next((c for c in bpy.data.collections if c.get(stamp) == uid), None)
    if coll is None:
        coll = bpy.data.collections.new(source.name + suffix)
        coll[stamp] = uid
    output_collections.link_collection(context,source,coll)
    return coll


def _remove(obj):
    for child in [c for c in getattr(obj, 'children', ()) if c.get('project_damage_rough_detail')]:
        child_mesh = child.data
        bpy.data.objects.remove(child, do_unlink=True)
        if child_mesh is not None and child_mesh.users == 0:
            bpy.data.meshes.remove(child_mesh)
    mesh = obj.data if obj.type == 'MESH' else None
    bpy.data.objects.remove(obj, do_unlink=True)
    if mesh is not None and mesh.users == 0:
        bpy.data.meshes.remove(mesh)






WORK_SCENE = '_ProjectDamage_Work'
_WORK_SCENE_NAME = None


def _work_scene():
    global _WORK_SCENE_NAME
    scene = bpy.data.scenes.get(_WORK_SCENE_NAME) if _WORK_SCENE_NAME else None
    if scene is None:
        scene = bpy.data.scenes.new(WORK_SCENE)
        _WORK_SCENE_NAME = scene.name
    return scene


def _work_link(obj):
    _work_scene().collection.objects.link(obj)


def _work_depsgraph():
    scene = _work_scene()
    with bpy.context.temp_override(scene=scene, view_layer=scene.view_layers[0]):
        dg = bpy.context.evaluated_depsgraph_get()
    dg.update()
    return dg


def _work_operand(obj):
    """`obj` itself when it is already a work object, else a stand-in sharing
    its mesh (no copy) at the same place."""
    scene = _work_scene()
    if obj.name in scene.objects:
        return obj
    proxy = bpy.data.objects.new('_pd_operand', obj.data)
    scene.collection.objects.link(proxy)
    proxy.matrix_world = obj.matrix_world
    return proxy


def _drop_work_scene():
    global _WORK_SCENE_NAME
    scene = bpy.data.scenes.get(_WORK_SCENE_NAME) if _WORK_SCENE_NAME else None
    if scene is None:
        _WORK_SCENE_NAME = None
        return
    for obj in list(scene.collection.all_objects):
        _remove(obj)
    bpy.data.scenes.remove(scene)
    _WORK_SCENE_NAME = None


def _inner_material(source):
    mat = settings(source).inner_material or getattr(source.project_damage, 'debris_material', None)
    if mat is not None:
        return mat
    mat = bpy.data.materials.get(INNER_DEFAULT)
    if mat is None:
        mat = bpy.data.materials.new(INNER_DEFAULT)
        enable_nodes(mat)
        mat.diffuse_color = (.3, .27, .23, 1)
        shader = mat.node_tree.nodes.get('Principled BSDF')
        if shader is not None:
            shader.inputs['Base Color'].default_value = mat.diffuse_color
            shader.inputs['Roughness'].default_value = .9
    return mat


def _mass(mesh):
    """(volume, centre of mass) of a closed mesh, in its own space."""
    mesh.calc_loop_triangles()
    n = len(mesh.loop_triangles)
    if not n:
        return 0.0, Vector()
    co = np.empty(len(mesh.vertices) * 3)
    mesh.vertices.foreach_get('co', co)
    co = co.reshape(-1, 3)
    tri = np.empty(n * 3, dtype=np.int64)
    mesh.loop_triangles.foreach_get('vertices', tri)
    a, b, c = co[tri[0::3]], co[tri[1::3]], co[tri[2::3]]
    vol = np.einsum('ij,ij->i', a, np.cross(b, c)) / 6.0
    total = vol.sum()
    if abs(total) < 1e-15:
        return 0.0, Vector(co.mean(axis=0))
    centre = ((a + b + c) * vol[:, None]).sum(axis=0) / (4.0 * total)
    return abs(float(total)), Vector(centre)


def _make_object(name, mesh, coll, source, local_matrix=None, *, apply_inner=True):
    """Object for a mesh in SOURCE-local space: origin at the centre of mass,
    no parent, same world placement as the source."""
    vol, com = _mass(mesh)
    mesh.transform(Matrix.Translation(-com))
    mesh.update()
    obj = bpy.data.objects.new(name, mesh)
    coll.objects.link(obj)
    base = source.matrix_world if local_matrix is None else local_matrix
    obj.matrix_world = base @ Matrix.Translation(com)
    obj.project_damage_og_object = ownership.source_of(source)
    if apply_inner:
        _apply_inner(obj, source)
    return obj, vol


def _islands(bm):
    """Face index lists of the loose parts of a bmesh."""
    bm.faces.index_update()
    bm.faces.ensure_lookup_table()
    seen = np.zeros(len(bm.faces), dtype=bool)
    parts = []
    for f in bm.faces:
        if seen[f.index]:
            continue
        stack = [f]
        seen[f.index] = True
        comp = []
        while stack:
            g = stack.pop()
            comp.append(g.index)
            for e in g.edges:
                for h in e.link_faces:
                    if not seen[h.index]:
                        seen[h.index] = True
                        stack.append(h)
        parts.append(comp)
    return parts


def _solid_islands(bm):
    """Keep an inward cavity shell with its enclosing outward surface.

    Face-connected islands are NOT solid volumes: erosion can remove a skin
    enclosing an untouched core. Separating those surfaces makes the cavity
    an inverted standalone solid which Cell Fracture cannot cut.
    """
    from mathutils.bvhtree import BVHTree
    parts=_islands(bm)
    if len(parts)<2:return parts
    shells=[]
    for ids in parts:
        vertices={v for i in ids for v in bm.faces[i].verts}
        co=np.array([v.co[:] for v in vertices]);lo=co.min(0);hi=co.max(0)

        origin=Vector(co.mean(0));volume=0.0
        for i in ids:
            points=[v.co-origin for v in bm.faces[i].verts]
            for j in range(1,len(points)-1):volume+=points[0].dot(points[j].cross(points[j+1]))/6.0
        shells.append(dict(ids=ids,lo=lo,hi=hi,volume=volume,vertices=vertices,bvh=None))
    positives=[s for s in shells if s['volume']>0]
    for cavity in (s for s in shells if s['volume']<0):
        candidates=sorted((s for s in positives if np.all(s['lo']<=cavity['lo']+1e-7) and np.all(s['hi']>=cavity['hi']-1e-7)),key=lambda s:s['volume'])
        point=next(iter(cavity['vertices'])).co
        for outer in candidates:
            if outer['bvh'] is None:
                vertices=list(outer['vertices']);index={v:i for i,v in enumerate(vertices)}
                outer['bvh']=BVHTree.FromPolygons([v.co for v in vertices],[[index[v] for v in bm.faces[i].verts] for i in outer['ids']])
            nearest,normal,_,_=outer['bvh'].find_nearest(point)
            if nearest is not None and (point-nearest).dot(normal)<0:
                outer['ids']=outer['ids']+cavity['ids'];cavity['ids']=[];break
    return [s['ids'] for s in shells if s['ids']]


def _bm_part(bm, keep):
    """A copy of bm holding only the faces in `keep` (indices)."""
    out = bm.copy()
    out.faces.ensure_lookup_table()
    keep = set(keep)
    drop = [f for f in out.faces if f.index not in keep]
    if drop:
        bmesh.ops.delete(out, geom=drop, context='FACES')
    loose = [v for v in out.verts if not v.link_faces]
    if loose:
        bmesh.ops.delete(out, geom=loose, context='VERTS')
    return out


def _split_pinches(bm):
    """Separate solids that touch along an edge (an edge shared by four or
    more faces, as an exact Boolean leaves them): the faces around such an
    edge are paired by the solid wedge they bound, each pair gets its own
    edge, so every loose part is a closed 2-manifold of its own."""
    bad = [e for e in bm.edges if len(e.link_faces) > 2]
    if not bad:
        return
    pairs = []
    for e in bad:
        u, v = e.verts
        d = v.co - u.co
        if d.length < 1e-12:
            continue
        d.normalize()
        ring = []
        e1 = None
        for f in e.link_faces:
            pv = f.calc_center_median() - u.co
            pv -= d * pv.dot(d)
            if pv.length < 1e-12:
                ring = None
                break
            pv.normalize()
            if e1 is None:
                e1, e2 = pv, d.cross(pv)
            ang = math.atan2(pv.dot(e2), pv.dot(e1)) % (2 * math.pi)


            ring.append((ang, f, f.normal.dot(d.cross(pv)) < 0.0))
        if not ring or len(ring) % 2:
            continue
        ring.sort(key=lambda r: r[0])
        k = len(ring)
        got = [(ring[i][1], ring[(i + 1) % k][1]) for i in range(k) if ring[i][2]]
        if len(got) * 2 == k and len({id(f) for pr in got for f in pr}) == k:
            pairs.append((u.co.copy(), v.co.copy(), got))
    bmesh.ops.split_edges(bm, edges=bad)
    targetmap = {}

    def edge_at(face, a, b):
        for ed in face.edges:
            p, q = ed.verts
            if ((p.co - a).length < 1e-9 and (q.co - b).length < 1e-9):
                return p, q
            if ((p.co - b).length < 1e-9 and (q.co - a).length < 1e-9):
                return q, p
        return None

    for a, b, got in pairs:
        for fa, fb in got:
            ea, eb = edge_at(fa, a, b), edge_at(fb, a, b)
            if ea is None or eb is None:
                continue
            for x, y in zip(eb, ea):
                if x is not y:
                    while y in targetmap:
                        y = targetmap[y]
                    if x is not y:
                        targetmap[x] = y
    if targetmap:
        bmesh.ops.weld_verts(bm, targetmap=targetmap)


def _closed(bm):
    return all(e.is_manifold for e in bm.edges)


def _object_mesh_local(obj, source):
    """obj's evaluated mesh carried into source-local space (a new datablock)."""
    dg = bpy.context.evaluated_depsgraph_get()
    mesh = bpy.data.meshes.new_from_object(obj.evaluated_get(dg), preserve_all_data_layers=True, depsgraph=dg)
    mesh.transform(source.matrix_world.inverted() @ obj.matrix_world)
    return mesh


def _manifold_solver():
    try:
        return 'MANIFOLD' in bpy.types.BooleanModifier.bl_rna.properties['solver'].enum_items.keys()
    except Exception:
        return False


def _mesh_closed(mesh):
    bm = bmesh.new()
    bm.from_mesh(mesh)
    ok = bool(bm.faces) and _closed(bm)
    bm.free()
    return ok


def _close_holes(mesh):
    """Caps the open edges of a nearly closed mesh (a sliver face lost in a cut)."""
    bm = bmesh.new()
    bm.from_mesh(mesh)
    open_edges = [e for e in bm.edges if len(e.link_faces) < 2]
    if open_edges:
        bmesh.ops.holes_fill(bm, edges=open_edges, sides=0)
        if bm.calc_volume() < 0:
            bmesh.ops.reverse_faces(bm, faces=bm.faces[:])
        bm.to_mesh(mesh)
        mesh.update()
    bm.free()


def _boolean(context, mesh, source, operands, operation='DIFFERENCE', *, solver=None):
    """Boolean of `mesh` (source-local) with `operands` (objects). Uses the
    Manifold solver (Blender 4.5+) when every input is closed: its output is
    closed and 2-manifold. Otherwise Exact, and solids that touch along an
    edge are separated afterwards. Returns (new mesh, solver); faces that come
    from an operand go on the last slot of `mesh` (the inner material)."""
    if not operands:
        return mesh, 'NONE'
    if solver is None:
        solver = 'EXACT'
        if _manifold_solver() and _mesh_closed(mesh) and all(_mesh_closed(o.data) for o in operands):
            solver = 'MANIFOLD'
    slots = len(mesh.materials)
    tmp = bpy.data.objects.new('_pd_fill_bool', mesh)
    _work_link(tmp)
    tmp.matrix_world = source.matrix_world
    try:
        for op in operands:
            mod = tmp.modifiers.new('b', 'BOOLEAN')
            mod.operation = operation
            mod.solver = solver
            mod.object = _work_operand(op)
            if hasattr(mod, 'material_mode'):
                mod.material_mode = 'TRANSFER'
        dg = _work_depsgraph()
        out = bpy.data.meshes.new_from_object(tmp.evaluated_get(dg), preserve_all_data_layers=True, depsgraph=dg)
    finally:
        bpy.data.objects.remove(tmp, do_unlink=True)
    if len(out.materials) > slots:
        idx = np.empty(len(out.polygons), dtype=np.int32)
        out.polygons.foreach_get('material_index', idx)
        idx[idx >= slots] = slots - 1
        out.polygons.foreach_set('material_index', idx)
        while len(out.materials) > slots:
            out.materials.pop()
    if mesh.users == 0:
        bpy.data.meshes.remove(mesh)
    return out, solver


def _temp_object(context, name, mesh, source):
    obj = bpy.data.objects.new(name, mesh)
    _work_link(obj)
    obj.matrix_world = source.matrix_world
    return obj


def _grown(context, obj, dist):
    """A temporary copy of `obj` grown by `dist` along its vertex normals.
    A part that sits flush in its cut (a whole brick in its socket) shares
    faces with the damage result, and a Boolean may then leave it in."""
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    for v in bm.verts:
        v.co += v.normal * dist
    mesh = bpy.data.meshes.new('_pd_grown')
    bm.to_mesh(mesh)
    bm.free()
    tmp = bpy.data.objects.new('_pd_grown', mesh)
    _work_link(tmp)
    tmp.matrix_world = obj.matrix_world
    tmp.hide_render = True
    return tmp


def _inner_body(context, source, depth):
    """The source shrunk `depth` inward (closed, source-local), or None when
    the source is too thin for that."""
    dims = sorted(source.dimensions)
    scale = max(source.matrix_world.to_scale())
    if depth * scale * 2.2 >= dims[0]:
        return None
    bm = bmesh.new()
    bm.from_mesh(source.data)
    if not _closed(bm):
        bm.free()
        return None
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
    moves = [(v, v.normal * (depth * v.calc_shell_factor())) for v in bm.verts]
    for v, d in moves:
        v.co -= d
    mesh = bpy.data.meshes.new('_pd_inner')
    bm.to_mesh(mesh)
    bm.free()
    return _temp_object(context, '_pd_inner', mesh, source)


def _outputs_inside(source):
    """The objects that sit inside the cut: laths / framing, bricks and mortar, rebar."""
    uid = _uid(source)
    stamps = ('project_damage_lath_of', 'project_damage_bricks_of', 'project_damage_rebar_of',
              'project_damage_bfrag_of',
              'project_damage_pipes_of',
              'project_damage_cables_of')
    return [o for o in bpy.data.objects if uid and o.type == 'MESH' and len(o.data.polygons)
            and any(o.get(s) == uid for s in stamps)]


def _auto_depth(context, source, module, s):
    """Fill depth in Blender units, or None for the whole cut."""
    p = source.project_damage
    if s.fill_depth == 'WHOLE':
        return None
    if s.fill_depth == 'SURFACE':
        return _cm(context, source, s.surface_depth)
    if module == 'lath':

        return _cm(context, source, min(p.lath_plaster_min, p.lath_plaster_max))
    return None





def _output_boxes(source, objects):
    """Bounding boxes (source-local) of the loose parts of `objects`."""
    boxes = []
    inv = source.matrix_world.inverted()
    for obj in objects:
        bm = bmesh.new()
        bm.from_mesh(obj.data)
        bm.transform(inv @ obj.matrix_world)
        for part in _islands(bm):
            co = np.array([v.co[:] for i in part for v in bm.faces[i].verts])
            boxes.append((co.min(axis=0), co.max(axis=0)))
        bm.free()
    return boxes


@_operation
def fill_damage(context, source, module, all_damage=False):
    """One closed object per damaged area, filling the hole exactly: the
    source minus the damage result and everything that sits inside the cut
    (laths, studs, bricks, mortar, rebar). Returns (objects, reports)."""
    s = settings(source)
    p = source.project_damage
    reports = []
    reason=fill_block_reason(source)
    if reason:return [],[({'WARNING'},reason)]
    if not module_enabled(p, module):
        return [], [({'ERROR'}, 'Fill Damage: enable %s on this object first' % LABELS[module])]
    if module in ADDITIVE:
        return _detach(context, source, module, s)
    if p.all_off:
        return [], [({'ERROR'}, 'Fill Damage: the damage is switched off (All Off)')]


    result = build.result_of(source)
    if result is None or result.type != 'MESH' or not len(result.data.polygons):
        return [], [({'ERROR'}, 'Fill Damage: press Update to build the damage first')]
    from . import live as _live
    if source.get('project_damage_uid') in _live._dirty:
        reports.append(({'WARNING'}, 'Fill Damage: the damage has changes waiting for Live Update; '
                         'the fill follows the damage as it is now'))

    from . import fill_surface
    recovered = fill_surface.try_fill(context, source, module, all_damage=all_damage)
    if recovered is not None:
        if not recovered[0] and not all_damage:
            from . import fill_exact
            recovered = fill_exact.recover(context, source, module, 'Fill Damage') or recovered
        return recovered

    previous_fills=fills_of(source, None if all_damage else module)
    from .params import unit_factor
    coll = _collection(context, source, FILL_COLLECTION, '_DamageFill')
    temps = []
    try:
        inside = _outputs_inside(source)
        grow = 0.01 * _gscale(p) / unit_factor(context)


        temps.append(_grown(context, result, grow / max(1e-9, min(result.matrix_world.to_scale()))))
        operands = [temps[-1]]
        for obj in inside:
            temps.append(_grown(context, obj, grow / max(1e-9, min(obj.matrix_world.to_scale()))))
            operands.append(temps[-1])
        depth = None if all_damage else _auto_depth(context, source, module, s)
        if depth is not None:
            body = _inner_body(context, source, depth)
            if body is None:
                reports.append(({'WARNING'}, 'Fill Damage: the object is too thin for the fill depth; filled the whole cut'))
            else:
                temps.append(body)
                operands.append(body)
        base = _fill_base(context, source)
        if not len(base.materials):
            base.materials.append(None)
        base.materials.append(_inner_material(source))


        for operand in operands:
            me=operand.data
            for name in ('_pd_fill_outer','_pd_fill_outer_normal'):
                attr=me.attributes.get(name)
                if attr is not None:me.attributes.remove(attr)
            me.attributes.new('_pd_fill_outer','INT','FACE')
            me.attributes.new('_pd_fill_outer_normal','FLOAT_VECTOR','CORNER')
        fill, solver = _boolean(context, base, source, operands)
    finally:
        for obj in temps:
            _remove(obj)


    from .params import unit_factor
    factor = unit_factor(context)
    g = _gscale(p)
    min_vol = max(1e-9, getattr(p, 'debris_min_volume', 0.05) * g ** 3) / factor ** 3
    min_thick = 0.0 if s.fill_cracks else getattr(p, 'debris_min_thickness', 0.3) * g / factor






    if module == 'dbevel':

        min_thick = 0.0
        min_vol = min(min_vol, 1e-10)
    hit_parts = module == 'hit' and min_thick > 0.0
    if hit_parts:
        min_thick = min(min_thick, 0.3 * max(0.0, getattr(p, 'hit_layer_thickness', 0.5)) * g / factor)
    biggest = 0.0
    if module == 'bullet':

        min_vol *= 0.2
        min_thick *= 0.5
    bm = bmesh.new()
    bm.from_mesh(fill)


    if solver != 'MANIFOLD':
        _triangulate_warped_faces(bm)
        _split_pinches(bm)
    wanted = (lambda part: True) if all_damage else _effect_parts(bm, module)


    flush = _output_boxes(source, inside)
    tol = 0.02 * g / factor
    out = []
    dropped = 0
    dropped_thin = 0
    parts = sorted(_solid_islands(bm), key=len, reverse=True)
    if hit_parts:


        def hit_volume(part):
            if not wanted(part):return -1.0
            piece=_bm_part(bm,part)
            try:return abs(piece.calc_volume()) if _closed(piece) else -1.0
            finally:piece.free()
        parts.sort(key=hit_volume,reverse=True)
    for part in parts:
        if not wanted(part):
            continue
        pb = _bm_part(bm, part)
        vol = abs(pb.calc_volume())
        area = sum(f.calc_area() for f in pb.faces)
        co = np.array([v.co[:] for v in pb.verts])
        lo, hi = co.min(axis=0), co.max(axis=0)
        same = any(np.abs(lo - a).max() < tol and np.abs(hi - b).max() < tol for a, b in flush)
        thin = 2.0 * vol / max(area, 1e-12) < min_thick
        if hit_parts and thin and vol >= 0.1 * biggest:
            thin = False
        if same or vol < min_vol or thin or not _closed(pb):
            dropped += 1
            if thin and not same and vol >= min_vol:
                dropped_thin += 1
            pb.free()
            continue
        biggest = max(biggest, vol)
        mesh = bpy.data.meshes.new('fill')
        pb.to_mesh(mesh)
        pb.free()
        for m in fill.materials:
            mesh.materials.append(m)
        obj, vol = _make_object('%s_Fill_%03d' % (source.name, len(out) + 1), mesh, coll, source)
        mesh.name = obj.name
        obj[FILL_STAMP] = _uid(source)
        ownership.bind(source, obj)
        obj[FILL_KIND] = module
        out.append(obj)
    bm.free()
    bpy.data.meshes.remove(fill)
    if not out:
        if not all_damage and not dropped_thin:
            from . import fill_exact
            recovered = fill_exact.recover(context, source, module, 'Fill Damage')
            if recovered is not None:
                return recovered
        if dropped_thin:
            return [], reports + [({'WARNING'}, 'Fill Damage: the damage is thinner than Damage Debris Min '
                                   'Thickness (%d part(s) dropped); lower Min Thickness' % dropped_thin)]
        return [], reports + [({'WARNING'}, 'Fill Damage: this damage removes nothing to fill')]
    for obj in previous_fills:_remove(obj)
    reports.append(({'INFO'}, 'Fill Damage: %d fill objects in %s' % (len(out), coll.name)))
    return out, reports


def _effect_parts(bm, module, any_face=False):
    """Predicate on island face lists: is this effect the one with the most
    faces in the island (a chip that runs into a crater belongs to one of
    them only)? True for all when the module has no stamp or the mesh carries
    none (an older core)."""
    ids = EFFECT_IDS.get(module)
    from .effect_groups import ATTRIBUTE
    layer = bm.faces.layers.int.get(ATTRIBUTE)
    if not ids or layer is None:
        return lambda part: True
    bm.faces.ensure_lookup_table()
    values = np.fromiter((f[layer] for f in bm.faces), dtype=np.int32, count=len(bm.faces))
    if not np.any(values > 0):
        return lambda part: True

    def wanted(part):
        v = values[np.asarray(part, dtype=np.int64)]
        v = v[v > 0]
        if not len(v):
            return False
        if any_face:
            return bool(np.isin(v, ids).any())
        keys, counts = np.unique(v, return_counts=True)
        return int(keys[np.argmax(counts)]) in ids
    wanted.kinds = lambda part: sorted({m for m in ADDITIVE for i in EFFECT_IDS[m]
                                        if np.isin(values[np.asarray(part, dtype=np.int64)], [i]).any()})
    return wanted


def _detach(context, source, module, s):
    """Bead, Corner Piles, Deposits: one closed object per lump the generator
    added, the result minus the source (separate Deposits: the deposits object
    itself). The faces that touched the wall get the inner material."""
    p = source.project_damage
    reports = []
    result = build.result_of(source)
    separate = None
    if module == 'deposits' and getattr(p, 'deposit_separate', False):
        separate = build.deposits_of(source)
        if separate is None or separate.type != 'MESH' or not len(separate.data.polygons):
            return [], [({'ERROR'}, 'Detach: press Update to build the deposits first')]
    elif result is None or result.type != 'MESH' or not len(result.data.polygons):
        return [], [({'ERROR'}, 'Detach: press Update to build the damage first')]
    previous = list(fills_of(source, module))
    coll = _collection(context, source, FILL_COLLECTION, '_DamageFill')
    origin = separate if separate is not None else result
    mesh = _object_mesh_local(origin, source)
    mats = [m for m in origin.data.materials] or [None]
    mesh.materials.clear()
    for m in mats:
        mesh.materials.append(m)
    mesh.materials.append(_inner_material(source))
    if separate is None:
        added, solver = _boolean(context, mesh, source, [source])
    else:
        added, solver = mesh, 'NONE'
    from .params import unit_factor
    factor = unit_factor(context)
    min_vol = max(1e-9, getattr(p, 'debris_min_volume', 0.05) * _gscale(p) ** 3) / factor ** 3
    bm = bmesh.new()
    bm.from_mesh(added)
    if solver == 'EXACT':
        _split_pinches(bm)

    wanted = _effect_parts(bm, module, any_face=True) if separate is None else (lambda part: True)
    kinds_of = getattr(wanted, 'kinds', lambda part: [module])
    out, dropped = [], 0
    for part in sorted(_islands(bm), key=len, reverse=True):
        if not wanted(part):
            continue
        pb = _bm_part(bm, part)
        if abs(pb.calc_volume()) < min_vol:
            dropped += 1
            pb.free()
            continue
        piece = bpy.data.meshes.new('lump')
        pb.to_mesh(piece)
        pb.free()
        for m in added.materials:
            piece.materials.append(m)
        if not _mesh_closed(piece):
            _close_holes(piece)
        obj, _ = _make_object('%s_%s_%03d' % (source.name, LABELS[module].replace(' ', ''), len(out) + 1),
                              piece, coll, source)
        piece.name = obj.name
        obj[FILL_STAMP] = _uid(source)
        ownership.bind(source, obj)
        obj[FILL_KIND] = module
        obj['project_damage_fill_kinds'] = sorted(set(kinds_of(part)) | {module})
        out.append(obj)
    bm.free()
    bpy.data.meshes.remove(added)
    if not out:
        return [], [({'WARNING'}, 'Detach: %s added nothing to this object' % LABELS[module])]

    for obj in previous:
        _remove(obj)
    reports.append(({'INFO'}, 'Detach: %d %s objects in %s' % (len(out), LABELS[module], coll.name)))
    joined = sorted({k for o in out for k in o['project_damage_fill_kinds']} - {module})
    if joined:
        reports.append(({'INFO'}, 'Detach: %s grown into the %s came off with it'
                         % (' and '.join(LABELS[k] for k in joined), LABELS[module])))
    return out, reports


def remove_from_wall(context, source, modules):
    """Switch the generators off and update the wall, so the detached lumps
    are not drawn twice."""
    props = [ENABLE_PROP[m] for m in modules if m in ENABLE_PROP
             and getattr(source.project_damage, ENABLE_PROP[m], False)]
    if not props:
        return []
    for prop in props:
        setattr(source.project_damage, prop, False)
    from . import sidebar
    sidebar.sync(source)
    result, reports = build.rebuild_object(context, source)
    return list(reports or [])





def _front_area(obj):
    """Area of the faces that carry a source material (not the inner slot)."""
    me = obj.data
    inner = len(me.materials) - 1
    area = np.empty(len(me.polygons))
    me.polygons.foreach_get('area', area)
    idx = np.empty(len(me.polygons), dtype=np.int32)
    me.polygons.foreach_get('material_index', idx)
    sx = max(obj.matrix_world.to_scale())
    return float(area[idx != inner].sum()) * sx * sx


def _piece_count(context, source, obj, size_cm):
    size = _cm(context, source, size_cm)
    n = _front_area(obj) / max(size * size, 1e-12)
    return max(1, min(settings(source).amount_budget, int(round(n))))

def _allocate_budget(requested,budget):
    """Allocate one shared operation budget before expensive fracture calls."""
    requested=[max(1,int(n)) for n in requested];budget=max(1,int(budget))
    if sum(requested)<=budget:return requested
    if len(requested)>=budget:return [1]*len(requested)
    available=budget-len(requested);weights=np.asarray(requested,dtype=float)-1
    if weights.sum()==0:return [1]*len(requested)
    quota=weights/weights.sum()*available;extra=np.floor(quota).astype(int)
    for i in np.argsort(-(quota-extra),kind='stable')[:available-int(extra.sum())]:extra[i]+=1
    return (extra+1).tolist()

def _enforce_piece_budget(context,pieces,budget):
    """Keep all geometry when disconnected regions or drawn cuts exceed it."""
    if len(pieces)<=budget:return pieces
    combined=pieces[budget-1:];active=combined[0]
    fills=sorted({name for o in combined for name in o.get('project_damage_piece_fills',[o.get('project_damage_piece_fill','')]) if name})
    with context.temp_override(active_object=active,object=active,selected_objects=combined,selected_editable_objects=combined):
        bpy.ops.object.join()
    active['project_damage_piece_fills']=fills
    return pieces[:budget-1]+[active]





class _Lattice:
    """The brick lattice of brick_damage.cpp (Lattice / Locate), in core cm."""
    def __init__(self, b):
        bs = max(1e-4, b.brick_scale)
        self.bond = max(0, min(int(b.bond), 4))
        self.m = max(0.0, b.mortar) * bs
        self.Lp = max(1e-3, b.brick_length) * bs + self.m
        self.Hp = 0.5 * self.Lp
        self.Pv = max(1e-3, b.brick_height) * bs + self.m
        self.W = max(1e-3, b.brick_width) * bs
        self.leaf = self.W + self.m
        self.run = min(1.0, max(0.0, b.bond_offset))
        self.offU = 0.0
        self.offV = 0.0

    def course(self, v):
        return np.floor((np.asarray(v) - self.offV) / self.Pv).astype(np.int64)

    def course_v(self, k):
        return self.offV + k * self.Pv

    def locate(self, w, k, u):
        """(a, len) of the cell holding U in course k, leaf w (vectorised)."""
        k = np.asarray(k, dtype=np.int64)
        u = np.asarray(u, dtype=np.float64)
        odd = (k & 1) != 0
        o = np.full(u.shape, self.offU + w * 0.37 * self.Lp)
        pitch = np.full(u.shape, self.Lp)
        if self.bond == 2:
            pitch = np.where(odd, self.Hp, pitch)
            o = np.where(odd, o - 0.5 * self.Hp, o)
        elif self.bond == 3:
            pf = self.Lp + self.Hp
            o = np.where(odd, o + 0.25 * self.Lp - self.Lp, o)
            t = u - o
            per = np.floor(t / pf)
            r = t - per * pf
            a = np.where(r < self.Lp, o + per * pf, o + per * pf + self.Lp)
            ln = np.where(r < self.Lp, self.Lp, self.Hp)
            return a, ln
        elif self.bond == 4:
            pitch = np.full(u.shape, self.Hp)
            o = np.where(odd, o + 0.5 * self.Hp, o)
        elif self.bond == 0:
            o = np.where(odd, o + self.run * self.Lp, o)
        jj = np.floor((u - o) / pitch)
        return o + jj * pitch, pitch


def _frame(up, n):
    """Frame() of brick_damage.cpp: t along the courses, b up the wall."""
    up = Vector(up)
    up = up.normalized() if up.length > 1e-9 else Vector((0, 0, 1))
    h = up.cross(n)
    if h.length < 0.3:
        x = Vector((1, 0, 0)) if abs(up.x) < 0.9 else Vector((0, 1, 0))
        h = x - n * x.dot(n)
    t = h - n * h.dot(n)
    t = t.normalized() if t.length > 1e-9 else n.orthogonal().normalized()
    return t, n.cross(t)


def _brick_samples(source, factor):
    """[(centre, verts)] of every separate brick, source-local cm."""
    from . import brick_output
    obj = brick_output.bricks_of(source)
    if obj is None:
        return []
    me = obj.data
    ids = me.attributes.get('project_damage_brick_id')
    if ids is None:
        return []
    fid = np.empty(len(me.polygons), dtype=np.int32)
    ids.data.foreach_get('value', fid)
    co = np.empty(len(me.vertices) * 3)
    me.vertices.foreach_get('co', co)
    mat = np.array(source.matrix_world.inverted() @ obj.matrix_world)
    co = co.reshape(-1, 3) @ mat[:3, :3].T + mat[:3, 3]
    co *= factor
    starts = np.empty(len(me.polygons), dtype=np.int64)
    totals = np.empty(len(me.polygons), dtype=np.int64)
    me.polygons.foreach_get('loop_start', starts)
    me.polygons.foreach_get('loop_total', totals)
    loops = np.empty(len(me.loops), dtype=np.int64)
    me.loops.foreach_get('vertex_index', loops)
    face_of_loop = np.repeat(np.arange(len(me.polygons)), totals)
    lid = fid[face_of_loop]
    out = []
    for b in np.unique(lid[lid >= 0]):
        verts = co[np.unique(loops[lid == b])]
        out.append(verts)
    return out


def _fit_offsets(L, t, b, bricks):
    """offU / offV of the lattice, fitted to the bricks that are there. A
    snapped brick keeps one true end, so each brick scores by its better end."""
    if not bricks:
        return 0
    T, B = np.array(t), np.array(b)
    u0 = np.array([(v @ T).min() for v in bricks])
    u1 = np.array([(v @ T).max() for v in bricks])
    v0 = np.array([(v @ B).min() for v in bricks])
    v1 = np.array([(v @ B).max() for v in bricks])

    ph = np.concatenate([v0 - 0.5 * L.m, v1 + 0.5 * L.m]) / L.Pv * 2 * math.pi
    L.offV = (math.atan2(np.sin(ph).mean(), np.cos(ph).mean()) / (2 * math.pi)) % 1.0 * L.Pv
    k = L.course(0.5 * (v0 + v1))
    uc = 0.5 * (u0 + u1)
    best, best_err = 0.0, None
    for cand in np.linspace(0.0, L.Lp, 521)[:-1]:
        L.offU = cand
        a, ln = L.locate(0, k, uc)
        err = np.median(np.minimum(np.abs(u0 - 0.5 * L.m - a), np.abs(u1 + 0.5 * L.m - a - ln)))
        if best_err is None or err < best_err:
            best, best_err = cand, err
    L.offU = best
    L.fit_error = float(best_err)
    return len(bricks)


class _BoxCutter:
    """Intersects one mesh with a series of boxes (Boolean modifier against a
    box object whose eight corners are moved for each cut). Exact solver: a
    cell face can come close to a fill face (a brick front, the wall
    surface), which the Manifold solver's tolerance does not survive."""
    def __init__(self, context, mesh, matrix):
        self.context = context
        self.slots = len(mesh.materials)
        box = bmesh.new()
        bmesh.ops.create_cube(box, size=2.0)
        bmesh.ops.recalc_face_normals(box, faces=box.faces[:])
        self.signs = [tuple(1 if c > 0 else 0 for c in v.co) for v in box.verts]
        self.faces = [[v.index for v in f.verts] for f in box.faces]
        self._cube = ([v.co[:] for v in box.verts], self.faces)
        bme = bpy.data.meshes.new('_pd_cell')
        box.to_mesh(bme)
        box.free()
        self.box = bpy.data.objects.new('_pd_cell', bme)
        self.obj = bpy.data.objects.new('_pd_cut', mesh)
        for o in (self.box, self.obj):
            _work_link(o)
            o.matrix_world = matrix
            o.hide_render = True
        self.box.display_type = 'WIRE'
        mod = self.obj.modifiers.new('cell', 'BOOLEAN')
        mod.operation = 'INTERSECT'
        mod.solver = 'EXACT'
        mod.object = self.box
        if hasattr(mod, 'material_mode'):
            mod.material_mode = 'TRANSFER'

    def cut(self, corner):
        """corner(sx, sy, sz) -> Vector for sx, sy, sz in {0, 1}; a new mesh or None."""
        co = []
        for sgn in self.signs:
            co.extend(corner(*sgn))
        self.box.data.vertices.foreach_set('co', co)
        self.box.data.update()
        return self._evaluate()

    def cut_checked(self, corner, limit):
        """cut() with a check: a cell is closed and holds no more than `limit`
        (the box or the whole mesh, whichever is less). Exact can fail on a box
        face flush with a mesh face; Manifold then takes the cut."""
        mod = self.obj.modifiers[0]
        first = mod.solver
        solvers = [first] + (['MANIFOLD'] if _manifold_solver() and first != 'MANIFOLD' else [])
        try:
            for solver in solvers:
                mod.solver = solver
                out = self.cut(corner)
                if out is not None and _mesh_closed(out) and _mass(out)[0] <= limit * 1.001:
                    return out
                if out is not None:
                    bpy.data.meshes.remove(out)
        finally:
            mod.solver = first
        return None

    def cut_with(self, bm):
        """Intersects with a closed bmesh (the box object's local space); a new
        mesh or None. The box gets the cell's geometry for this cut only."""
        me = self.box.data
        bm.to_mesh(me)
        me.update()
        try:
            return self._evaluate()
        finally:
            me.clear_geometry()
            me.from_pydata(self._cube[0], [], self._cube[1])
            me.update()

    def _evaluate(self):
        dg = _work_depsgraph()
        out = bpy.data.meshes.new_from_object(self.obj.evaluated_get(dg), preserve_all_data_layers=True, depsgraph=dg)
        if not len(out.polygons):
            bpy.data.meshes.remove(out)
            return None
        if len(out.materials) > self.slots:
            idx = np.empty(len(out.polygons), dtype=np.int32)
            out.polygons.foreach_get('material_index', idx)
            idx[idx >= self.slots] = self.slots - 1
            out.polygons.foreach_set('material_index', idx)
            while len(out.materials) > self.slots:
                out.materials.pop()
        return out

    def free(self):
        mesh = self.obj.data
        _remove(self.box)
        bpy.data.objects.remove(self.obj, do_unlink=True)
        if mesh.users == 0:
            bpy.data.meshes.remove(mesh)


def _voronoi_cells(lo, hi, points):
    """Closed convex Voronoi cells of `points` clipped to the box lo..hi
    (bmesh per point; None for a cell that comes out empty)."""
    cells = []
    for i, p in enumerate(points):
        bm = bmesh.new()
        bmesh.ops.create_cube(bm, size=1.0)
        for v in bm.verts:
            v.co = Vector([lo[a] if v.co[a] < 0 else hi[a] for a in range(3)])
        order = np.argsort(np.linalg.norm(points - p, axis=1))
        for j in order[1:]:
            q = points[j]
            r = max((Vector(v.co) - Vector(p)).length for v in bm.verts) if bm.verts else 0.0
            if np.linalg.norm(q - p) > 2.0 * r:
                break
            mid = Vector(0.5 * (p + q))
            nrm = Vector(q - p).normalized()
            bmesh.ops.bisect_plane(bm, geom=bm.verts[:] + bm.edges[:] + bm.faces[:],
                                   plane_co=mid, plane_no=nrm, clear_outer=True, dist=1e-7)

            pts = [v.co.copy() for v in bm.verts]
            bm.free()
            bm = bmesh.new()
            for c in pts:
                bm.verts.new(c)
            if len(pts) < 4:
                break
            hull = bmesh.ops.convex_hull(bm, input=bm.verts[:])
            loose = list({v for v in hull['geom_interior'] + hull['geom_unused'] if isinstance(v, bmesh.types.BMVert)})
            bmesh.ops.delete(bm, geom=loose, context='VERTS')
            if len(bm.faces) < 4:
                break
        if len(bm.faces) < 4:
            bm.free()
            cells.append(None)
            continue
        bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
        cells.append(bm)
    return cells


def _voronoi_fracture(context, source, mesh, count, seed, solver=None, points=None):
    """Cuts a closed source-local mesh into `count` Voronoi pieces (or one per
    point of `points`) with a Boolean per cell, so every piece is closed.
    Returns a list of meshes."""
    co = np.empty(len(mesh.vertices) * 3)
    mesh.vertices.foreach_get('co', co)
    co = co.reshape(-1, 3)
    lo, hi = co.min(axis=0), co.max(axis=0)
    pad = 0.01 * max(float((hi - lo).max()), 1e-6)
    lo, hi = lo - pad, hi + pad
    if points is None:
        rng = np.random.default_rng(seed)
        points = lo + rng.random((max(2, count), 3)) * (hi - lo)
    else:
        lo = np.minimum(lo, points.min(axis=0) - pad)
        hi = np.maximum(hi, points.max(axis=0) + pad)
    closed = _mesh_closed(mesh)
    cutter = _BoxCutter(context, mesh, source.matrix_world)
    if solver is None and closed and _manifold_solver():
        solver = 'MANIFOLD'
    if solver == 'MANIFOLD':



        cutter.obj.modifiers[0].solver = 'MANIFOLD'
    out = []
    try:
        for bm in _voronoi_cells(lo, hi, points):
            if bm is None:
                continue
            piece = cutter.cut_with(bm)
            bm.free()
            if piece is not None:
                out.append(piece)
    finally:
        cutter.free()
    return out


def _impact_points(mesh, centres, count, center, seed):
    """`count` Voronoi sites on the vertices of `mesh`, crowding round the
    impact `centres` (source-local) by `center` 0..1: shards small at the
    impact and larger further out, like the glass Fill Hole."""
    co = np.empty(len(mesh.vertices) * 3)
    mesh.vertices.foreach_get('co', co)
    co = co.reshape(-1, 3)
    rng = np.random.default_rng(seed)
    if not len(centres):
        centres = [co.mean(axis=0)]
    d = np.min([np.linalg.norm(co - np.asarray(c), axis=1) for c in centres], axis=0)
    reach = max(float(d.max()), 1e-9)

    near = 1.0 / (d / reach + 0.02) ** 2
    w = (1.0 - center) / len(co) + center * near / near.sum()
    w = w / w.sum()
    pick = rng.choice(len(co), size=min(max(2, count), len(co)), replace=False, p=w)
    jitter = (rng.random((len(pick), 3)) - 0.5) * 1e-4 * reach
    return co[pick] + jitter


def _brick_fracture(context, source, fill, s, coll, hint=None):
    """Breaks a Brick Damage fill along the wall's brick lattice (the same
    lattice brick_damage.cpp lays, fitted to the bricks that are there): one
    piece per brick, the joints with the bricks or as separate pieces, a few
    bricks snapped in two. The render (stucco) in front of the bricks breaks
    into concrete plates."""
    from .params import fill_params, unit_factor
    from . import core_loader
    factor = unit_factor(context)
    core = core_loader.load()
    bp = fill_params(source.project_damage, core, obj=source).brick
    L = _Lattice(bp)
    me = fill.data
    inner = len(me.materials) - 1
    to_local = source.matrix_world.inverted() @ fill.matrix_world

    co = np.empty(len(me.vertices) * 3)
    me.vertices.foreach_get('co', co)
    tl = np.array(to_local)
    verts = (co.reshape(-1, 3) @ tl[:3, :3].T + tl[:3, 3]) * factor

    nrm = np.empty(len(me.polygons) * 3)
    me.polygons.foreach_get('normal', nrm)
    nrm = nrm.reshape(-1, 3) @ np.linalg.inv(tl[:3, :3])
    area = np.empty(len(me.polygons))
    me.polygons.foreach_get('area', area)
    idx = np.empty(len(me.polygons), dtype=np.int32)
    me.polygons.foreach_get('material_index', idx)
    front = idx != inner
    wall_area = float(area[front].sum())
    if not front.any():
        front[:] = True



    fn, fa = nrm[front], area[front]
    fn = fn / np.maximum(np.linalg.norm(fn, axis=1), 1e-12)[:, None]
    best_n, best_a = None, -1.0
    for i in np.argsort(-fa)[:64]:
        a = fa[(fn @ fn[i]) > 0.95].sum()
        if a > best_a * 1.001:
            best_n, best_a = fn[i], a
    little = wall_area < 0.2 * area.sum()
    if little and hint is not None:


        best_n = np.array(hint)
    samples = _brick_samples(source, factor)
    lo, hi = verts.min(axis=0) - L.Lp, verts.max(axis=0) + L.Lp
    bricks = [v for v in samples if ((v.mean(axis=0) > lo) & (v.mean(axis=0) < hi)).all()]
    if len(bricks) < 2:
        bricks = samples
    n = Vector(best_n).normalized()
    N = np.array(n)
    s0 = float((verts @ N).max())
    if little:
        sco = np.empty(len(source.data.vertices) * 3)
        source.data.vertices.foreach_get('co', sco)
        s0 = float((sco.reshape(-1, 3) @ N).max()) * factor
    fronts = [s0 - float((v @ N).max()) for v in bricks]
    render = max(0.0, float(np.median(fronts))) if fronts else max(0.0, bp.render_max)
    t, b = _frame((bp.up_x, bp.up_y, bp.up_z), n)
    T, B, N = np.array(t), np.array(b), np.array(n)
    _fit_offsets(L, t, b, bricks)
    fill['project_damage_fill_normal'] = tuple(n)
    if bpy.app.debug_value == 77:
        print('brick lattice: %d bricks, fit error %.3f cm, offU %.2f offV %.2f, render %.2f cm, n %s'
              % (len(bricks), getattr(L, 'fit_error', -1), L.offU, L.offV, render, tuple(round(x, 2) for x in n)))
    rng = random.Random(1000003 * (s.seed + 1) + len(verts))
    U, V, D = verts @ T, verts @ B, s0 - verts @ N
    pad = L.Lp
    pieces = []

    def corner_fn(u0, u1, v0, v1, d0, d1):
        def corner(sx, sy, sz):
            p = t * (u1 if sx else u0) + b * (v1 if sy else v0) + n * (s0 - (d0 if sz else d1))
            return p / factor
        return corner

    gs = _gscale(source.project_damage)


    split = render
    m2 = 0.5 * L.m if s.brick_mortar == 'SEPARATE' else 0.0
    leaves = max(1, int(math.ceil((D.max() - split) / L.leaf - 1e-6)))
    k0, k1 = int(L.course(V.min())), int(L.course(V.max()))
    dmax = D.max() + 1.0


    taken = set()
    for v in bricks:
        bu, bv = float((v @ T).mean()), float((v @ B).mean())
        bd = s0 - float((v @ N).mean())
        if abs(bd - (render + 0.5 * L.W)) < 0.25 * L.W:
            kk = int(L.course(bv))
            aa, _ = L.locate(0, np.array([kk]), np.array([bu]))
            taken.add((kk, round(float(aa[0]), 3)))



    umin, umax = U.min() - 1.0, U.max() + 1.0
    boxes = []
    if render > 0.05:
        vlo, vhi = V.min() - 1.0, V.max() + 1.0
        boxes.append((umin, umax, vlo, vhi, -1.0, split, 'plate'))
    for w in range(leaves):
        d0 = (split if render > 0.05 else -1.0) if w == 0 else split + w * L.leaf
        d1 = dmax if w == leaves - 1 else split + (w + 1) * L.leaf
        for k in range(k0, k1 + 1):
            v0, v1 = L.course_v(k) - m2, L.course_v(k + 1) - m2
            a, ln = L.locate(w, np.array([k]), np.array([U.min()]))
            a, ln = float(a[0]), float(ln[0])
            while a < U.max() + 1e-6:
                if w == 0 and (k, round(a, 3)) in taken:
                    boxes.append((a - m2, a + ln - m2, v0, v1, d0, d1, 'plate'))
                elif m2 > 0:



                    e = 0.15 * m2
                    boxes.append((a + m2 + e, a + ln - m2 - e, v0 + 2 * m2 + e, v1 - e, d0, d1, 'brick'))
                    boxes.append((a - m2 - e, a + ln - m2 - e, v0 - e, v0 + 2 * m2 + e, d0, d1, 'joint'))
                    boxes.append((a - m2 - e, a + m2 + e, v0 + 2 * m2 + e, v1 - e, d0, d1, 'joint'))
                else:
                    boxes.append((a - m2, a + ln - m2, v0, v1, d0, d1, 'brick'))
                a, ln = L.locate(w, np.array([k]), np.array([a + ln + 1e-6]))
                a, ln = float(a[0]), float(ln[0])

    base = me.copy()
    base.transform(to_local)
    fill_vol = _mass(base)[0]
    cutter = _BoxCutter(context, base, source.matrix_world)
    plates = []
    try:
        for u0, u1, cv0, cv1, d0, d1, kind in boxes:
            limit = min(fill_vol, abs((u1 - u0) * (cv1 - cv0) * (d1 - d0)) / factor ** 3)
            cell = cutter.cut_checked(corner_fn(u0, u1, cv0, cv1, d0, d1), limit)
            if cell is None:
                continue
            if kind == 'plate':
                plates.append(cell)
                continue
            if kind == 'brick' and s.brick_snap > 0 and rng.random() < s.brick_snap:

                um = 0.5 * (u0 + u1) + (rng.random() - .5) * .3 * (u1 - u0)
                tilt = (rng.random() - .5) * 0.8 * (cv1 - cv0)
                lean = (rng.random() - .5) * 0.6 * (cv1 - cv0)

                def half(left, um=um, tilt=tilt, lean=lean, u0=u0, u1=u1, cv0=cv0, cv1=cv1, d0=d0, d1=d1):
                    def corner(sx, sy, sz):
                        shift = (tilt if sy else -tilt) + (lean if sz else -lean)
                        uu = (u0 if not sx else um + shift) if left else (um + shift if not sx else u1)
                        return (t * uu + b * (cv1 if sy else cv0) + n * (s0 - (d0 if sz else d1))) / factor
                    return corner
                bpy.data.meshes.remove(cell)
                for left in (True, False):
                    h = cutter.cut_checked(half(left), limit)
                    if h is not None:
                        pieces.append(h)
                continue
            pieces.append(cell)
    finally:
        cutter.free()
    out = []

    min_vol = max(1e-3, getattr(source.project_damage, 'debris_min_volume', 0.05)) * _gscale(source.project_damage) ** 3 / factor ** 3
    sx = max(source.matrix_world.to_scale())

    def parts(meshes):
        """Loose parts (source-local meshes) of `meshes`, which are freed."""
        res = []
        for mesh in meshes:
            bm = bmesh.new()
            bm.from_mesh(mesh)
            _split_pinches(bm)
            for part in _islands(bm):
                q = _bm_part(bm, part)
                open_edges = [e for e in q.edges if len(e.link_faces) < 2]
                if open_edges:


                    bmesh.ops.holes_fill(q, edges=open_edges, sides=0)
                if q.calc_volume() < 0:
                    bmesh.ops.reverse_faces(q, faces=q.faces[:], flip_multires=True)
                if abs(q.calc_volume()) >= min_vol:
                    pm = bpy.data.meshes.new('piece')
                    q.to_mesh(pm)
                    for m in me.materials:
                        pm.materials.append(m)
                    res.append(pm)
                q.free()
            bm.free()
            bpy.data.meshes.remove(mesh)
        return res

    def emit(meshes, kind, label):
        for pm in meshes:
            obj, _ = _make_object('%s_%s_%03d' % (fill.name, label, len(out) + 1), pm, coll, source)
            pm.name = obj.name
            obj['project_damage_fill_piece_kind'] = kind
            out.append(obj)

    emit(parts(pieces), 'brick', 'Brick')


    size = _cm(context, source, s.piece_size)
    for i, pm in enumerate(parts(plates)):
        area = np.empty(len(pm.polygons))
        pm.polygons.foreach_get('area', area)
        idx = np.empty(len(pm.polygons), dtype=np.int32)
        pm.polygons.foreach_get('material_index', idx)
        front = float(area[idx != inner].sum()) * sx * sx
        count = max(1, min(600, int(round(front / max(size * size, 1e-12)))))
        vol = abs(_mass(pm)[0])
        if count > 1 and vol > 50 * min_vol:
            got = []
            for solver in (None, 'EXACT'):
                got = parts(_voronoi_fracture(context, source, pm.copy(), count, s.seed * 7919 + i, solver))
                total = sum(_mass(g)[0] for g in got)
                if got and abs(total - vol) <= 0.01 * vol and all(_mesh_closed(g) for g in got):
                    break
                for g in got:
                    bpy.data.meshes.remove(g)
                got = []
            if got:
                bpy.data.meshes.remove(pm)
            else:
                got = [pm]
            emit(got, 'render', 'Render')
        else:
            emit([pm], 'render', 'Render')
    return out


def _volume_points(mesh, count, seed):
    """`count` sites spread evenly through the inside of a closed mesh."""
    from mathutils.bvhtree import BVHTree
    bvh = BVHTree.FromPolygons([v.co.copy() for v in mesh.vertices], [tuple(p.vertices) for p in mesh.polygons])
    co = np.empty(len(mesh.vertices) * 3)
    mesh.vertices.foreach_get('co', co)
    co = co.reshape(-1, 3)
    lo, hi = co.min(axis=0), co.max(axis=0)
    rng = np.random.default_rng(seed)
    out = []
    for _ in range(60):
        for p in lo + rng.random((max(64, 4 * count), 3)) * (hi - lo):
            loc, nrm, _i, _d = bvh.find_nearest(Vector(p))
            if loc is not None and (loc - Vector(p)).dot(nrm) > 0.0:
                out.append(p)
                if len(out) >= count:
                    return np.array(out)
    if len(out) < 2:
        return co[rng.choice(len(co), size=min(max(2, count), len(co)), replace=False)]
    return np.array(out)


def _hit_fracture(context, source, fill, coll, count, center, seed, proximity=None,
                  even=False, label='Shard', kind='shard'):
    """Voronoi pieces of a fill, cut with a Boolean per cell straight on the
    fill object (no temporary copy of the source is built). Concrete Hit:
    shards small at the impacts (the empties of Impact Empties) and larger
    further out. even: concrete and lath fills, pieces spread evenly.
    Returns the piece objects."""
    from .params import unit_factor
    factor = unit_factor(context)
    to_local = source.matrix_world.inverted() @ fill.matrix_world
    mesh = fill.data.copy()
    mesh.transform(to_local)
    inv = source.matrix_world.inverted()
    centres = []
    if proximity is not None:
        centres = [np.array(inv @ o.matrix_world.translation) for o in proximity.all_objects if o.type == 'EMPTY']
    if even:
        points = _volume_points(mesh, count, seed)
    else:
        points = _impact_points(mesh, centres, count, center, seed)
    vol = _mass(mesh)[0]
    got = []
    for solver in (None, 'EXACT'):
        got = _voronoi_fracture(context, source, mesh.copy(), len(points), seed, solver, points=points)
        if got and abs(sum(_mass(g)[0] for g in got) - vol) <= 0.01 * vol and all(_mesh_closed(g) for g in got):
            break
        for g in got:
            bpy.data.meshes.remove(g)
        got = []
    bpy.data.meshes.remove(mesh)
    if not got:
        raise RuntimeError('the shards did not cut cleanly; try another Seed')
    min_vol = max(1e-3, getattr(source.project_damage, 'debris_min_volume', 0.05)) * _gscale(source.project_damage) ** 3 / factor ** 3
    out = []
    for g in got:
        bm = bmesh.new()
        bm.from_mesh(g)
        for part in _islands(bm):
            q = _bm_part(bm, part)
            if abs(q.calc_volume()) >= min_vol:
                pm = bpy.data.meshes.new('shard')
                q.to_mesh(pm)
                for m in fill.data.materials:
                    pm.materials.append(m)
                obj, _ = _make_object('%s_%s_%03d' % (fill.name, label, len(out) + 1), pm, coll, source)
                pm.name = obj.name
                obj['project_damage_fill_piece_kind'] = kind
                out.append(obj)
            q.free()
        bm.free()
        bpy.data.meshes.remove(g)
    return out


_CORE_MATERIAL = {'WOOD': 0, 'GLASS': 1, 'CONCRETE': 2, 'ROCK': 3, 'SOIL': 4}


def _core_fracture(context, source, fill, coll, material, count, seed, centres=None, center=0.0, grain='Z'):
    """Cell Fracture of the native core, run straight on the fill mesh (no
    object is built through the add-on): pieces with material-shaped break
    faces that tile the fill exactly. Returns the piece objects, or None when
    the core has no Cell Fracture."""
    from . import background_compute as bg, core_loader, fracture_uv, mesh_io
    from .params import unit_factor
    core = core_loader.load()
    if core is None or not hasattr(core.Params(), 'cellf'):
        return None
    factor = unit_factor(context)
    g = _gscale(source.project_damage)
    me = fill.data
    materials=list(me.materials)
    inner_mat=_inner_material(source)
    if inner_mat not in materials:materials.append(inner_mat)
    inner=materials.index(inner_mat)
    bm = bmesh.new()
    bm.from_mesh(me)
    bmesh.ops.triangulate(bm, faces=[f for f in bm.faces if len(f.verts) > 4])
    tri = bpy.data.meshes.new('_pd_cf_in')
    bm.to_mesh(tri)
    bm.free()
    try:
        points, polys = mesh_io.read_mesh(tri)
        layer = tri.uv_layers.active
        uvs = mesh_io.read_uvs(tri, layer) if layer is not None else None
        mat_idx = np.empty(len(tri.polygons), dtype=np.int32)
        tri.polygons.foreach_get('material_index', mat_idx)
        original_uv = fracture_uv.capture(tri, by_polygon=True)
        from . import material_update
        outer = material_update.face_values(tri, '_pd_fill_outer')
        original_inner = outer == 0 if outer is not None else material_update.face_values(tri, 'project_damage_physics_inner')
        if original_inner is None:
            original_inner = mat_idx == inner
    finally:
        bpy.data.meshes.remove(tri)
    points = points * factor
    params = core.Params()
    c = params.cellf
    c.enable = True
    c.material = _CORE_MATERIAL[material]
    c.pieces = max(1, min(settings(source).amount_budget,int(count)))
    c.max_pieces = max(c.pieces * 3, 60)
    c.seed = int(seed)
    c.gap = 0.0
    c.inner_uv_scale = 10.0 * g
    c.grain_x, c.grain_y, c.grain_z = {'X': (1, 0, 0), 'Y': (0, 1, 0), 'Z': (0, 0, 1)}[grain]
    if centres:
        inv = fill.matrix_world.inverted()
        prox = []
        for w in centres:
            q = core.CellProximity()
            q.x, q.y, q.z = (float(v) * factor for v in (inv @ w))
            q.radius, q.strength = 0.0, 1.0
            prox.append(q)
        c.proximity = prox
        c.prox_density = 1.0 + 19.0 * center
        c.prox_falloff = 1.0 + 3.0 * center
    paths=_layer_paths(context,source)['GUIDANCE']
    extra={}
    if paths and source.project_damage.cellf_guidance_amount>0:
        # Glass refines its own pattern in modifier guidance mode.
        c.stroke_mode = 4 if material == 'GLASS' else 3
        from .params import fill_params
        original=fill_params(source.project_damage,core,source).cellf
        for field in ('guidance_radius','guidance_depth','guidance_falloff','guidance_distance'):
            setattr(c,field,getattr(original,field))
        c.guidance_amount=min(source.project_damage.cellf_guidance_amount,max(0,settings(source).amount_budget-1))
        c.pieces=min(c.pieces,max(1,settings(source).amount_budget-c.guidance_amount))
        c.max_pieces=settings(source).amount_budget
        matrix=np.asarray(fill.matrix_world.inverted()@source.matrix_world)
        extra['cellf_strokes']=[np.ascontiguousarray((path@matrix[:3,:3].T+matrix[:3,3])*factor,np.float64) for path in paths]
    if uvs is not None:extra['uvs']=uvs
    try:
        # A knife or plane edit can reuse the same uncut automatic cells.
        request = bg.Request('Fracturing fill', core.build_worn, (points, polys, params), extra, core)
        out = bg.evaluate(request)
    except Exception as exc:
        raise RuntimeError('Cell Fracture: %s' % exc)
    pts = np.asarray(out.get('cellf_points', ()), dtype=np.float64).reshape(-1, 3) / factor
    fp = np.asarray(out.get('cellf_polys', ()), dtype=np.int64).reshape(-1, 4)
    piece = np.asarray(out.get('cellf_piece', ()), dtype=np.int64)
    base = np.asarray(out.get('cellf_base_poly', ()), dtype=np.int64)
    fuv = np.asarray(out.get('cellf_uvs', ()), dtype=np.float64).reshape(-1, 4, 2)
    if not len(fp):
        raise RuntimeError('Cell Fracture made no pieces')
    to_local = source.matrix_world.inverted() @ fill.matrix_world
    min_vol = max(1e-3, getattr(source.project_damage, 'debris_min_volume', 0.05)) * g ** 3 / factor ** 3
    objs = []
    for k in np.unique(piece):
        sel = np.flatnonzero(piece == k)
        rows = fp[sel]
        is_tri = rows[:, 2] == rows[:, 3]
        used = np.unique(np.concatenate([rows[:, :3].ravel(), rows[~is_tri, 3]]))
        remap = np.full(len(pts), -1, dtype=np.int64)
        remap[used] = np.arange(len(used))
        faces = [tuple(remap[r[:3]]) if t else tuple(remap[r]) for r, t in zip(rows, is_tri)]
        pm = bpy.data.meshes.new('cellf')
        pm.from_pydata([tuple(v) for v in pts[used]], [], faces)
        for m in materials:
            pm.materials.append(m)
        b = base[sel] if len(base) == len(fp) else np.full(len(sel), -1)
        idx = np.where((b >= 0) & (b < len(mat_idx)), mat_idx[np.clip(b, 0, len(mat_idx) - 1)], inner)
        pm.polygons.foreach_set('material_index', idx.astype(np.int32))
        pm.update()
        fracture_uv.transfer(pm, b, [original_uv])
        valid = (b >= 0) & (b < len(mat_idx))
        mask = ~valid
        mask[valid] = original_inner[b[valid]]
        pm.attributes.new('project_damage_physics_inner', 'BOOLEAN', 'FACE').data.foreach_set('value', mask)
        pm.transform(to_local)
        pm.update()
        if not _mesh_closed(pm):
            _close_holes(pm)
        if _mass(pm)[0] < min_vol:
            bpy.data.meshes.remove(pm)
            continue
        obj, _ = _make_object('%s_%s_%03d' % (fill.name, material.title(), len(objs) + 1), pm, coll, source)
        if len(fuv) == len(fp) and len(pm.polygons) == len(sel):
            loops = np.concatenate([fuv[i, :3] if t else fuv[i] for i, t in zip(sel, is_tri)])
            fracture_uv.project(obj, mask, native=loops)
        pm.name = obj.name
        obj['project_damage_fill_piece_kind'] = material.lower()
        objs.append(obj)
    return objs





def _front_normal(mesh, inner):
    """Wall normal of a source-local mesh: the largest group of front
    (source-material) faces facing one way."""
    n = len(mesh.polygons)
    nrm = np.empty(n * 3)
    mesh.polygons.foreach_get('normal', nrm)
    nrm = nrm.reshape(-1, 3)
    area = np.empty(n)
    mesh.polygons.foreach_get('area', area)
    idx = np.empty(n, dtype=np.int32)
    mesh.polygons.foreach_get('material_index', idx)
    front = idx != inner
    if not front.any():
        front[:] = True
    fn, fa = nrm[front], area[front]
    best, best_a = Vector((0, 0, 1)), -1.0
    for i in np.argsort(-fa)[:64]:
        a = fa[(fn @ fn[i]) > 0.95].sum()
        if a > best_a * 1.001:
            best, best_a = Vector(fn[i]), a
    return best.normalized()


def _simplify(path, tol):
    """Ramer-Douglas-Peucker on a polyline (N x 3)."""
    if len(path) < 3:
        return path
    keep = np.zeros(len(path), dtype=bool)
    keep[0] = keep[-1] = True
    stack = [(0, len(path) - 1)]
    while stack:
        a, b = stack.pop()
        if b <= a + 1:
            continue
        seg = path[b] - path[a]
        L = np.linalg.norm(seg)
        rel = path[a + 1:b] - path[a]
        if L < 1e-12:
            d = np.linalg.norm(rel, axis=1)
        else:
            d = np.linalg.norm(np.cross(rel, seg / L), axis=1)
        k = int(np.argmax(d))
        if d[k] > tol:
            keep[a + 1 + k] = True
            stack += [(a, a + 1 + k), (a + 1 + k, b)]
    return path[keep]


def drawn_lines(context, source):
    """The strokes of Fracture Lines as source-local polylines."""
    drawing = settings(source).cut_drawing
    if drawing is None:
        return []
    from . import paint
    return paint.read_paths(context, source, drawing)


class _LineCutter:
    """Cuts meshes along drawn lines: every line segment becomes a thin box
    through the whole wall (along the wall normal), and the boxes are
    subtracted in one Boolean. The parts that come apart are the pieces."""
    def __init__(self, context, source, paths, normal, lo, hi, gap):
        self.context = context
        self.source = source
        n = Vector(normal).normalized()
        tol = max(gap, 1e-6)
        self.coll = bpy.data.collections.new('_pd_lines')
        _work_scene().collection.children.link(self.coll)
        self.boxes = []
        for path in paths:
            path = _simplify(np.asarray(path, dtype=np.float64), 2.0 * tol)
            for a, b in zip(path[:-1], path[1:]):
                a, b = Vector(a), Vector(b)
                a -= n * a.dot(n)
                b -= n * b.dot(n)
                t = b - a
                if t.length < 1e-9:
                    continue
                t.normalize()
                w = n.cross(t).normalized() * (0.5 * gap)
                a, b = a - t * (0.5 * gap), b + t * (0.5 * gap)
                co = []
                for p in (a, b):
                    for side in (-w, w):
                        for depth in (lo, hi):
                            co.append(tuple(p + side + n * depth))

                faces = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)]
                me = bpy.data.meshes.new('_pd_line')
                me.from_pydata(co, [], faces)
                me.update()
                bm = bmesh.new()
                bm.from_mesh(me)
                bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
                bm.to_mesh(me)
                bm.free()
                obj = bpy.data.objects.new('_pd_line', me)
                self.coll.objects.link(obj)
                obj.matrix_world = source.matrix_world
                self.boxes.append((np.minimum.reduce(co), np.maximum.reduce(co)))

    def touches(self, lo, hi):
        return any((blo <= hi).all() and (bhi >= lo).all() for blo, bhi in self.boxes)

    def cut(self, mesh):
        """Source-local closed mesh -> list of source-local closed parts (or None if uncut)."""
        co = np.empty(len(mesh.vertices) * 3)
        mesh.vertices.foreach_get('co', co)
        co = co.reshape(-1, 3)
        if not self.boxes or not self.touches(co.min(axis=0), co.max(axis=0)):
            return None
        slots = len(mesh.materials)
        tmp = bpy.data.objects.new('_pd_linecut', mesh)
        _work_link(tmp)
        tmp.matrix_world = self.source.matrix_world
        mod = tmp.modifiers.new('lines', 'BOOLEAN')
        mod.operation = 'DIFFERENCE'
        mod.operand_type = 'COLLECTION'
        mod.collection = self.coll
        mod.solver = 'MANIFOLD' if _manifold_solver() and _mesh_closed(mesh) else 'EXACT'
        if hasattr(mod, 'material_mode'):
            mod.material_mode = 'TRANSFER'
        try:
            dg = _work_depsgraph()
            out = bpy.data.meshes.new_from_object(tmp.evaluated_get(dg), preserve_all_data_layers=True, depsgraph=dg)
        finally:
            bpy.data.objects.remove(tmp, do_unlink=True)
        if len(out.materials) > slots:
            idx = np.empty(len(out.polygons), dtype=np.int32)
            out.polygons.foreach_get('material_index', idx)
            idx[idx >= slots] = slots - 1
            out.polygons.foreach_set('material_index', idx)
            while len(out.materials) > slots:
                out.materials.pop()
        bm = bmesh.new()
        bm.from_mesh(out)
        parts = []
        for part in _islands(bm):
            q = _bm_part(bm, part)
            open_edges = [e for e in q.edges if len(e.link_faces) < 2]
            if open_edges:
                bmesh.ops.holes_fill(q, edges=open_edges, sides=0)
            if q.calc_volume() < 0:
                bmesh.ops.reverse_faces(q, faces=q.faces[:])
            pm = bpy.data.meshes.new('part')
            q.to_mesh(pm)
            q.free()
            for m in out.materials:
                pm.materials.append(m)
            parts.append(pm)
        bm.free()
        bpy.data.meshes.remove(out)
        return parts

    def free(self):
        for obj in list(self.coll.objects):
            _remove(obj)
        bpy.data.collections.remove(self.coll)


def _layer_paths(context,source):
    from . import paint
    paths={role:paint.read_cell_paths(context,source,role) for role in ('GUIDANCE','THROUGH','SURFACE')}
    legacy=settings(source).cut_drawing
    if legacy and not paths['THROUGH']:
        paths['THROUGH']=paint.read_paths(context,source,legacy)
    return paths


def _local_bounds(obj, inverse):
    transform = inverse @ obj.matrix_world
    corners = np.asarray([tuple(transform @ Vector(corner)) for corner in obj.bound_box])
    return corners.min(axis=0), corners.max(axis=0)


def _cut_pieces(context, source, fill, objects, coll, paths, plane):
    """Use the Cell Fracture cutters and preserve the existing face data."""
    from . import cell_cut_plane, cell_cuts, fracture_uv
    lines = bool(paths['THROUGH'] or paths['SURFACE'])
    inverse = source.matrix_world.inverted_safe()
    skipped = []
    if plane is not None and not lines:
        low, high = plane[0].min(axis=0), plane[0].max(axis=0)
        candidates = []
        for obj in objects:
            minimum, maximum = _local_bounds(obj, inverse)
            (candidates if np.all(maximum >= low) and np.all(minimum <= high) else skipped).append(obj)
        objects = candidates
    if not objects:
        return skipped, 0, 0
    points=[];polys=[];ids=[];base=[];uv=[];norm=[];mats=[];face_mats=[];face_inner=[];offset=0
    original_uv = []
    for owner,obj in enumerate(objects):
        me=obj.data.copy();me.transform(inverse@obj.matrix_world);me.calc_loop_triangles()
        points.extend([v.co[:] for v in me.vertices])
        original_uv.append(fracture_uv.capture(me))
        inner_attr=me.attributes.get('project_damage_physics_inner')
        inside_name = fracture_uv.layer_name(me)
        inside_layer = me.uv_layers.get(inside_name) if inside_name else None
        exterior_layer = me.uv_layers.active
        for tri in me.loop_triangles:
            face=me.polygons[tri.polygon_index]
            mat=me.materials[face.material_index] if face.material_index<len(me.materials) else None
            if mat not in mats:mats.append(mat)
            face_mats.append(mats.index(mat));face_inner.append(bool(inner_attr.data[face.index].value) if inner_attr else False)
            indices=[*tri.vertices,tri.vertices[-1]];loops=[*tri.loops,tri.loops[-1]]
            polys.append([v+offset for v in indices]);ids.append(owner);base.append(len(base))
            layer = (inside_layer if inside_layer is not None and face_inner[-1] else exterior_layer)
            uv.append([tuple(layer.data[l].uv) if layer is not None else (0.,0.) for l in loops])
            norm.append([tuple(me.corner_normals[l].vector) for l in loops])
        offset+=len(me.vertices);bpy.data.meshes.remove(me)
    out=dict(cellf_points=np.array(points,np.float64),cellf_polys=np.array(polys,np.int32),cellf_piece=np.array(ids,np.int32),
             cellf_base_poly=np.array(base,np.int32),cellf_uvs=np.array(uv,np.float64),cellf_normals=np.array(norm,np.float64))
    result = cell_cuts.apply(context, source, out, 1., paths_by_role=paths) if lines else out
    drawn_cuts = source.get('project_damage_cell_cut_count', 0) if lines else 0
    plane_cuts = invalid = 0
    if plane is not None:
        result, plane_cuts, invalid = cell_cut_plane.cut_mesh(result, *plane, source.project_damage.cellf_inner_uv_scale)
    if not (drawn_cuts or plane_cuts):
        return objects + skipped, plane_cuts, invalid
    result['cellf_uvs']=np.asarray(result['cellf_uvs']).reshape(-1,4,2)
    created=[]
    try:
        inner=_inner_material(source)
        if inner not in mats:mats.append(inner)
        for owner in np.unique(result['cellf_piece']):
            rows=np.flatnonzero(result['cellf_piece']==owner);faces=result['cellf_polys'][rows];used,remap=np.unique(faces,return_inverse=True)
            local=remap.reshape(-1,4);is_tri=local[:,2]==local[:,3]
            me=bpy.data.meshes.new('Cut_Fragment');me.from_pydata(result['cellf_points'][used],[],[tuple(f[:3] if tri else f) for f,tri in zip(local,is_tri)])
            for mat in mats:me.materials.append(mat)
            src=result['cellf_base_poly'][rows];mask=src<0
            me.polygons.foreach_set('material_index',np.array([mats.index(inner) if i<0 else face_mats[i] for i in src],np.int32))
            mask|=np.array([False if i<0 else face_inner[i] for i in src])
            at=me.attributes.new('project_damage_physics_inner','BOOLEAN','FACE');at.data.foreach_set('value',mask)
            me.update()
            fracture_uv.transfer(me, src, original_uv)
            obj,_=_make_object(fill.name+'_Cut',me,coll,source)
            created.append(obj)
            loops = np.concatenate([result['cellf_uvs'][r,:3 if tri else 4] for r,tri in zip(rows,is_tri)])
            fracture_uv.project(obj, mask, native=loops)
        for obj in objects:_remove(obj)
        return created + skipped, plane_cuts, invalid
    except Exception:
        for obj in created:_remove(obj)
        raise


def _whole_fill(context, source, fill, coll):
    """A copy of the fill as the one piece that the drawn lines then cut."""
    mesh = fill.data.copy()
    mesh.transform(source.matrix_world.inverted() @ fill.matrix_world)
    obj, _ = _make_object(fill.name + '_whole', mesh, coll, source)
    mesh.name = obj.name
    return obj





def _bullet_material(source):
    """Fracture Type Auto for Bullet Impacts: by the bullet material."""
    m = str(getattr(source.project_damage, 'bullet_material', '2'))
    return {'0': 'ROCK', '1': 'WOOD'}.get(m, 'CONCRETE')


@_operation
def fracture(context, source, module, fills=None, proximity=None, shards=None, center=None, *, live_preview=False):
    """Breaks the fill objects into pieces. Returns (pieces, reports)."""
    s = settings(source)
    fills = [o for o in (fills if fills is not None else fills_of(source, module)) if o.type == 'MESH']
    if not fills:
        return [], [({'ERROR'}, 'Fracture: press Fill Damage first')]
    from . import cell_cut_plane, fracture_uv
    p = source.project_damage
    plane = None
    if p.cellf_cut_plane is not None:
        if p.cellf_cut_plane in fills:
            return [], [({'ERROR'}, 'Cut Plane must be a separate mesh from the pieces being cut')]
        inverse = source.matrix_world.inverted_safe()
        bounds = [_local_bounds(fill, inverse) for fill in fills]
        diagonal = max(float(np.linalg.norm(hi - lo)) for lo, hi in bounds)
        try:
            plane = cell_cut_plane.mesh_arrays(context, source, skin=max(diagonal * 2e-6, 1e-7))
        except (ValueError, RuntimeError) as exc:
            return [], [({'ERROR'}, f'Cut Plane: {exc}')]
        if plane is None:
            return [], [({'ERROR'}, 'Cut Plane has no faces. Extrude edges into faces.')]
    paths = _layer_paths(context, source)
    lines = bool(paths['THROUGH'] or paths['SURFACE'])
    guidance = bool(paths['GUIDANCE']) and p.cellf_guidance_amount > 0
    if not live_preview and s.cut_mode == 'DRAWN' and not (lines or guidance or plane is not None):
        return [], [({'ERROR'}, 'Fracture: add a Cut Plane or draw fractures first')]
    coll = _collection(context, source, PIECE_COLLECTION, '_FillPieces')
    out, reports = [], []
    fills.sort(key=lambda o: (-_front_area(o), o.name))
    requested=[]
    for fill in fills:
        kind=fill.get(FILL_KIND,module)
        requested.append(shards if shards is not None else s.hit_fill_shards if kind=='hit' else
                         _piece_count(context,source,fill,s.bullet_piece_size if kind=='bullet' else s.piece_size))
    allocations=_allocate_budget(requested,s.amount_budget)
    hint = None
    plane_cuts = invalid_cuts = 0
    uid = _uid(source)
    for i, fill in enumerate(fills):
        kind = fill.get(FILL_KIND, module)
        previous_pieces=[o for o in bpy.data.objects if o.get(PIECE_STAMP)==uid and o.get('project_damage_piece_fill')==fill.name]
        pieces = None
        try:
            material = s.material
            if material == 'AUTO':
                material = 'BRICK' if kind == 'brick' else 'SOIL' if kind in ADDITIVE else 'CONCRETE'
                if kind == 'bullet':
                    material = _bullet_material(source)
            if kind == 'hit':
                count = shards if shards is not None else s.hit_fill_shards
                ctr = s.hit_fill_center if center is None else center
                prox = proximity if proximity is not None else source.project_damage.hit_collection
            else:
                size = s.bullet_piece_size if kind == 'bullet' else s.piece_size
                count = shards if shards is not None else _piece_count(context, source, fill, size)
                ctr, prox = 0.0, proximity
            count=min(max(1,int(count)),allocations[i])
            centres = [o.matrix_world.translation.copy() for o in prox.all_objects] if prox is not None else []
            if (count<=1 or s.cut_mode == 'DRAWN') and not guidance:
                pieces = [_whole_fill(context, source, fill, coll)]
            elif material == 'BRICK' and not guidance:
                pieces = _brick_fracture(context, source, fill, s, coll, hint)
                if hint is None and 'project_damage_fill_normal' in fill:
                    hint = tuple(fill['project_damage_fill_normal'])
            elif material != 'FLAT' or guidance:
                pieces = _core_fracture(context, source, fill, coll, material if material in _CORE_MATERIAL else 'CONCRETE', 1 if s.cut_mode=='DRAWN' else count, s.seed + i,
                                        centres, ctr, s.grain)
                if pieces is None:
                    reports.append(({'WARNING'}, 'Fracture: this core has no Cell Fracture; used Flat Cells'))
            if pieces is None:
                pieces = _hit_fracture(context, source, fill, coll, count, ctr, s.seed + i, prox,
                                       even=kind != 'hit', label='Piece', kind=kind)
            if lines or plane is not None:
                pieces, cuts, invalid = _cut_pieces(context, source, fill, pieces, coll, paths, plane)
                plane_cuts += cuts
                invalid_cuts += invalid
        except (RuntimeError, ValueError) as exc:
            for obj in pieces or ():
                _remove(obj)
            reports.append(({'WARNING'}, '%s: %s' % (fill.name, exc)))
            continue
        if not pieces:
            reports.append(({'WARNING'},fill.name+': no valid pieces; previous result kept'))
            continue
        for old in previous_pieces:_remove(old)
        for j, obj in enumerate(pieces, 1):
            if obj.name not in coll.objects:
                for c in list(obj.users_collection):
                    c.objects.unlink(obj)
                coll.objects.link(obj)
            obj.name = '%s_%03d' % (fill.name.replace('_Fill_', '_Piece_'), j)
            obj.data.name = obj.name
            fracture_uv.copy_flags(obj.data, *fracture_uv.flags(fill.data))
            _apply_inner(obj,source)
            obj[PIECE_STAMP] = _uid(source)
            obj['project_damage_piece_fill'] = fill.name
            obj['project_damage_piece_module']=kind
            if not _mesh_closed(obj.data):
                _close_holes(obj.data)
            com = _mass(obj.data)[1]
            if com.length > 1e-9:
                obj.data.transform(Matrix.Translation(-com))
                obj.matrix_world = obj.matrix_world @ Matrix.Translation(com)
            ownership.bind(source, obj)
            obj['project_damage_fracture_rest_matrix']=[float(v) for row in (source.matrix_world.inverted()@obj.matrix_world) for v in row]
        out += pieces
        if s.hide_fill:
            fill.hide_set(True)
            fill.hide_render = True
    out=_enforce_piece_budget(context,out,s.amount_budget)
    try:
        from . import inner_rough
        inner_rough.after_fracture(context, source, out)
    except Exception as exc:
        reports.append(({'WARNING'}, 'Break Faces skipped: %s' % exc))
    from . import baked_materials, material_uv
    from .params import unit_factor
    uv_scale = 10.0 * _gscale(source.project_damage) / unit_factor(context)
    for obj in fracture_uv.with_details(out):
        mask = baked_materials.inner_mask(obj)
        uv_name = fracture_uv.layer_name(obj.data) or fracture_uv.project(obj, mask, uv_scale)
        if uv_name:
            material_uv.configure_zones(obj, mask, uv_name)
    if plane is not None:
        reports.append(({'INFO'}, f'Cut Plane split {plane_cuts} pieces'))
    if invalid_cuts:
        reports.append(({'WARNING'}, f'Cut Plane kept {invalid_cuts} invalid pieces unchanged'))
    reports.append(({'INFO'}, 'Fracture: %d pieces from %d objects in %s (Amount Budget %d)' % (len(out), len(fills), coll.name,s.amount_budget)))
    return out, reports





@_operation
def fill_hole(context, source):
    """Concrete Hit: fill each crater and break it into shards, denser at the impact."""
    s = settings(source)
    p = source.project_damage
    fills, reports = fill_damage(context, source, 'hit')
    if not fills:
        return [], reports
    prox = p.hit_collection
    pieces, rep = fracture(context, source, 'hit', fills=fills, proximity=prox,
                           shards=s.hit_fill_shards, center=s.hit_fill_center)
    return pieces, reports + rep


@_operation
def separate_pieces(context, source):
    """One closed object per loose island of the damage result, origin at its
    centre of mass, no parent (the largest island first: the wall itself)."""
    result = build.result_of(source)
    if result is None:
        return [], [({'ERROR'}, 'Separate to Pieces: build the damage first')]
    uid = _uid(source)
    for obj in [o for o in bpy.data.objects if uid and o.get(SPLIT_STAMP) == uid]:
        _remove(obj)
    coll = _collection(context, source, 'project_damage_split_collection', '_Pieces')
    bm = bmesh.new()
    bm.from_mesh(result.data)
    out = []
    for part in sorted(_islands(bm), key=len, reverse=True):
        pb = _bm_part(bm, part)
        mesh = bpy.data.meshes.new('piece')
        pb.to_mesh(mesh)
        pb.free()
        for m in result.data.materials:
            mesh.materials.append(m)
        obj, _ = _make_object('%s_piece_%03d' % (source.name, len(out) + 1), mesh, coll, result,
                              local_matrix=result.matrix_world)
        mesh.name = obj.name
        obj[SPLIT_STAMP] = uid
        obj.project_damage_og_object = source
        if not out:
            obj['project_damage_split_wall'] = True

        out.append(obj)
    bm.free()

    result.hide_set(True)
    result.hide_render = True
    return out, [({'INFO'}, 'Separate to Pieces: %d objects in %s' % (len(out), coll.name))]


FRAGMENT_STAMP = 'project_damage_fragment_of'


def replaced_by_pieces(source, result):
    """Kept for an ownership.py patched by the previous update: never hide."""
    return False


def after_fragments(result, pieces):
    """Separate Fragments on Non-through Chunks: the largest fragment is the
    body that stays (Quick Physics makes it the wall collider)."""
    source = ownership.source_of(result) if result.get(build.RESULT_STAMP) else None
    p = getattr(source, 'project_damage', None)
    if p is None or not pieces:
        return
    if p.fract_enable and p.fract_physics and not p.fract_through_enable:
        pieces[0]['project_damage_split_wall'] = True





def _quietly(execute):
    """Runs an operator with the add-on's live handlers paused: the temporary
    cutter objects of a Boolean must not dirty (and rebuild) any source."""
    def run(self, context):
        previous = live._APPLYING
        live._APPLYING = True
        try:
            return execute(self, context)
        finally:
            try:
                _drop_work_scene()
            finally:
                live._APPLYING = previous
    run.__doc__ = execute.__doc__
    return run


def _live_physics_objects(objects):
    results = []
    for obj in objects or ():
        if obj is None:
            continue
        try:
            if bpy.data.objects.get(obj.name) == obj:
                results.append(obj)
        except ReferenceError:
            continue
    return results


class _SourceOp:
    bl_options = {'REGISTER', 'UNDO'}
    module: bpy.props.EnumProperty(items=tuple((m, LABELS[m], '') for m in MODULES), options={'HIDDEN'})

    @classmethod
    def poll(cls, context):
        src = ownership.context_source(context)
        return (context.mode in {'OBJECT', 'PAINT_GREASE_PENCIL', 'PAINT_GPENCIL', 'EDIT_CURVE'}
                and not live._APPLYING and not live._HISTORY_RESTORING
                and src is not None and src.type == 'MESH')

    def invoke(self, context, event):

        if context.mode != 'OBJECT' and context.object is not None:
            bpy.ops.object.mode_set(mode='OBJECT')
        return self.execute(context)

    def _finish(self, context, source, objects, reports):
        for kind, msg in reports:
            self.report(kind, msg)
        results = _live_physics_objects(objects)
        if not results:
            return {'CANCELLED'}

        view_layer = context.view_layer
        view_layer.update()
        selectable = set(context.selectable_objects or ())
        visible_results = [obj for obj in results if obj in selectable]
        if visible_results:
            # Changing selection invalidates Blender's live selected-objects iterator.
            for obj in tuple(context.selected_objects or ()):
                if obj is not None:
                    obj.select_set(False, view_layer=view_layer)
            for obj in visible_results:
                obj.select_set(True, view_layer=view_layer)
            view_layer.objects.active = visible_results[0]
        from . import cellf_output
        if source.get(cellf_output.SWAP_ALL):
            cellf_output.sync_switch(context, source)
        return {'FINISHED'}


# действие «Fill Damage»
class PROJECT_DAMAGE_OT_physics_fill(_SourceOp, bpy.types.Operator):
    """Make one closed object per damaged area that fills the hole exactly and looks like the undamaged wall"""
    bl_idname = 'project_damage.physics_fill'
    bl_label = 'Fill Damage'

    @classmethod
    def poll(cls,context):
        if not _SourceOp.poll(context):return False
        reason=fill_block_reason(ownership.context_source(context))
        if reason:cls.poll_message_set(reason)
        return not reason

    @_quietly
    def execute(self, context):
        source = ownership.context_source(context)
        reason=fill_block_reason(source)
        if reason:
            self.report({'WARNING'},reason)
            return {'CANCELLED'}
        from . import python_jobs
        if python_jobs.queue(context,source,'FILL',self.module,fragments_only=getattr(self,'fragments_only',False)):
            self.report({'INFO'},'Calculation queued in background')
            return {'FINISHED'}
        objects, reports = fill_damage(context, source, self.module)
        objects = _live_physics_objects(objects)
        if objects and self.module in ADDITIVE and settings(source).detach_remove:
            kinds = {k for o in objects for k in o.get('project_damage_fill_kinds', [self.module])}
            reports += remove_from_wall(context, source, sorted(kinds))
        return self._finish(context, source, objects, reports)


# действие «Fill V2»
class PROJECT_DAMAGE_OT_physics_fill_v2(_SourceOp, bpy.types.Operator):
    bl_idname = 'project_damage.physics_fill_v2'
    bl_label = 'Fill V2'
    bl_description = ('Experimental local fill from damage face/vertex groups. Computes only on click; '
                      'does not prepare or cache fill geometry during generator updates. '
                      'Invalid local fragments are skipped and previous fills survive a failed calculation')

    @classmethod
    def poll(cls,context):
        if not _SourceOp.poll(context):return False
        reason=fill_block_reason(ownership.context_source(context))
        if reason:cls.poll_message_set(reason)
        return not reason

    @_quietly
    def execute(self, context):
        from . import fill_v2
        source=ownership.context_source(context)
        reason=fill_block_reason(source)
        if reason:
            self.report({'WARNING'},reason)
            return {'CANCELLED'}
        from . import python_jobs
        if python_jobs.queue(context,source,'FILL_V2',self.module,fragments_only=getattr(self,'fragments_only',False)):
            self.report({'INFO'},'Calculation queued in background')
            return {'FINISHED'}
        try:objects,reports=fill_v2.fill(context,source,self.module)
        except (ValueError,RuntimeError) as exc:
            self.report({'ERROR'},'Fill V2: '+str(exc))
            return {'CANCELLED'}
        return self._finish(context,source,objects,reports)


# действие «Exact Matcher»
class PROJECT_DAMAGE_OT_physics_fill_exact(_SourceOp, bpy.types.Operator):
    bl_idname = 'project_damage.physics_fill_exact'
    bl_label = 'Exact Matcher'
    bl_description = ('Restore the full removed volume of all active solid damage effects. '
                      'Ignores standalone fractures, surface deformations, and added geometry')

    @classmethod
    def poll(cls, context):
        return PROJECT_DAMAGE_OT_physics_fill.poll(context)

    @_quietly
    def execute(self, context):
        from . import fill_exact, python_jobs

        source = ownership.context_source(context)
        if python_jobs.queue(context, source, 'FILL_EXACT', self.module):
            self.report({'INFO'}, 'Calculation queued in background')
            return {'FINISHED'}
        objects, reports = fill_exact.fill(context, source)
        return self._finish(context, source, objects, reports)


# действие «Separate Fragments»
class PROJECT_DAMAGE_OT_physics_separate_exact(_SourceOp, bpy.types.Operator):
    bl_idname = 'project_damage.physics_separate_exact'
    bl_label = 'Separate Fragments'
    bl_description = 'Split the existing Exact Matcher fill into separate solid objects'

    @classmethod
    def poll(cls, context):
        if not _SourceOp.poll(context):
            return False
        from . import fill_exact

        source = ownership.context_source(context)
        return fill_exact.combined_fill(fills_of(source)) is not None

    @_quietly
    def execute(self, context):
        from . import fill_exact, python_jobs

        source = ownership.context_source(context)
        if python_jobs.queue(context, source, 'SEPARATE_EXACT', self.module):
            self.report({'INFO'}, 'Calculation queued in background')
            return {'FINISHED'}
        objects, reports = fill_exact.separate(context, source, self.module)
        return self._finish(context, source, objects, reports)


# действие «Fracture»
class PROJECT_DAMAGE_OT_physics_fracture(_SourceOp, bpy.types.Operator):
    """Break the fill objects into pieces for a rigid-body simulation (selected fill objects only, when any are selected)"""
    bl_idname = 'project_damage.physics_fracture'
    bl_label = 'Fracture'
    fragments_only: bpy.props.BoolProperty(default=False,options={'HIDDEN'})
    notify_live_disabled: bpy.props.BoolProperty(default=True, options={'HIDDEN', 'SKIP_SAVE'})

    @classmethod
    def poll(cls, context):
        if context.mode != 'EDIT_MESH':
            return super().poll(context)
        source = ownership.context_source(context)
        return (source is not None and context.object == source.project_damage.cellf_cut_plane
                and not live._APPLYING and not live._HISTORY_RESTORING)

    @_quietly
    def execute(self, context):
        source = ownership.context_source(context)
        if context.mode == 'EDIT_MESH':
            bpy.ops.object.mode_set(mode='OBJECT')
        from . import physics_live, python_jobs
        try:
            chosen=fracture_targets(context,source,self.module,self.fragments_only)
            physics_live.manual(context, source, self.module, chosen)
            from .boolean_break import FRAGMENT_STAMP
            uid = _uid(source)
            fragments = self.module == 'boolean' and bool(chosen and chosen[0].get(FRAGMENT_STAMP) == uid)
            scope = (tuple(obj.name for obj in chosen) if chosen else (), fragments, self.notify_live_disabled)
            if python_jobs.queue(context,source,'FRACTURE',self.module,fragments_only=self.fragments_only,
                                 fracture_scope=scope):
                self.report({'INFO'},'Calculation queued in background')
                return {'FINISHED'}
            objects, reports = fracture(context, source, self.module, fills=chosen)
        except RuntimeError as exc:
            self.report({'ERROR'}, str(exc))
            return {'CANCELLED'}
        status = self._finish(context, source, objects, reports)
        if status == {'FINISHED'}:
            physics_live.fracture_finished(context, source, self.module, scope[0], scope[1], notify=scope[2])
        return status


# действие «Fill Hole»
class PROJECT_DAMAGE_OT_physics_fill_hole(_SourceOp, bpy.types.Operator):
    """Concrete Hit: fill each crater with loose shards, smaller and denser at the impact centre"""
    bl_idname = 'project_damage.physics_fill_hole'
    bl_label = 'Fill Hole'

    @_quietly
    def execute(self, context):
        source = ownership.context_source(context)
        from . import python_jobs
        if python_jobs.queue(context,source,'FILL_HOLE',self.module,fragments_only=getattr(self,'fragments_only',False)):
            self.report({'INFO'},'Calculation queued in background')
            return {'FINISHED'}
        try:
            objects, reports = fill_hole(context, source)
        except RuntimeError as exc:
            self.report({'ERROR'}, str(exc))
            return {'CANCELLED'}
        return self._finish(context, source, objects, reports)


class PROJECT_DAMAGE_OT_physics_separate(_SourceOp, bpy.types.Operator):
    """One closed object per loose piece of the damage result, origin at its centre of mass, no parent"""
    bl_idname = 'project_damage.physics_separate'
    bl_label = 'Separate to Pieces'

    @_quietly
    def execute(self, context):
        source = ownership.context_source(context)
        from . import python_jobs
        if python_jobs.queue(context,source,'SEPARATE',self.module,fragments_only=getattr(self,'fragments_only',False)):
            self.report({'INFO'},'Calculation queued in background')
            return {'FINISHED'}
        objects, reports = separate_pieces(context, source)
        return self._finish(context, source, objects, reports)


# действие «Draw Fracture Lines»
class PROJECT_DAMAGE_OT_physics_lines_new(_SourceOp, bpy.types.Operator):
    """Make a Grease Pencil drawing for fracture lines and start drawing on the surface. Each stroke cuts straight through the wall"""
    bl_idname = 'project_damage.physics_lines_new'
    bl_label = 'Draw Fracture Lines'

    def execute(self, context):
        if bpy.app.version < (4, 3, 0):
            self.report({'ERROR'}, 'Drawing needs Blender 4.3 or newer; pick a Curve as Fracture Lines instead')
            return {'CANCELLED'}
        from . import paint
        source = ownership.context_source(context)
        s = settings(source)
        if s.cut_drawing is not None:
            paint._start_drawing(context, s.cut_drawing)
            return {'FINISHED'}
        name = source.name + '_FractureLines'
        gp = bpy.data.grease_pencils.new(name)
        layer = gp.layers.new('Fracture Lines', set_active=True)
        layer.frames.new(guide_timing.FRAME)
        obj = bpy.data.objects.new(name, gp)
        output_collections.link_object(context,source,obj)
        obj.parent = source
        obj.matrix_parent_inverse = source.matrix_world.inverted()
        obj['project_damage_paint_source'] = source
        obj.hide_render = True
        mat = bpy.data.materials.new(name + '_Ink')
        bpy.data.materials.create_gpencil_data(mat)
        mat.grease_pencil.color = (1.0, 0.85, 0.05, 1.0)
        gp.materials.append(mat)
        s.cut_drawing = obj
        if s.cut_mode == 'AUTO':
            s.cut_mode = 'BOTH'
        paint._start_drawing(context, obj)
        return {'FINISHED'}


class PROJECT_DAMAGE_OT_physics_lines_done(_SourceOp, bpy.types.Operator):
    """Finish drawing and go back to the source object"""
    bl_idname = 'project_damage.physics_lines_done'
    bl_label = 'Finish Drawing'

    def execute(self, context):
        from . import paint
        source = ownership.context_source(context)
        paint._activate(context, source)
        result = build.result_of(source)
        if result is not None and source.hide_get():
            paint._activate(context, result)
        return {'FINISHED'}





# здесь хранятся значения, которые сохраняются вместе с объектом
class ProjectDamageQuickPhysics(bpy.types.PropertyGroup):
    hit_area: bpy.props.PointerProperty(name='Hit Area',type=bpy.types.Object,poll=lambda self,obj:obj.type=='EMPTY',description='Sphere Empty that releases shards it reaches in Only Hit Pieces. Move, scale or animate it, then use Calculate Hit Releases to preview the change. The Empty applies no force')
    colliders: bpy.props.CollectionProperty(type=physics_workflow.ProjectDamageCollider)
    material_preset: bpy.props.EnumProperty(name='Material Preset',items=(('CONCRETE','Concrete','Density 2400 kg/m³, friction 0.65, bounce 0.05'),('WOOD','Wood','Density 650 kg/m³, friction 0.5, bounce 0.15'),('CUSTOM','Custom','Use your own simulation values')),default='CONCRETE',update=physics_workflow.preset_changed,
        description='Sets mass density, friction and bounce. Gravity and free-fall acceleration stay the same for every material')
    collider_object: bpy.props.PointerProperty(
        name='Collider',type=bpy.types.Object,poll=lambda self,obj:obj.type=='MESH',
        description='Pick a collider with the eyedropper without changing the selected pieces')
    start_frame: bpy.props.IntProperty(
        name='Start Frame', default=1,min=0,max=300000,
        description='Frame where the simulation starts; before it every piece stays in place')
    end_frame: bpy.props.IntProperty(name='End Frame', default=250,min=1,max=300000,description='Last frame of the live simulation')
    bake_start_frame: bpy.props.IntProperty(name='Bake Start', min=0, max=300000,
        get=lambda self: self.get('bake_start_frame', self.id_data.frame_start),
        set=lambda self, value: self.__setitem__('bake_start_frame', value),
        description='First frame to write keyframes. Defaults to the scene start. Simulation runs from its start before recording begins')
    bake_end_frame: bpy.props.IntProperty(name='Bake End', min=0, max=300000,
        get=lambda self: self.get('bake_end_frame', self.id_data.frame_end),
        set=lambda self, value: self.__setitem__('bake_end_frame', value),
        description='Last frame to write keyframes, inclusive. Defaults to the scene end. Extends the scene and simulation cache as needed')
    hold: bpy.props.EnumProperty(
        name='Hold', default='NONE',
        items=(('HIT', 'Only Hit Pieces', 'Hold the shards until the collider or Hit Area reaches them. Enable Chain Reaction to pass the impact between shards'),
               ('TOGETHER', 'Wake Together', 'Pieces sleep until something touches them; touching pieces wake '
                'together, so one hit can bring down a whole fill'),
               ('NONE', 'Fall at Start', 'Pieces fall as soon as the simulation starts')),
        description='What keeps the pieces in the wall until they are hit')
    hit_margin: bpy.props.FloatProperty(
        name='Touch Distance (cm)', default=0.5, min=0.0, soft_max=10.0,
        description='Contact tolerance for the collider, Hit Area and moving shards when Chain Reaction is enabled')
    chain_reaction: bpy.props.BoolProperty(
        name='Chain Reaction', default=False,
        description='Under Only Hit Pieces, moving shards can release held neighbors they touch. Use Calculate Hit Releases to preview the timing, or Quick Bake to record the resulting motion')
    shape: bpy.props.EnumProperty(
        name='Piece Shape', default='CONVEX_HULL',
        items=(('CONVEX_HULL', 'Convex Hull', 'Fast and stable; the pieces are near convex'),
               ('MESH', 'Mesh', 'Exact shape; slower')),
        description='Collision shape of the pieces')
    density: bpy.props.FloatProperty(
        name='Density (kg/m³)', default=2400.0, min=1.0, soft_max=8000.0,
        description='Mass of each piece = volume x density (concrete 2400, brick 1900, plaster 1200, glass 2500)')
    friction: bpy.props.FloatProperty(name='Friction', default=0.65, min=0.0, max=1.0)
    bounce: bpy.props.FloatProperty(name='Bounciness', default=0.05, min=0.0, max=1.0)
    wall_collider: bpy.props.BoolProperty(
        name='Self object as collider', default=True,
        description='Use the remaining damaged object as a fixed mesh collider. '
                    'Turn this off and press Refresh Self Collider or Enable / Update Physics to remove its rigid body')
    collider_shape: bpy.props.EnumProperty(
        name='Collider Shape', default='MESH', update=physics_workflow._schedule,
        items=(('MESH', 'Mesh', 'Exact shape'), ('CONVEX_HULL', 'Convex Hull', 'Fast; for convex colliders')),
        description='Collision shape of the colliders')


def quick(context):
    return context.scene.project_damage_quick_physics


def _hit_margin(context, q):
    scale = context.scene.unit_settings.scale_length or 1.0
    return q.hit_margin / 100.0 / scale


def _is_owned_output(obj):
    return any(obj.get(k) for k in ownership.STAMPS)


def _quick_targets(context):
    """Selected meshes that are pieces (anything but a damage source, its
    outputs or a fill object), else every piece of the active source."""
    from . import baked_materials
    applied = baked_materials.target(context)
    if applied is not None:
        return physics_workflow.targets(context, applied)
    sel = [o for o in context.selected_objects if o.type == 'MESH' and (
           o.get(PIECE_STAMP) or o.get(SPLIT_STAMP) or o.get('project_damage_cellf_of') or o.get('project_damage_rbf_of') or o.get('project_damage_bfrag_of')
           or o.get(FRAGMENT_STAMP) or not (_is_source(o) or _is_owned_output(o) or o.get(FILL_STAMP)))]

    sel = list(dict.fromkeys(o.parent if o.get('project_damage_rough_detail') and o.parent else o
                             for o in sel))
    if sel:
        return sel
    src = ownership.context_source(context)
    if src is None:
        return []
    uid = _uid(src)
    return [o for o in bpy.data.objects if uid and o.type == 'MESH'
            and (any(o.get(stamp)==uid for stamp in (PIECE_STAMP,SPLIT_STAMP,'project_damage_cellf_of','project_damage_rbf_of','project_damage_bfrag_of'))
                 or (o.get(FRAGMENT_STAMP) and ownership.source_of(o)==src)) and o.name in context.view_layer.objects]


def _is_source(obj):
    p = getattr(obj, 'project_damage', None)
    if p is None:
        return False
    return bool(obj.get('project_damage_uid')) and not obj.get(FILL_STAMP)


def _ensure_world(context):
    scene = context.scene
    if scene.rigidbody_world is None:
        with context.temp_override(scene=scene):
            bpy.ops.rigidbody.world_add()
    world = scene.rigidbody_world
    world.enabled = True
    if world.collection is None:
        world.collection = bpy.data.collections.new('RigidBodyWorld')
    return world


def _add_bodies(context, objects, kind):
    """Rigid bodies on `objects` (kind ACTIVE or PASSIVE) without changing the selection."""
    todo = [o for o in objects if o.rigid_body is None]
    if todo:
        from . import cellf_output
        with cellf_output.editable_shards(context, todo):
            with context.temp_override(selected_objects=todo, selected_editable_objects=todo,
                                       active_object=todo[0], object=todo[0]):
                bpy.ops.rigidbody.objects_add(type=kind)
    for o in objects:
        if o.rigid_body is not None:
            o.rigid_body.type = kind


def _sync_self_colliders(context, sources):
    from . import baked_materials
    q = quick(context)
    view_objects = context.view_layer.objects
    explicit = {row.object for row in q.colliders if row.object is not None}
    split_walls = [obj for obj in view_objects if obj.get('project_damage_split_wall')]
    enabled = set()
    candidates = set()
    for source in sources:
        if baked_materials.is_applied(source):
            batch = baked_materials.batch_objects(context, source)
            walls = baked_materials.split_walls(batch)
            surfaces = [obj for obj in batch if obj.get('project_damage_baked_surface')]
            candidates.update(surfaces)
            if not walls:
                walls = [obj for obj in surfaces if not obj.hide_render]
        else:
            walls = [obj for obj in split_walls if ownership.source_of(obj) == source]
            result = build.result_of(source)
            if result is not None and result.name in view_objects:
                candidates.add(result)
                if not walls and not (result.hide_render and source.project_damage.cellf_enable):
                    walls = [result]
        candidates.update(walls)
        if q.wall_collider:
            enabled.update(walls)

    enabled.difference_update(explicit)
    remove = [obj for obj in candidates - enabled - explicit if obj.rigid_body is not None]
    if not enabled and not remove:
        return 0, 0

    if physics_workflow.rigid_cache_is_baked(context):
        raise RuntimeError('Remove Baked Cache before changing the self collider')

    if enabled:
        _ensure_world(context)
        _add_bodies(context, list(enabled), 'PASSIVE')
        for obj in enabled:
            body = obj.rigid_body
            if body is None:
                raise RuntimeError(f'Could not add a rigid body to {obj.name}')
            body.collision_shape = 'MESH'
            body.friction = q.friction
            body.use_margin = True
            body.collision_margin = 0.0005 / (context.scene.unit_settings.scale_length or 1.0)
            body.kinematic = False
            body.enabled = True

    for obj in remove:
        with context.temp_override(object=obj, active_object=obj, selected_objects=[obj],
                                   selected_editable_objects=[obj]):
            bpy.ops.rigidbody.object_remove()
        if obj.rigid_body is not None:
            raise RuntimeError(f'Could not remove the rigid body from {obj.name}')
    return len(enabled), len(remove)


def _animated(obj):
    """Moves by itself: keyframes, drivers, constraints or a moving parent."""
    while obj is not None:
        ad = obj.animation_data
        if ad is not None and (ad.action is not None or len(ad.drivers) or len(ad.nla_tracks)):
            return True
        if len(obj.constraints):
            return True
        obj = obj.parent
    return False


HOLD_STAMP = 'project_damage_hold_hit'


def _clear_hold(obj):
    ad = obj.animation_data
    if ad is not None and ad.action is not None:
        for fc in [fc for fc in _fcurves(ad.action) if fc.data_path == 'rigid_body.kinematic']:
            try:
                _fcurves(ad.action).remove(fc)
            except Exception:
                pass
    if HOLD_STAMP in obj:
        del obj[HOLD_STAMP]


def _fcurves(action):
    """The F-curves of an action (Blender 4.4+ slotted actions or older)."""
    fcs = getattr(action, 'fcurves', None)
    if fcs is not None:
        return fcs
    for layer in action.layers:
        for strip in layer.strips:
            for bag in strip.channelbags:
                return bag.fcurves
    return []


def _moving_colliders(context):
    world = context.scene.rigidbody_world
    if world is None or world.collection is None:
        return []
    return [o for o in world.collection.objects if o.rigid_body is not None and o.rigid_body.type == 'PASSIVE'
            and o.rigid_body.kinematic and not o.get(PIECE_STAMP) and not o.get(SPLIT_STAMP)
            and not o.get(HOLD_STAMP)]


def _world_bvh(obj, depsgraph):
    from mathutils.bvhtree import BVHTree
    ev = obj.evaluated_get(depsgraph)
    me = ev.to_mesh()
    try:
        mw = ev.matrix_world
        verts = [mw @ v.co for v in me.vertices]
        bvh = BVHTree.FromPolygons(verts, [tuple(p.vertices) for p in me.polygons])
        co = np.array([tuple(v) for v in verts]) if verts else np.zeros((1, 3))
    finally:
        ev.to_mesh_clear()
    return bvh, co.min(axis=0), co.max(axis=0)


def _hold_piece(obj, start):
    _clear_hold(obj)
    obj[HOLD_STAMP] = True
    obj.rigid_body.kinematic = True
    obj.keyframe_insert('rigid_body.kinematic', frame=start)


def _release_hit(obj, start, frame):
    obj.rigid_body.kinematic = True
    obj.keyframe_insert('rigid_body.kinematic', frame=max(start, frame - 1))
    obj.rigid_body.kinematic = False
    obj.keyframe_insert('rigid_body.kinematic', frame=max(start + 1, frame))


def plan_hits(context, pieces, start, end, margin):
    """Only Hit Pieces: every piece is Animated (held in place) until the frame
    a moving collider first touches it, then it turns dynamic. The colliders'
    own animation is played through (the simulation switched off meanwhile)
    and tested against each piece. Returns {piece: frame}."""
    scene = context.scene
    world = scene.rigidbody_world
    colliders = _moving_colliders(context)
    for obj in pieces:
        _hold_piece(obj, start)
    hits = {}
    q = quick(context)
    marker = q.hit_area
    if not colliders and marker is None:
        return hits
    frame0, subframe0 = scene.frame_current, scene.frame_subframe
    scene.frame_set(start)
    dg = context.evaluated_depsgraph_get()
    piece_data = []
    for o in pieces:
        bvh, lo, hi = _world_bvh(o, dg)
        piece_data.append((o, bvh, lo, hi))
    was = world.enabled
    world.enabled = False
    try:
        for f in range(start, end + 1):
            for sub in (0.0, 0.5):
                scene.frame_set(f, subframe=sub)
                dg = context.evaluated_depsgraph_get()
                if marker is not None:
                    ev=marker.evaluated_get(dg)
                    center=ev.matrix_world.translation
                    radius=marker.empty_display_size*max(abs(s) for s in ev.matrix_world.to_scale())+margin
                    for o,bvh,lo,hi in piece_data:
                        if o in hits:continue
                        near=bvh.find_nearest(center)
                        inside=False
                        if np.all(np.asarray(center)>=lo) and np.all(np.asarray(center)<=hi):
                            direction=Vector((.793,.371,.483)).normalized();origin=center.copy();crossings=0
                            for _ in range(512):
                                loc,normal,index,distance=bvh.ray_cast(origin,direction)
                                if loc is None:break
                                crossings+=1;origin=loc+direction*1e-6
                            inside=bool(crossings%2)
                        if inside or (near[0] is not None and near[3]<=radius):hits[o]=f
                for col in colliders:
                    cb, clo, chi = _world_bvh(col, dg)
                    for o, bvh, lo, hi in piece_data:
                        if o in hits or (lo - margin > chi).any() or (hi + margin < clo).any():
                            continue
                        touch = bool(cb.overlap(bvh))
                        if not touch and margin > 0:
                            for v in (lo, hi, 0.5 * (lo + hi)):
                                loc, _n, _i, d = cb.find_nearest(Vector(v), margin)
                                if loc is not None:
                                    touch = True
                                    break
                        if touch:
                            hits[o] = f
            if len(hits) == len(piece_data):
                break
    finally:
        world.enabled = was
        scene.frame_set(frame0, subframe=subframe0)
    for obj, frame in hits.items():
        _release_hit(obj, start, frame)
    if q.chain_reaction and hits and len(pieces) > 1:
        from . import shard_activation
        shard_activation.propagate(context, pieces, piece_data, hits, start, end, margin)
    return hits


# действие «Refresh Self Collider»
class PROJECT_DAMAGE_OT_refresh_self_collider(bpy.types.Operator):
    bl_idname = 'project_damage.refresh_self_collider'
    bl_label = 'Refresh Self Collider'
    bl_description = ('Apply Self object as collider to the remaining damaged object. '
                      'When unchecked, remove its rigid body')
    bl_options = {'UNDO', 'INTERNAL'}

    @classmethod
    def poll(cls, context):
        return context.mode == 'OBJECT'

    @_quietly
    def execute(self, context):
        from . import baked_materials
        applied = baked_materials.target(context)
        source = applied or ownership.context_source(context)
        if source is None or (applied is None and not _is_source(source)):
            self.report({'WARNING'}, 'Select a Project Damage object')
            return {'CANCELLED'}
        try:
            enabled, removed = _sync_self_colliders(context, [source])
        except RuntimeError as exc:
            self.report({'ERROR'}, str(exc))
            return {'CANCELLED'}
        scene = context.scene
        if (enabled or removed) and scene.rigidbody_world is not None:
            scene.frame_set(scene.rigidbody_world.point_cache.frame_start)
        if enabled:
            self.report({'INFO'}, 'Updated self collider')
        elif removed:
            self.report({'INFO'}, 'Removed self collider rigid body')
        else:
            self.report({'INFO'}, 'No self collider to update')
        return {'FINISHED'}


def _configure_active_body(context, obj, settings):
    body = obj.rigid_body
    scale = context.scene.unit_settings.scale_length or 1.0
    volume = _mass(obj.data)[0] * abs(obj.matrix_world.determinant())
    body.mass = max(1e-3, volume * scale ** 3 * settings.density)
    body.collision_shape = settings.shape
    body.friction = settings.friction
    body.restitution = settings.bounce
    body.use_margin = True
    body.collision_margin = 0.0005 / scale
    body.use_deactivation = settings.hold == 'TOGETHER'
    body.use_start_deactivated = settings.hold == 'TOGETHER'
    body.enabled = True
    body.kinematic = False


# действие «Quick Physics»
class PROJECT_DAMAGE_OT_quick_physics(bpy.types.Operator):
    """Add or update live rigid bodies and colliders. Baking is handled by Quick Bake to End Frame"""
    bl_idname = 'project_damage.quick_physics'
    bl_label = 'Quick Physics'
    bl_options = {'REGISTER', 'UNDO'}
    whole_group: bpy.props.BoolProperty(default=False,options={'SKIP_SAVE','HIDDEN'})

    @classmethod
    def poll(cls, context):
        return context.mode == 'OBJECT'

    @_quietly
    def execute(self, context):
        from . import baked_materials
        q = quick(context)
        applied = baked_materials.target(context)
        selected = _quick_targets(context)
        pieces = [obj for obj in selected if not obj.get('project_damage_split_wall')]
        if not pieces and selected:
            pieces = physics_workflow.targets(context)
        pieces, passive = physics_workflow.motion_groups(context, pieces, self.whole_group)
        if not (pieces or passive) and (applied is None or self.whole_group):
            self.report({'ERROR'}, 'Quick Physics: no pieces. Fracture (or Separate to Pieces) first, or select the pieces')
            return {'CANCELLED'}
        if (physics_workflow.rigid_cache_is_baked(context)
                or any(physics_workflow.baked_action(obj) for obj in pieces + passive)):
            self.report({'WARNING'}, 'Remove Baked Cache before updating the live rigid bodies')
            return {'CANCELLED'}
        physics_workflow.sync_colliders(context)
        if applied is not None:
            sources = [applied]
        else:
            sources = {ownership.source_of(obj) for obj in pieces + passive}
            sources.add(ownership.context_source(context))
            sources = {source for source in sources if source is not None and _is_source(source)}
        try:
            walls, _ = _sync_self_colliders(context, sources)
        except RuntimeError as exc:
            self.report({'ERROR'}, str(exc))
            return {'CANCELLED'}
        world = _ensure_world(context)
        cache = world.point_cache
        cache.frame_start = q.start_frame
        cache.frame_end = max(q.end_frame, q.start_frame + 1)
        scene = context.scene
        if scene.frame_end < cache.frame_end:
            scene.frame_end = cache.frame_end
        _add_bodies(context, pieces, 'ACTIVE')
        _add_bodies(context, passive, 'PASSIVE')
        for o in passive:
            rb=o.rigid_body
            rb.collision_shape=q.shape
            rb.friction=q.friction
            rb.restitution=q.bounce
            rb.use_margin=True
            rb.collision_margin=0.0005/(scene.unit_settings.scale_length or 1.0)
            rb.enabled=True
            rb.kinematic=False
            if o.get(HOLD_STAMP):
                _clear_hold(o)
        for o in pieces:
            held = o.rigid_body.kinematic if o.get(HOLD_STAMP) else True
            _configure_active_body(context, o, q)
            if q.hold == 'HIT':
                o[HOLD_STAMP] = True
                o.rigid_body.kinematic = held
            elif o.get(HOLD_STAMP):
                _clear_hold(o)
        from . import cellf_output
        for obj in pieces + passive:
            if obj.get('project_damage_cellf_of') or cellf_output._switch_track(obj, 'visibility'):
                owner = obj.get('project_damage_baked_backup') or ownership.source_of(obj)
                if owner is not None:
                    cellf_output.sync_activation(owner, obj)
        context.view_layer.update()
        physics_workflow.record_settings_applied(context, sources)
        self.report({'INFO'}, 'Updated live physics: %d active, %d passive%s; start frame %d'
                    % (len(pieces), len(passive), ', self collider' if walls else '', q.start_frame))
        return {'FINISHED'}


# действие «Make Colliders»
class PROJECT_DAMAGE_OT_quick_collider(bpy.types.Operator):
    """Make the selected objects colliders. Animated objects (keyframes, constraints, a moving parent) become moving colliders that push the pieces"""
    bl_idname = 'project_damage.quick_collider'
    bl_label = 'Make Colliders'
    bl_options = {'REGISTER', 'UNDO'}
    motion: bpy.props.EnumProperty(
        name='Motion', default='AUTO',
        items=(('AUTO', 'Auto', 'Moving when the object is animated, else fixed'),
               ('MOVING', 'Moving', 'Follows its animation and pushes the pieces'),
               ('FIXED', 'Fixed', 'Does not move')))

    @classmethod
    def poll(cls, context):
        return context.mode == 'OBJECT' and any(o.type == 'MESH' for o in context.selected_objects)

    @_quietly
    def execute(self, context):
        q = quick(context)
        objs = [o for o in context.selected_objects if o.type == 'MESH' and not o.get(PIECE_STAMP)
                and not o.get(SPLIT_STAMP)]
        if not objs:
            self.report({'ERROR'}, 'Make Colliders: select the collider meshes (not the pieces)')
            return {'CANCELLED'}
        if physics_workflow.rigid_cache_is_baked(context):
            self.report({'WARNING'}, 'Remove Baked Cache before changing colliders')
            return {'CANCELLED'}
        _ensure_world(context)
        _add_bodies(context, objs, 'PASSIVE')
        moving = 0
        for o in objs:
            rb = o.rigid_body
            rb.collision_shape = q.collider_shape
            rb.friction = q.friction
            rb.restitution = q.bounce
            rb.kinematic = self.motion == 'MOVING' or (self.motion == 'AUTO' and _animated(o))
            moving += rb.kinematic
        self.report({'INFO'}, 'Make Colliders: %d colliders (%d moving)' % (len(objs), moving))
        return {'FINISHED'}


# действие «Remove Physics»
class PROJECT_DAMAGE_OT_quick_physics_clear(bpy.types.Operator):
    """Remove the rigid bodies from the pieces (the selected ones, else every piece of this source)"""
    bl_idname = 'project_damage.quick_physics_clear'
    bl_label = 'Remove Physics'
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return context.mode == 'OBJECT'

    @_quietly
    def execute(self, context):
        from . import cellf_output
        source = physics_workflow.settings_source(context)
        physics_workflow.clear_bake(context, restore_bodies=False)
        pieces = list(dict.fromkeys(_quick_targets(context) + physics_workflow.targets(context)))
        removed = []
        with cellf_output.editable_shards(context, pieces):
            for o in pieces:
                _clear_hold(o)
                cellf_output.clear_activation(o)
                if o.rigid_body is not None:
                    with context.temp_override(object=o, active_object=o, selected_objects=[o], selected_editable_objects=[o]):
                        bpy.ops.rigidbody.object_remove()
                    removed.append(o)
        physics_workflow.clear_settings_applied(context, source)
        self.report({'INFO'}, 'Removed physics from %d pieces' % len(removed))
        return {'FINISHED'}


class PROJECT_DAMAGE_OT_assign_collider(bpy.types.Operator):
    bl_idname='project_damage.assign_collider'
    bl_label='Assign Collider'
    bl_options={'REGISTER','UNDO'}

    @classmethod
    def poll(cls,context):return context.mode=='OBJECT' and quick(context).collider_object is not None

    def execute(self,context):
        obj=quick(context).collider_object
        if obj.name not in context.view_layer.objects:
            self.report({'ERROR'},'Collider must be in the active view layer');return {'CANCELLED'}
        with context.temp_override(object=obj,active_object=obj,selected_objects=[obj],selected_editable_objects=[obj]):
            return bpy.ops.project_damage.quick_collider(motion='AUTO')


def draw_quick_physics(layout, context):
    physics_workflow.draw_quick(layout,context)


# выстраиваем шаги заполнения, дробления и настройки физики
def draw(layout, p, context, module):
    from . import physics_ui
    physics_ui.draw(layout,p,context,module)


_CLASSES = (physics_workflow.ProjectDamageCollider,ProjectDamagePhysicsSettings, ProjectDamageQuickPhysics, PROJECT_DAMAGE_OT_physics_fill, PROJECT_DAMAGE_OT_physics_fill_v2,
            PROJECT_DAMAGE_OT_physics_fill_exact, PROJECT_DAMAGE_OT_physics_separate_exact, PROJECT_DAMAGE_OT_physics_fracture, PROJECT_DAMAGE_OT_physics_fill_hole, PROJECT_DAMAGE_OT_physics_separate,
            PROJECT_DAMAGE_OT_physics_lines_new, PROJECT_DAMAGE_OT_physics_lines_done,
            PROJECT_DAMAGE_OT_quick_physics, PROJECT_DAMAGE_OT_quick_collider, PROJECT_DAMAGE_OT_quick_physics_clear,
            PROJECT_DAMAGE_OT_assign_collider, PROJECT_DAMAGE_OT_refresh_self_collider)+physics_workflow.CLASSES


# подключаем свойства и действия этого модуля к Blender
def register():
    from . import physics_ui
    physics_ui.register()
    for cls in _CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.Object.project_damage_physics = bpy.props.PointerProperty(type=ProjectDamagePhysicsSettings)
    bpy.types.Scene.project_damage_quick_physics = bpy.props.PointerProperty(type=ProjectDamageQuickPhysics)


# убираем подключения модуля при выключении аддона
def unregister():
    from . import physics_ui
    physics_ui.unregister()
    physics_workflow.stop()
    if hasattr(bpy.types.Object, 'project_damage_physics'):
        del bpy.types.Object.project_damage_physics
    if hasattr(bpy.types.Scene, 'project_damage_quick_physics'):
        del bpy.types.Scene.project_damage_quick_physics
    for cls in reversed(_CLASSES):
        try:
            bpy.utils.unregister_class(cls)
        except RuntimeError:
            pass


def draw_fragment_fracture(layout,source):
    s=settings(source)
    box=layout.box()
    from . import material_controls
    title_row=box.row(align=True)
    material_controls.centered_heading(title_row,'Fragment Fracture Type',icon='MOD_EXPLODE')
    ui_image_hints.button(title_row,'fracture_type')
    for options in (('AUTO','CONCRETE','ROCK','SOIL'),('WOOD','GLASS','FLAT')):
        row=box.row(align=True);row.scale_y=1.5
        for value in options:row.prop_enum(s,'material',value)
    if s.material=='WOOD':box.prop(s,'grain',expand=True)
    col=box.column(align=True);col.prop(s,'piece_size');col.prop(s,'amount_budget');col.prop(s,'seed')
    from . import inner_rough
    from . import cell_cuts
    cell_cuts.draw_layers(box,bpy.context,source,physics=True)
    op=box.operator('project_damage.physics_fracture',text='Fracture Selected Fragments',icon='MOD_EXPLODE')
    op.module='boolean';op.fragments_only=True
    inner_rough.draw(box,source,'pieces')
