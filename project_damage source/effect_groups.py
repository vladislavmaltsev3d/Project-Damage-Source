# помечаем области эффектов и собираем группы вершин повреждения
# SPDX-License-Identifier: GPL-3.0-or-later
"""Output groups selected by the core's per-face effect stamp.

The core tags every result face with the effect whose cutter or added
geometry produced it (`effect_ids`, written as project_damage_effect_id).
That stamp is exact: it does not depend on materials, so effects sharing a
shader (or Apply One Material to All) no longer bleed into each other, and
the original skin never joins a group.

Materials are only a fallback, for an enabled effect that has no stamped face
(older core, displacement-only effects); the fallback may only take damaged
faces no stamp claimed. Groups hold all vertices of their faces, shared rims
included; the exact face set is also written as a boolean face attribute
'PD_<Effect> Faces' (Select > By Attribute, or the Attribute node).
Separate generated objects get the corresponding group on all their vertices.
Only groups and attributes recorded as owned here are refreshed or removed.
"""
from contextlib import contextmanager

import bpy
import numpy as np

KEYS = ('', 'edge', 'corner', 'surface', 'fracture', 'buildup', 'heat',
        'rust', 'coating', 'fabric', 'concrete', 'hit', 'wood', 'lath',
        'wallpaper', 'bullet', 'blast', 'ground', 'brick', 'glass',
        'fissure', 'bevel', 'scratch', 'boolean', 'plaster', 'metal',
        'hang', 'rusty',                                                                             
        'piles', 'deposits')                                                                       
ATTRIBUTE = 'project_damage_effect_id'
OWNED = 'project_damage_effect_group_names'
OWNED_FACES = 'project_damage_effect_face_masks'
WELD = 'project_damage_weld_id'                                                   
SPLIT_MARK = 'project_damage_split_zones'                                      
                                                           
STAMP_ALIASES = {'fabric_solid': 'fabric', 'concrete_v2': 'concrete', 'hit_through': 'hit',
                 'lath_v2': 'lath', 'lath_riven': 'lath', 'lath_framing': 'lath',
                 'brick_v2': 'brick', 'wallhole': 'brick', 'glass_hit': 'glass',
                 'glass_drawn': 'glass', 'glass_through': 'glass'}
                                                                    
SEPARATE_ONLY = {'weld', 'grass', 'gravel', 'foam', 'pipes', 'cables', 'ivy', 'roots',
                 'debris', 'rbf', 'cellf', 'surface_rebar'}
                                                                         
                                                   
PARTS = {'surface': ('Additional Damage', 0), 'concrete': ('Rebar', 5), 'concrete_v2': ('Rebar', 5)}


def tag_mesh(mesh, out, props):
    ids = np.asarray(out.get('effect_ids', ()), dtype=np.int32)
    if len(ids) != len(mesh.polygons):
        return
    attr = mesh.attributes.get(ATTRIBUTE)
    if attr is None:
        attr = mesh.attributes.new(ATTRIBUTE, 'INT', 'FACE')
    attr.data.foreach_set('value', ids)
    from .recording_tools import apply_material_regions
    apply_material_regions(mesh,out,props)
                                                                          
                                                                               
    mat = props.surface_damage_material
    if mat is not None and np.any(ids == 2):
        slot = next((i for i, m in enumerate(mesh.materials) if m == mat), None)
        if slot is None:
            slot = len(mesh.materials); mesh.materials.append(mat)
        values = np.empty(len(mesh.polygons), dtype=np.int32)
        mesh.polygons.foreach_get('material_index', values)
        values[ids == 2] = slot
        mesh.polygons.foreach_set('material_index', values)


def active_keys(source):
    from . import sidebar
    from .fracture_assets import active_key
    p = source.project_damage
    keys = {key for key, _, prop, _ in sidebar.EFFECTS if getattr(p, prop, False)}
    if p.fabric_enable or p.fabric_solid_enable:
        keys.discard('fabric_solid');keys.add('fabric')
                                                                          
    keys.difference_update(key for key in tuple(keys) if key.startswith('fracture'))
    from . import fracture_stack
    keys.update(key for key,_ in fracture_stack.layers(source))
    return keys


