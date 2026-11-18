# главный путь от исходного меша к готовому повреждённому объекту
# ProjectDamage for Blender - worn edge damage cut into mesh objects.
# Copyright (C) 2026 Cornelius Dammrich
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <https://www.gnu.org/licenses/>.
"""The Update operator: source mesh -> core -> `<name>_project_damage` result object.

Parametric-object semantics, the closest Blender gets to C4D's generator: the
SOURCE object and its settings are never written to, the result is a separate
object rebuilt in place on every Update. live.py drives this same
rebuild_object() automatically, deferred, from a depsgraph handler and prop
edits - see that module.
"""
from . import output_collections
from .blender_compat import enable_nodes
import hashlib
import struct
import time
import uuid

import bpy
from . import ownership
from .mesh_io import ShadingProvenance, SourceShading, apply_phong, shading_region_group
import numpy as np

from . import core_loader
from .mesh_io import (BASE_CLASS, _apply_classes, _result_mesh, _write_result_uvs,
                      copy_input_materials, read_avoid_polys, read_guide_splines,
                      read_color_mask, read_mask_weights,
                      read_mesh, read_uvs)
from .params import _nothing_asked_for, fill_breakers, fill_fract_proximity, fill_params, unit_factor
from .params import fill_build_paint, fill_corner_paint, fill_shard_points


RESULT_STAMP = "project_damage_result_of"
DEPOSIT_STAMP = "project_damage_deposits_of"
COATING_STAMP = "project_damage_coating_of"


def _place_deposits(context, source, out, factor, kind="deposits"):
    stamp = "project_damage_" + kind + "_of"
    effect, field, label = {"deposits": (29,"deposit_material","Deposits"), "beads": (5,"buildup_material","Beads"), "piles": (28,"pile_material","Corner Piles")}[kind]
    """Own one optional deposit object per source; refresh or remove it safely."""
    uid = source.get("project_damage_uid")
    existing = next((o for o in bpy.data.objects
                     if uid is not None and o.type == 'MESH' and o.get(stamp) == uid), None)
    polygons = out.get(kind+"_polys")
    if polygons is None or len(polygons) == 0:
        if existing is not None:
            old_mesh = existing.data
            bpy.data.objects.remove(existing, do_unlink=True)
            if old_mesh.users == 0:
                bpy.data.meshes.remove(old_mesh)
        return None
    name = source.name + "_project_damage_" + kind
    mesh = _result_mesh(name, out[kind+"_points"] / factor, polygons, ShadingProvenance.generated_object())
    from . import material_controls
    from .recording_tools import region_material
    materials = {5:region_material(source.project_damage,field,'ProjectDamage_'+label)}
    _apply_classes(mesh, np.full(len(polygons), 5, dtype=np.int32), damage_materials=materials)
    from . import effect_groups
    effect_groups.tag_mesh(mesh, {'effect_ids': np.full(len(polygons), effect, dtype=np.int32)}, source.project_damage)
    if existing is None:
        existing = bpy.data.objects.new(name, mesh)
    else:
        old_mesh = existing.data
        existing.data = mesh
        if old_mesh.users == 0:
            bpy.data.meshes.remove(old_mesh)
    existing[stamp] = uid
    ownership.bind(source, existing)
    output_collections.link_object(context,source,existing)
    existing.matrix_world = source.matrix_world
    return existing


def _place_coating(context, source, out, factor):
    """Own one optional coating object per source; refresh or remove it safely."""
    uid = source.get("project_damage_uid")
    existing = next((o for o in bpy.data.objects
                     if uid is not None and o.type == 'MESH' and o.get(COATING_STAMP) == uid), None)
    polygons = out.get("coating_polys")
    if polygons is None or len(polygons) == 0:
        if existing is not None:
            old_mesh = existing.data
            bpy.data.objects.remove(existing, do_unlink=True)
            if old_mesh.users == 0:
                bpy.data.meshes.remove(old_mesh)
        return None
    name = source.name + "_project_damage_coating"
    mesh = _result_mesh(name, out["coating_points"] / factor, polygons, ShadingProvenance.generated_object())
    material = source.project_damage.coating_shell_material or bpy.data.materials.get("ProjectDamage_Coating")
    if material is None:
        material = bpy.data.materials.new("ProjectDamage_Coating")
        material.diffuse_color = (0.14, 0.36, 0.65, 1.0)
        enable_nodes(material)
        bsdf = material.node_tree.nodes.get("Principled BSDF")
        if bsdf is not None:
            bsdf.inputs["Base Color"].default_value = material.diffuse_color
            bsdf.inputs["Roughness"].default_value = 0.6
    mesh.materials.append(material)
    if existing is None:
        existing = bpy.data.objects.new(name, mesh)
    else:
        old_mesh = existing.data
        existing.data = mesh
        if old_mesh.users == 0:
            bpy.data.meshes.remove(old_mesh)
    existing[COATING_STAMP] = uid
    ownership.bind(source, existing)
    output_collections.link_object(context,source,existing)
    existing.matrix_world = source.matrix_world
    return existing


def _write_rust_attributes(mesh, rust, streak):
    """Rust material attributes (POINT, FLOAT): project_damage_rust (amount 0..1, with
    the ragged front) and project_damage_rust_streak (runs down along gravity)."""
    if rust is None or len(rust) != len(mesh.vertices) or not len(rust):
        return False
    for name, values in (("project_damage_rust", rust), ("project_damage_rust_streak", streak)):
        if values is None or len(values) != len(mesh.vertices):
            continue
        attr = mesh.attributes.get(name)
        if attr is not None and (attr.domain != 'POINT' or attr.data_type != 'FLOAT'):
            mesh.attributes.remove(attr)
            attr = None
        attr = attr or mesh.attributes.new(name, 'FLOAT', 'POINT')
        attr.data.foreach_set("value", np.asarray(values, dtype=np.float32))
    return True


def result_of(source):
    """The result object OWNED by `source`, or None. Ownership is the
    RESULT_STAMP custom property carrying the source's project_damage_uid - never the
    name: a user's own object that merely happens to be called
    '<source>_project_damage' is not ours, and a result living only in another scene
    is still found (and replaced in place there by _place)."""
    from . import runtime_lookup
    return runtime_lookup.output(source, RESULT_STAMP)


def deposits_of(source):
    uid = source.get("project_damage_uid")
    return next((o for o in bpy.data.objects if uid is not None
                 and o.type == 'MESH' and o.get(DEPOSIT_STAMP) == uid), None)


def coating_of(source):
    uid = source.get("project_damage_uid")
    return next((o for o in bpy.data.objects if uid is not None
                 and o.type == 'MESH' and o.get(COATING_STAMP) == uid), None)


STALE_MESH = 'project_damage_stale_mesh'
STALE_DELAY = 0.5
DEFER_MESH_FREE = None      # None: defer in the UI, free at once in background mode


# освобождаем старые меши, которые больше никому не нужны
def stale_mesh_flush(keep=None):
    """Free retired result meshes nothing uses any more. Returns the count."""
    done = 0
    for me in [m for m in bpy.data.meshes if m.get(STALE_MESH)]:
        if me == keep or me.users:
            continue
        try:
            bpy.data.meshes.remove(me)
            done += 1
        except (ReferenceError, RuntimeError):
            pass
    return done


def _stale_flush_timer():
    from . import live
    if live._busy():return .25
    try:
        stale_mesh_flush()
    except Exception as exc:
        print("ProjectDamage: stale mesh cleanup failed: %r" % exc)
    return None


def _schedule_stale_flush():
    if not bpy.app.timers.is_registered(_stale_flush_timer):
        bpy.app.timers.register(_stale_flush_timer, first_interval=STALE_DELAY)


# заменяем геометрию результата, сохраняя сам объект
def _place(context, obj, mesh, name):
    """The result object, replaced IN PLACE if `obj` already owns one (same
    transform, same collection) so an Update never piles up objects and never
    touches the source; created under `name` otherwise - Blender dedupes the
    name to '.001' if a foreign object holds it, and that object is left alone.

    The name is passed in rather than read off the mesh: the previous
    result's mesh datablock still holds `name` at this point, so the new
    one was created as `name.001`."""
    res = result_of(obj)
    if res is None:
        from . import preparation_tools
        res = preparation_tools.reuse_target(obj)
    if res is None:
        res = bpy.data.objects.new(name, mesh)
    else:
        old = res.data
        res.data = mesh
        if old.users == 0:
            from . import live
            defer = (not bpy.app.background) if DEFER_MESH_FREE is None else DEFER_MESH_FREE
            if defer:
                # Properties and node editors may still hold the previous mesh.
                # Release it after the UI has redrawn, including manual updates.
                old.name = name + '_stale'
                old[STALE_MESH] = True
                mesh.name = name
                _schedule_stale_flush()
            else:
                bpy.data.meshes.remove(old)
                mesh.name = name
    uid = obj.get("project_damage_uid")
    if uid is not None:
        res[RESULT_STAMP] = uid
    output_collections.link_object(context,obj,res)
    res.matrix_world = obj.matrix_world
    ownership.bind(obj, res)
    return res











# Beyond this the fracture chain walk runs for minutes with no way to cancel.
_CACHES = {}


def _get_cache(obj, core):
    """The per-object session Cache for `obj`, creating both the `project_damage_uid`
    custom property and the Cache on first use. Lazy sweep: every call first
    drops registry entries whose uid no longer belongs to any object in
    bpy.data.objects, so a deleted object's Cache (and the ProjectDamageDamageCache it
    owns) is freed on the very next Update anywhere, not held for the rest of
    the session.

    The same sweep also settles UID OWNERSHIP: a duplicated object (Shift+D,
    obj.copy()) copies `project_damage_uid` as a plain custom property, so two objects
    can carry the identical uid until one of them is built here. `setdefault`
    in bpy.data.objects' name order means the alphabetically-first object
    keeps the uid; any other object holding that same uid is not its owner
    and re-mints a fresh one on this, its own first build. Without this, the
    live-update system (live.py, keyed on the same uid) would resolve a
    duplicate's dirty mark back to its unrelated original and rebuild that
    instead - the duplicate never live-updates at all."""
    live = {}
    for o in bpy.data.objects:
        u = o.get("project_damage_uid")
        if u is not None:
            live.setdefault(u, o)
    for uid in [u for u in _CACHES if u not in live]:
        del _CACHES[uid]
    uid = obj.get("project_damage_uid")
    if uid is None or live.get(uid) is not obj:
        uid = uuid.uuid4().hex
        obj["project_damage_uid"] = uid
        obj['project_damage_defaults_v2'] = True
    from . import incremental
    if not incremental.cache_enabled():
        _CACHES.pop(uid,None)
        return None
    cache = _CACHES.get(uid)
    if cache is None:
        cache = core.Cache()
        _CACHES[uid] = cache
    return cache











_DAMAGE_FIELDS = (
    "damage_shading", "damage_smooth_angle", "base_smooth_angle",



    "seed", "threshold_deg", "topology", "base_map", "axis_x", "axis_y", "axis_z",
    "amount", "size_min", "size_max", "depth_min", "depth_max",
    "detail", "stretch", "chip_rim_soft", "norm_enable", "norm_max_size",
    "offset", "random_offset", "noise_strength", "noise_scale",
    "sec_amount", "sec_size", "sec_depth",
    "surf_amount", "surf_size_min", "surf_size_max",
    "surf_type", "surf_grain_x", "surf_grain_y", "surf_grain_z", "surf_knife_jitter", "surf_knife_back",
    "surf_depth_min", "surf_depth_max", "surf_detail", "surf_noise", "surf_noise_in",
    "surf_sec_amount", "surf_sec_size", "surf_sec_depth",
    "build_coverage", "build_corner_prob", "build_detail",
    "build_width", "build_height", "build_corner_size", "build_irreg", "build_blend",
    "deposit_amount", "deposit_style", "deposit_size_min", "deposit_size_max", "deposit_height",
    "deposit_gravity", "deposit_gravity_x", "deposit_gravity_y", "deposit_gravity_z",
    "deposit_shelter", "deposit_shelter_radius", "deposit_embed", "deposit_layers",
    "deposit_porosity", "deposit_separate",
    "fract_recipe", "fract_thickness_cap", "fract_prox_mode", "fract_prox_radius",
    "fract_prox_strength", "fract_radial_spokes", "fract_radial_rings", "fract_radial_ring_fill",
    "fract_radial_crush", "fract_glass_spokes", "fract_glass_rings", "fract_glass_branch",
    "fract_glass_ring_fill", "fract_glass_width", "fract_glass_depth", "fract_glass_crush",
    "fract_through", "fract_through_min_piece", "fract_through_fill", "fract_through_fill_size", "fract_through_rough",
    "fract_type","fract_impacts","fract_grain_x","fract_grain_y","fract_grain_z","fract_grain_pull",
    "fract_physics","fract_physics_depth","fract_physics_close",
    "fract_helper_snap",
    "rust_enable", "rust_seed", "rust_intensity", "rust_edge_bias", "rust_bottom_bias", "rust_noise",
    "rust_noise_scale", "rust_detail", "rust_crust_start", "rust_crust_height", "rust_crust_rough",
    "rust_pits", "rust_pit_start", "rust_pit_size", "rust_pit_depth", "rust_hole_start", "rust_hole_depth",
    "rust_flakes", "rust_flake_size", "rust_paint", "rust_paint_start", "rust_band", "rust_paint_flakes",
    "rust_craze", "rust_craze_cell", "rust_blisters", "rust_blister_size", "rust_streak",
    "rust_streak_length", "rust_gravity_x", "rust_gravity_y", "rust_gravity_z",
    "heat_enable", "heat_seed", "heat_intensity", "heat_diffuse", "heat_spread", "heat_duration", "heat_steps", "heat_char_start", "heat_shrink_depth", "heat_detail", "heat_block_round", "heat_cell_min", "heat_cell_max", "heat_crack_cells", "heat_crack_width", "heat_crack_depth", "heat_blisters", "heat_blister_min", "heat_blister_max", "heat_blister_low", "heat_blister_high", "heat_blister_burst", "heat_char_layer",
    "coating_zone_enable", "coating_zone_threshold", "coating_zone_smooth", "coating_zone_inset", "coating_zone_detail", "coating_flake_side", "coating_flake_side_x", "coating_flake_side_y", "coating_flake_side_z",
    "coating_enable", "coating_amount", "coating_thickness", "coating_patch_min", "coating_patch_max",
    "coating_jagged", "coating_flakes", "coating_curl", "coating_cracks",
    "coating_seed", "coating_satellites", "coating_stretch", "coating_dir_x", "coating_dir_y", "coating_dir_z", "coating_edge_wrap", "coating_facing_only", "coating_facing_x", "coating_facing_y", "coating_facing_z", "coating_facing_angle", "coating_flake_size", "coating_crack_length",
    "fract_detail",
    "fract_disp_strength", "fract_disp_scale", "fract_depth_mult", "fract_rim_soft",






    "corner_prob", "corner_min_edges",
    "corner_size_min", "corner_size_max", "corner_depth_min", "corner_depth_max",
    "corner_detail", "corner_rim_soft",
    "corner_sec_amount", "corner_sec_size", "corner_sec_depth",




    "avoid_chips", "avoid_surf", "avoid_build", "avoid_fract", "avoid_corner",
)
_FRACT_LEVEL_FIELDS = ("amount", "width", "depth", "bias", "chips",
                       "chip_amount", "chip_size", "chip_depth", "subdiv", "length", "branches", "branch_length")








_BELOW_DAMAGE_FIELDS = ("fract_tile_offset", "fract_tile_seed")

