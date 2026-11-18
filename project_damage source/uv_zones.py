# разворачиваем только области повреждений, сохраняя внешнюю UV-карту
# SPDX-License-Identifier: GPL-3.0-or-later
"""Explicit, undoable UV generation on owned output material zones only."""
from contextlib import ExitStack

import bpy,bmesh
import numpy as np
from . import ownership,build,live,baked_materials
from .mesh_io import CLASS_NAMES,BASE_CLASS,SHADING_REGION_ATTR
from .result_transfer import MATERIAL_REGIONS
UV_NAME='ProjectDamage_Zones'
PRESERVE_OUTPUT_UV = 'project_damage_preserve_output_uv'
_PROCEDURAL_OUTPUTS = (
    PRESERVE_OUTPUT_UV, 'pd_structure_output', 'project_damage_gen',
    'project_damage_generator_key', 'project_damage_bfrag_of',
    'project_damage_rebar_of', 'project_damage_drawn_rebar_of', 'project_damage_concrete_rebar_of',
    'project_damage_lath_of', 'project_damage_bricks_of', 'project_damage_pipes_of',
    'project_damage_cables_of', 'project_damage_weld_of', 'project_damage_grass_of',
    'project_damage_gravel_of', 'project_damage_foam_of', 'project_damage_paper_generator_of',
    'project_damage_ivy_generated', 'project_damage_roots_generated',
)
_STRUCTURE_ATTRIBUTES = ('rebar_id', 'rebar_part', 'bar_id', 'pipe_id', 'cable_id',
                         'project_damage_lath_piece', 'project_damage_brick_id')


def preserves_output_uv(obj):
    if obj.type != 'MESH':
        return False
    if (any(obj.get(key) for key in _PROCEDURAL_OUTPUTS)
            or obj.get('project_damage_baked_effect') == 'boolean'
            or obj.get('project_damage_fill_kind') == 'boolean'
            or obj.get('project_damage_piece_module') == 'boolean'):
        return True
    # Older applied outputs lost their generator stamps but retain their piece IDs.
    if obj.get('project_damage_baked_surface') or obj.get(build.RESULT_STAMP):
        return False
    return any(name in obj.data.attributes for name in _STRUCTURE_ATTRIBUTES)


def targets(context,source):
    applied=baked_materials.target(context)
    if applied is not None:return [applied]
    uid=source.get('project_damage_uid')
    return [ob for ob in context.view_layer.objects if ob.type=='MESH' and ob!=source
            and uid and any(ob.get(key)==uid for key in ownership.STAMPS)]

def _exclude_authored_materials(ob,mask):
    from .material_uv import preserve_generated_uv
    protected={i for i,m in enumerate(ob.data.materials) if preserve_generated_uv(m)}
    if protected:mask &= np.array([f.material_index not in protected for f in ob.data.polygons],bool)
    return mask


def zone_mask(ob, *, protect_generated_uv=True):
    from .material_uv import preserve_output_uv
    mesh=ob.data
    if protect_generated_uv and preserve_output_uv(ob):return np.zeros(len(mesh.polygons),bool)
    if baked_materials.fragment(ob) or any(ob.get(key) for key in baked_materials._FRAGMENT_STAMPS):
        mask = baked_materials.inner_mask(ob)
        return _exclude_authored_materials(ob, mask) if protect_generated_uv else mask
    main_surface=ob.get(build.RESULT_STAMP) or ob.get('project_damage_baked_surface')
    if baked_materials.is_applied(ob) and 'project_damage_baked_surface' not in ob:
        main_surface=mesh.attributes.get(CLASS_NAMES[BASE_CLASS]) is not None
    if not main_surface:
        mask = np.ones(len(mesh.polygons), bool)
        return _exclude_authored_materials(ob, mask) if protect_generated_uv else mask
    mask=np.zeros(len(mesh.polygons),bool)
    names=[name for i,name in enumerate(CLASS_NAMES) if i!=BASE_CLASS]+[row[1] for row in MATERIAL_REGIONS]+[SHADING_REGION_ATTR]
    for name in names:
        attr=mesh.attributes.get(name)
        if attr is not None and attr.domain=='FACE' and attr.data_type=='BOOLEAN':
            values=np.empty(len(mask),bool);attr.data.foreach_get('value',values);mask|=values
    ids=mesh.attributes.get('project_damage_effect_id')
    if ids is not None and ids.domain=='FACE':
        values=np.empty(len(mask),np.int32);ids.data.foreach_get('value',values);mask |= values==33
    return _exclude_authored_materials(ob, mask) if protect_generated_uv else mask