def _replace(obj, masks, labels, reserved=()):
    previous = dict(obj.get(OWNED, {}))
    current = {}
    for key, vertices in masks.items():
        name = previous.pop(key, None)
        group = obj.vertex_groups.get(name) if name and name not in reserved else None
        if group is None:
            group = obj.vertex_groups.new(name='PD_' + labels.get(key, key.replace('_', ' ').title()))
        elif len(obj.data.vertices):
            group.remove(list(range(len(obj.data.vertices))))
        vertices = np.asarray(vertices, dtype=np.int32)
        for start in range(0, len(vertices), 32000):
            group.add(vertices[start:start+32000].tolist(), 1.0, 'REPLACE')
        current[key] = group.name
    for name in previous.values():
        if name in reserved:
            continue
        group = obj.vertex_groups.get(name)
        if group is not None:
            obj.vertex_groups.remove(group)
    obj[OWNED] = current


def _replace_face_masks(obj, faces, labels):
    """Exact face sets as boolean FACE attributes, beside the vertex groups."""
    mesh = obj.data
    previous = dict(mesh.get(OWNED_FACES, {}))
    taken = {g.name for g in obj.vertex_groups}
    current = {}
    for key, mask in faces.items():
        name = previous.pop(key, None)
        attr = mesh.attributes.get(name) if name else None
        if attr is not None and (attr.domain != 'FACE' or attr.data_type != 'BOOLEAN'):
            mesh.attributes.remove(attr); attr = None
        if attr is None:
            base = name = 'PD_' + labels.get(key, key.replace('_', ' ').title()) + ' Faces'
                                                                           
            attr = mesh.attributes.get(name)
            if attr is None or attr.domain != 'FACE' or attr.data_type != 'BOOLEAN' or name in taken:
                attr, n = None, 1
                while name in taken or mesh.attributes.get(name) is not None:
                    name = '%s.%03d' % (base, n); n += 1
                attr = mesh.attributes.new(name, 'BOOLEAN', 'FACE')
        attr.data.foreach_set('value', np.asarray(mask, dtype=bool))
        current[key] = attr.name
    for name in previous.values():
        attr = mesh.attributes.get(name)
        if attr is not None:
            mesh.attributes.remove(attr)
    mesh[OWNED_FACES] = current


def materials_for_effect(props, key, resolved=None):
    """Resolve UI materials plus the defaults actually used by shared core slots."""
    from . import material_controls, material_defaults
    from .mesh_io import DAMAGE_MATERIAL_NAMES
    from .fracture_assets import TYPE_BY_ASSET
    from . import fracture_stack
    if key in fracture_stack.FRACTURE_KEYS and getattr(props,'id_data',None) is not None:
        props=next((p for k,p in fracture_stack.layers(props.id_data) if k==key),props)
    module = 'fracture' if key in TYPE_BY_ASSET else {'fabric_solid':'fabric','concrete_v2':'concrete','lath_v2':'lath','lath_riven':'lath','lath_framing':'lath','brick_v2':'brick','glass_hit':'glass','glass_drawn':'glass'}.get(key,key)
    materials = set()
    if key=='ground' and props.ground_rubble_enable:
        materials.add(props.buildup_material or material_defaults.get('gravel'))
    for name, _ in material_controls.material_fields(module):
        mat = getattr(props, name, None)
        if mat is not None:
            materials.add(mat)
    def slot_material(slot):
        explicit = getattr(props, material_controls.CLASS_PROPS.get(slot, ''), None)
        return (explicit or (resolved or {}).get(slot)
                or bpy.data.materials.get(DAMAGE_MATERIAL_NAMES[slot]))
    for slot, _ in material_controls.EFFECT_SLOTS.get(module, ()):
                                                                             
                                                                              
        if key == 'corner' and props.surface_damage_material is None:
            slot = 0
        mat = slot_material(slot)
        if mat is not None:
            materials.add(mat)
    if key == 'hang':
        materials.add(slot_material(7))
        materials.add(props.rebar_material or material_defaults.get('rebar'))
        materials.add(props.hang_cable_material or bpy.data.materials.get('ProjectDamage_Cable_Rubber'))
    if key in {'brick','brick_v2','wallhole'}:
        materials.add(props.brick_front_material or material_defaults.get('brick'))
        materials.add(props.brick_break_material or material_defaults.get('brick'))
        materials.add(props.brick_seam_material or material_defaults.get('mortar'))
    if module == 'lath':
        if key=='lath_framing':
            from . import stud_framing
            materials.add(props.lath_material or stud_framing.wood_material())
            materials.add(stud_framing.nail_material())
        else:
            materials.add(props.lath_material or material_defaults.get('wood_planks'))
    if key == 'boolean' and props.rebar_enable:
        fill = '2' if props.rebar_fill == '5' else props.rebar_fill
        name = {'0':'rebar_material', '1':'rebar_brick_material', '2':'rebar_plank_material','3':'rebar_framing_material','4':'rebar_chips_material'}[fill]
        mat = getattr(props, name, None)
        if fill=='4':
            from . import chips
            mat=mat or chips.wood_material()
        if fill=='3':
            from . import framing
            mat=mat or framing.wood_material()
            materials.add(props.rebar_nail_material or framing.nail_material())
        if mat is None:
            mat = material_defaults.get({'0':'rebar','1':'brick','2':'wood_planks'}[fill])
        if mat is not None:
            materials.add(mat)
        if fill == '1' and props.rebar_mortar_joints:
            materials.add(props.rebar_mortar_material or material_defaults.get('mortar'))
    return {mat for mat in materials if mat is not None}