# The RESOLVED break params ComputeFingerprint hashes at project_damage_plugin.cpp
# 1598-1599 - break_rim_enable is deliberately NOT here, because fill_params
# already folds it (and all_off) into break_rim_amount/break_surf_dmg before
# this reads them. Hashing the raw props instead was tried and was WRONG:
# all_off zeroes the resolved fields but is not itself one of the six props,
# so a scene with every module already off, one breaker linked and Rim
# Damage on could toggle All Effects Off - dropping the rim chips - with
# full_fp not moving at all (fix round 1, review Important 1). Reading the
# resolved fields is both the fix and the simpler code: five values, no
# derivation to re-trace, matching the authority directly.
_BREAK_RESOLVED = ("break_surf_dmg",
                   "break_rim_amount", "break_rim_size", "break_rim_depth")

_FP_VERSION = b"project_damage-fp-v1"




_WORN_V2_FIELDS = (
    "chip_cluster", "chip_fine", "chip_profile", "chip_asym", "chip_guard",
    "corner_style", "corner_rough", "corner_tilt", "corner_flakes",
    "corner_paint_only", "corner_paint_radius",
    "surf_cluster", "surf_size_bias", "surf_depth_follow", "surf_guard",
    "fract_rim_jag", "fract_flare",
    "build_shape", "build_lumps", "pile_height", "pile_lumps", "pile_detail", "deposit_detail",
)


_SHARD_FIELDS = ("fract_shard_enable", "fract_shard_dir", "fract_shard_amount", "fract_shard_radius",
                 "fract_shard_falloff", "fract_shard_tilt", "fract_shard_random", "fract_shard_seed")


def _hash_value(h, v):
    """Every value type fingerprints() ever feeds it: bool BEFORE int (bool
    is an int subclass in Python), then int, float, str. Canonical encoding:
    ints as little-endian 64-bit, masked to unsigned before packing - most
    fields here are small non-negative counts/enums where this is a no-op,
    but spec.custom_hash (bake_custom's FNV-1a) is a genuine u64 that sets
    the top bit on roughly half of all shapes, which struct.pack('<q', ...)
    raises on. Masking first gives byte-identical output to a plain '<q'
    pack for every value that already fit (two's complement is the same
    bits either way) and no longer raises on the ones that didn't. Floats as
    IEEE-754 little-endian double (struct.pack('<d', ...)) - one fixed
    choice, not repr-bytes, so the same value always hashes to the same
    bytes regardless of how Python printed it."""
    if isinstance(v, bool):
        h.update(b"\x01" if v else b"\x00")
    elif isinstance(v, int):
        h.update(struct.pack("<Q", v & 0xFFFFFFFFFFFFFFFF))
    elif isinstance(v, float):
        h.update(struct.pack("<d", v))
    elif isinstance(v, str):
        h.update(v.encode("utf-8"))
    else:
        raise TypeError("fingerprints: unhandled value type %r" % (v,))


# запоминаем, что именно влияло на этот результат
def fingerprints(obj, props, core, points, polys, params, factor, uvs=None,
                 mask_weights=None, avoid_polys=None, guide_splines=None, heat_weights=None, coating_zone_weights=None,
                 guide_levels=None, surface_paint_paths=None, surface_paint_normals=None, rust_weights=None,
                 fabric_weights=None, fabric_avoid_polys=None, concrete_strokes=None, coating_paths=None, coating_normals=None, rust_paths=None, rust_normals=None, effect_inputs=None):
    """(full_fp, damage_fp) - two u64 blake2b(digest_size=8) fingerprints
    mirroring ComputeFingerprint's split (project_damage_plugin.cpp): damage_fp covers
    everything damage PLACEMENT reads and is the core's own ProjectDamageDamageCache
    key (`cache_fingerprint`); full_fp adds the RESOLVED breaker specs and the
    resolved break fields on top. A breaker MOVE must flip full_fp and leave
    damage_fp untouched - that is the core cache's hit condition, so getting
    the split wrong either throws away a hit or serves stale cutters under a
    moved breaker. full_fp's only consumer is the E2E's invariants; the
    live-update system is event-driven and never polls it.

    `params.breakers` must already be resolved (fill_breakers). `factor` is
    the caller's own `unit_factor(context)`: a timer callback's `bpy.context`
    can differ from the context the rebuild runs against. `obj`/`core` are
    accepted for signature symmetry with fill_params and not read; `props`
    supplies the mask group and avoid attribute NAMES (renaming which group is
    linked must invalidate even if the new one carries the same weights).

    `uvs`, `mask_weights`, `avoid_polys` are the caller's own read_* outputs
    or None - the same values passed to build_worn. Each is hashed as a
    None-ness sentinel byte first (absent-vs-present is never confused with
    present-but-empty, which is also how toggling the switch invalidates),
    then, only while present, a length prefix and the raw bytes - the mask
    block also carries the two knobs (Mask Amount/Size), mirroring
    ComputeFingerprint's own `if (h.fieldLinked)` block."""
    h = hashlib.blake2b(digest_size=8)
    h.update(b"project_damage-core-0.13.7")
    h.update(_FP_VERSION)
    for name in _DAMAGE_FIELDS:
        _hash_value(h, getattr(params, name))
    _hash_value(h, params.surf_seed)
    for name in ('noise_roughness', 'noise_distortion'):
        _hash_value(h, getattr(params, name, 0.))
    for name in ('concrete', 'lath', 'brick', 'fissure', 'ground'):
        block = getattr(params, name)
        if block.enable:_hash_value(h, getattr(block, 'generated_geometry', True))
    _hash_value(h, getattr(params, "preserve_surface_shells", False))
    from . import incremental, background_wire
    _hash_value(h, incremental.digest(background_wire.encode(params.fracture_layers,core)))
    if getattr(params,'fract_rounded',False):h.update(b'fract_rounded')
    if getattr(params, 'fract_surface_proxy', False):h.update(b'fract_surface_proxy_v1')
    if getattr(params, 'surface_guide', False):h.update(b'all_surface_guide_v1')
    for level in (params.fract1, params.fract2, params.fract3):
        for name in _FRACT_LEVEL_FIELDS:
            _hash_value(h, getattr(level, name))




    _hash_value(h, len(points))
    _hash_value(h, len(polys))
    h.update(points.tobytes())
    h.update(polys.tobytes())
    _hash_value(h, factor)
    have_uvs = uvs is not None
    _hash_value(h, have_uvs)
    if have_uvs:
        _hash_value(h, len(uvs))
        h.update(uvs.tobytes())




    have_mask = mask_weights is not None
    _hash_value(h, have_mask)
    if have_mask:



        _hash_value(h, len(props.mask_group))
        _hash_value(h, props.mask_group)
        _hash_value(h, len(mask_weights))
        h.update(mask_weights.tobytes())
        _hash_value(h, props.mask_amount / 100.0)
        _hash_value(h, props.mask_size / 100.0)
    _hash_value(h, heat_weights is not None)
    if heat_weights is not None:
        _hash_value(h, len(heat_weights))
        h.update(heat_weights.tobytes())
    _hash_value(h, coating_zone_weights is not None)
    if coating_zone_weights is not None:
        _hash_value(h, len(coating_zone_weights))
        h.update(coating_zone_weights.tobytes())
    have_avoid = avoid_polys is not None
    _hash_value(h, have_avoid)
    if have_avoid:
        _hash_value(h, len(props.avoid_attr))
        _hash_value(h, props.avoid_attr)
        _hash_value(h, len(avoid_polys))
        h.update(avoid_polys.tobytes())




    guides = guide_splines or []
    _hash_value(h, len(guides))
    for poly in guides:
        _hash_value(h, len(poly))
        h.update(poly.tobytes())

    have_rust = rust_weights is not None
    _hash_value(h, have_rust)
    if have_rust:
        _hash_value(h, props.rust_mask_source)
        _hash_value(h, len(rust_weights))
        h.update(np.ascontiguousarray(rust_weights, dtype=np.float64).tobytes())
    from . import new_effects, through_holes
    new_effects.hash_inputs(h, params, effect_inputs or {})
    through_holes.hash_inputs(h, params, _hash_value)
    from . import damaged_bevel
    damaged_bevel.hash_params(h, params)
    from . import concrete_pits
    concrete_pits.hash_params(h, params)
    from . import metal_tear
    metal_tear.hash_params(h,params)
    metal_tear.hash_inputs(h,bpy.context,obj)
    from . import clay
    clay.hash_params(h,params)
    clay.hash_inputs(h,obj)
    from . import voxel_erosion
    voxel_erosion.hash_params(h, params)
    voxel_erosion.hash_inputs(h, obj)
    from .effect_schema import EFFECT_FIELDS
    for module, fields in EFFECT_FIELDS.items():
        block = getattr(params, module)
        if block.enable:
            _hash_value(h, module)
            for name, _ in fields:
                _hash_value(h, getattr(block, name, None))
    for module in ('concrete','lath','brick'):
        block=getattr(params,module)
        if block.enable:_hash_value(h,getattr(block,'organic',False))
    from . import damage_proximity
    damage_proximity.hash_params(h,params,_hash_value)
    from . import fissure_realism
    fissure_realism.hash_props(h, props, params, _hash_value)
    wallpaper = params.wallpaper
    if wallpaper.enable:
        _hash_value(h, len(getattr(wallpaper, "flap_markers", ())))
        for value in getattr(wallpaper, "flap_markers", ()):_hash_value(h, value)
        _hash_value(h, getattr(wallpaper, "marker_only", False))
    if wallpaper.enable and getattr(wallpaper, 'finder_enable', False):
        for value in (wallpaper.finder_x, wallpaper.finder_y, wallpaper.finder_z):
            _hash_value(h, value)
    from . import asset_noise as _asset_noise
    _asset_noise.hash_props(h, props, params, _hash_value)


    fabric_block = getattr(params, "fabric", None)
    if fabric_block is not None and fabric_block.enable and hasattr(fabric_block, "tear_style"):
        _hash_value(h, "fabric-flat-surface-v3")
        for name in ("woven_threads", "tear_style", "tear_rip", "tear_corner", "tear_worn", "tear_puncture", "grain_snap",
                     "yarn_size", "flap_lift", "solid_bevel", "solid_bevel_segments",
                     "distribution_scale", "distribution_strength", "shape_noise_scale", "shape_noise_strength"):
            _hash_value(h, getattr(fabric_block, name, None))
    for array in (fabric_weights, fabric_avoid_polys):
        _hash_value(h, array is not None)
        if array is not None:
            _hash_value(h, len(array))
            h.update(array.tobytes())
    _hash_value(h, len(concrete_strokes or []))
    for stroke in concrete_strokes or []:
        _hash_value(h, len(stroke))
        h.update(stroke.tobytes())
    if params.coating_enable or params.rust_enable:
        _hash_value(h, "inline-coating4-rust2")
        _hash_value(h, params.rust_use_paint)
        _hash_value(h, params.surf_paint_snap)
        for paths, normals in ((coating_paths, coating_normals), (rust_paths, rust_normals)):
            _hash_value(h, len(paths or []))
            for path in paths or []:
                _hash_value(h, len(path))
                h.update(path.tobytes())
            if normals is not None:
                h.update(normals.tobytes())
        for name in dir(params):
            if name.startswith("coating_"):
                _hash_value(h, getattr(params, name))






    if params.fract_paint_only or params.surf_paint_only:
        _hash_value(h, "paint-v2-surface")
        for value in (params.fract_paint_only, params.surf_paint_only, params.surf_paint_snap):
            _hash_value(h, value)
        for level in (params.fract1, params.fract2, params.fract3):
            _hash_value(h, level.paint_radius)
        if params.fract_paint_only:
            _hash_value(h, 'paint-branch-shape-v2')
            for level in (params.fract2,params.fract3):
                for value in (level.length,level.paint_length_min,level.paint_distortion,level.paint_distortion_scale):
                    _hash_value(h,value)
        _hash_value(h, len(guide_levels or []))
        for value in guide_levels or []:
            _hash_value(h, value)
        paths = surface_paint_paths or []
        _hash_value(h, len(paths))
        for path in paths:
            _hash_value(h, len(path))
            h.update(path.tobytes())
        if surface_paint_normals is not None:
            h.update(surface_paint_normals.tobytes())
    _hash_value(h, getattr(params,"chip_seed",0))
    if hasattr(params, "chip_cluster"):
        _hash_value(h, "worn-looks-v2")
        for name in _WORN_V2_FIELDS:
            _hash_value(h, getattr(params, name))
        counts = list(params.corner_paint_count)
        _hash_value(h, len(counts))
        for value in counts:
            _hash_value(h, int(value))
        if counts:
            h.update(np.asarray(params.corner_paint_pts, dtype=np.float64).tobytes())
    if getattr(params, "build_paint_only", False):
        _hash_value(h, "bead-draw-zones")
        _hash_value(h, params.build_paint_radius)
        counts = list(params.build_paint_count)
        _hash_value(h, len(counts))
        for value in counts:
            _hash_value(h, int(value))
        if counts:
            h.update(np.asarray(params.build_paint_pts, dtype=np.float64).tobytes())
    if params.scratch.enable:
        _hash_value(h,"scratches-built-in-looks-v2")
    if params.glass.enable:
        _hash_value(h, "glass-opening-cell-fill-v4")
        _hash_value(h, params.glass.thickness_logic)
        if getattr(params.glass, 'fill_origin_enabled', False):
            for coord in (params.glass.fill_origin_x, params.glass.fill_origin_y, params.glass.fill_origin_z):
                _hash_value(h, coord)
    if params.breakers:_hash_value(h,"boolean-overlapping-shells-v3")
    if hasattr(params.plaster,'zone_weights') and len(params.plaster.zone_weights):
        _hash_value(h,'plaster-zone-v1')
        for weight in params.plaster.zone_weights:_hash_value(h,weight)
    if params.concrete.enable and hasattr(params.concrete,'surface_distortion'):
        for field in ('surface_distortion','surface_roughness','surface_strength'):
            _hash_value(h,getattr(params.concrete,field))
    if params.concrete.enable:_hash_value(h,"concrete-separate-reinforcement-v1")
    if params.surf_sec_amount>0:_hash_value(h,"surface-secondary-material-zone-v1")
    if params.cellf.enable:_hash_value(h,"cell-fracture-material-network-v2")
    damage_fp = int.from_bytes(h.digest(), "little")
    if params.cellf.enable:
        from . import cell_cuts
        cell_cuts.hash_inputs(h,obj)
    _hash_value(h,bool(props.split_effect_zones))


    if props.perf_mesh_preparation or props.perf_ngon_restore:
        _hash_value(h, 'host-performance-v2-split')
        _hash_value(h, props.perf_mesh_preparation)
        _hash_value(h, props.perf_ngon_restore)






    if params.fract_tile_offset > 0.0:
        for name in _BELOW_DAMAGE_FIELDS:
            _hash_value(h, getattr(params, name))
    if getattr(params, "fract_shard_enable", False):
        _hash_value(h, "shard-displace-v1")
        for name in _SHARD_FIELDS:
            _hash_value(h, getattr(params, name))
        shards = list(params.fract_shard_points)
        _hash_value(h, len(shards))
        for fp in shards:
            for value in (fp.x, fp.y, fp.z, fp.radius):
                _hash_value(h, float(value))







    if params.base_map and obj is not None:
        mat_index = np.empty(len(obj.data.polygons), dtype=np.int32)
        obj.data.polygons.foreach_get("material_index", mat_index)
        _hash_value(h, len(mat_index))
        h.update(mat_index.tobytes())
        _hash_value(h, len(obj.data.materials))

    _hash_value(h, len(params.fract_proximity))
    for fp in params.fract_proximity:
        for v in (fp.x, fp.y, fp.z, fp.radius, fp.strength):
            _hash_value(h, v)
    _hash_value(h, len(params.breakers))
    for spec in params.breakers:
        _hash_value(h, spec.mode)
        _hash_value(h, spec.flip)
        _hash_value(h, spec.seed)
        _hash_value(h, spec.length)
        _hash_value(h, spec.width)
        _hash_value(h, spec.radius)
        _hash_value(h, spec.thickness)
        _hash_value(h, spec.resolution)
        _hash_value(h, spec.strength)
        _hash_value(h, spec.macro_h)
        _hash_value(h, spec.turb_h)
        _hash_value(h, spec.fine_h)
        _hash_value(h, spec.noise_scale)
        _hash_value(h, spec.rim_amp)
        _hash_value(h, spec.rim_scale)
        for v in spec.rel:
            _hash_value(h, v)
        _hash_value(h, spec.custom_hash)
        _hash_value(h, spec.custom_sheet)
        _hash_value(h, spec.rebar)
        for name in ('surf_mode','tex_path','tex_scale','tex_strength','tex_edge','tex_exit','tex_macro','tex_offset_x','tex_offset_y','tex_offset_z','smooth_slope','smooth_width'):
            _hash_value(h,getattr(spec,name))
        from . import break_surface as _break_surface
        _break_surface.hash_spec(h, spec, _hash_value)
    for name in _BREAK_RESOLVED:
        _hash_value(h, getattr(params, name))
    _hash_value(h, getattr(params.break_rebar, 'generated_geometry', True))
    _hash_value(h,getattr(params.break_rebar,'experimental_riven',False))
    for name in ('stable_volume', 'volume_axis', 'volume_budget', 'volume_clearance', 'volume_min_length', 'volume_min_section', 'volume_origin_x', 'volume_origin_y', 'volume_origin_z'):
        _hash_value(h,getattr(params.break_rebar,name,False))
    from .rebar import FIELDS
    for name,kind,default,is_length in FIELDS:
        if hasattr(params.break_rebar,name):_hash_value(h,getattr(params.break_rebar,name))
    _hash_value(h, getattr(params.break_rebar, 'existing_damage', False))
    _hash_value(h,getattr(params.break_rebar,'axis_mask',0))
    if params.break_rebar.material!=0:
        for name in ('up_x','up_y','up_z'):_hash_value(h,getattr(params.break_rebar,name))
    for values in (params.break_rebar.stroke_pts,params.break_rebar.stroke_count):
        _hash_value(h,len(values))
        for value in values:_hash_value(h,value)
    from . import wall_hole as _wall_hole
    for name in _wall_hole.HASH_FIELDS:_hash_value(h,getattr(params.wall_hole,name))
    _hash_value(h,len(params.wall_hole.sites))
    for value in params.wall_hole.sites:_hash_value(h,value)
    full_fp = int.from_bytes(h.digest(), "little")
    return full_fp, damage_fp