def uv_state(mesh):
    layer=mesh.uv_layers.get(UV_NAME)
    values=None
    if layer is not None:
        values=np.empty(len(mesh.loops)*2,np.float32);layer.data.foreach_get('uv',values)
    return values,mesh.uv_layers.active.name if mesh.uv_layers.active else None,next((u.name for u in mesh.uv_layers if u.active_render),None)

def restore_uv(mesh,state,rollback=False):
    values,active,render=state;layer=mesh.uv_layers.get(UV_NAME)
    if rollback and layer is not None:
        if values is None:mesh.uv_layers.remove(layer)
        else:layer.data.foreach_set('uv',values)
    if active and mesh.uv_layers.get(active):mesh.uv_layers.active=mesh.uv_layers[active]
    if render and mesh.uv_layers.get(render):mesh.uv_layers[render].active_render=True

def unwrap_one(context,ob,mask):
    mesh=ob.data;old_name=mesh.uv_layers.active.name if mesh.uv_layers.active else None
    layer=mesh.uv_layers.get(UV_NAME)
    if layer is None:
        layer=mesh.uv_layers.new(name=UV_NAME)
        old_active=mesh.uv_layers.get(old_name) if old_name else None
        if old_active is not None:
            values=np.empty(len(mesh.loops)*2,np.float32);old_active.data.foreach_get('uv',values);layer.data.foreach_set('uv',values)
    mesh.uv_layers.active=layer
    initial=np.empty(len(mesh.loops)*2,np.float32);layer.data.foreach_get('uv',initial)
    projected=np.zeros(len(mesh.loops),bool)
    for face,yes in zip(mesh.polygons,mask):
        if yes:projected[list(face.loop_indices)]=True
    groups=sorted({poly.material_index for poly,yes in zip(mesh.polygons,mask) if yes})
    selection=[]
    for seq in (mesh.vertices,mesh.edges,mesh.polygons):
        flags=np.empty(len(seq),bool);seq.foreach_get('select',flags);selection.append(flags)
    try:
        bpy.ops.object.mode_set(mode='EDIT')
        bm=bmesh.from_edit_mesh(mesh);bm.faces.ensure_lookup_table()
        for slot in groups:
            for seq in (bm.faces,bm.edges,bm.verts):
                for element in seq:element.select_set(False)
            for face in bm.faces:
                if mask[face.index] and face.material_index==slot:face.select_set(True)
            bm.select_flush_mode();bmesh.update_edit_mesh(mesh,loop_triangles=False,destructive=False)
            if bpy.ops.uv.cube_project(cube_size=2.0,correct_aspect=False,clip_to_bounds=False,scale_to_bounds=False)!={'FINISHED'}:
                raise RuntimeError('UV projection did not finish for '+ob.name)
    finally:
        if context.mode!='OBJECT':bpy.ops.object.mode_set(mode='OBJECT')
        for seq,flags in zip((mesh.vertices,mesh.edges,mesh.polygons),selection):seq.foreach_set('select',flags)
        layer=mesh.uv_layers.get(UV_NAME)
        values=np.empty(len(mesh.loops)*2,np.float32);layer.data.foreach_get('uv',values)
        untouched=np.repeat(~projected,2);values[untouched]=initial[untouched]
        layer.data.foreach_set('uv',values)
        mesh.update()
    return len(groups)

def _fracture_owner(context):
    from . import inner_rough
    source = baked_materials.target(context) or ownership.context_source(context)
    if source is not None and source.get(inner_rough.DETAIL_STAMP):
        source = ownership.source_of(source.parent)
    return source


def fracture_targets(context):
    from . import fracture_uv
    source = _fracture_owner(context)
    if source is None:
        return []
    scene_objects = context.scene.objects
    if baked_materials.is_applied(source):
        pieces = baked_materials.fracture_objects(context, source, scene_wide=True)
        walls = set(baked_materials.split_walls(pieces))
        pieces = [obj for obj in pieces if obj not in walls]
    else:
        uid = source.get('project_damage_uid')
        if not uid:
            return []
        stamps = [stamp for stamp in baked_materials._FRAGMENT_STAMPS if stamp != 'project_damage_fragment_of']
        pieces = [obj for obj in scene_objects if obj.type == 'MESH'
                  and not obj.get('project_damage_split_wall')
                  and (any(obj.get(stamp) == uid for stamp in stamps)
                       or (obj.get('project_damage_fragment_of') and ownership.source_of(obj) == source)
                       or (obj.get('project_damage_baked_backup') == source and baked_materials.fragment(obj)))]
    return list(dict.fromkeys(obj for obj in fracture_uv.with_details(pieces)
                              if obj.type == 'MESH' and obj.name in scene_objects))


