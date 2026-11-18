# небольшие помощники для рисунков, масок и настроек формы
# SPDX-License-Identifier: GPL-3.0-or-later
"""Seed creation, editable weld masks, and isolated additive-material regions."""
from .blender_compat import enable_nodes
import bmesh
import bpy
import numpy as np
from . import ownership,live


def add_properties(cls):
    for name,label in [('pile_material','Corner Piles'),('concrete_rebar_material','Concrete Reinforcement')]:
        cls.__annotations__[name]=bpy.props.PointerProperty(name=label,type=bpy.types.Material,update=live.touch_prop,
            description='Material for this generator region only; does not replace other effects')
    cls.__annotations__['rusty_master_scale']=bpy.props.FloatProperty(name='Master Scale',default=100.,min=1.,soft_max=400.,subtype='PERCENTAGE',update=live.touch_prop,
        description='Scale Rusty Holes sizes, crack widths, sheet thickness and drawing distances together')

    cls.__annotations__['split_effect_zones']=bpy.props.BoolProperty(name='Split Effect Zones',default=True,update=live.touch_prop,description='Duplicate boundary vertices so damage groups select exact zones. Geometry stays in place; modifiers may open the seams.')

    cls.__annotations__['hang_master_scale']=bpy.props.FloatProperty(name='Master Scale',default=100.,min=1.,soft_max=400.,subtype='PERCENTAGE',update=live.touch_prop,description='Multiply the minimum and maximum opening size together')
    cls.__annotations__['glass_master_scale']=bpy.props.FloatProperty(name='Master Scale',default=100.,min=1.,soft_max=400.,subtype='PERCENTAGE',update=live.touch_prop,description='Scale glass holes, cracks, chips and drawing widths together')

    cls.__annotations__['brick_back_resolution']=bpy.props.FloatProperty(name='Resolution',default=1.,min=.05,soft_max=4.,update=live.touch_prop,
        description='Controls the mesh density of backing relief. 0.05 gives a coarse mesh and 1 is the standard density. Higher values add detail up to the triangle budget. Texture uses this setting when Mesh Edge Size is 0')


def region_material(props,field,default_name):
    if field=='metaltear_material' and getattr(props,field) is None:
        from .metal_tear import default_material
        return default_material()
    material=getattr(props,field)
    if material is None:
        if field=='concrete_rebar_material':
            from .material_defaults import get
            material=get('rebar')
        else:
            material=bpy.data.materials.get(default_name)
            if material is None:
                material=bpy.data.materials.new(default_name);enable_nodes(material)
                color=(.24,.19,.13,1.) if field=='pile_material' else (.32,.28,.20,1.)
                material.diffuse_color=color
                material.node_tree.nodes.get('Principled BSDF').inputs['Base Color'].default_value=color
        previous=live._APPLYING;live._APPLYING=True
        try:setattr(props,field,material)
        finally:live._APPLYING=previous
    return material


def apply_material_regions(mesh,out,props):
    ids=np.asarray(out.get('effect_ids',()),dtype=np.int32)
    classes=np.asarray(out.get('tri_class',()),dtype=np.int32)
    if len(ids)!=len(mesh.polygons):return
    values=np.empty(len(mesh.polygons),np.int32);mesh.polygons.foreach_get('material_index',values)
    for key,field,name,mask in (
        (34,'metaltear_material','ProjectDamage_MetalTear',ids==34),
        (32,'erosion_material','ProjectDamage_Erosion',ids==32),
        (31,'surface_secondary_material','',ids==31),
        (28,'pile_material','ProjectDamage_Corner_Piles',ids==28),
        (29,'deposit_material','ProjectDamage_Deposits',ids==29),
        (10,'concrete_rebar_material','', (ids==10)&(classes==5) if len(classes)==len(ids) else np.zeros(len(ids),bool)),
    ):
        if not mask.any():continue
        if field=='surface_secondary_material' and props.surface_secondary_material is None:continue
        material=region_material(props,field,name)
        slot=next((i for i,m in enumerate(mesh.materials) if m==material),None)
        if slot is None:slot=len(mesh.materials);mesh.materials.append(material)
        values[mask]=slot
    mesh.polygons.foreach_set('material_index',values)