def _fabric_tear_sources(p, effect_inputs):
    """Names of the Fabric tear sources that can actually cut something."""
    found = []
    if p.fabric_holes_enable and p.fabric_hole_amount > 0:
        found.append('Interior Tears')
    if p.fabric_edge_enable and p.fabric_edge_amount > 0:
        found.append('Edge Tears')
    if getattr(p, 'fabric_hang_enable', False) and getattr(p, 'fabric_hang_amount', 0) > 0:
        found.append('Hanging Tears')
    inputs = effect_inputs or {}
    if p.fabric_paint_holes_enable and len(inputs.get('fabric_holes') or ()):
        found.append('drawn holes')
    if p.fabric_paint_edges_enable and len(inputs.get('fabric_edges') or ()):
        found.append('drawn torn edges')
    return found


def _enabled_effect_labels(p):
    from . import sidebar
    labels = []
    for row in sidebar.EFFECTS:
        key, label, prop = row[0], row[1], row[2]
        if prop and getattr(p, prop, False):
            labels.append(label)
    return labels


def passthrough_reason(obj, params, prep, polys, effect_inputs, out):
    """The report for a result that passed through unchanged - specific
    instead of the generic "not a closed solid": topology counts, the Fabric
    mode that would work, or "no tears" when nothing could cut. None when
    the core already explained itself (input_valid) and nothing is missing."""
    p = obj.project_damage
    fabric_on = (p.fabric_enable or p.fabric_solid_enable) and not p.all_off
    if fabric_on and not _fabric_tear_sources(p, effect_inputs):
        mode = 'Fabric Solidify' if p.fabric_solid_enable else (
            'Fabric Classic (Flat)' if p.fabric_flat else 'Fabric Classic')
        return ({'WARNING'}, "ProjectDamage: %s has no tears to cut - Interior/Edge/Hanging Tears are off "
                             "or their Amount is 0, and no drawn contour was usable - so the mesh "
                             "passed through unchanged" % mode)
    if out.get("input_valid"):
        if fabric_on:
            return ({'INFO'}, "ProjectDamage: the fabric tears did not reach this mesh "
                              "(try a larger tear size or more holes), so it passed through unchanged")
        return None
    if _nothing_asked_for(params):
        return ({'INFO'}, "ProjectDamage: no damage is enabled, so the mesh passed through unchanged")
    from . import fabric_prep
    n_open, n_nm = fabric_prep.topology_report(np.asarray(polys))
    if p.fabric_solid_enable and not p.all_off:
        stats = getattr(prep, 'stats', None) or {}
        failed = int(stats.get('failed', 0))
        return ({'WARNING'},
                "ProjectDamage: Fabric Solidify could not close this cloth (%d open edges, %d non-manifold "
                "edges, %d pieces could not be closed), so it passed through unchanged. Fabric Classic "
                "cuts open cloth directly" % (n_open, n_nm, failed))
    if p.fabric_enable and p.fabric_flat and not p.all_off:
        return ({'WARNING'},
                "ProjectDamage: Fabric Classic (Flat) produced no tears on this mesh (%d open edges, %d "
                "non-manifold edges), so it passed through unchanged" % (n_open, n_nm))
    effects = [label for label in _enabled_effect_labels(p) if label not in ('Fabric Classic',)]
    skipped = (" Skipped: " + ", ".join(effects[:8]) + ("..." if len(effects) > 8 else "") + ".") if effects else ""
    return ({'WARNING'},
            "ProjectDamage: the mesh is not a closed solid (%d open edges, %d non-manifold edges), so it "
            "passed through unchanged.%s Set Mesh Preparation to Auto or Surface, or use Fabric Classic "
            "for cloth" % (n_open, n_nm, skipped))


def prepare_glass_drawn_mesh(obj):
    """Keep an untouched source datablock before subdividing a single-face input."""
    if not obj.project_damage.glass_drawn_enable or obj.project_damage.all_off:return False
    if len(obj.data.polygons)!=1:return False
    import bmesh
    original=obj.data
    mesh=original.copy();mesh.name=original.name+'_GlassDrawn'
    bm=bmesh.new()
    try:
        bm.from_mesh(mesh)
        bmesh.ops.subdivide_edges(bm,edges=list(bm.edges),cuts=1,use_grid_fill=True)
        if len(bm.faces)<2:
            bmesh.ops.triangulate(bm,faces=list(bm.faces))
        bm.to_mesh(mesh)
    finally:bm.free()
    if len(mesh.polygons)<2:
        bpy.data.meshes.remove(mesh)
        raise ValueError('Glass Drawn requires a face that can be subdivided')
    obj['project_damage_glass_source_mesh']=original
    obj.data=mesh
    return True


# обновляем результат для выбранного исходного объекта
def rebuild_object(context,obj):
    output_collections.repair(context,obj)
    from . import background_compute as bg, live
    if not live._HISTORY_RESTORING and bg.async_for(obj):
        result=result_of(obj)
        if result is not None:
            bg.queue(obj,'build_worn',lambda: _rebuild_steps(bpy.context,obj))
            return result,[({'INFO'},'Project Damage: queued for background calculation')]
    return bg.immediate(_rebuild_steps(context,obj))