def draw_inside_uv(layout, context, *, text='Quick UV Map', has_pieces=None):
    row = layout.row(align=True)
    row.enabled = bool(fracture_targets(context)) if has_pieces is None else has_pieces
    row.operator('project_damage.unwrap_zones', text=text, icon='UV').fracture_inside = True
    row.operator('project_damage.edit_zone_uv', text='Edit Scale', icon='FULLSCREEN_ENTER').fracture_inside = True


def _restore_inside_uv(obj, original, name, coords, flags, owner_tag, slot_count, indices):
    from . import fracture_uv
    mesh = obj.data
    if mesh != original:
        obj.data = original
        if mesh.users == 0:
            bpy.data.meshes.remove(mesh)
        return
    current = fracture_uv.layer_name(mesh)
    if name is None and current is not None:
        mesh.uv_layers.remove(mesh.uv_layers[current])
    elif name is not None:
        mesh.uv_layers[name].uv.foreach_set('vector', coords)
    if owner_tag is not None:
        mesh[fracture_uv.OWNER] = owner_tag
    elif fracture_uv.OWNER in mesh:
        del mesh[fracture_uv.OWNER]
    while len(mesh.materials) > slot_count:
        mesh.materials.pop(index=len(mesh.materials)-1)
    mesh.polygons.foreach_set('material_index', indices)
    fracture_uv.copy_flags(mesh, *flags)


def unwrap_fracture_inside(context):
    from . import fracture_uv, material_update, material_uv

    targets = [(obj, baked_materials.inner_mask(obj)) for obj in fracture_targets(context)]
    targets = [(obj, mask) for obj, mask in targets if mask.any()]
    if not targets:
        raise ValueError('No stored inside faces found on the fracture pieces of this Project Damage object')
    if any(not obj.is_editable or obj.data.library is not None or obj.mode != 'OBJECT' for obj, _ in targets):
        raise ValueError('Make the fracture pieces local before unwrapping')
    with material_update.shader_edit(context), ExitStack() as rollback:
        for obj, mask in targets:
            mesh = obj.data
            name = fracture_uv.layer_name(mesh)
            coords = None
            if name is not None:
                coords = np.empty(len(mesh.loops)*2, np.float32)
                mesh.uv_layers[name].uv.foreach_get('vector', coords)
            indices = np.empty(len(mesh.polygons), np.int32)
            mesh.polygons.foreach_get('material_index', indices)
            rollback.callback(_restore_inside_uv, obj, mesh, name, coords, fracture_uv.flags(mesh),
                              mesh.get(fracture_uv.OWNER), len(mesh.materials), indices)
            if mesh.users > 1:
                obj.data = mesh.copy()
            name = fracture_uv.project(obj, mask)
            material_uv.configure_zones(obj, mask, name)
        rollback.pop_all()
    return len(targets)


