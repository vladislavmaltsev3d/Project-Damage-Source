# встраиваем новые генераторы в список эффектов и общий Apply
from types import SimpleNamespace
import bpy
from mathutils import Matrix

KINDS={'gen_metal':'METAL','gen_wood':'WOOD','gen_tiles':'TILES','gen_erosion':'EROSION','gen_crater':'CRATER'}
ROWS=(('gen_metal','Torn Metal','gen_metal_enable','MOD_SOLIDIFY'),
      ('gen_wood','Split Wood','gen_wood_enable','MOD_BUILD'),
      ('gen_tiles','Broken Tiles','gen_tiles_enable','MESH_GRID'),
      ('gen_erosion','Weathered Stone','gen_erosion_enable','MOD_DISPLACE'),
      ('gen_crater','Blast Crater','gen_crater_enable','MOD_EXPLODE'))
RETIRED_KEYS=frozenset({'gen_wood','gen_tiles','gen_crater'})
RETIRED_KINDS=frozenset(KINDS[key] for key in RETIRED_KEYS)
STAMPS=tuple('project_damage_'+key+'_of' for key in KINDS)


def owner(obj):
    if obj is None or obj.get('project_damage_generator_key') not in KINDS:return None
    source=getattr(obj,'project_damage_og_object',None)
    return source if source is not None and source!=obj else None


def output(source,key):
    from . import runtime_lookup
    return runtime_lookup.output(source, 'project_damage_'+key+'_of', strict_uid=True)


def settings(source,key):
    from . import damage_gen_common as G
    return getattr(source,G.KINDS[KINDS[key]].prop)


def _replaces(s):
    return getattr(s,'mode','')=='OBJECT' or getattr(s,'shape','')=='SOURCE'


def ensure(context,source,result,key):
    from . import damage_gen_common as G
    obj=output(source,key)
    if obj is not None:return obj
    kind=G.KINDS[KINDS[key]];previous=G._building;G._building=True
    try:
        if kind.add_object:
            was_selected=result.select_get();result.select_set(True)
            try:
                with context.temp_override(object=result,active_object=result,selected_objects=[result],selected_editable_objects=[result]):
                    obj=kind.add_object(None,context)
            finally:result.select_set(was_selected)
        else:
            obj=bpy.data.objects.new(source.name+' · '+kind.label,bpy.data.meshes.new(kind.label))
            (result.users_collection[0] if result.users_collection else context.scene.collection).objects.link(obj)
            obj.matrix_world=result.matrix_world
            obj[G.STAMP]=kind.key
        s=settings(source,key);fitted=getattr(obj,kind.prop)
        if not s.get('_in_hierarchy'):
            for prop in fitted.bl_rna.properties:
                name=prop.identifier
                if name=='rna_type' or prop.is_readonly or name=='resolution_quality':continue
                if prop.type not in {'BOOLEAN','INT','FLOAT','STRING','ENUM','POINTER'}:continue
                if not s.is_property_set(name):setattr(s,name,getattr(fitted,name))
            s['_resolution_reference']=s.resolution
            entry=context.preferences.addons.get(__package__)
            if entry and getattr(entry.preferences,'low_details_mode',False):s.resolution_quality=20.0
            s['_in_hierarchy']=True
        if hasattr(s,'source'):s.source=result
        obj['project_damage_generator_key']=key
        obj['project_damage_'+key+'_of']=source['project_damage_uid']
        obj.project_damage_og_object=source
        world=obj.matrix_world.copy();obj.parent=source;obj.matrix_world=world
        return obj
    finally:G._building=previous


def after_build(context,source,result,reports=None):
    from . import damage_gen_common as G,incremental
    reports=reports if reports is not None else []
    for key in KINDS:
        obj=output(source,key)
        on=getattr(source.project_damage,key+'_enable',False) and not source.project_damage.all_off
        if on:
            obj=ensure(context,source,result,key);s=settings(source,key)
            if key in {'gen_tiles','gen_crater'} and not s.get('_surface_on_object'):
                old=G._building;G._building=True
                try:
                    if s.mode!='OBJECT':
                        from . import damage_gen_surface
                        s.mode='OBJECT';s.source=result;damage_gen_surface.fit(obj,s,result)
                    s['_surface_on_object']=True
                finally:G._building=old
            if hasattr(s, 'source') and s.source != result:
                old = G._building
                G._building = True
                try:
                    s.source = result
                finally:
                    G._building = old
            if not incremental.reuse_output(context,source,result,key,obj,s):
                try:
                    G.rebuild(context,obj)
                    incremental.remember_output(context,source,result,key,obj,s)
                except Exception as exc:
                    reports.append(({'ERROR'},G.KINDS[KINDS[key]].label+': '+str(exc)))
                    on=False
        if obj is not None:
            hidden = not on
            if obj.hide_get() != hidden:
                obj.hide_set(hidden)
            if obj.hide_render != hidden:
                obj.hide_render = hidden