# сначала собираем входные данные, затем рассчитываем и размещаем результат
def _rebuild_steps(context, obj, *, capture_solid=False, fill_modules=None):
    """Source object -> core -> `<name>_project_damage` result object: the whole body
    of `PROJECT_DAMAGE_OT_update.execute` minus the operator plumbing, plus the session
    cache wiring. This is the SINGLE entry point both the Update operator
    and the deferred live-update timer (live.py) call - the
    timer has no operator `self` to report through, which is why messages
    come back as data instead of being reported directly here.

    Returns (result_obj, report_tuples). `result_obj` is None on failure (a
    bad mesh, a core load failure, an exception out of the core), and then
    `report_tuples` ends with an ERROR message - preceded by the modifier
    warning too if one was already queued before the failure, so a caller
    replaying every tuple in order never silently drops it; otherwise
    `result_obj` is the placed `<name>_project_damage` object and `report_tuples` holds
    the INFO/WARNING summary the caller should surface."""
    from . import live
    if live._HISTORY_RESTORING:
        return None, [({'WARNING'}, 'Wait for Undo/Redo to finish before regenerating damage')]
    reports = []
    from . import boolean_stack, incremental, init_parenting, wall_stack, through_holes
    wall_instances = wall_stack.SERIAL in obj and fill_modules is None
    wall_layers = wall_stack.layers(obj) if wall_instances else []
    boolean_instances = boolean_stack.SERIAL in obj and (fill_modules is None or 'boolean' in fill_modules)
    boolean_layers = boolean_stack.layers(obj) if boolean_instances else []
    if not capture_solid and init_parenting.normalize_legacy(context,obj):
        reports.append(({'INFO'},'Normalized source scale while preserving its world-space geometry'))
    previous_result=result_of(obj)
    drawing_frames = live.drawing_frames(obj) if not capture_solid else None
    if not boolean_instances and not capture_solid and previous_result is not None and previous_result.mode=='OBJECT' and incremental.body_unchanged(context,obj,previous_result):
        from . import generated_assets, ivy, paper_generator, roots
        generated_assets.after_build(context,obj,previous_result,reports)
        ivy.set_visibility(obj,obj.project_damage.ivy_enable and not obj.project_damage.all_off)
        roots.set_visibility(obj,obj.project_damage.roots_enable and not obj.project_damage.all_off)
        ivy.queue_update(obj);roots.queue_update(obj)
        from . import damage_generator_assets
        damage_generator_assets.visibility(context,obj,previous_result)
        live.remember_drawing_frames(obj, drawing_frames)
        return previous_result,reports+[({'INFO'},'Project Damage: unchanged body retained; updated changed generators only')]
    from . import material_uv
    uv_snapshots=material_uv.capture(obj) if not capture_solid else None
    if not capture_solid and prepare_glass_drawn_mesh(obj):
        reports.append(({'INFO'},'Glass Drawn: subdivided single-face input; original mesh kept as backup'))
    if (fill_modules is None and obj.project_damage.ui_boolean_enable and obj.project_damage.rebar_enable
            and obj.project_damage.rebar_fill == '1' and obj.project_damage.rebar_brick_3d
            and not obj.project_damage.all_off):
        from . import brick3d
        try:
            brick3d.validate_library(obj.project_damage)
        except (OSError, RuntimeError, ValueError) as exc:
            return None, [({'ERROR'}, '3D Bricks: ' + str(exc))]
    current_result = result_of(obj)
    if current_result is not None and current_result.mode != 'OBJECT':
        return None, [({'WARNING'}, 'Exit Edit/Sculpt mode on the deformable mesh before regenerating damage')]
    try:
        core = core_loader.load()
    except RuntimeError as exc:
        return None, [({'ERROR'}, str(exc))]
    needs_surface_update = any(getattr(obj.project_damage,m+'_mode')=='2' or getattr(obj.project_damage,m+'_model_type')=='ORGANIC' for m in ('concrete','lath','brick'))
    if needs_surface_update and not getattr(core,'organic_damage',False):
        return None,[({'ERROR'},'Proximity, Organic Models and experimental wood need the updated core. Save your scene and restart Blender.')]
    prefs_entry = context.preferences.addons.get(__package__)
    if (prefs_entry is not None and getattr(prefs_entry.preferences, 'fracture_surface_proxy', False)
            and not hasattr(core, 'surface_guide_field')):
        return None, [({'ERROR'}, 'Surface Guide needs the updated core. Save your scene and restart Blender.')]
    if not hasattr(core.Params(), 'profile_core'):
        return None, [({'ERROR'}, 'Performance controls need the updated native core. Restart Blender after installing the update.')]
    if not hasattr(core.Params(), 'fract_through_fill'):
        return None, [({'ERROR'}, 'Fracture Volume needs the updated native core. Restart Blender after installing the update.')]
    if (not hasattr(core.Params(), "deposit_amount") or not hasattr(core.Params(), "coating_zone_enable")
            or not hasattr(core.Params(), "fract_glass_branch")
            or not hasattr(core.Params(), "fract_paint_only")
            or not hasattr(core.Params(), "fabric") or not hasattr(core.Params(), "concrete")
            or not hasattr(core.Params(), "rust_use_paint") or not hasattr(core.Params(), "wallpaper") or not hasattr(core.Params(), "fissure")
            or not hasattr(core.Params(), "metal") or not hasattr(core.Params(), "bevel") or not hasattr(core.Params(), "scratch") or not hasattr(core.Params(), "break_rebar")
            or not hasattr(core.Params().break_rebar,'set_strokes')
            or not getattr(core, "wallpaper_material_regions", False)
            or not getattr(core, "wallpaper_parameter_recovery", False)
            or not getattr(core, "brick_material_regions", False)
            or not getattr(core, "wallpaper_intact_material", False)):
        return None, [({'ERROR'}, "ProjectDamage: this addon needs the updated Materials core. Install the current ZIP and restart Blender.")]



    preserve = bool(getattr(obj.project_damage, 'preserve_data', False))
    if not hasattr(core.Params(), 'fract_through'):
        return None, [({'ERROR'}, 'Fracture Through needs the updated core. Restart Blender after installing the update.')]
    if not hasattr(core.Params().fract1, 'length'):
        return None, [({'ERROR'}, 'Fracture Length needs the updated core. Restart Blender after installing the update.')]
    if not hasattr(core.Params(),'debris'):
        return None,[({'ERROR'},'Through/Debris needs the updated core. Restart Blender after installing the update.')]
    if not hasattr(core.Params().debris,'inner_uv_scale'):
        return None,[({'ERROR'},'Debris material/UV transfer needs the updated core. Restart Blender after installing the update.')]
    if not hasattr(core.Params().debris,'smooth_angle'):
        return None,[({'ERROR'},'Debris smooth normals needs the updated core. Restart Blender after installing the update.')]
    if not hasattr(core.Params(),'fract_type'):
        return None,[({'ERROR'},'Fracture Types needs the updated core. Restart Blender after installing the update.')]
    if not hasattr(core.Params(),'rbf'):
        return None,[({'ERROR'},'Rigid Body Fracture needs the updated core. Restart Blender after installing the update.')]
    if not hasattr(core.Params(),'cellf'):
        return None,[({'ERROR'},'Cell Fracture needs the updated core. Rebuild/install the core and restart Blender.')]
    if not hasattr(core.Params(),'fract_physics'):
        return None,[({'ERROR'},'Non-through Chunks needs the updated core. Restart Blender after installing the update.')]
    if not hasattr(core.Params(),'fract_helper_snap'):
        return None,[({'ERROR'},'Fracture Helper needs the updated core. Restart Blender after installing the update.')]
    if not hasattr(core,'compute_damage_shading'):
        return None,[({'ERROR'},'Damage Shading v2 needs the updated core. Restart Blender after installing the update.')]
    if not hasattr(core.Params(),'surf_type'):
        return None,[({'ERROR'},'Surface Wood Knife needs the updated core. Restart Blender after installing the update.')]
    from . import rbf_output,cellf_output
    if not capture_solid and any(o.rigid_body is not None for o in rbf_output.objects_of(obj)+cellf_output.objects_of(obj)):
        world=context.scene.rigidbody_world
        if ((world is not None and world.point_cache.is_baked) or context.scene.frame_current!=context.scene.frame_start
                or (context.screen is not None and context.screen.is_animation_playing)):
            return None,[({'ERROR'},'Existing rigid bodies: free the physics bake and return to the scene start frame before rebuilding.')]
    if obj.project_damage.draw_mask_enable and not getattr(core, 'draw_mask', False):
        return None, [({'ERROR'}, 'Draw Mask needs the updated core. Restart Blender after installing the update.')]
    if not getattr(core,'worn_v2',False):

        reports.append(({'WARNING'},'Edge / Corner / Surface / Fracture / Bead / Corner Piles / Deposits: rebuild the core and restart Blender to get the new looks, Shard Displace and Corner Draw.'))
    if obj.project_damage.chips_mode=='PAINT' and not getattr(core,'edge_paint',False):
        return None,[({'ERROR'},'Edge Draw needs the updated core. Install the update and restart Blender.')]
    factor = unit_factor(context)





    from . import input_prep
    try:
        prep_start = time.perf_counter()
        prepared = yield from input_prep.plan_steps(context, obj, factor, solid_damage=fill_modules is not None)
        if context.window_manager.project_damage_core_profiler:
            print('[pd-profile-host] Mesh preparation (%s): %.4fs' % (
                'optimized' if obj.project_damage.perf_mesh_preparation else 'original',
                time.perf_counter() - prep_start), flush=True)
    except ValueError as exc:
        return None, [({'ERROR'}, "ProjectDamage: %s" % exc)]
    snap = prep = None
    if prepared is None:
        try:
            points, polys = read_mesh(obj.data)
        except ValueError as exc:
            return None, [({'ERROR'}, "ProjectDamage: %s" % exc)]
        points = points * factor
    else:
        snap, prep = prepared
        points, polys = prep.points, prep.polys
        reports.append(({'INFO'}, "ProjectDamage: mesh prepared (%s): %s"
                        % (prep.path, "; ".join(prep.notes) or "no changes needed")))
    from . import damage_shading
    source_corner_normals = (damage_shading.source_normals(obj.data, snap, prep)
                             if fill_modules is None and obj.project_damage.damage_shading else None)
    # The AUTHORED mesh, not the evaluated one. C4D resolves its child
    # through the hierarchy clone, so deformers there DO apply; the Blender
    # equivalent (evaluated_get + to_mesh) is not built, so the difference
    # is said out loud rather than left to surprise.
    if (snap is None or not snap.evaluated) and any(mod.show_viewport for mod in obj.modifiers):
        reports.append(({'WARNING'}, "ProjectDamage: this object's modifiers are ignored - "
                                     "the damage is cut into the base mesh. Apply "
                                     "them first, or enable Mesh Preparation > Use Modifiers"))




    if snap is None:
        uv_layer = obj.data.uv_layers.active if obj.project_damage.keep_uvs or preserve else None
        uvs_in = read_uvs(obj.data, uv_layer) if uv_layer is not None else None
        uv_layer_name = uv_layer.name if uv_layer is not None else None
    else:
        uv_layer_name = snap.uv_active if obj.project_damage.keep_uvs or preserve else None
        uvs_in = prep.uvs(snap.uv[uv_layer_name]) if uv_layer_name is not None else None

    # Damage mask: read the AUTHORED mesh, same
    # as points/uvs above. A group NAME that does not resolve is neutral -
    # mask_weights_in stays None, matching an empty C4D Field list.
    vg = obj.vertex_groups.get(obj.project_damage.mask_group) if obj.project_damage.mask_group else None
    if snap is None:
        mask_weights_in = read_mask_weights(obj.data, vg.index) if vg is not None else None
    else:
        mask_weights_in = prep.point_values(snap.groups[vg.index]) if vg is not None else None



    plaster_group=obj.vertex_groups.get(obj.project_damage.plaster_zone_group) if obj.project_damage.plaster_zone_group else None
    plaster_zone_weights=(None if plaster_group is None else
        read_mask_weights(obj.data,plaster_group.index) if snap is None else
        prep.point_values(snap.groups[plaster_group.index]))



    if snap is None:
        avoid_attr = (obj.data.attributes.get(obj.project_damage.avoid_attr)
                     if obj.project_damage.avoid_attr else None)
        if avoid_attr is not None and (avoid_attr.domain != 'FACE' or avoid_attr.data_type != 'BOOLEAN'):
            avoid_attr = None
        avoid_polys_in = read_avoid_polys(obj.data, avoid_attr) if avoid_attr is not None else None
    else:


        user_avoid = snap.face_bools.get(obj.project_damage.avoid_attr) if obj.project_damage.avoid_attr else None
        avoid_polys_in = (prep.avoid_polys(user_avoid)
                          if user_avoid is not None or prep.helper.any() else None)





    guides_in = []
    if fill_modules is None and obj.project_damage.fract_mode == 'PROCEDURAL' and not obj.project_damage.all_off and (obj.project_damage.fract_enable or obj.project_damage.fract_through_enable):
        guides_in = read_guide_splines(context.evaluated_depsgraph_get(), obj,
                                       obj.project_damage.guides)


        guides_in = [g * factor for g in guides_in]

    from . import paint
    drawn, guide_levels, surface_paths, surface_normals, paint_reports = paint.collect(
        context, obj, points, polys, factor)
    reports.extend(paint_reports)
    if obj.project_damage.fract_mode == 'PAINT':
        guides_in = drawn

    heat_weights_in = None
    zone_weights_in = None
    from . import extra_effects
    try:
        if fill_modules is None:
            rust_weights_in, fabric_weights_in, fabric_avoid_in, concrete_strokes = extra_effects.resolve(
                context, obj, snap, prep, points, polys, factor, avoid_polys_in)
        else:
            rust_weights_in = fabric_weights_in = fabric_avoid_in = None
            p = obj.project_damage
            concrete_strokes = (extra_effects.concrete_draw_paths(context, obj, points, polys, factor)
                                if 'concrete' in fill_modules and p.concrete_mode == '1'
                                and (p.concrete_enable or p.concrete_v2_enable) else [])
    except ValueError as exc:
        return None, [({'ERROR'}, "ProjectDamage: " + str(exc))]
    concrete_props = obj.project_damage
    if (not wall_instances and (concrete_props.concrete_enable or concrete_props.concrete_v2_enable)
            and concrete_props.concrete_mode == '1' and not concrete_strokes and not concrete_props.all_off):
        reports.append(({'WARNING'}, "Concrete Paint: no current-frame strokes; no concrete damage generated"))

    coating_paths, coating_normals = [], None
    rust_paths, rust_normals = [], None
    if fill_modules is None and obj.project_damage.coating_enable and obj.project_damage.coating_zone_enable and not obj.project_damage.all_off:
        coating_paths, coating_normals = paint.read_regions(context, obj, obj.project_damage.coating_drawing, points, polys, factor)
        if not coating_paths:
            reports.append(({'WARNING'}, 'Coating Paint: no closed contours; only procedural Amount can generate peeling'))
    from . import new_effects
    effect_inputs, effect_reports = new_effects.collect(context, obj, points, polys, factor)
    reports.extend(effect_reports)
    try:
        from . import fracture_stack
        fracture_props = obj.project_damage
        if fill_modules is None:
            fracture_stack.ensure(obj)
            fracture_props = next(iter(fracture_stack.layers(obj)), ('', fracture_props))[1]
        params = fill_params(fracture_props, core, obj=obj, geometry=(points,polys,factor))
    except ValueError as exc:
        return None,reports+[({'ERROR'},'Project Damage: '+str(exc))]
    from . import wallpaper_controls
    wallpaper_controls.pack(context, obj, params.wallpaper, factor)
    finders = wallpaper_controls.finders(obj.project_damage)
    if params.wallpaper.enable and finders:
        wallpaper = params.wallpaper
        if not hasattr(wallpaper, 'finder_enable'):
            return None, [({'ERROR'}, 'Wallpaper Finder needs the updated geometry core. Restart Blender after installing it.')]
        graph = context.evaluated_depsgraph_get()
        inverse = obj.evaluated_get(graph).matrix_world.inverted_safe()
        wallpaper.finder_points = [v for finder in finders for v in ((inverse @ finder.evaluated_get(graph).matrix_world.translation) * factor)]
        wallpaper.finder_enable = True
    centre = obj.project_damage.glass_fill_origin
    if params.glass.fill_hole and centre is not None:
        glass = params.glass
        if not hasattr(glass, 'fill_origin_enabled'):
            return None, [({'ERROR'}, 'Glass Density Centre needs the updated geometry core. Restart Blender.')]
        graph = context.evaluated_depsgraph_get()
        position = centre.evaluated_get(graph).matrix_world.translation
        inverse = obj.evaluated_get(graph).matrix_world.inverted_safe()
        glass.fill_origin_x, glass.fill_origin_y, glass.fill_origin_z = (inverse @ position) * factor
        glass.fill_origin_enabled = True
    if params.plaster.enable and plaster_zone_weights is not None and hasattr(params.plaster,'zone_weights'):
        params.plaster.zone_weights=plaster_zone_weights.tolist()
    from .impact_deform_tools import fill_protect_weights
    fill_protect_weights(obj, params, snap, prep)
    if obj.project_damage.lath_riven_enable and not hasattr(params.lath, 'style'):
        reports.append(({'WARNING'}, 'Riven Lath: rebuild the native core and restart Blender; '
                         'until then it builds like Lath Damage'))
    if obj.project_damage.lath_framing_enable and not hasattr(params.lath, 'noggin_spacing'):
        reports.append(({'WARNING'}, 'Stud Framing: rebuild the native core and restart Blender; '
                         'until then it builds like Lath Damage'))
    try:
        if fill_modules is None or 'hit' in fill_modules:
            params.hit.empties = new_effects.fill_hit_empties(context, obj, core, factor)
        if fill_modules is None:
            params.metal.impactors = new_effects.fill_metal_impactors(context, obj, core, factor)
            params.metal.empties = new_effects.fill_hit_empties(context, obj, core, factor, 'metal_crush') if params.metal.style == 1 else []
            params.blast.empties = new_effects.fill_blast_empties(context, obj, core, factor)
    except ValueError as exc:
        return None, [({'ERROR'}, str(exc))]
    if not boolean_instances and (fill_modules is None or {'hang', 'boolean'}.intersection(fill_modules)):
        params.breakers = fill_breakers(obj, obj.project_damage, core, factor)
    if fill_modules is None:
        from . import concrete_pits, metal_tear, clay
        concrete_pits.before_core(context, obj, params, snap, prep, points, polys, factor, reports)
        metal_tear.before_core(context,obj,params,snap,prep,points,polys,factor,reports)
        clay.before_core(context,obj,params,snap,prep,points,polys,factor,reports)
    from . import voxel_erosion
    voxel_erosion.before_core(context, obj, params, snap, prep, points, polys, factor, reports)
    if fill_modules is None or 'concrete' in fill_modules:
        from . import pipes, cables
        for service in (pipes, cables):
            if wall_instances:
                continue
            if fill_modules is None and service.wanted(obj.project_damage) and obj.get('project_damage_uid') is None:
                _get_cache(obj,core)
            if fill_modules is None or service.damage_wanted(obj.project_damage):
                concrete_strokes = service.before_core(context, obj, params, points, polys, factor, concrete_strokes, reports)
    if boolean_instances:
        try:
            if not hasattr(params, 'boolean_layers'):
                raise RuntimeError('Restart Blender to load the core with multiple Boolean fills')
            boolean_stack.pack(context,obj,core,params,factor,boolean_layers)
            if fill_modules is None:
                from . import pipes, cables
                for _, layer in boolean_layers:
                    for service in (pipes, cables):
                        if service.boolean_wanted(layer):
                            service.before_core(context,obj,params,points,polys,factor,[],reports,props=layer)
        except (ValueError,RuntimeError) as exc:
            return None, reports + [({'ERROR'}, str(exc))]
    elif fill_modules is None or ('boolean' in fill_modules and params.break_rebar.material in {3, 4}):
        from .rebar import fill_strokes
        try:fill_strokes(context,obj,params.break_rebar,factor)
        except (ValueError,RuntimeError) as exc:return None,[({'ERROR'},'Rebar drawing: '+str(exc))]
    if prep is not None:


        params.base_map = True
        if prep.path == "surface":



            params.topology = 3
            for name in ("avoid_chips", "avoid_surf", "avoid_build", "avoid_fract", "avoid_corner"):
                if hasattr(params, name):
                    setattr(params, name, True)
    if fill_modules is None:
        params.fract_proximity = [] if params.fract_paint_only else fill_fract_proximity(obj, fracture_props, core, factor)
    if fill_modules is None and getattr(params, 'fract_shard_enable', False):
        params.fract_shard_points = fill_shard_points(obj, obj.project_damage, core, factor)
        if not params.fract_shard_points:
            reports.append(({'WARNING'}, 'Shard Displace: link a Points collection of empties (or Proximity points).'))
    if getattr(params, 'corner_paint_only', False) and not fill_corner_paint(context, obj, obj.project_damage, params, factor):
        reports.append(({'WARNING'}, 'Corner Draw: no strokes at this frame; no corners broken.'))
    if getattr(params, 'build_paint_only', False) and not fill_build_paint(context, obj, obj.project_damage, params, factor):
        reports.append(({'WARNING'}, 'Bead Draw Zones: no strokes at this frame; no bead.'))
    if (obj.project_damage.build_enable and obj.project_damage.build_mode == 'PAINT'
            and not hasattr(params, 'build_paint_only')):
        reports.append(({'WARNING'}, 'Bead Draw Zones needs the rebuilt core; using Edges Covered.'))
    if hasattr(params, "preserve_surface_shells"):
        params.preserve_surface_shells = bool(not capture_solid and prep is not None and prep.path == "surface" and prep.helper.any())
    if fill_modules is None:
        fracture_stack.pack(context,obj,core,params,points,polys,factor,reports)
        if params.fracture_layers:
            guides_in=[];guide_levels=[]
        if wall_instances:
            if not hasattr(params, 'wall_layers'):
                return None, reports + [({'ERROR'}, 'Restart Blender to load the core with multiple wall generators')]
            effect_inputs.pop('lath_strokes', None)
            concrete_strokes = []
            _get_cache(obj, core)
            try:
                wall_stack.pack(context, obj, core, params, points, polys, factor, reports, wall_layers)
            except ValueError as exc:
                return None, reports + [({'ERROR'}, str(exc))]
    else:
        from . import fill_exact, background_compute
        fill_exact.filter_params(params, fill_modules, effect_inputs)
    try:
        through_holes.pack(context, obj, core, params, factor, wall_layers)
    except (ValueError, np.linalg.LinAlgError) as exc:
        return None, reports + [({'ERROR'}, str(exc))]
    if fill_modules is not None:
        params.damage_shading = False
        try:
            out = yield from background_compute.native(
                core, 'build_worn', points, polys, params, uvs=uvs_in,
                mask_weights=mask_weights_in, mask_amount=obj.project_damage.mask_amount / 100.0,
                mask_size=obj.project_damage.mask_size / 100.0, avoid_polys=avoid_polys_in,
                surface_paint_paths=surface_paths if 'surface' in fill_modules else [],
                surface_paint_normals=surface_normals if 'surface' in fill_modules else None,
                concrete_strokes=concrete_strokes if 'concrete' in fill_modules else [],
                **effect_inputs)
        except Exception as exc:
            return None, reports + [({'ERROR'}, 'Project Damage could not reconstruct the cut: ' + str(exc))]
        return (out, prep, factor, points, polys), reports
    _full_fp, damage_fp = fingerprints(obj, obj.project_damage, core, points, polys, params,
                                       factor, uvs=uvs_in, mask_weights=mask_weights_in,
                                       avoid_polys=avoid_polys_in,
                                       guide_splines=guides_in, heat_weights=heat_weights_in,
                                       coating_zone_weights=zone_weights_in, guide_levels=guide_levels,
                                       surface_paint_paths=surface_paths, surface_paint_normals=surface_normals, rust_weights=rust_weights_in,
                                       fabric_weights=fabric_weights_in, fabric_avoid_polys=fabric_avoid_in,
                                       concrete_strokes=concrete_strokes, coating_paths=coating_paths, coating_normals=coating_normals,
                                       rust_paths=rust_paths, rust_normals=rust_normals, effect_inputs=effect_inputs)
    from . import incremental
    incremental.prepare(obj,core,params,points,polys,
        uvs=uvs_in,src_corner_normals=source_corner_normals,mask_weights=mask_weights_in,mask_amount=obj.project_damage.mask_amount,
        mask_size=obj.project_damage.mask_size,avoid_polys=avoid_polys_in,
        guide_splines=guides_in,guide_levels=guide_levels,heat_weights=heat_weights_in,
        coating_zone_weights=zone_weights_in,surface_paint_paths=surface_paths,
        surface_paint_normals=surface_normals,rust_weights=rust_weights_in,
        fabric_weights=fabric_weights_in,fabric_avoid_polys=fabric_avoid_in,
        concrete_strokes=concrete_strokes,coating_paint_paths=coating_paths,
        coating_paint_normals=coating_normals,rust_paint_paths=rust_paths,
        rust_paint_normals=rust_normals,**effect_inputs)
    cache = _get_cache(obj, core)
    t0 = time.perf_counter()


    final_shading = params.damage_shading
    params.damage_shading = False
    params.profile_core = int(getattr(context.window_manager, 'project_damage_core_profiler', False))
    try:
        from . import background_compute
        out = yield from background_compute.native(core, 'build_worn', points, polys, params, cache=cache,
                              cache_fingerprint=damage_fp, uvs=uvs_in,
                              mask_weights=mask_weights_in, mask_amount=obj.project_damage.mask_amount / 100.0,
                              mask_size=obj.project_damage.mask_size / 100.0, avoid_polys=avoid_polys_in,
                              guide_splines=guides_in, heat_weights=heat_weights_in,
                              coating_zone_weights=zone_weights_in, guide_levels=guide_levels,
                              surface_paint_paths=surface_paths, surface_paint_normals=surface_normals, rust_weights=rust_weights_in,
                              fabric_weights=fabric_weights_in, fabric_avoid_polys=fabric_avoid_in,
                              concrete_strokes=concrete_strokes,
                              coating_paint_paths=coating_paths, coating_paint_normals=coating_normals,
                              rust_paint_paths=rust_paths, rust_paint_normals=rust_normals,
                              src_corner_normals=source_corner_normals, **effect_inputs)
    except background_compute.Deferred:
        return result_of(obj), reports + [({'INFO'}, 'ProjectDamage: calculation queued in background')]
    except Exception as exc:
        reports.append(({'ERROR'}, "ProjectDamage: the core refused this mesh: %s" % exc))
        return None, reports
    finally:
        params.damage_shading = final_shading
    if capture_solid:
        return (out, prep, factor, points, polys), reports
    ms = (time.perf_counter() - t0) * 1000.0


    out.pop('corner_normals', None)
    if prep is not None and out.get("passthrough") and params.rust_enable:


        for key in ("rust", "rust_streak", "rust_source"):
            values = out.get(key)
            if values is not None and len(values) == len(prep.points):
                restored = np.zeros(len(snap.points), dtype=np.float32)
                seen = np.zeros(len(snap.points), dtype=bool)
                for i, source_index in enumerate(prep.point_src):
                    if not seen[source_index]:
                        restored[source_index] = values[i]
                        seen[source_index] = True
                out[key] = restored
    if prep is not None:


        if len(out.get('debris_base_poly',())):
            # Native debris IDs refer to PREPARED polygons, not authored
            # n-gons. Never use them to index original material slots directly.
            bp=np.asarray(out['debris_base_poly'],dtype=np.int32)
            mapped=np.full(len(bp),-1,dtype=np.int32)
            valid=(bp>=0)&(bp<len(prep.poly_src))
            mapped[valid]=prep.poly_src[bp[valid]]
            mapped[np.flatnonzero(valid)[prep.helper[bp[valid]]]]=-1
            out=dict(out);out['debris_base_poly']=mapped
        if len(out.get('rbf_base_poly',())):
            bp=np.asarray(out['rbf_base_poly'],dtype=np.int32)
            mapped=np.full(len(bp),-1,dtype=np.int32)
            valid=(bp>=0)&(bp<len(prep.poly_src))
            mapped[valid]=prep.poly_src[bp[valid]]
            mapped[np.flatnonzero(valid)[prep.helper[bp[valid]]]]=-1
            out=dict(out);out['rbf_base_poly']=mapped
        if len(out.get('cellf_base_poly',())):
            bp=np.asarray(out['cellf_base_poly'],dtype=np.int32)
            mapped=np.full(len(bp),-1,dtype=np.int32)
            valid=(bp>=0)&(bp<len(prep.poly_src))
            mapped[valid]=prep.poly_src[bp[valid]]
            mapped[np.flatnonzero(valid)[prep.helper[bp[valid]]]]=-1
            out=dict(out);out['cellf_base_poly']=mapped
        try:
            out = prep.finish(out, point_keys=("heat_exposure", "heat_char", "heat_char_depth", "rust", "rust_streak", "rust_source", "rust_source_streak", "edge_wear", "bevel_damage", "scratch", "metal_dent", "metal_crease", "metal_contact"))
        except ValueError as exc:
            reports.append(({'ERROR'}, "ProjectDamage: %s" % exc))
            return None, reports
        if out.get("removed_helper_faces"):
            reports.append(({'INFO'}, "ProjectDamage: %d temporary surface faces removed"
                            % out["removed_helper_faces"]))



    if obj.project_damage.quad_remesh and not out.get("passthrough"):
        from .quad_remesh import quad_remesh, Settings as QuadSettings
        qs = QuadSettings()
        qs.output = obj.project_damage.quad_remesh_output
        qs.quad_size = obj.project_damage.quad_remesh_size
        try:
            remeshed, remesh_notes = quad_remesh(out, source_points=points, settings=qs)
        except Exception as exc:

            remeshed = out
            remesh_notes = ['quad remesh skipped: %s' % exc]
        out = remeshed
        for msg in remesh_notes:
            reports.append(({'WARNING'}, 'ProjectDamage: ' + msg))
        info = out.get('quad_remesh')
        if info:
            reports.append(({'INFO'}, 'ProjectDamage: quad remesh, %d -> %d faces'
                            % (info['faces_in'], info['faces'])))
    elif obj.project_damage.smart_remesh and not out.get("passthrough"):
        from .smart_remesh import smart_remesh, Settings as RemeshSettings
        rs = RemeshSettings()
        rs.density = obj.project_damage.smart_remesh_density
        rs.grade = obj.project_damage.smart_remesh_grade
        try:
            remeshed, remesh_notes = smart_remesh(out, source_points=points, settings=rs)
        except Exception as exc:


            remeshed = out
            remesh_notes = ["smart remesh skipped: %s" % exc]
        out = remeshed
        for msg in remesh_notes:
            reports.append(({'WARNING'}, "ProjectDamage: " + msg))
        info = out.get("smart_remesh")
        if info:
            reports.append(({'INFO'}, "ProjectDamage: smart remesh, %d damaged/rim faces -> %d triangles"
                            % (info["region_faces"], info["triangles_after"])))

    damage_materials = _existing_damage_materials(obj) if preserve else None
    damage_materials = extra_effects.damage_materials(obj, params, damage_materials)
    from . import material_controls
    damage_materials = material_controls.overrides(obj, damage_materials)
    if boolean_layers and damage_materials:
        from .mesh_io import CLASS_NAMES
        damage_materials.pop(CLASS_NAMES.index('break_P'), None)
    transfer = None
    transfer_snap = None
    if preserve:
        transfer_snap = snap if snap is not None else input_prep.read_source(context, obj, False)
    name = obj.name + "_project_damage"
    if out["passthrough"]:




        reason = passthrough_reason(obj, params, prep, polys, effect_inputs, out)
        if reason is not None:
            reports.append(reason)
        res_mesh = obj.data.copy() if snap is None else input_prep.source_mesh_copy(context, obj, snap)
        if preserve:
            for index, mat in enumerate(transfer_snap.materials):
                if res_mesh.materials[index] != mat:
                    res_mesh.materials[index] = mat
        tri_class = None
    else:
        res_mesh = _result_mesh(name, out["points"] / factor, out["polys"], None)
        tri_class = out["tri_class"]



        base_poly = out.get("base_poly")
        shared_cracks = (obj.project_damage.fract_enable or obj.project_damage.fract_through_enable) and not obj.project_damage.all_off
        if (obj.project_damage.keep_selections or preserve) and base_poly is not None and len(base_poly):
            if preserve:
                offset = input_prep.copy_materials(res_mesh, transfer_snap)
                _apply_classes(res_mesh, tri_class, base_poly=base_poly,
                               offset=offset, src_index=transfer_snap.material_index,
                               damage_materials=damage_materials, shared_cracks=shared_cracks, skip_base_material=params.deform.enable)
            elif snap is None:
                offset = copy_input_materials(res_mesh, obj.data)
                _apply_classes(res_mesh, tri_class, base_poly=base_poly,
                               src_mesh=obj.data, offset=offset, damage_materials=damage_materials, shared_cracks=shared_cracks, skip_base_material=params.deform.enable)
            else:
                offset = input_prep.copy_materials(res_mesh, snap)
                _apply_classes(res_mesh, tri_class, base_poly=base_poly,
                               offset=offset, src_index=snap.material_index, damage_materials=damage_materials, shared_cracks=shared_cracks, skip_base_material=params.deform.enable)
        else:
            _apply_classes(res_mesh, tri_class, damage_materials=damage_materials, shared_cracks=shared_cracks, skip_base_material=params.deform.enable)



        fracture_stack.apply_materials(obj,res_mesh,out)
        if wall_layers:
            wall_stack.face_materials(res_mesh, out, wall_layers, wall_stack.default_materials())
        if boolean_layers:
            boolean_stack.face_materials(res_mesh, out, boolean_layers, bpy.data.materials.get('ProjectDamage_Break'))
        if not preserve and uvs_in is not None and out["uvs"] is not None and len(out["uvs"]):
            _write_result_uvs(res_mesh, uv_layer_name, out["uvs"],
                              out["points"], out["polys"], tri_class)

    from .material_ids import normalize_mesh
    normalize_mesh(res_mesh)
    background_compute.stage_mesh(res_mesh)
    yield None
    if preserve and tri_class is not None:
        from . import result_transfer
        background_compute.stage_mesh(res_mesh)
        transfer=result_transfer.Transfer.__new__(result_transfer.Transfer)
        yield from transfer.prepare(transfer_snap,res_mesh,out)
        skipped = yield from transfer.attribute_steps(out,factor)
        if skipped:
            reports.append(({'WARNING'}, "ProjectDamage: unsupported attributes not transferred: " + ", ".join(skipped)))
    if tri_class is not None and params.wallpaper.enable:
        from . import result_transfer
        result_transfer.apply_wallpaper_materials(res_mesh, out, obj.project_damage)
    if tri_class is not None and (params.brick.enable or params.wall_hole.enable):

        from . import result_transfer
        result_transfer.apply_brick_materials(res_mesh, out, obj.project_damage)
    if tri_class is not None:
        from . import result_transfer
        result_transfer.apply_region_materials(res_mesh,out,obj.project_damage,result_transfer.EXTRA_REGIONS)
        from . import effect_groups
        effect_groups.tag_mesh(res_mesh, out, obj.project_damage)
        metal_tear.after_result(context,obj,res_mesh)
        from . import clay
        clay.after_result(context,obj,res_mesh)
        from . import damaged_bevel
        damaged_bevel.store_segments(obj, out, factor)
        from . import voxel_erosion
        voxel_erosion.after_result(context, obj, res_mesh, core)
        from . import glass_materials
        glass_materials.inherit(obj,res_mesh,out)
        from . import asset_updates
        asset_updates.ground_rubble_material(res_mesh,out,obj.project_damage)
        for key in ('edge_wear','bevel_damage','scratch','metal_dent','metal_crease','metal_contact'):
            values=out.get(key,())
            if len(values)==len(res_mesh.vertices) and len(values):
                attr_name='project_damage_'+key
                attr=res_mesh.attributes.get(attr_name) or res_mesh.attributes.new(attr_name,'FLOAT','POINT')
                attr.data.foreach_set('value',values)
        props = obj.project_damage
        hanging_breakers = props.hang_enable and props.hang_mode == '1'
        shader_pin = props.ui_boolean_enable and props.break_shader_displacement
        if (not boolean_instances and not props.all_off and (props.ui_boolean_enable or hanging_breakers)
                and (shader_pin or props.break_smooth_inside > 0)):
            from . import break_surface
            pin = (props.hang_shader_pin_distance if hanging_breakers and props.hang_shader_pin_enable and not shader_pin
                   else props.break_displacement_pin)
            yield from break_surface.finish_cut_surface_steps(
                res_mesh, obj.matrix_world, factor, pin,
                props.break_smooth_inside, write_mask=shader_pin,
                rim_damage=props.break_rim_enable and props.break_rim_amount > 0)
        for serial, layer in boolean_layers:
            if layer.break_shader_displacement or layer.break_smooth_inside > 0:
                from . import break_surface
                yield from break_surface.finish_cut_surface_steps(
                    res_mesh, obj.matrix_world, factor, layer.break_displacement_pin,
                    layer.break_smooth_inside, write_mask=layer.break_shader_displacement,
                    rim_damage=layer.break_rim_enable and layer.break_rim_amount > 0, owner=serial)
        if not props.all_off:
            from . import displacement_shader
            yield from displacement_shader.finish_effects(obj, res_mesh, factor, points, polys)
    if params.heat_enable:
        _apply_heat_attributes(res_mesh, out, factor, obj.project_damage.heat_char_material)
    if tri_class is not None:
        shading_snap=transfer_snap if transfer_snap is not None else snap
        source_shading=shading_snap.shading if shading_snap is not None else SourceShading.from_mesh(obj.data,raw=True)
        from .mesh_io import phong_steps
        background_compute.stage_mesh(res_mesh)
        for message in (yield from phong_steps(res_mesh,ShadingProvenance.from_result(out,source_shading))):
            reports.append(({'WARNING'},'ProjectDamage: shading: '+message))
    s_prep = input_prep.settings(obj)
    if prep is not None and tri_class is not None:
        if s_prep.transfer_data and not preserve:
            input_prep.transfer_face_bools(res_mesh, snap, out["base_poly"])
        if s_prep.restore_ngons and np.any(snap.loop_total>4) and 'quad_remesh' not in out and not (obj.project_damage.peeling_enable and not obj.project_damage.all_off and not obj.project_damage.peeling_adaptive):
            ngon_start = time.perf_counter()
            groups=yield from background_compute.task('Finding original polygons',input_prep.preserved_ngons,prep,out.get('base_poly'),out.get('tri_class'))
            restored = input_prep.restore_ngons(res_mesh, groups,
                                                performance=obj.project_damage.perf_ngon_restore)
            if context.window_manager.project_damage_core_profiler:
                print('[pd-profile-host] N-gon restore (%s): %.4fs' % (
                    'batched' if obj.project_damage.perf_ngon_restore else 'original',
                    time.perf_counter() - ngon_start), flush=True)
            if restored:
                reports.append(({'INFO'}, "ProjectDamage: %d untouched n-gons restored" % restored))
    custom_shading = False
    if tri_class is not None and params.damage_shading:
        try:
            custom_shading = yield from damage_shading.apply_steps(res_mesh, core, points, polys,
                                                  source_corner_normals, params, factor)
        except Exception as exc:
            reports.append(({'WARNING'}, 'ProjectDamage: custom shading fallback: ' + str(exc)))
    preserve_fabric_normals = params.fabric.enable and params.fabric.flat and not params.fabric.solid
    fabric_source_normals = None
    if tri_class is not None and (params.fract_paint_only or preserve_fabric_normals):
        from . import base_normals
        from .mesh_io import SHADING_REGION_ATTR
        try:
            src_mesh,release=base_normals.source_mesh(context,obj,snap is not None and snap.evaluated)
            try:
                source_normals=base_normals.SourceNormals(src_mesh)
            finally:
                release()
            if preserve_fabric_normals:
                fabric_source_normals = source_normals
            else:
                restored,_faces=yield from base_normals.restore_steps(
                    res_mesh,source_normals,base_normals.surviving_faces(res_mesh,SHADING_REGION_ATTR))
                if context.window_manager.project_damage_core_profiler:
                    print('[pd-profile-host] Source normals restored on %d corners' % restored,flush=True)
        except Exception as exc:
            reports.append(({'WARNING'},'ProjectDamage: surface normal restore skipped: '+str(exc)))
    if len(out.get("rust", [])):
        _write_rust_attributes(res_mesh, out["rust"], out.get("rust_streak"))
    elif len(out.get("rust_source", [])) and len(out["rust_source"]) == len(res_mesh.vertices):
        _write_rust_attributes(res_mesh, out["rust_source"], out.get("rust_source_streak"))
    from . import cell_cuts
    out=yield from cell_cuts.apply_steps(context,obj,out,factor)
    res = _place(context, obj, res_mesh, name)
    if preserve:
        _copy_object_properties(obj, res)
    if transfer is not None:
        transfer.groups(res)
    elif preserve and tri_class is None:
        input_prep.copy_group_names(res, obj)
    if prep is not None and s_prep.transfer_data and not preserve:
        if tri_class is not None:
            input_prep.transfer_vertex_groups(res, snap, prep, factor)
        else:
            input_prep.copy_group_names(res, obj)
    coating = _place_coating(context, obj, out, factor)
    if coating is not None and len(out.get("coating_rust", [])):
        _write_rust_attributes(coating.data, out["coating_rust"], out.get("coating_rust_streak"))
    if coating is not None:
        reports.append(({'INFO'}, "ProjectDamage: coating created (%d polygons)" % len(coating.data.polygons)))
    from .rebar import _place_rebar, place_drawn_steps
    from .lath_output import place_laths
    from .brick_output import place_bricks
    from .debris_output import place_debris
    debris=place_debris(context,obj,out,factor,source_snapshot=snap if snap is not None else transfer_snap,
                        uv_layer_name=uv_layer_name)
    if debris:reports.append(({'INFO'},'ProjectDamage: %d debris chunks'%len(debris)))
    from . import rbf_output
    rbf_pieces=rbf_output.place(context,obj,out,factor,
        source_snapshot=snap if snap is not None else transfer_snap,uv_layer_name=uv_layer_name)
    if rbf_pieces:reports.append(({'INFO'},'Rigid Body Fracture: %d pieces'%len(rbf_pieces)))
    from . import cellf_output
    cellf_pieces=cellf_output.place(context,obj,out,factor,cuts_prepared=True,
        source_snapshot=snap if snap is not None else transfer_snap,uv_layer_name=uv_layer_name)
    if cellf_pieces:reports.append(({'INFO'},'Cell Fracture: %d pieces'%len(cellf_pieces)))
    if obj.project_damage.cellf_enable and obj.get('project_damage_cell_cut_invalid',0):
        reports.append(({'WARNING'},'Cell Fracture: %d invalid cells were kept unchanged; other cuts completed'%obj['project_damage_cell_cut_invalid']))
    bricks=place_bricks(context,obj,out,factor)
    if bricks is not None:reports.append(({'INFO'},'ProjectDamage: %d separate bricks'%out['bricks_count']))
    laths=place_laths(context,obj,{} if wall_instances else out,factor)
    if laths is not None:reports.append(({'INFO'},'ProjectDamage: separate wooden laths created'))
    from . import concrete_rebar_output
    concrete_rebar=concrete_rebar_output.place(context,obj,{} if wall_instances else out,factor)
    instances = []
    if wall_instances:
        wall_stack.cleanup_outputs(obj, {serial for serial, _, _ in wall_layers})
        for serial, key, props in wall_layers:
            if key in wall_stack.WOOD:
                data = wall_stack.geometry_for(out, 'lath', serial)
                extra = place_laths(context, obj, data, factor, props=props, instance=serial)
            elif key in {'concrete', 'concrete_v2'}:
                data = wall_stack.geometry_for(out, 'concrete_rebar', serial)
                extra = concrete_rebar_output.place(context, obj, data, factor, props=props, instance=serial)
            else:
                continue
            instances.append((extra, key))
    rb=_place_rebar(context,obj,out,factor)
    drawn = yield from place_drawn_steps(context, obj, core, factor,
                                         None if boolean_instances else obj.project_damage, 0, reports)
    instances.append((drawn, 'boolean'))
    if boolean_instances:
        boolean_stack.cleanup_outputs(obj, boolean_layers)
        fills = out.get('boolean_fills', {})
        for serial, props in boolean_layers:
            instance = boolean_stack.OUTPUT_OFFSET + serial
            extra = _place_rebar(context, obj, fills.get(serial, {}), factor, props=props,
                                 instance=instance)
            instances.append((extra, 'boolean'))
            drawn = yield from place_drawn_steps(context, obj, core, factor, props, instance, reports)
            instances.append((drawn, 'boolean'))
    from . import wall_hole as _wall_hole
    wallhole_output = _wall_hole.after_build(context,obj,out,factor)
    if rb is not None:
        count = len(out.get('rebar_brick_frame', [])) or out['rebar_bars']
        reports.append(({'INFO'},'ProjectDamage: %d internal fill pieces'%count))
    beads = _place_deposits(context, obj, out, factor, "beads")
    piles = _place_deposits(context, obj, out, factor, "piles")
    deposits = _place_deposits(context, obj, out, factor)
    if deposits is not None:
        reports.append(({'INFO'}, "ProjectDamage: separate deposits created (%d polygons)" % len(deposits.data.polygons)))
    # Bead and Corner Piles only grow in inner (concave) corners. On a convex
    # mesh such as a plain cube they produced nothing and said nothing.
    _p = obj.project_damage
    if not _p.all_off:
        if _p.build_enable and beads is None and _p.build_mode != 'PAINT':
            reports.append(({'WARNING'}, "Bead: no inner edges found - a bead runs along concave edges "
                            "(a wall meeting a floor, an inside corner); a convex mesh like a cube has none"))
        if _p.pile_enable and piles is None:
            reports.append(({'WARNING'}, "Corner Piles: no inner corners found - piles sit where two or more "
                            "inner edges meet (the corner of a room, a box with an open top); try Bead for single inner edges"))

    if params.rust_enable and obj.project_damage.rust_materials:
        explicit = material_controls.explicit_materials(obj.project_damage)
        extra_effects.rust_materials(res, exclude=explicit)
        extra_effects.rust_materials(coating, exclude=explicit)














    # Keep the modifier identity. Fabric passthrough uses an empty mask so
    # its Properties / Geometry Nodes UI does not change during a mode click.
    wn = res.modifiers.get("Weighted Normal")
    if wn is None or wn.type != 'WEIGHTED_NORMAL':
        wn = next((mod for mod in res.modifiers if mod.type == 'WEIGHTED_NORMAL'
                   and mod.name.startswith("Weighted Normal")), None)
    use_wn = tri_class is not None and not custom_shading
    if wn is None and use_wn:
        wn = res.modifiers.new("Weighted Normal", 'WEIGHTED_NORMAL')
        wn.mode = 'FACE_AREA'
        wn.keep_sharp = True
    keep_fabric_mask = wn is not None and tri_class is None and (obj.project_damage.fabric_enable or obj.project_damage.fabric_solid_enable)
    show_wn = use_wn or keep_fabric_mask
    if wn is not None:
        if wn.show_viewport != show_wn:
            wn.show_viewport = show_wn
        if wn.show_render != show_wn:
            wn.show_render = show_wn
    if show_wn:
        group = shading_region_group(res, empty=keep_fabric_mask)
        if wn.vertex_group != group:
            wn.vertex_group = group

    n_pts, n_polys, n_chips = out["counts"]











    extent = float((points.max(axis=0) - points.min(axis=0)).max())



    scale_hint = ("the mesh is %.3g cm across while chips are %.3g-%.3g cm "
                  "and craters %.3g-%.3g cm: resize the mesh (apply the "
                  "object's scale with Ctrl+A), or lower the Size and "
                  "Depth values"
                  % (extent, params.size_min, params.size_max,
                     params.surf_size_min, params.surf_size_max))
    if tri_class is None:
        pass
    elif n_polys == 0:
        reports.append(({'WARNING'},
                    "ProjectDamage: the damage removed the ENTIRE object - %s (%.0f ms)"
                    % (scale_hint, ms)))
    elif params.plaster.enable and not np.any(tri_class != BASE_CLASS):
        reports.append(({'INFO'}, 'project_damage: plaster displacement evaluated (%d polygons, %.0f ms)' % (n_polys, ms)))
    elif (any(np.any(out.get(k, ())) for k in ('metal_dent', 'metal_contact'))
          or len(out.get('bevel_polys', ()))) and not np.any(tri_class != BASE_CLASS):
        reports.append(({'INFO'}, 'ProjectDamage: deformation / bevel evaluated (%d polygons, %.0f ms)' % (n_polys, ms)))
    elif getattr(getattr(params, 'clay', None), 'enable', False) and not np.any(tri_class != BASE_CLASS):
        reports.append(({'INFO'}, 'ProjectDamage: Clay deformation evaluated (%d polygons, %.0f ms)' % (n_polys, ms)))
    elif beads is not None or piles is not None or deposits is not None:
        reports.append(({'INFO'}, 'ProjectDamage: separate surface additions evaluated (%.0f ms)' % ms))
    elif rbf_pieces or cellf_pieces:
        pass
    elif getattr(getattr(params, 'pits', None), 'enable', False) and not np.any(tri_class != BASE_CLASS):
        # Pitting displaces the skin without changing any triangle's class.
        reports.append(({'INFO'}, 'ProjectDamage: Concrete Pitting evaluated (%d polygons, %.0f ms)' % (n_polys, ms)))
    elif getattr(getattr(params, 'fabric', None), 'enable', False) and n_pts != len(points) and not np.any(tri_class != BASE_CLASS):
        # Tears cut the sheet apart; the faces that survive keep the base class.
        reports.append(({'INFO'}, 'ProjectDamage: fabric tears evaluated (%d polygons, %.0f ms)' % (n_polys, ms)))
    elif not np.any(tri_class != BASE_CLASS):
        reports.append(({'WARNING'},
                    "ProjectDamage: no damage landed on the mesh - %s (%d polygons, "
                    "%.0f ms)" % (scale_hint, n_polys, ms)))
    else:
        reports.append(({'INFO'}, "ProjectDamage: %d points, %d polygons, %d chip polygons in %.0f ms"
                    % (n_pts, n_polys, n_chips, ms)))
    from . import effect_groups
    effect_ids = out.get('effect_ids')
    if (params.hang.enable and params.hang.mode == 0 and params.hang.amount > 0
            and effect_ids is not None and not np.any(effect_ids == effect_groups.KEYS.index('hang'))):
        reports.append(({'WARNING'}, 'Hanging Rebar Holes: no automatic hole was cut. Check Thickness and opening size.'))
    from . import peeling
    peeling.after_build(context,obj,res,reports)
    material_controls.apply_shared_output_material(obj)
    material_uv.after_build(obj,uv_snapshots)
    from . import displacement_shader
    displacement_shader.restore_materials(obj,res)
    effect_groups.refresh(obj, res, ((coating, 'coating'), (bricks, 'brick'),
        (concrete_rebar, 'concrete_v2' if obj.project_damage.concrete_v2_enable else 'concrete'),
        (laths, 'lath_framing' if obj.project_damage.lath_framing_enable else 'lath_riven' if obj.project_damage.lath_riven_enable else 'lath_v2' if obj.project_damage.lath_v2_enable else 'lath'), (rb, 'hang' if params.hang.enable else 'boolean' if params.break_rebar.enable else 'concrete_v2' if obj.project_damage.concrete_v2_enable else 'concrete'),
        (beads, 'buildup'), (piles, 'piles'), (deposits, 'deposits'), (debris, 'debris'), (rbf_pieces, 'rbf'), (cellf_pieces, 'cellf'), (wallhole_output,'wallhole'), *instances), damage_materials)
    if fabric_source_normals is not None:
        try:
            restored,_faces=yield from base_normals.restore_steps(
                res.data,fabric_source_normals,base_normals.surviving_faces(res.data,SHADING_REGION_ATTR))
            if context.window_manager.project_damage_core_profiler:
                print('[pd-profile-host] Fabric source normals restored on %d corners' % restored,flush=True)
        except Exception as exc:
            reports.append(({'WARNING'},'ProjectDamage: fabric normal restore skipped: '+str(exc)))
    from . import boolean_break, pipes, cables
    body_has_damage = tri_class is not None and np.any(tri_class != BASE_CLASS)
    if boolean_instances:
        base_for_tubes = boolean_stack.BaseTubes(obj.project_damage)
        pipes.after_build(context, obj, res, reports, body_has_damage=body_has_damage, props=base_for_tubes, damage=not wall_instances)
        cables.after_build(context, obj, res, reports, body_has_damage=body_has_damage, props=base_for_tubes, damage=not wall_instances)
        for serial, props in boolean_layers:
            local_body = out.get('boolean_bodies', {}).get(serial)
            for service in (pipes, cables):
                service.after_build(context, obj, res, reports, body_has_damage=body_has_damage and local_body is not None,
                                    props=props, instance=boolean_stack.OUTPUT_OFFSET + serial, damage=False,
                                    cut_body=local_body)
    else:
        pipes.after_build(context, obj, res, reports, body_has_damage=body_has_damage, damage=not wall_instances)
        cables.after_build(context, obj, res, reports, body_has_damage=body_has_damage, damage=not wall_instances)
    for serial, key, props in wall_layers:
        service = {'pipes': pipes, 'cables': cables}.get(key)
        if service is not None:
            service.after_build(context, obj, res, reports, body_has_damage=body_has_damage,
                                props=props, instance=serial)
    if out['passthrough']:
        if not obj.get(boolean_break.STAGE_KEY):
            boolean_break.drop_fragments(obj)
    else:
        boolean_break.after_build(context, obj, res, reports, source_geometry=(points, polys))
    from . import generated_assets, paper_generator
    generated_assets.after_build(context,obj,res,reports)
    yield from paper_generator.after_build(context, obj, res, reports)
    from . import surface_rebar
    surface_rebar.after_build(context,obj,res,reports,geometry=out,input_geometry=(points,polys))
    from . import boolean_restructure
    boolean_restructure.capture(obj, res)
    ownership.focus_result(context, obj, res, hide_original=previous_result is None)
    from . import damage_generator_assets
    damage_generator_assets.visibility(context,obj,res)
    from . import ivy, roots
    ivy.set_visibility(obj, obj.project_damage.ivy_enable and not obj.project_damage.all_off)
    ivy.queue_update(obj)
    roots.set_visibility(obj, obj.project_damage.roots_enable and not obj.project_damage.all_off)
    roots.queue_update(obj)
    if rbf_pieces:rbf_output.focus(context,obj,res,rbf_pieces)
    if cellf_pieces:
        cellf_output.focus(context,obj,res,cellf_pieces)
    cellf_output.sync_switch(context,obj,cellf_pieces)
    if cellf_pieces:
        from . import paint
        paint.refresh_cell_knife_projection(context,obj,cellf_pieces)
    from . import material_controls
    material_controls.remember_defaults(obj, res)
    incremental.remember_body(context,obj,res)
    incremental.remember_slots(obj)
    live.remember_drawing_frames(obj, drawing_frames)
    return res, reports