# действие «Quick UV Map — Cube Projection»
class PROJECT_DAMAGE_OT_unwrap_zones(bpy.types.Operator):
    bl_idname='project_damage.unwrap_zones'
    bl_label='Quick UV Map — Cube Projection'
    bl_description='Cube-project generated material zones into ProjectDamage_Zones, including after Apply All. On fracture pieces only inner cut faces are mapped; exterior UVs stay unchanged. On an applied mesh this button affects only the selected object'
    bl_options={'REGISTER','UNDO'}
    all_fragments:bpy.props.BoolProperty(default=False,options={'HIDDEN','SKIP_SAVE'})
    fracture_inside:bpy.props.BoolProperty(default=False,options={'HIDDEN','SKIP_SAVE'})
    @classmethod
    def description(cls,context,properties):
        if properties.fracture_inside or properties.all_fragments:
            return 'Cube-project inside faces on every fracture piece of this Project Damage object, including baked physics and applied geometry'
        return cls.bl_description
    @classmethod
    def poll(cls,context):
        source = _fracture_owner(context)
        return context.mode == 'OBJECT' and source is not None and source.type == 'MESH' and (
            baked_materials.is_applied(source) or bool(source.get('project_damage_uid')) or build.result_of(source) is not None)
    def execute(self,context):
        if self.fracture_inside or self.all_fragments:
            try:
                count = unwrap_fracture_inside(context)
            except (ValueError, RuntimeError) as exc:
                self.report({'WARNING'}, str(exc))
                return {'CANCELLED'}
            self.report({'INFO'}, 'Mapped inside faces on %d fracture pieces' % count)
            return {'FINISHED'}
        source=ownership.context_source(context)
        chosen=targets(context,source) if source is not None else []
        objects=[(ob,zone_mask(ob)) for ob in chosen]
        objects=[(ob,mask) for ob,mask in objects if mask.any()]
        if not objects:
            self.report({'INFO'},'No generated material zones to unwrap');return {'CANCELLED'}
        if any(ob.library is not None or ob.data.library is not None or ob.data.users>1 for ob,_ in objects):
            self.report({'ERROR'},'Generated outputs must be local, single-user meshes before unwrapping');return {'CANCELLED'}
        snapshots=[(ob,uv_state(ob.data),ob.hide_get(),ob.hide_viewport) for ob,_ in objects]
        material_snapshots={ob:(list(ob.data.materials),[p.material_index for p in ob.data.polygons]) for ob,_ in objects}
        active=context.view_layer.objects.active;selected=list(context.selected_objects)
        select_mode=context.tool_settings.mesh_select_mode[:];previous=live._APPLYING;live._APPLYING=True
        total=0;success=False
        try:
            context.tool_settings.mesh_select_mode=(False,False,True)
            for ob,mask in objects:
                for current in tuple(context.selected_objects):current.select_set(False)
                ob.hide_viewport=False;ob.hide_set(False);ob.select_set(True);context.view_layer.objects.active=ob
                if not ob.visible_get():raise RuntimeError('Unhide the generated collection containing '+ob.name)
                total+=unwrap_one(context,ob,mask)
                from . import material_uv
                material_uv.configure_zones(ob,mask)
            success=True
        except Exception as exc:
            self.report({'ERROR'},'Zone unwrap failed: '+str(exc))
        finally:
            if context.mode!='OBJECT':bpy.ops.object.mode_set(mode='OBJECT')
            for ob,state,hidden,hide_viewport in snapshots:
                restore_uv(ob.data,state,rollback=not success);ob.hide_set(hidden);ob.hide_viewport=hide_viewport
                if not success:
                    materials,indices=material_snapshots[ob]
                    ob.data.materials.clear()
                    for material in materials:ob.data.materials.append(material)
                    ob.data.polygons.foreach_set('material_index',indices)
            for ob in tuple(context.selected_objects):ob.select_set(False)
            for ob in selected:
                if ob.name in context.view_layer.objects:ob.select_set(True)
            context.view_layer.objects.active=active;context.tool_settings.mesh_select_mode=select_mode;live._APPLYING=previous
        if not success:return {'CANCELLED'}
        from . import mesh_finalization
        for ob,_ in objects:mesh_finalization.remove_unused_slots(ob)
        baked_materials.invalidate()
        self.report({'INFO'},'Unwrapped %d material zones on %d outputs into %s; original UV maps unchanged'%(total,len(objects),UV_NAME))
        return {'FINISHED'}

def editable_uv_outputs(context, fracture_inside=False):
    if fracture_inside:
        from . import fracture_uv
        selectable = set(context.selectable_objects)
        return [obj for obj in fracture_targets(context) if fracture_uv.layer_name(obj.data)
                and obj in selectable and obj.is_editable and obj.data.library is None]
    source=ownership.context_source(context)
    if source is None:return []
    from .material_uv import preserve_output_uv
    return [ob for ob in targets(context,source)
            if not preserve_output_uv(ob) and ob.data.uv_layers.get(UV_NAME) is not None and ob.visible_get()
            and ob.library is None and ob.data.library is None]