# действие «Add Growth Seed»
class PROJECT_DAMAGE_OT_add_growth_seed(bpy.types.Operator):
    bl_idname='project_damage.add_growth_seed';bl_label='Add Growth Seed';bl_options={'REGISTER','UNDO'}
    bl_description='Create the Seeds collection if needed, then add a clearly named Empty at the 3D cursor'
    kind:bpy.props.EnumProperty(items=(('ivy','Ivy','Ivy seed'),('roots','Roots','Root seed')))
    @classmethod
    def poll(cls,context):return context.mode=='OBJECT' and ownership.context_source(context) is not None
    def execute(self,context):
        source=ownership.context_source(context);s=getattr(source,'project_damage_'+self.kind)
        collection=s.seeds
        if collection is None:
            collection=bpy.data.collections.new(source.name+' | '+self.kind.title()+' Seeds');context.scene.collection.children.link(collection)
            s.seeds=collection
        elif collection!=context.scene.collection and collection not in tuple(_collections(context.scene.collection)):
            context.scene.collection.children.link(collection)
        empty=bpy.data.objects.new(source.name+' | '+self.kind.title()+' Seed',None)
        empty.empty_display_type='SPHERE';empty.empty_display_size=.05
        collection.objects.link(empty);empty.location=context.scene.cursor.location.copy()
        empty['project_damage_growth_seed']=self.kind
                                                                                       
        from . import ivy,roots
        (ivy if self.kind=='ivy' else roots).queue_update(source)
        return {'FINISHED'}


def needs_seed(source,kind):
    """Ivy or Roots is on but nothing tells it where to start: nothing can grow."""
    if source is None or not getattr(source.project_damage,kind+'_enable',False):return False
    s=getattr(source,'project_damage_'+kind,None)
    if s is None:return False
    if s.seeds is not None and any(o.type=='EMPTY' for o in s.seeds.all_objects):return False
    for attr in ('guide','curve'):
        if getattr(s,attr,None) is not None:return False
    return True


def _view3d(context):
    window=context.window
    if window is None:return None,None
    area=context.area if context.area is not None and context.area.type=='VIEW_3D' else next(
        (a for a in window.screen.areas if a.type=='VIEW_3D'),None)
    if area is None:return None,None
    return area,next((r for r in area.regions if r.type=='WINDOW'),None)


def request_seed(context,source,kinds):
    """Ask the user to place the 3D cursor and press Enter for each growth kind
    that has no seed yet. False when there is no window to ask in."""
    kinds=[k for k in kinds if needs_seed(source,k)]
    if not kinds or bpy.app.background:return False
    area,region=_view3d(context)
    if area is None or region is None:return False
    with context.temp_override(window=context.window,area=area,region=region):
        result=bpy.ops.project_damage.place_growth_seed('INVOKE_DEFAULT',source_name=source.name,kinds=','.join(kinds))
    return 'RUNNING_MODAL' in result


# действие «Place Growth Seed»
class PROJECT_DAMAGE_OT_place_growth_seed(bpy.types.Operator):
    bl_idname='project_damage.place_growth_seed';bl_label='Place Growth Seed'
    bl_description='Place the 3D cursor where the ivy or roots start (Shift+Right-click), then press Enter'
    bl_options={'REGISTER','UNDO'}
    source_name:bpy.props.StringProperty(options={'HIDDEN','SKIP_SAVE'})
    kinds:bpy.props.StringProperty(default='ivy',options={'HIDDEN','SKIP_SAVE'})
    PROMPT='Place the 3D cursor where the {what} should start (Shift+Right-click on the surface), then press Enter  |  Esc: skip'

    def _source(self,context):
        return bpy.data.objects.get(self.source_name) if self.source_name else ownership.context_source(context)

    def _kinds(self):
        return [k for k in self.kinds.split(',') if k in {'ivy','roots'}]

    def invoke(self,context,event):
        source=self._source(context)
        if source is None or not self._kinds():return {'CANCELLED'}
        self.source_name=source.name
        text=self.PROMPT.format(what=' and '.join(self._kinds()))
        try:context.workspace.status_text_set(text)
        except (AttributeError,RuntimeError):pass
        if context.area is not None:context.area.header_text_set(text)
        context.window_manager.modal_handler_add(self)
        self.report({'INFO'},text)
        return {'RUNNING_MODAL'}

    def _finish(self,context):
        try:context.workspace.status_text_set(None)
        except (AttributeError,RuntimeError):pass
        window=context.window
        for area in (window.screen.areas if window is not None else ()):
            if area.type=='VIEW_3D':
                try:area.header_text_set(None)
                except RuntimeError:pass

    def modal(self,context,event):
        source=self._source(context)
        if source is None:
            self._finish(context);return {'CANCELLED'}
        if event.type=='ESC' and event.value=='PRESS':
            self._finish(context)
            self.report({'INFO'},'No seed placed: use Add Growth Seed in the Ivy / Roots panel when ready')
            return {'CANCELLED'}
        if event.type in {'RET','NUMPAD_ENTER'} and event.value=='PRESS':
            self._finish(context)
            return self.execute(context)
        # orbit, zoom, Shift+Right-click to move the cursor: everything else passes through
        return {'PASS_THROUGH'}

    def execute(self,context):
        source=self._source(context)
        if source is None:return {'CANCELLED'}
        grown=[]
        with context.temp_override(active_object=source,object=source,selected_objects=[source],selected_editable_objects=[source]):
            for kind in self._kinds():
                if bpy.ops.project_damage.add_growth_seed(kind=kind)!={'FINISHED'}:continue
                try:
                    if getattr(bpy.ops.project_damage,'grow_'+kind)()=={'FINISHED'}:grown.append(kind)
                except RuntimeError as exc:
                    self.report({'WARNING'},'Seed placed; grow it with Grow %s (%s)'%(kind.title(),exc))
        if grown:self.report({'INFO'},'Seed placed at the 3D cursor: '+', '.join(grown)+' grown')
        return {'FINISHED'}