def _apply_extra(context, operator, module):
    if module in {'heat','bevel','rust'}:
        operator.report({'WARNING'}, 'This effect has been removed from ProjectDamage')
        return {'CANCELLED'}
    from . import live
    previous = live._APPLYING
    live._APPLYING = True
    try:
        obj = ownership.context_source(context)
        obj.project_damage.all_off = False
        if module in {'fabric','fabric_solid','concrete','concrete_v2','lath','lath_v2','lath_riven','lath_framing','brick','brick_v2','glass','glass_hit','glass_drawn'}:
            from . import sidebar
            sidebar.set_enabled(obj, module, True)
        else:
            setattr(obj.project_damage, module + "_enable", True)
        result, reports = rebuild_object(context, obj)
    finally:
        live._APPLYING = previous
    for report_type, message in reports:
        operator.report(report_type, message)
    return {'FINISHED'} if result is not None else {'CANCELLED'}


class PROJECT_DAMAGE_OT_apply_fabric(bpy.types.Operator):
    bl_idname = "project_damage.apply_fabric"
    bl_label = "Apply Fabric Tears"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return PROJECT_DAMAGE_OT_update.poll(context)

    def execute(self, context):
        from . import sidebar
        sidebar.set_enabled(ownership.context_source(context), 'fabric', True)
        return _apply_extra(context, self, "fabric")


