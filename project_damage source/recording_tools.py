# небольшие помощники для рисунков, масок и настроек формы
# SPDX-License-Identifier: GPL-3.0-or-later
"""Seed creation, editable weld masks, and isolated additive-material regions."""
from .blender_compat import enable_nodes
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


# действие «Edit Weld Mask»
class PROJECT_DAMAGE_OT_weld_mask(bpy.types.Operator):
    bl_idname='project_damage.weld_mask';bl_label='Edit Weld Mask';bl_options={'REGISTER','UNDO'}
    bl_description='Create a Weld Mask group on the current generated mesh; paint weights or assign selected vertices'
    action:bpy.props.EnumProperty(items=(('PAINT','Weight Paint','Paint the weld area'),('EDIT','Edit Mode','Select vertices for the weld area'),('ASSIGN','Assign Selection','Assign selected vertices to the mask'),('FINISH','Finish Mask','Return to the damage mesh')))
    @classmethod
    def poll(cls,context):return ownership.context_source(context) is not None
    def execute(self,context):
        from . import build,paint
        source=ownership.context_source(context);s=source.project_damage_weld
        target=source
        if self.action=='FINISH':
            if context.mode!='OBJECT':bpy.ops.object.mode_set(mode='OBJECT')
            result=build.result_of(source)
            if result is not None:
                source.hide_set(True);result.hide_set(False);paint._activate(context,result)
            live.touch_prop(s,context)
            return {'FINISHED'}
        if self.action=='ASSIGN':
            if context.active_object!=target or context.mode!='EDIT_MESH':return {'CANCELLED'}
            bpy.ops.object.vertex_group_assign();return {'FINISHED'}
        if context.mode!='OBJECT':bpy.ops.object.mode_set(mode='OBJECT')
        group=target.vertex_groups.get(s.mask_group) if s.mask_group else None
        if group is None:group=target.vertex_groups.new(name='PD_Weld_Mask')
        s.mask_group=group.name;target.vertex_groups.active_index=group.index
        target.hide_set(False);paint._activate(context,target)
        bpy.ops.object.mode_set(mode='WEIGHT_PAINT' if self.action=='PAINT' else 'EDIT')
        self.report({'INFO'},'Paint weights for welds' if self.action=='PAINT' else 'Select vertices, then use Assign to Weld Mask')
        return {'FINISHED'}

CLASSES=(PROJECT_DAMAGE_OT_add_growth_seed,PROJECT_DAMAGE_OT_weld_mask)


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