# действие «Edit Weld Mask»
class PROJECT_DAMAGE_OT_weld_mask(bpy.types.Operator):
    bl_idname='project_damage.weld_mask';bl_label='Edit Weld Mask';bl_options={'REGISTER','UNDO'}
    bl_description='Edit the weld mask on the original mesh using weights or the On Edges selection'
    action:bpy.props.EnumProperty(items=(
        ('PAINT','Weight Paint','Paint the vertex group'),
        ('EDIT','Edit Mode','Select vertices or edges for the weld area'),
        ('ASSIGN','Assign Selection','Add the selection to the current mask'),
        ('FINISH','Finish Mask','Return to the damage mesh'),
        ('CLEAR_EDGES','Clear Edge Mask','Remove all assigned edges from the edge mask'),
    ))
    @classmethod
    def poll(cls,context):return ownership.context_source(context) is not None
    def execute(self,context):
        from . import build,paint,weld
        source=ownership.context_source(context);s=source.project_damage_weld
        target=source
        editing_source = context.active_object == target and context.mode == 'EDIT_MESH'
        if self.action=='FINISH':
            if context.mode!='OBJECT':bpy.ops.object.mode_set(mode='OBJECT')
            result=build.result_of(source)
            if result is not None:
                source.hide_set(True);result.hide_set(False);paint._activate(context,result)
            live.touch_prop(s,context)
            return {'FINISHED'}
        if self.action=='CLEAR_EDGES':
            if not editing_source or not s.on_edges:
                return {'CANCELLED'}
            bm = bmesh.from_edit_mesh(target.data)
            mask = bm.edges.layers.int.get(weld.EDGE_MASK)
            if mask is not None:
                bm.edges.layers.int.remove(mask)
                bmesh.update_edit_mesh(target.data, loop_triangles=False, destructive=False)
            return {'FINISHED'}
        if self.action=='ASSIGN':
            if not editing_source:return {'CANCELLED'}
            if s.on_edges:
                self._assign_edges(target.data, weld.EDGE_MASK)
                return {'FINISHED'}
            return bpy.ops.object.vertex_group_assign()
        if self.action=='EDIT' and s.on_edges:
            if not editing_source:
                if context.mode!='OBJECT':bpy.ops.object.mode_set(mode='OBJECT')
                target.hide_set(False);paint._activate(context,target)
                bpy.ops.object.mode_set(mode='EDIT')
            bpy.ops.mesh.select_mode(type='EDGE')
            if not editing_source:
                mesh = target.data
                bm = bmesh.from_edit_mesh(mesh)
                mask = bm.edges.layers.int.get(weld.EDGE_MASK)
                if mask is not None:
                    bpy.ops.mesh.select_all(action='DESELECT')
                    for edge in bm.edges:
                        if not edge.hide and edge[mask]:
                            edge.select_set(True)
                    bm.select_flush_mode()
                    bmesh.update_edit_mesh(mesh, loop_triangles=False, destructive=False)
            self.report({'INFO'},'Select edges and use Assign to Weld Mask to add them')
            return {'FINISHED'}
        if context.mode!='OBJECT':bpy.ops.object.mode_set(mode='OBJECT')
        group=target.vertex_groups.get(s.mask_group) if s.mask_group else None
        if group is None:group=target.vertex_groups.new(name='PD_Weld_Mask')
        s.mask_group=group.name;target.vertex_groups.active_index=group.index
        target.hide_set(False);paint._activate(context,target)
        bpy.ops.object.mode_set(mode='WEIGHT_PAINT' if self.action=='PAINT' else 'EDIT')
        self.report({'INFO'},'Paint weights for welds' if self.action=='PAINT' else 'Select vertices, then use Assign to Weld Mask')
        return {'FINISHED'}

    @staticmethod
    def _assign_edges(mesh, name):
        bm = bmesh.from_edit_mesh(mesh)
        if not any(edge.select and not edge.hide for edge in bm.edges):
            return
        mask = bm.edges.layers.int.get(name)
        if mask is None:
            mask = bm.edges.layers.int.new(name)
        for edge in bm.edges:
            if edge.select and not edge.hide:
                edge[mask] = 1
        bmesh.update_edit_mesh(mesh, loop_triangles=False, destructive=False)