def visibility(context,source,result):
    enabled=[(key,output(source,key)) for key in KINDS if getattr(source.project_damage,key+'_enable',False) and not source.project_damage.all_off]
    replaces=[obj for key,obj in enabled if obj is not None and len(obj.data.polygons) and not obj.hide_get() and _replaces(settings(source,key))]
    if replaces:
        if not result.hide_get():
            result.hide_set(True)
        if not result.hide_render:
            result.hide_render = True
        active=context.view_layer.objects.active
        if active in (source,result):
            rows=source.project_damage_effects
            key=rows[source.project_damage_effect_index].key if rows and source.project_damage_effect_index<len(rows) else ''
            chosen=output(source,key) if key in KINDS else replaces[-1]
            if chosen is None or chosen.hide_get():chosen=replaces[-1]
            for obj in context.selected_objects:obj.select_set(False)
            chosen.select_set(True);context.view_layer.objects.active=chosen
    elif result.get('project_damage_generated_body_hidden'):
        result.hide_set(False);result.hide_render=False
    if bool(replaces) != bool(result.get('project_damage_generated_body_hidden')):
        result['project_damage_generated_body_hidden'] = bool(replaces)


def refresh(context,source,key,action):
    from . import build,live
    previous=live._APPLYING;live._APPLYING=True
    try:
        result=build.result_of(source)
        if result is None:raise RuntimeError('Use Add Project Damage first')
        if action=='REMOVE':
            obj=output(source,key)
            if obj is not None:
                for marker in list(obj.children):
                    if marker.get('pd_gen_marker'):bpy.data.objects.remove(marker,do_unlink=True)
                mesh=obj.data;bpy.data.objects.remove(obj,do_unlink=True)
                if not mesh.users:bpy.data.meshes.remove(mesh)
        reports=[]
        after_build(context,source,result,reports)
        visibility(context,source,result)
        errors=[message for level,message in reports if 'ERROR' in level]
        if errors:raise RuntimeError('; '.join(errors))
        return {'FINISHED'}
    finally:live._APPLYING=previous


# собираем видимые настройки этого раздела
def draw(layout,context,key):
    from . import build,damage_gen_common as G,ownership
    source=ownership.context_source(context);obj=output(source,key)
    if obj is None:
        op=layout.operator('project_damage.effect_action',text='Generate '+dict((r[0],r[1]) for r in ROWS)[key],icon='FILE_REFRESH')
        op.effect=key;op.action='APPLY';return
    s=settings(source,key)
    kind=G.KINDS[KINDS[key]]
    kind.draw(layout,s,context,obj)
    box=G.box(layout,'Materials','MATERIAL')
    from .material_library import draw_property
    for slot in obj.material_slots:
        draw_property(box,slot,'material',text=slot.name or 'Material')


def bake_selected(context,source,key):
    from . import build,damage_gen_common as G,live,mesh_finalization,mesh_states
    obj=output(source,key)
    if obj is None:raise RuntimeError('Generate the effect before applying it')
    s=settings(source,key);previous=live._APPLYING;live._APPLYING=True
    try:
        G.rebuild(context,obj)
        for marker in list(obj.children):
            if marker.get(G.MARKER):bpy.data.objects.remove(marker,do_unlink=True)
        if _replaces(s):
            old=source.data
            new=obj.data.copy();new.transform(source.matrix_world.inverted_safe()@obj.matrix_world)
            source.data=new
            setattr(source.project_damage,key+'_enable',False)
            result,reports=build.rebuild_object(context,source)
            if result is None:
                source.data=old;setattr(source.project_damage,key+'_enable',True)
                if not new.users:bpy.data.meshes.remove(new)
                raise RuntimeError('Could not rebuild remaining effects')
            mesh_states.remember_initial(source, old)
            mesh_finalization.remove_unused_slots(source)
            mesh_finalization.remove_unused_slots(result)
            if mesh_states.save_all(context):source['project_damage_before_'+key]=old
            else:mesh_states.discard_unused((old,))
            mesh=obj.data;bpy.data.objects.remove(obj,do_unlink=True)
            if not mesh.users:bpy.data.meshes.remove(mesh)
        else:
            setattr(source.project_damage,key+'_enable',False)
            world=obj.matrix_world.copy();obj.parent=None;obj.matrix_world=world
            for name in (G.STAMP,G.SOURCE,'project_damage_generator_key','project_damage_'+key+'_of'):
                if name in obj:del obj[name]
            obj.project_damage_og_object=None
            mesh_finalization.remove_unused_slots(obj)
            visibility(context,source,build.result_of(source))
        for i in reversed(range(len(source.project_damage_effects))):
            if source.project_damage_effects[i].key==key:source.project_damage_effects.remove(i)
        source.project_damage_effect_index=min(source.project_damage_effect_index,max(0,len(source.project_damage_effects)-1))
        return []
    finally:live._APPLYING=previous