def prepare_uv_edit(context, objects, fracture_inside=False):
    """Select only generated material zones, keeping the source mesh untouched."""
    source=ownership.context_source(context)
    preferred=context.active_object
    if preferred not in objects:
        preferred=build.result_of(source)
    if preferred not in objects:preferred=objects[0]
    for ob in tuple(context.selected_objects):ob.select_set(False)
    from . import fracture_uv
    names = {obj: fracture_uv.layer_name(obj.data) if fracture_inside else UV_NAME for obj in objects}
    for ob in objects:
        ob.data.uv_layers.active=ob.data.uv_layers[names[ob]]
        ob.select_set(True)
    context.view_layer.objects.active=preferred
    context.tool_settings.use_uv_select_sync=False
    context.tool_settings.mesh_select_mode=(False,False,True)
    masks={ob:(baked_materials.inner_mask(ob) if fracture_inside else zone_mask(ob)) for ob in objects}
    bpy.ops.object.mode_set(mode='EDIT')
    for ob in objects:
        bm=bmesh.from_edit_mesh(ob.data)
        bm.faces.ensure_lookup_table()
        layer=bm.loops.layers.uv.get(names[ob])
        for seq in (bm.faces,bm.edges,bm.verts):
            for item in seq:item.select_set(False)
        for face in bm.faces:
            selected=bool(masks[ob][face.index])
            face.select_set(selected)
        bm.select_flush_mode()
        if hasattr(bm,'uv_select_sync_from_mesh'):
            bm.uv_select_sync_from_mesh(sticky_select_mode='DISABLED')
        for face in bm.faces:
            selected=bool(masks[ob][face.index])
            for loop in face.loops:
                if hasattr(loop,'uv_select_vert_set'):
                    loop.uv_select_vert_set(selected)
                    loop.uv_select_edge_set(selected)
                else:
                    uv=loop[layer]
                    uv.select=selected
                    uv.select_edge=selected
        if hasattr(bm,'uv_select_flush'):bm.uv_select_flush(True)
        bmesh.update_edit_mesh(ob.data,loop_triangles=False,destructive=False)


# действие «Edit UV Scale»
class PROJECT_DAMAGE_OT_edit_zone_uv(bpy.types.Operator):
    bl_idname='project_damage.edit_zone_uv'
    bl_label='Edit UV Scale'
    bl_description='Open ProjectDamage_Zones in a separate UV Editor with generated zones selected. Press S to scale their UVs; run Quick UV Map first if needed'

    fracture_inside:bpy.props.BoolProperty(default=False,options={'HIDDEN','SKIP_SAVE'})

    @classmethod
    def description(cls, context, properties):
        if properties.fracture_inside:
            return 'Open the dedicated Inside Faces UV map with only the fracture interiors selected'
        return cls.bl_description

    @classmethod
    def poll(cls,context):
        return context.mode=='OBJECT' and bool(editable_uv_outputs(context) or editable_uv_outputs(context, True))

    def execute(self,context):
        objects=editable_uv_outputs(context, self.fracture_inside)
        if not objects:
            self.report({'INFO'}, 'Create a UV map first with Quick UV Map')
            return {'CANCELLED'}
        if bpy.app.background:
            self.report({'WARNING'},'Opening a UV Editor window requires the Blender interface')
            return {'CANCELLED'}
        windows=set(context.window_manager.windows)
        if bpy.ops.wm.window_new()!={'FINISHED'}:return {'CANCELLED'}
        window=next((w for w in context.window_manager.windows if w not in windows),None)
        if window is None:
            self.report({'ERROR'},'Blender did not create a new window')
            return {'CANCELLED'}
        area=max(window.screen.areas,key=lambda a:a.width*a.height)
        area.type='IMAGE_EDITOR'
        area.ui_type='UV'
        region=next(r for r in area.regions if r.type=='WINDOW')
        previous=live._APPLYING;live._APPLYING=True
        try:
            with context.temp_override(window=window,area=area,region=region):
                prepare_uv_edit(context,objects,self.fracture_inside)
                if bpy.ops.image.view_selected.poll():bpy.ops.image.view_selected()
        finally:
            live._APPLYING=previous
        self.report({'INFO'}, 'Press S in the UV Editor to change texture scale')
        return {'FINISHED'}


_CLASSES=(PROJECT_DAMAGE_OT_unwrap_zones,PROJECT_DAMAGE_OT_edit_zone_uv)
# подключаем свойства и действия этого модуля к Blender
def register():
    for cls in _CLASSES:bpy.utils.register_class(cls)
# убираем подключения модуля при выключении аддона
def unregister():
    for cls in reversed(_CLASSES):bpy.utils.unregister_class(cls)
