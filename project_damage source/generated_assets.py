# встраиваем отдельные результаты пены, сварки и рассеивания в общий эффект
# SPDX-License-Identifier: GPL-3.0-or-later
"""Source-owned standalone Weld, Grass and Gravel outputs in the damage stack."""
from . import output_collections
from types import SimpleNamespace
import bpy
from . import ownership

KEYS = ('weld', 'grass', 'gravel', 'foam')
LABELS = {'weld':'Welded Edges', 'grass':'Grass Scatter', 'gravel':'Gravel Scatter', 'foam':'Expanding Foam', 'paper_generator':'Paper Generator'}


def module(key):
    from . import weld, grass, gravel, foam
    return {'weld':weld, 'grass':grass, 'gravel':gravel, 'foam':foam}[key]


def output(source, key):
    uid=source.get('project_damage_uid')
    return next((ob for ob in bpy.data.objects
                 if uid and ob.type=='MESH' and ob.get('project_damage_'+key+'_of')==uid), None)


def place(context, source, target, key, mesh, *, matrix=None):
    """Replace only mesh data; keep the output identity, physics and constraints."""
    from . import material_uv, material_controls, effect_groups
    snapshots=material_uv.capture(source)
    obj=output(source,key)
    if obj is None:
        obj=bpy.data.objects.new(source.name+'_project_damage_'+key,mesh)
    else:
        previous=obj.data
        obj.data=mesh
        if previous.users==0:bpy.data.meshes.remove(previous)
    output_collections.link_object(context,source,obj)
    obj['project_damage_'+key+'_of']=source['project_damage_uid']
    obj.project_damage_og_object=source
    obj.parent=source
    obj.matrix_world=target.matrix_world if matrix is None else matrix
    obj.hide_set(False)
    obj.hide_render=False
    material_controls.apply_shared_output_material(source)
    material_uv.after_build(source,snapshots)
    materials={mat for mat in obj.data.materials if mat is not None}
    effect_groups._replace(obj,{key:effect_groups.material_vertices(obj.data,materials)},LABELS)
    from . import incremental
    settings=getattr(source,'project_damage_'+key,None)
    incremental.remember_output(context,source,target,key,obj,settings)
    return obj


def material_changed(settings, context):
    """Changing a standalone shader does not need native geometry evaluation."""
    from . import live, material_update
    if live._APPLYING or live._HISTORY_RESTORING:return
    source=settings.id_data
    for key in KEYS:
        if getattr(source,'project_damage_'+key)!=settings:continue
        obj=output(source,key)
        if obj is not None and settings.material is not None:
            with material_update.shader_edit(context):
                material_update.assign_slot(obj, 0, settings.material)
            material_update.expect_rna_notification(source)
        break


def after_build(context, source, result, reports=None):
    reports=reports if reports is not None else []
    for key in KEYS:
        visible=getattr(source.project_damage,key+'_enable') and not source.project_damage.all_off
        obj=output(source,key)
        from . import incremental
        settings=getattr(source,'project_damage_'+key)
        if visible and not incremental.reuse_output(context,source,result,key,obj,settings):
            obj=module(key).generate(context,source,result,getattr(source,'project_damage_'+key),reports)
            if obj is None:
                obj=output(source,key)
                visible=False
        if obj is not None:
            obj.hide_set(not visible)
            obj.hide_render=not visible

    from . import damage_generator_assets
    damage_generator_assets.after_build(context,source,result,reports)


# показываем настройки выбранного эффекта
def draw_settings(layout, context, key):
    from . import sidebar
    source=ownership.context_source(context)
    if source is None:return
    module(key)._PANEL.draw(SimpleNamespace(layout=sidebar._Layout(layout,key)),
                            SimpleNamespace(active_object=source))


# подключаем свойства и действия этого модуля к Blender
def register():
    registered=[]
    try:
        for key in KEYS:
            item=module(key);item.register();registered.append(item)
    except Exception:
        for item in reversed(registered):item.unregister()
        raise


# убираем подключения модуля при выключении аддона
def unregister():
    for key in reversed(KEYS):module(key).unregister()