class PROJECT_DAMAGE_OT_apply_fabric_solid(bpy.types.Operator):
    bl_idname = 'project_damage.apply_fabric_solid'
    bl_label = 'Apply Fabric Solidify'
    bl_description = 'Boolean tears through one panel of evaluated solidified cloth'
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return PROJECT_DAMAGE_OT_update.poll(context)

    def execute(self, context):
        from . import sidebar
        sidebar.set_enabled(ownership.context_source(context), 'fabric_solid', True)
        return _apply_extra(context, self, 'fabric_solid')


class PROJECT_DAMAGE_OT_apply_lath_v2(bpy.types.Operator):
    bl_idname = 'project_damage.apply_lath_v2'
    bl_label = 'Apply Lath and Plaster V2'
    bl_options = {'REGISTER', 'UNDO'}
    @classmethod
    def poll(cls, context):return PROJECT_DAMAGE_OT_update.poll(context)
    def execute(self, context):
        from . import sidebar
        sidebar.set_enabled(ownership.context_source(context), 'lath_v2', True)
        return _apply_extra(context, self, 'lath_v2')

class PROJECT_DAMAGE_OT_apply_brick_v2(bpy.types.Operator):
    bl_idname = 'project_damage.apply_brick_v2'
    bl_label = 'Apply Brick Damage V2'
    bl_options = {'REGISTER', 'UNDO'}
    @classmethod
    def poll(cls, context):return PROJECT_DAMAGE_OT_update.poll(context)
    def execute(self, context):
        from . import sidebar
        sidebar.set_enabled(ownership.context_source(context), 'brick_v2', True)
        return _apply_extra(context, self, 'brick_v2')