CLASSES=(PROJECT_DAMAGE_OT_add_growth_seed,PROJECT_DAMAGE_OT_place_growth_seed,PROJECT_DAMAGE_OT_weld_mask)


def toggle_box(layout,props,field,title):
    box=layout.box();row=box.row(align=True);row.alignment='CENTER';row.scale_y=1.5
    row.prop(props,field,text=title,toggle=True,icon='CHECKMARK')
    from . import editorial
    editorial.button(row,title)
    return box.column(align=True) if getattr(props,field) else None


def _collections(root):
    for child in root.children:
        yield child
        yield from _collections(child)


class PROJECT_DAMAGE_OT_concrete_noise_preset(bpy.types.Operator):
    bl_idname='project_damage.concrete_noise_preset';bl_label='Concrete Surface Noise';bl_options={'REGISTER','UNDO'}
    preset:bpy.props.EnumProperty(items=(('CONCRETE','Concrete','Rough exposed concrete'),('SOFT','Soft Concrete','Softer exposed concrete')))
    def execute(self,context):
        source=ownership.context_source(context)
        if source is None:return {'CANCELLED'}
        p=source.project_damage;old=live._APPLYING;live._APPLYING=True
        try:
            p.concrete_aggregate_size=.6 if self.preset=='CONCRETE' else 1.2
            p.concrete_surface_detail=3 if self.preset=='CONCRETE' else 2
            p.concrete_surface_distortion=.2 if self.preset=='CONCRETE' else .05
            p.concrete_surface_roughness=45 if self.preset=='CONCRETE' else 25
            p.concrete_surface_strength=100 if self.preset=='CONCRETE' else 60
        finally:live._APPLYING=old
        live.touch_prop(p,context);return {'FINISHED'}
CLASSES += (PROJECT_DAMAGE_OT_concrete_noise_preset,)


# действие «Back Surface Noise Preset»
class PROJECT_DAMAGE_OT_back_noise_preset(bpy.types.Operator):
    bl_idname='project_damage.back_noise_preset';bl_label='Back Surface Noise Preset';bl_options={'REGISTER','UNDO'}
    preset:bpy.props.EnumProperty(items=(('CONCRETE','Concrete','Rough relief'),('SOFT','Soft Concrete','Gentle relief'),('FLAT','Flat','No relief')))
    def execute(self,context):
        source=ownership.context_source(context)
        if source is None:return {'CANCELLED'}
        p=source.project_damage;old=live._APPLYING;live._APPLYING=True
        try:
            p.lath_surf_mode='0'
            p.lath_noise_strength={'CONCRETE':.6,'SOFT':.25,'FLAT':0.}[self.preset]
            p.lath_noise_scale=10. if self.preset=='CONCRETE' else 15.
            p.lath_noise_detail=4. if self.preset=='CONCRETE' else 2.
            p.lath_noise_roughness=.5 if self.preset=='CONCRETE' else .3
            p.lath_noise_distortion=.2 if self.preset=='CONCRETE' else 0.
        finally:live._APPLYING=old
        live.touch_prop(p,context);return {'FINISHED'}