def _face_values(mesh, name, dtype):
    attr = mesh.attributes.get(name)
    if attr is None or attr.domain != 'FACE' or not len(mesh.polygons):
        return None
    values = np.empty(len(mesh.polygons), dtype=dtype)
    attr.data.foreach_get('value', values)
    return values


def face_vertices(mesh, faces):
    """Unique vertices of the faces in a boolean face mask (vectorised)."""
    n = len(mesh.polygons)
    faces = np.asarray(faces, dtype=bool)
    if not n or len(faces) != n or not faces.any():
        return np.empty(0, dtype=np.int32)
    totals = np.empty(n, dtype=np.int32)
    mesh.polygons.foreach_get('loop_total', totals)
    corners = np.empty(len(mesh.loops), dtype=np.int32)
    mesh.loops.foreach_get('vertex_index', corners)
                                                         
    return np.unique(corners[np.repeat(faces, totals)]).astype(np.int32)


def material_faces(mesh, materials):
    """Material identity, not slot number: duplicate slots must match too.
    A shared shader on the original skin must not select the whole source,
    so surviving base faces never match."""
    from .material_uv import base_material
    from .mesh_io import CLASS_NAMES, BASE_CLASS
    n = len(mesh.polygons)
    roots = {base_material(mat) for mat in materials if mat is not None}
    slots = [i for i, mat in enumerate(mesh.materials) if mat is not None and base_material(mat) in roots]
    if not slots or not n:
        return np.zeros(n, dtype=bool)
    indices = np.empty(n, dtype=np.int32)
    mesh.polygons.foreach_get('material_index', indices)
    faces = np.isin(indices, slots)
    base = _face_values(mesh, CLASS_NAMES[BASE_CLASS], bool)
    if base is not None:
        faces &= ~base
    return faces


def material_vertices(mesh, materials):
    return face_vertices(mesh, material_faces(mesh, materials))


def stamp_vertices(mesh, effect_id):
    """Vertices of the faces stamped with this effect id, or None when the
    mesh carries no stamp."""
    ids = _face_values(mesh, ATTRIBUTE, np.int32)
    if ids is None:
        return None
    return face_vertices(mesh, ids == effect_id)


def stamp_of(key):
    """The core's effect stamp for a sidebar key, or None (no core stamp)."""
    if key == 'metaltear':return 34
    if key == 'peeling':return 33
    if key == 'dbevel':return 21
    if key == 'erosion':return 32
    if key.startswith('fracture'):
        from .fracture_stack import IDS
        return IDS.get(key,4)
    key = STAMP_ALIASES.get(key, key)
    return KEYS.index(key) if key and key in KEYS else None


def _regions(key):
    from .result_transfer import WALLPAPER_REGIONS, BRICK_REGIONS, EXTRA_REGIONS
    return (WALLPAPER_REGIONS if key == 'wallpaper' else
            BRICK_REGIONS if key in {'brick','brick_v2','wallhole'} else
            [row for row in EXTRA_REGIONS if ('scratch' in row[0]) == (key == 'scratch')]
            if key in {'scratch', 'bevel', 'dbevel'} else ())


def region_faces(mesh, key):
    """Explicit shader zones the core reports for Brick / Wallpaper /
    Scratches / Bevel, or None when the mesh has none of them."""
    faces = None
    for _, name, _ in _regions(key):
        mask = _face_values(mesh, name, bool)
        if mask is not None:
            faces = mask if faces is None else faces | mask
    return faces