class PROJECT_DAMAGE_OT_apply_concrete_v2(bpy.types.Operator):
    bl_idname = 'project_damage.apply_concrete_v2'
    bl_label = 'Apply Concrete Damage V2'
    bl_options = {'REGISTER', 'UNDO'}
    @classmethod
    def poll(cls, context):return PROJECT_DAMAGE_OT_update.poll(context)
    def execute(self, context):
        from . import sidebar
        sidebar.set_enabled(ownership.context_source(context), 'concrete_v2', True)
        return _apply_extra(context, self, 'concrete_v2')


class PROJECT_DAMAGE_OT_apply_hang(bpy.types.Operator):
    bl_idname = 'project_damage.apply_hang'
    bl_label = 'Apply Hanging Rebar Holes'
    bl_options = {'REGISTER', 'UNDO'}
    @classmethod
    def poll(cls, context):return PROJECT_DAMAGE_OT_update.poll(context)
    def execute(self, context):
        from . import sidebar
        sidebar.set_enabled(ownership.context_source(context), 'hang', True)
        return _apply_extra(context, self, 'hang')


class PROJECT_DAMAGE_OT_apply_glass_hit(bpy.types.Operator):
    bl_idname = 'project_damage.apply_glass_hit'
    bl_label = 'Apply Hit Glass'
    bl_options = {'REGISTER', 'UNDO'}
    @classmethod
    def poll(cls, context):return PROJECT_DAMAGE_OT_update.poll(context)
    def execute(self, context):
        from . import sidebar
        sidebar.set_enabled(ownership.context_source(context), 'glass_hit', True)
        return _apply_extra(context, self, 'glass_hit')


class PROJECT_DAMAGE_OT_apply_glass_drawn(bpy.types.Operator):
    bl_idname = 'project_damage.apply_glass_drawn'
    bl_label = 'Apply Glass Drawn'
    bl_options = {'REGISTER', 'UNDO'}
    @classmethod
    def poll(cls, context):return PROJECT_DAMAGE_OT_update.poll(context)
    def execute(self, context):
        from . import sidebar
        sidebar.set_enabled(ownership.context_source(context), 'glass_drawn', True)
        return _apply_extra(context, self, 'glass_drawn')


class PROJECT_DAMAGE_OT_apply_concrete(bpy.types.Operator):
    bl_idname = "project_damage.apply_concrete"
    bl_label = "Apply Concrete Damage"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return PROJECT_DAMAGE_OT_update.poll(context)

    def execute(self, context):
        return _apply_extra(context, self, "concrete")


# действие «Apply Rust»
class PROJECT_DAMAGE_OT_apply_rust(bpy.types.Operator):
    """Enable Rust and rebuild. Other enabled effects are kept; the paint
    layer is the coating object."""
    bl_idname = "project_damage.apply_rust"
    bl_label = "Apply Rust"
    bl_description = "Enable rust and rebuild the ProjectDamage result (and its paint layer) using the current settings"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return PROJECT_DAMAGE_OT_update.poll(context)

    def execute(self, context):
        obj = ownership.context_source(context)
        from . import live
        previous = live._APPLYING
        live._APPLYING = True
        try:
            obj.project_damage.all_off = False
            obj.project_damage.rust_enable = True
            result, reports = rebuild_object(context, obj)
        finally:
            live._APPLYING = previous
        for report_type, message in reports:
            self.report(report_type, message)
        return {'FINISHED'} if result is not None else {'CANCELLED'}


def draw_init_modifier_menu(layout, target_name):
    layout.label(text='Apply the visible modifier stack, or use the original mesh?')
    layout.operator_context = 'EXEC_DEFAULT'
    for action, title, icon in (('APPLY', 'Apply Modifiers', 'CHECKMARK'),
                                ('IGNORE', 'Ignore Modifiers', 'FORWARD')):
        button = layout.operator('project_damage.update', text=title, icon=icon)
        button.modifier_action = action
        button.target_name = target_name


# действие «INIT_project_damage»
class PROJECT_DAMAGE_OT_update(bpy.types.Operator):
    bl_idname = "project_damage.update"
    bl_label = "INIT_project_damage"
    bl_description = "Initialize this mesh; a selected loose Fill Hole or physics piece becomes an independent source. Cut damage into its own copy"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        obj = ownership.initialization_source(context)
        return (context.mode == 'OBJECT' and obj is not None and obj.type == 'MESH'
                and not obj.get(RESULT_STAMP) and not obj.get(DEPOSIT_STAMP) and not obj.get("project_damage_rebar_of")
                and not obj.get(COATING_STAMP))

    modifier_action: bpy.props.EnumProperty(
        items=(('IGNORE', 'Ignore Modifiers', 'Initialize from the original mesh'),
               ('APPLY', 'Apply Modifiers', 'Bake the visible modifier stack before initialization')),
        default='IGNORE', options={'HIDDEN', 'SKIP_SAVE'})
    target_name: bpy.props.StringProperty(options={'HIDDEN', 'SKIP_SAVE'})

    def invoke(self, context, event):
        obj = bpy.data.objects.get(self.target_name) or ownership.initialization_source(context)
        self.target_name = obj.name
        from . import preparation_tools
        if preparation_tools.prepared(obj):return self.execute(context)
        first_init = result_of(obj) is None or any(obj.get(k) for k in ownership.LOOSE_STAMPS)
        if first_init and obj.modifiers and not self.properties.is_property_set('modifier_action'):
            window = context.window
            area = context.area or next((a for a in window.screen.areas if a.type == 'VIEW_3D'), None)
            region = next((r for r in area.regions if r.type == 'WINDOW'), None) if area else None
            target_name = obj.name


            ui_scale = context.preferences.system.ui_scale
            window.cursor_warp(int(window.width / 2 - 215 * ui_scale),
                               int(window.height / 2 + 60 * ui_scale))
            def open_prompt():
                if window not in bpy.context.window_manager.windows.values():
                    return None
                with bpy.context.temp_override(window=window, area=area, region=region):
                    target = bpy.data.objects.get(target_name)
                    if target is not None and target == ownership.initialization_source(bpy.context):
                        bpy.context.window_manager.popup_menu(
                            lambda menu, _context: draw_init_modifier_menu(menu.layout, target_name),
                            title='Initialize Project Damage', icon='MODIFIER')
                return None
            bpy.app.timers.register(open_prompt, first_interval=0.05)
            return {'CANCELLED'}
        return self.execute(context)

    def execute(self, context):
        from . import live
        obj = bpy.data.objects.get(self.target_name) if self.target_name else ownership.initialization_source(context)
        if obj is None or obj.type != 'MESH' or obj.mode != 'OBJECT' or obj.name not in context.view_layer.objects:
            self.report({'ERROR'}, 'The selected mesh is no longer available')
            return {'CANCELLED'}
        from . import preparation_tools
        if preparation_tools.prepared(obj):
            try:return preparation_tools.initialize(self, context, obj)
            except Exception as exc:
                self.report({'ERROR'}, 'Project Damage initialization failed: ' + str(exc))
                return {'CANCELLED'}
        previous = live._APPLYING
        old_mesh = obj.data
        baked = None
        applied = []
        committed = False
        parent_preparation = None
        live._APPLYING = True
        old_use_modifiers = obj.project_damage_input.use_modifiers
        from . import baked_materials
        was_applied=baked_materials.is_applied(obj)
        try:
            if self.modifier_action == 'APPLY' and obj.modifiers:
                applied = [(mod, mod.show_viewport) for mod in obj.modifiers if mod.show_viewport]
                if applied:
                    depsgraph = context.evaluated_depsgraph_get()
                    baked = bpy.data.meshes.new_from_object(obj.evaluated_get(depsgraph),
                        preserve_all_data_layers=True, depsgraph=depsgraph)
                    if baked is None or not baked.polygons:
                        raise RuntimeError('The modifier stack produced an empty mesh')
                    from .material_ids import normalize_mesh
                    normalize_mesh(baked)
                    obj.data = baked
                    for mod, visible in applied:
                        mod.show_viewport = False
            # Both choices now use an authored mesh: either baked or original.
            if obj.modifiers and (result_of(obj) is None or any(obj.get(k) for k in ownership.LOOSE_STAMPS)):
                obj.project_damage_input.use_modifiers = False
            from . import init_parenting
            parent_preparation=init_parenting.prepare(context,obj)
            status = self.initialize(context, obj)
            if status == {'FINISHED'}:
                if was_applied:
                    from . import mesh_finalization
                    new_result=result_of(obj)
                    mesh_finalization.preserve_surface_data(obj.data,new_result.data)
                    mesh_finalization.remove_unused_slots(new_result)
                    obj.project_damage.property_unset('live_update')
                    if context.window_manager.project_damage_pinned_source is not None:
                        context.window_manager.project_damage_pinned_source=obj
                    for name in ('project_damage_applied','project_damage_baked_surface'):
                        if name in obj:del obj[name]
                    baked_materials.invalidate()
                for mod, visible in applied:
                    obj.modifiers.remove(mod)
                committed = True
                return status
        except Exception as exc:
            self.report({'ERROR'}, 'Project Damage initialization failed: ' + str(exc))
        finally:
            if parent_preparation is not None:
                if not committed:parent_preparation.rollback()
                else:parent_preparation.cleanup()
            if not committed and baked is not None and obj.data == baked:
                obj.data = old_mesh
            if not committed:
                for mod, visible in applied:
                    mod.show_viewport = visible
                obj.project_damage_input.use_modifiers = old_use_modifiers
            if baked is not None and not baked.users:
                bpy.data.meshes.remove(baked)
            live._APPLYING = previous
        return {'CANCELLED'}

    def initialize(self, context, obj):
        from . import live
        previous=live._APPLYING;live._APPLYING=True
        try:released=ownership.release_loose_piece(context,obj)
        finally:live._APPLYING=previous
        if result_of(obj) is None:


            obj.project_damage['preserve_data'] = True
            obj.project_damage['keep_uvs'] = True
            obj.project_damage['keep_selections'] = True
            obj.project_damage['split_effect_zones'] = True
            obj.project_damage['topology'] = 3
            obj.project_damage['perf_mesh_preparation'] = True
            obj.project_damage['perf_ngon_restore'] = True
            obj.project_damage['perf_prep_cache'] = True
            context.window_manager.project_damage_core_profiler = True
        from . import incremental
        incremental.invalidate_body(obj)
        try:res, reports = rebuild_object(context, obj)
        except Exception:
            ownership.restore_loose_piece(context,obj,released)
            raise
        if res is None:ownership.restore_loose_piece(context,obj,released)
        for report_type, msg in reports:
            self.report(report_type, msg)
        return {'FINISHED'} if res is not None else {'CANCELLED'}


def _copy_object_properties(source, result):
    for name in source.keys():
        if ownership.is_internal_key(name) or name.startswith("_"):
            continue
        value = source[name]
        result[name] = value.to_dict() if hasattr(value, "to_dict") else value
    result.color = source.color


def _existing_damage_materials(source):
    result = result_of(source)
    if result is None:
        return {}
    saved = result.get("project_damage_damage_materials", {})
    overrides = {int(k): bpy.data.materials.get(name) for k, name in saved.items() if name != "ProjectDamage_Heat_Char"}
    mesh = result.data
    indices = np.empty(len(mesh.polygons), dtype=np.int32)
    mesh.polygons.foreach_get("material_index", indices)


    from .result_transfer import MATERIAL_REGIONS
    wallpaper = np.zeros(len(mesh.polygons), dtype=bool)
    for _, attr_name, _ in MATERIAL_REGIONS:
        region = mesh.attributes.get(attr_name)
        if region is not None and region.domain == 'FACE':
            values = np.empty(len(mesh.polygons), dtype=bool)
            region.data.foreach_get('value', values)
            wallpaper |= values
    from .mesh_io import CLASS_NAMES
    effect_attr = mesh.attributes.get('project_damage_effect_id')
    corner_faces = np.zeros(len(mesh.polygons), dtype=bool)
    if effect_attr is not None and effect_attr.domain == 'FACE':
        effect_ids = np.empty(len(mesh.polygons), dtype=np.int32)
        effect_attr.data.foreach_get('value', effect_ids)
        corner_faces = effect_ids == 2
    for cls, name in enumerate(CLASS_NAMES):
        if cls == BASE_CLASS:
            continue
        attr = mesh.attributes.get(name)
        if attr is None or attr.domain != 'FACE':
            continue
        mask = np.empty(len(mesh.polygons), dtype=bool)
        attr.data.foreach_get("value", mask)
        eligible = indices[mask & ~wallpaper & (~corner_faces if cls == 0 else True)]
        eligible = np.array([i for i in eligible if i < len(mesh.materials)
                             and mesh.materials[int(i)] is not None
                             and mesh.materials[int(i)] != source.project_damage.heat_char_material
                             and mesh.materials[int(i)].name != "ProjectDamage_Heat_Char"], dtype=np.int32)
        used, counts = np.unique(eligible, return_counts=True)
        if len(used):
            slot = int(used[counts.argmax()])
            if slot < len(mesh.materials) and mesh.materials[slot] is not None:
                overrides[cls] = mesh.materials[slot]
    result["project_damage_damage_materials"] = {str(k): m.name for k, m in overrides.items() if m is not None}
    return {k: m for k, m in overrides.items() if m is not None}