CLASSES += (PROJECT_DAMAGE_OT_back_noise_preset,)


# действие «Take from Cavity»
class PROJECT_DAMAGE_OT_surface_material_from_cavity(bpy.types.Operator):
    bl_idname='project_damage.surface_material_from_cavity';bl_label='Take from Cavity';bl_options={'REGISTER','UNDO'}
    bl_description='Copy the Cavities material to Additional Damage; the two selectors remain independent afterwards'
    def execute(self,context):
        source=ownership.context_source(context)
        if source is None:return {'CANCELLED'}
        p=source.project_damage;mat=p.surface_damage_material
        if mat is None:
            from . import build
            result=build.result_of(source)
            if result is not None:
                ids=result.data.attributes.get('project_damage_effect_id')
                if ids is not None:
                    for face,stamp in zip(result.data.polygons,ids.data):
                        if stamp.value==3 and face.material_index<len(result.data.materials):
                            mat=result.data.materials[face.material_index]
                            if mat is not None:break
        if mat is None:
            from .mesh_io import DAMAGE_MATERIAL_NAMES
            name=DAMAGE_MATERIAL_NAMES[4]
            mat=bpy.data.materials.get(name) or bpy.data.materials.get('project_damage_holes_P')
            if mat is None:
                mat=bpy.data.materials.new(name);enable_nodes(mat)
                mat.diffuse_color=(.18,.15,.12,1.)
                shader=mat.node_tree.nodes.get('Principled BSDF')
                shader.inputs['Base Color'].default_value=mat.diffuse_color
                shader.inputs['Roughness'].default_value=.8
        p.surface_secondary_material=mat
        return {'FINISHED'}
CLASSES += (PROJECT_DAMAGE_OT_surface_material_from_cavity,)


# действие «Add Hit at Cursor»
class PROJECT_DAMAGE_OT_glass_add_impact(bpy.types.Operator):
    bl_idname='project_damage.glass_add_impact';bl_label='Add Hit at Cursor';bl_options={'REGISTER','UNDO'}
    bl_description='Create an impact Empty at the 3D cursor; create and assign the Hit Empties collection if needed'
    def execute(self,context):
        source=ownership.context_source(context)
        if source is None:return {'CANCELLED'}
        p=source.project_damage;old=live._APPLYING;live._APPLYING=True
        try:
            collection=p.glass_hit_collection
            if collection is None:
                collection=bpy.data.collections.new(source.name+' | Glass Impacts')
                context.scene.collection.children.link(collection);p.glass_hit_collection=collection
            elif collection!=context.scene.collection and collection not in tuple(_collections(context.scene.collection)):
                context.scene.collection.children.link(collection)
            empty=bpy.data.objects.new(source.name+' | Glass Hit',None)
            empty.empty_display_type='SPHERE';empty.empty_display_size=.1
            collection.objects.link(empty);empty.location=context.scene.cursor.location.copy()
        finally:live._APPLYING=old
        live.touch_prop(p,context)
        return {'FINISHED'}
CLASSES += (PROJECT_DAMAGE_OT_glass_add_impact,)


# действие «Add Cell Fracture Point»
class PROJECT_DAMAGE_OT_cellf_add_point(bpy.types.Operator):
    bl_idname='project_damage.cellf_add_point';bl_label='Add Cell Fracture Point';bl_options={'REGISTER','UNDO'}
    bl_description='Create a proximity Empty at the 3D cursor and assign its collection'
    def execute(self,context):
        source=ownership.context_source(context)
        if source is None:return {'CANCELLED'}
        p=source.project_damage;old=live._APPLYING;live._APPLYING=True
        try:
            collection=p.cellf_prox_collection
            if collection is None:
                collection=bpy.data.collections.new(source.name+' | Cell Fracture Points')
                context.scene.collection.children.link(collection);p.cellf_prox_collection=collection
            elif collection!=context.scene.collection and collection not in tuple(_collections(context.scene.collection)):
                context.scene.collection.children.link(collection)
            point=bpy.data.objects.new(source.name+' | Fracture Point',None)
            point.empty_display_type='SPHERE';point.empty_display_size=.2
            collection.objects.link(point);point.location=context.scene.cursor.location.copy()
        finally:live._APPLYING=old
        live.touch_prop(p,context);return {'FINISHED'}
CLASSES += (PROJECT_DAMAGE_OT_cellf_add_point,)