def region_materials(mesh, key):
    """Materials on the explicit shader zones (material fallback only)."""
    faces = region_faces(mesh, key)
    if faces is None or not faces.any():
        return set()
    indices = np.empty(len(mesh.polygons), dtype=np.int32)
    mesh.polygons.foreach_get('material_index', indices)
    return {mesh.materials[int(i)] for i in np.unique(indices[faces])
            if i < len(mesh.materials) and mesh.materials[int(i)] is not None}


def effect_faces(mesh, keys, props, resolved_materials=None):
    """{key: boolean face mask} for the damage mesh, plus PARTS sub-groups."""
    from .mesh_io import CLASS_NAMES
    n = len(mesh.polygons)
    ids = _face_values(mesh, ATTRIBUTE, np.int32)
    claimed = ids != 0 if ids is not None else np.zeros(n, dtype=bool)
    faces, fallback = {}, []
    for key in sorted(keys):
        if key in SEPARATE_ONLY:
            continue
        stamp = stamp_of(key)
        mask = ids == stamp if ids is not None and stamp is not None else None
        if key=='surface' and ids is not None:mask=(ids==3)|(ids==31)
        region = region_faces(mesh, key)
        if region is not None:
            mask = region if mask is None else mask | region
            claimed = claimed | region
        if mask is None or not mask.any():
            fallback.append(key)
        else:
            faces[key] = mask
    for key in fallback:
        materials = materials_for_effect(props, key, resolved_materials) | region_materials(mesh, key)
        faces[key] = material_faces(mesh, materials) & ~claimed
    for key, (suffix, cls) in PARTS.items():
        if key in faces:
            part = _face_values(mesh, CLASS_NAMES[cls], bool)
            if part is not None and (faces[key] & part).any():
                faces[key + '_' + suffix.lower()] = faces[key] & part
    return faces


def split_zones(mesh, faces):
    """Split Effect Zones: cut the mesh along every edge between two zones
    (an effect, its parts, the original skin). Nothing moves; the rim
    vertices are duplicated, so a zone owns its own vertices and a vertex
    group then selects exactly its material zone. A skin face whose corners
    all sit on hole rims no longer shares them. Returns the split edge count;
    running it on an already split mesh does nothing.

    Every vertex keeps the index it had before the cut in WELD, so the
    copies of one rim vertex can be joined again exactly (`welded`)."""
    import bmesh
    n = len(mesh.polygons)
    if not n or not faces:
        return 0
    zone = np.zeros(n, dtype=np.int32)
                                                                 
    parts = {key + '_' + suffix.lower() for key, (suffix, _) in PARTS.items()}
    for i, key in enumerate(sorted(faces, key=lambda k: (k in parts, k)), 1):
        zone[np.asarray(faces[key], dtype=bool)] = i
    totals = np.empty(n, dtype=np.int32)
    mesh.polygons.foreach_get('loop_total', totals)
    edges = np.empty(len(mesh.loops), dtype=np.int32)
    mesh.loops.foreach_get('edge_index', edges)
    corner_zone = np.repeat(zone, totals)
    lo = np.full(len(mesh.edges), np.iinfo(np.int32).max, dtype=np.int32)
    hi = np.full(len(mesh.edges), -1, dtype=np.int32)
    np.minimum.at(lo, edges, corner_zone)
    np.maximum.at(hi, edges, corner_zone)
    cut_mask=(hi >= 0) & (lo != hi)
    ids=_face_values(mesh,ATTRIBUTE,np.int32)
    if ids is not None:
        attached=np.zeros(len(mesh.edges),dtype=bool)
        np.logical_or.at(attached,edges,np.repeat(ids==33,totals))
        cut_mask &= ~attached
    cut = np.flatnonzero(cut_mask)
    if not len(cut):
        return 0
    if _weld_ids(mesh) is None:
        attr = mesh.attributes.get(WELD)
        if attr is not None:
            mesh.attributes.remove(attr)
        attr = mesh.attributes.new(WELD, 'INT', 'POINT')
        attr.data.foreach_set('value', np.arange(len(mesh.vertices), dtype=np.int32))
    bm = bmesh.new()
    try:
        bm.from_mesh(mesh)
        bm.edges.ensure_lookup_table()
        bmesh.ops.split_edges(bm, edges=[bm.edges[int(i)] for i in cut])
        bm.to_mesh(mesh)
    finally:
        bm.free()
    mesh.update()
    mesh[SPLIT_MARK] = len(mesh.vertices)
    return len(cut)