def _bake_damage_outputs(context, source, result):
    """Finalize only this source's outputs; retain the hidden editable source.

    Stage owned shading modifiers on temporary copies before detaching IDs.
    User-authored modifiers are deliberately left in place and unchanged.
    """
    from . import sidebar, live
    uid = source['project_damage_uid']
    outputs = [o for o in bpy.data.objects
               if any(o.get(stamp) == uid for stamp in ownership.STAMPS)]


    for obj in bpy.data.objects:
        if obj.type=='MESH' and any(obj.get(stamp) for stamp in ownership.LOOSE_STAMPS):
            if (any(obj.get(stamp)==uid for stamp in ownership.LOOSE_STAMPS)
                    or getattr(obj,'project_damage_og_object',None)==source
                    or obj.get('project_damage_fragment_of')==result.name):
                if obj not in outputs:outputs.append(obj)
    ivy_result = source.project_damage_ivy.result
    if (source.project_damage.ivy_enable and ivy_result is not None and
            ivy_result.parent == source and ivy_result.get('project_damage_ivy_generated')):
        outputs.append(ivy_result)
    roots_result = source.project_damage_roots.result
    if (source.project_damage.roots_enable and roots_result is not None and
            roots_result.parent == source and roots_result.get('project_damage_roots_generated')):
        outputs.append(roots_result)
    if result not in outputs:
        raise RuntimeError('Cannot apply damage: the result has no matching owner')
    from . import damage_generator_assets
    replacements=[o for o in outputs if o.get('project_damage_generator_key') in damage_generator_assets.KINDS
                  and not o.hide_render and damage_generator_assets._replaces(damage_generator_assets.settings(source,o['project_damage_generator_key']))]
    physics_outputs=[o for o in outputs if o.get('project_damage_rbf_of')==uid or o.get('project_damage_cellf_of')==uid]
    split_walls = {obj for obj in outputs if obj.get('project_damage_split_wall')}
    physics_keys = {'project_damage_hold_hit', 'project_damage_physics_bake_matrix',
                    'project_damage_physics_previous_action', 'project_damage_physics_previous_slot',
                    'project_damage_physics_bake_body'}
    staged = []
    try:
        for obj in outputs:
            wn = obj.modifiers.get('Weighted Normal') if obj.type == 'MESH' else None
            if wn is None or wn.type != 'WEIGHTED_NORMAL':
                continue
            temp = obj.copy()
            temp.data = obj.data.copy()
            temp_mesh = temp.data
            context.scene.collection.objects.link(temp)
            try:
                temp.hide_viewport = False
                temp.hide_set(False)
                for mod in list(temp.modifiers):
                    if mod.name != wn.name:
                        temp.modifiers.remove(mod)
                context.view_layer.update()
                depsgraph = context.evaluated_depsgraph_get()
                mesh = bpy.data.meshes.new_from_object(temp.evaluated_get(depsgraph),
                    preserve_all_data_layers=True, depsgraph=depsgraph)
                if mesh is None:
                    raise RuntimeError('Failed to bake shading for ' + obj.name)
                staged.append((obj, wn.name, mesh))
            finally:
                bpy.data.objects.remove(temp, do_unlink=True)
                if temp_mesh.users == 0:
                    bpy.data.meshes.remove(temp_mesh)
    except Exception:
        for _, _, mesh in staged:
            if mesh.users == 0:
                bpy.data.meshes.remove(mesh)
        raise
    for obj, mod_name, mesh in staged:
        old = obj.data
        obj.data = mesh
        obj.modifiers.remove(obj.modifiers[mod_name])
        if old.users == 0:
            bpy.data.meshes.remove(old)

    source.project_damage.live_update = False
    if ownership.pinned_source(context)==source:
        context.window_manager.project_damage_pinned_source=result
    live._dirty.pop(uid, None)
    live._BREAKER_SIG.pop(uid, None)
    _CACHES.pop(uid, None)
    from . import mesh_finalization,baked_materials,material_uv
    applied_batch=uuid.uuid4().hex
    for obj in outputs:
        preserve_uv=material_uv.preserve_output_uv(obj)
        is_fragment=obj.type=='MESH' and any(obj.get(stamp) for stamp in baked_materials._FRAGMENT_STAMPS)
        if is_fragment:
            mask=baked_materials.inner_mask(obj)
            attr=obj.data.attributes.get('project_damage_physics_inner')
            if attr is None:attr=obj.data.attributes.new('project_damage_physics_inner','BOOLEAN','FACE')
            attr.data.foreach_set('value',mask)
        if obj.type == 'MESH':mesh_finalization.remove_unused_slots(obj)
        world = obj.matrix_world.copy()
        if obj.parent == source:
            obj.parent = None
            obj.matrix_world = world
        obj.project_damage_og_object = None
        obj.project_damage_effects.clear()
        obj.project_damage_effect_index = 0
        obj.project_damage.live_update = False
        for field in sidebar.ENABLE_PROPS | {'heat_enable','bevel_enable','rust_enable'}:
            setattr(obj.project_damage, field, False)
        for key in list(obj.keys()):
            if (key.startswith(('project_damage_', 'edgy_')) and key not in physics_keys) or key == 'pd_gen_source':
                del obj[key]
        if preserve_uv:obj[material_uv.PRESERVE_OUTPUT_UV]=True
        obj['project_damage_baked_backup'] = source
        obj['project_damage_applied'] = True
        obj['project_damage_baked_surface'] = obj == result
        obj['project_damage_applied_batch']=applied_batch
        obj['project_damage_baked_fragment']=is_fragment
        obj['project_damage_baked_split_wall'] = obj in split_walls
    from . import mesh_states
    from . import drawing_cleanup
    drawing_cleanup.after_apply(source, outputs)
    if not mesh_states.save_all(context):mesh_states.restore_initial(source)
    for obj in tuple(context.selected_objects):
        obj.select_set(False)
    result.hide_set(False)
    result.select_set(True)
    context.view_layer.objects.active = result
    if replacements and not physics_outputs:
        result.hide_set(True);result.hide_render=True;result.select_set(False)
        replacements[-1].hide_set(False);replacements[-1].select_set(True)
        context.view_layer.objects.active=replacements[-1]
    if physics_outputs:
        result.hide_set(True);result.hide_render=True;result.select_set(False)
        physics_outputs[0].hide_set(False);physics_outputs[0].select_set(True)
        context.view_layer.objects.active=physics_outputs[0]
    elif split_walls:
        result.hide_set(True)
        result.hide_render = True
        result.select_set(False)
        wall = next(obj for obj in outputs if obj in split_walls)
        wall.hide_set(False)
        wall.select_set(True)
        context.view_layer.objects.active = wall
    return outputs


# действие «Apply All Damage»
class PROJECT_DAMAGE_OT_apply_all_damage(bpy.types.Operator):
    bl_idname = "project_damage.apply_all_damage"
    bl_label = "Apply All Damage"
    bl_description = "Bake the visible result and preserve edited UVs and materials. Draft Mode recalculates at Master Quality first. Keep the original source without creating extra backups; other modifiers are untouched"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        from . import live
        source = ownership.context_source(context)
        return (not live._HISTORY_RESTORING and not live._APPLYING
                and PROJECT_DAMAGE_OT_update.poll(context) and result_of(source) is not None)

    def execute(self, context):
        from . import live
        source = ownership.context_source(context)
        previous = live._APPLYING
        live._APPLYING = True
        try:
            source.project_damage.preserve_data = True
            source.project_damage.keep_uvs = True
            source.project_damage.keep_selections = True
            from . import background_compute
            background_compute.cancel(source)
            from . import preview_quality
            with background_compute.synchronous(), preview_quality.final_quality(source):
                if source.project_damage.draft_enable or result_of(source) is None:
                    result, reports = rebuild_object(context, source)
                else:
                    result, reports = result_of(source), []
                errors = [message for levels, message in reports if 'ERROR' in levels]
                if errors:
                    raise RuntimeError('; '.join(errors))
                if result is not None and source.project_damage.ivy_enable:
                    if bpy.ops.project_damage.grow_ivy()!={'FINISHED'}:
                        raise RuntimeError('Ivy failed; nothing was baked')
                if result is not None and source.project_damage.roots_enable:
                    if bpy.ops.project_damage.grow_roots()!={'FINISHED'}:
                        raise RuntimeError('Roots failed; nothing was baked')
            if result is not None:
                outputs = _bake_damage_outputs(context, source, result)
                reports.append(({'INFO'}, 'Applied Project Damage on %d objects; original visibility preserved' % len(outputs)))
        except Exception as exc:
            self.report({'ERROR'}, 'Apply All Damage failed: ' + str(exc))
            return {'CANCELLED'}
        finally:
            live._APPLYING = previous
        for report_type, message in reports:
            self.report(report_type, message)
        return {'FINISHED'} if result is not None else {'CANCELLED'}


# действие «Apply Buildup & Deposits»
class PROJECT_DAMAGE_OT_apply_buildup(bpy.types.Operator):
    bl_idname = "project_damage.apply_buildup"
    bl_label = "Apply Buildup & Deposits"
    bl_description = "Enable surface deposits and build the ProjectDamage result using the current settings"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return PROJECT_DAMAGE_OT_update.poll(context)

    def execute(self, context):
        obj = ownership.context_source(context)
        from . import live
        previous = live._APPLYING
        live._APPLYING = True
        try:
            obj.project_damage.all_off = False
            obj.project_damage.build_enable = True
            obj.project_damage.deposit_enable = True
            if obj.project_damage.deposit_amount == 0:
                obj.project_damage.deposit_amount = 120
            result, reports = rebuild_object(context, obj)
        finally:
            live._APPLYING = previous
        for report_type, message in reports:
            self.report(report_type, message)
        return {'FINISHED'} if result is not None else {'CANCELLED'}


def _apply_heat_attributes(mesh, out, factor, char_material=None):
    for name, key, scale in (("project_damage_heat_exposure", "heat_exposure", 1),
                             ("project_damage_heat_char", "heat_char", 1),
                             ("project_damage_heat_char_depth", "heat_char_depth", 1 / factor)):
        values = out.get(key, [])
        if len(values) != len(mesh.vertices):
            continue
        attr = mesh.attributes.get(name)
        if attr is None:
            attr = mesh.attributes.new(name=name, type='FLOAT', domain='POINT')
        attr.data.foreach_set("value", values * scale)
    char = out.get("heat_char", [])
    if len(char) != len(mesh.vertices) or not len(char):
        return
    mat = char_material or bpy.data.materials.get("ProjectDamage_Heat_Char")
    if mat is None:
        mat = bpy.data.materials.new("ProjectDamage_Heat_Char")
        mat.diffuse_color = (0.04, 0.025, 0.015, 1)
        enable_nodes(mat)
        nodes = mat.node_tree.nodes
        shader = nodes.get("Principled BSDF")
        shader.inputs["Roughness"].default_value = 0.9
        attr = nodes.new("ShaderNodeAttribute")
        attr.attribute_name = "project_damage_heat_char"
        ramp = nodes.new("ShaderNodeValToRGB")
        ramp.color_ramp.elements[0].color = (0.3, 0.22, 0.12, 1)
        ramp.color_ramp.elements[1].color = (0.008, 0.005, 0.003, 1)
        mat.node_tree.links.new(attr.outputs["Fac"], ramp.inputs["Fac"])
        mat.node_tree.links.new(ramp.outputs["Color"], shader.inputs["Base Color"])
    index = next((i for i, m in enumerate(mesh.materials) if m == mat), None)
    if index is None:
        index = len(mesh.materials)
        mesh.materials.append(mat)
    for poly in mesh.polygons:
        if sum(float(char[v]) for v in poly.vertices) / len(poly.vertices) > 0.02:
            poly.material_index = index


# действие «Apply Heat Damage»
class PROJECT_DAMAGE_OT_apply_heat(bpy.types.Operator):
    bl_idname = "project_damage.apply_heat"
    bl_label = "Apply Heat Damage"
    bl_description = "Apply heat shrinkage, char cracks and blisters to the result mesh"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return PROJECT_DAMAGE_OT_update.poll(context)

    def execute(self, context):
        from . import live
        previous = live._APPLYING
        live._APPLYING = True
        try:
            ownership.context_source(context).project_damage.all_off = False
            ownership.context_source(context).project_damage.heat_enable = True
            result, reports = rebuild_object(context, ownership.context_source(context))
        finally:
            live._APPLYING = previous
        for report_type, message in reports:
            self.report(report_type, message)
        return {'FINISHED'} if result is not None else {'CANCELLED'}


# действие «Apply Coating Peeling»
class PROJECT_DAMAGE_OT_apply_coating(bpy.types.Operator):
    bl_idname = "project_damage.apply_coating"
    bl_label = "Apply Coating Peeling"
    bl_description = "Create a separate peeled coating on the authored source mesh"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return PROJECT_DAMAGE_OT_update.poll(context)

    def execute(self, context):
        from . import live
        previous = live._APPLYING
        live._APPLYING = True
        try:
            ownership.context_source(context).project_damage.all_off = False
            ownership.context_source(context).project_damage.coating_enable = True
            result, reports = rebuild_object(context, ownership.context_source(context))
        finally:
            live._APPLYING = previous
        for report_type, message in reports:
            self.report(report_type, message)
        return {'FINISHED'} if result is not None else {'CANCELLED'}


# подключаем свойства и действия этого модуля к Blender
def register():
    bpy.utils.register_class(PROJECT_DAMAGE_OT_apply_concrete_v2)
    bpy.utils.register_class(PROJECT_DAMAGE_OT_apply_lath_v2)
    bpy.utils.register_class(PROJECT_DAMAGE_OT_apply_brick_v2)
    bpy.utils.register_class(PROJECT_DAMAGE_OT_apply_hang)
    bpy.utils.register_class(PROJECT_DAMAGE_OT_apply_glass_hit)
    bpy.utils.register_class(PROJECT_DAMAGE_OT_apply_glass_drawn)
    bpy.utils.register_class(PROJECT_DAMAGE_OT_apply_fabric_solid)
    bpy.utils.register_class(PROJECT_DAMAGE_OT_apply_all_damage)
    bpy.utils.register_class(PROJECT_DAMAGE_OT_apply_coating)
    bpy.utils.register_class(PROJECT_DAMAGE_OT_update)
    bpy.utils.register_class(PROJECT_DAMAGE_OT_apply_buildup)
    bpy.utils.register_class(PROJECT_DAMAGE_OT_apply_fabric)
    bpy.utils.register_class(PROJECT_DAMAGE_OT_apply_concrete)


# убираем подключения модуля при выключении аддона
def unregister():
    if bpy.app.timers.is_registered(_stale_flush_timer):
        bpy.app.timers.unregister(_stale_flush_timer)
    stale_mesh_flush()
    bpy.utils.unregister_class(PROJECT_DAMAGE_OT_apply_brick_v2)
    bpy.utils.unregister_class(PROJECT_DAMAGE_OT_apply_lath_v2)
    bpy.utils.unregister_class(PROJECT_DAMAGE_OT_apply_concrete_v2)
    bpy.utils.unregister_class(PROJECT_DAMAGE_OT_apply_hang)
    bpy.utils.unregister_class(PROJECT_DAMAGE_OT_apply_glass_hit)
    bpy.utils.unregister_class(PROJECT_DAMAGE_OT_apply_glass_drawn)
    bpy.utils.unregister_class(PROJECT_DAMAGE_OT_apply_fabric_solid)
    bpy.utils.unregister_class(PROJECT_DAMAGE_OT_apply_concrete)
    bpy.utils.unregister_class(PROJECT_DAMAGE_OT_apply_fabric)
    bpy.utils.unregister_class(PROJECT_DAMAGE_OT_apply_all_damage)
    bpy.utils.unregister_class(PROJECT_DAMAGE_OT_apply_coating)
    bpy.utils.unregister_class(PROJECT_DAMAGE_OT_apply_buildup)
    bpy.utils.unregister_class(PROJECT_DAMAGE_OT_update)
