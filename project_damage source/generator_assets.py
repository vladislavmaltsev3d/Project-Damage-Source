# кнопки добавления самостоятельных генераторов и их настройки
# SPDX-License-Identifier: GPL-3.0-or-later
"""Standalone generators live in the shared asset browser and Settings panel."""
from types import SimpleNamespace
import bpy

ASSETS=(('plank_surface','Plank Surface','','MOD_BUILD'),('cable_curve','Cable Curve','','OUTLINER_DATA_CURVE'),
        ('cable_proximity','Cable Proximity','','EMPTY_DATA'),
        ('cloth_hanging','Cloth Hanging','','MOD_CLOTH'),
        ('cloth_curve','Cloth Along Curve','','MOD_CLOTH'),
        ('cloth_empties','Cloth Around Empties','','MOD_CLOTH'))
DAMAGE_KINDS = {'gen_metal':'METAL','gen_erosion':'EROSION'}
DUAL_ASSETS=(('gen_metal','Torn Metal','','MOD_SOLIDIFY'),)
from . import structure_surface
ASSETS += tuple((key,title,'','OUTLINER_OB_MESH') for key,(title,kind) in structure_surface.KINDS.items())
KEYS={r[0] for r in ASSETS}

def owner(context):
    target=structure_surface.owner(context)
    if target is not None:return target
    from . import damage_gen_common
    generated = damage_gen_common.generator_of(context.active_object)
    if generated is not None:
        from . import damage_generator_assets
        if damage_generator_assets.owner(generated) is None:return generated
        return None
    from . import plank_surface
    target=plank_surface.owner(context)
    if target is not None:return target
    ob=context.active_object
    if ob is None:return None
    from . import cloth,cable_generator
    co=cloth.owner_of(ob)
    if co is not None:return co
    ca=cable_generator.owner_of(ob)
    return ca if ca is not None and ca.project_damage_cable_gen.enabled else None

# собираем видимые настройки этого раздела
def draw(layout,context):
    from . import editorial,plank_surface
    ob=owner(context)
    if ob is None:return
    if structure_surface.owner(context) is not None:
        with editorial.scope(ob['pd_structure_asset']):
            return structure_surface.draw(layout,context)
    from . import damage_gen_common
    if damage_gen_common.kind_of(ob) is not None:
        damage_gen_common.draw_settings(layout, context, ob)
        return
    key='plank_surface' if plank_surface.owner(context) is not None else ob.get('project_damage_cloth_asset')
    if not key:key='cable_proximity' if ob.type=='EMPTY' else 'cable_curve'
    with editorial.scope(key):
        return _draw_geometry(layout,context)


def _draw_geometry(layout,context):
    from . import plank_surface
    if plank_surface.owner(context) is not None:
        plank_surface.draw(layout,context);return
    from . import cloth,cable_generator,material_controls
    ob=owner(context)
    if ob is None:return
    layout.label(text=ob.name,icon='OBJECT_DATA')
    if cloth.owner_of(ob) is not None:
        material_controls.centered_heading(layout,dict((r[0],r[1]) for r in ASSETS).get(ob.get('project_damage_cloth_asset',''),'Cloth Sheets'),icon='MOD_CLOTH')
        cloth.PROJECT_DAMAGE_PT_cloth_sheets.draw(SimpleNamespace(layout=layout),context)
    else:cable_generator.draw_settings(layout,context,show_sources=False)

# действие «Add Generator Asset»
class PROJECT_DAMAGE_OT_add_generator_asset(bpy.types.Operator):
    bl_idname='project_damage.add_generator_asset'
    bl_label='Add Generator Asset'
    bl_options={'REGISTER','UNDO'}
    asset:bpy.props.StringProperty()
    @classmethod
    def poll(cls,context):return context.mode=='OBJECT'
    def execute(self,context):
        if self.asset in structure_surface.KINDS:return bpy.ops.project_damage.structure_surface(action='ADD',asset=self.asset)
        from . import cloth
        if self.asset in DAMAGE_KINDS:
            return bpy.ops.project_damage.gen_add(kind=DAMAGE_KINDS[self.asset])
        if self.asset == 'paper_generator':
            return bpy.ops.project_damage.effect_action(effect=self.asset, action='ADD')
        if self.asset=='plank_surface':return bpy.ops.project_damage.plank_surface(action='ADD')
        if self.asset=='cable_curve':return bpy.ops.project_damage.cable_gen_add()
        if self.asset=='cable_proximity':return bpy.ops.project_damage.cable_gen_add_spawner()
        if self.asset not in KEYS:return {'CANCELLED'}
        title=dict((r[0],r[1]) for r in ASSETS)[self.asset]
        if self.asset=='cloth_empties':
            ob=bpy.data.objects.new(title,None);ob.empty_display_type='SPHERE';ob.empty_display_size=.2
            collection=bpy.data.collections.new(title+' Sources');context.scene.collection.children.link(collection)
            collection.objects.link(ob)
        else:
            name='Cloth Hanging Curve' if self.asset=='cloth_hanging' else title
            data=bpy.data.curves.new(name,'CURVE');data.dimensions='3D';data.use_path=False
            sp=data.splines.new('BEZIER');sp.bezier_points.add(1);sp.resolution_u=16
            for point,co in zip(sp.bezier_points,((-1,0,0),(1,0,0))):
                point.co=co;point.handle_left_type=point.handle_right_type='AUTO'
            ob=bpy.data.objects.new(name,data);context.collection.objects.link(ob)
        ob.location=context.scene.cursor.location
        ob['project_damage_cloth_asset']=self.asset
        from . import quality_defaults
        quality_defaults.standalone(ob,self.asset)
        s=ob.project_damage_cloth
        s.source={'cloth_hanging':'HANGING','cloth_curve':'CURVE','cloth_empties':'EMPTIES'}[self.asset]
        if s.source=='EMPTIES':s.empties=collection
        else:s.curve=ob
        if self.asset=='cloth_hanging':s.sheet_type='CURTAIN'
        for selected in context.selected_objects:selected.select_set(False)
        ob.select_set(True);context.view_layer.objects.active=ob
        context.view_layer.update()
        return bpy.ops.project_damage.cloth_sheets()