def _weld_ids(mesh):
    """WELD values when they belong to this geometry (written by split_zones
    for exactly this vertex count), else None."""
    if mesh.get(SPLIT_MARK) != len(mesh.vertices):
        return None
    attr = mesh.attributes.get(WELD)
    if attr is None or attr.domain != 'POINT' or attr.data_type != 'INT':
        return None
    ids = np.empty(len(mesh.vertices), dtype=np.int32)
    attr.data.foreach_get('value', ids)
    return ids


def clear_split(mesh):
    attr = mesh.attributes.get(WELD)
    if attr is not None:
        mesh.attributes.remove(attr)
    if SPLIT_MARK in mesh:
        del mesh[SPLIT_MARK]


def weld(mesh):
    """Join the copies split_zones made (same WELD id) back into one vertex.
    Exact, no merge distance. Returns True when anything was joined."""
    import bmesh
    ids = _weld_ids(mesh)
    if ids is None or len(np.unique(ids)) == len(ids):
        return False
                                                                    
    co = np.empty(len(mesh.vertices) * 3, dtype=np.float32)
    mesh.vertices.foreach_get('co', co)
    co = co.reshape(-1, 3)
    order = np.argsort(ids, kind='stable')
    same = ids[order][1:] == ids[order][:-1]
    if np.abs(co[order][1:][same] - co[order][:-1][same]).max(initial=0.0) > 1e-6:
        return False
    bm = bmesh.new()
    try:
        bm.from_mesh(mesh)
        bm.verts.ensure_lookup_table()
        keep = {}
        targets = {}
        for i, key in enumerate(ids.tolist()):
            j = keep.setdefault(key, i)
            if j != i:
                targets[bm.verts[i]] = bm.verts[j]
        bmesh.ops.weld_verts(bm, targetmap=targets)
        bm.to_mesh(mesh)
    finally:
        bm.free()
    mesh.update()
    mesh[SPLIT_MARK] = len(mesh.vertices)
    return True


def resplit(obj):
    """Split again after `weld`, from the owned face masks, and give the
    owned groups their exact vertices on the new topology."""
    mesh = obj.data
    names = dict(mesh.get(OWNED_FACES, {}))
    faces = {}
    for key, name in names.items():
        mask = _face_values(mesh, name, bool)
        if mask is not None:
            faces[key] = mask
    split_zones(mesh, faces)
    groups = dict(obj.get(OWNED, {}))
    _replace(obj, {key: face_vertices(mesh, faces[key]) for key in groups if key in faces},
             {key: name[3:] for key, name in groups.items()})
    from . import wallpaper_flaps
    wallpaper_flaps.rewrite(obj)


@contextmanager
def welded(obj):
    """The result whole again for an operation that needs a closed solid
    (Fill Damage, Detach, Separate to Pieces, Fracture), split again after.
    Does nothing for a result that is not split."""
    if obj is None or obj.type != 'MESH' or not weld(obj.data):
        yield obj
        return
    try:
        yield obj
    finally:
        try:
            resplit(obj)
        except ReferenceError:
            pass


def refresh(source, result, auxiliaries=(), resolved_materials=None):
    from . import sidebar
    enabled = active_keys(source)
    labels = {key: label for key, label, _, _ in sidebar.EFFECTS}
    for key, (suffix, _) in PARTS.items():
        labels[key + '_' + suffix.lower()] = labels.get(key, key.title()) + ' ' + suffix
    p = source.project_damage
    mesh = result.data
    n = len(mesh.polygons)
    faces = effect_faces(mesh, enabled, p, resolved_materials)
    if p.all_off:
        faces = {key: np.zeros(n, dtype=bool) for key in faces}
    elif getattr(p, 'split_effect_zones', False):
                                                                        
        split_zones(mesh, faces)
    if not getattr(p, 'split_effect_zones', False):
        clear_split(mesh)
    _replace(result, {key: face_vertices(mesh, mask) for key, mask in faces.items()},
             labels, {g.name for g in source.vertex_groups})
    _replace_face_masks(result, faces, labels)
    from . import wallpaper_flaps
    wallpaper_flaps.refresh(source, result)
    for objects, key in auxiliaries:
        if objects is None:
            continue
        if isinstance(objects, bpy.types.Object):
            objects = [objects] + list(objects.children_recursive)
        for obj in objects:
            if obj.type != 'MESH':
                continue
                                                                           
                                                                            
            vertices = (np.arange(len(obj.data.vertices), dtype=np.int32) if not p.all_off
                        else np.empty(0, dtype=np.int32))
            if obj.data.users > 1:
                obj.data = obj.data.copy()
            _replace(obj, {key: vertices}, labels)
