# сохраняем модификатор Smooth by Angle на деревянном результате
from pathlib import Path
import math
import bpy

_TAG='pd_structure_smooth_by_angle'


def angle_socket(group):
    if group is None:return None
    return next((s for s in group.interface.items_tree if s.item_type=='SOCKET' and s.in_out=='INPUT' and s.name=='Angle'),None)


def modifier(target):
    if target is None:return None
    return next((m for m in target.modifiers if m.type=='NODES' and m.node_group is not None and
                 (m.node_group.get(_TAG) or m.node_group.name.split('.')[0]=='Smooth by Angle') and angle_socket(m.node_group) is not None),None)


def set_input(mod,socket,value):
    inputs=getattr(getattr(mod,'properties',None),'inputs',None)
    if inputs is not None:getattr(inputs,socket.identifier).value=value
    else:
        mod[socket.identifier]=value
        if getattr(socket,'subtype','')=='ANGLE':mod.id_properties_ui(socket.identifier).update(subtype='ANGLE',min=0.,max=math.pi)


# сохраняем один Smooth by Angle вместо повторного добавления модификаторов
def ensure(source,target):
    if target is None or source.pd_structure.kind not in {'LATH','STUD','BRICK'}:return None
    current=modifier(target)
    if current is not None:return current
    group=next((g for g in bpy.data.node_groups if g.get(_TAG) and angle_socket(g) is not None),None)
    if group is None:
        root=Path(bpy.utils.system_resource('DATAFILES'))/'assets'
        path=next((p for p in (root/'nodes/geometry_nodes_essentials.blend',root/'geometry_nodes/smooth_by_angle.blend') if p.is_file()),None)
        if path is None:raise RuntimeError('Blender Smooth by Angle asset is missing from the installation')
        with bpy.data.libraries.load(str(path),link=False) as (available,loaded):
            if 'Smooth by Angle' not in available.node_groups:raise RuntimeError('Blender Smooth by Angle node group is unavailable')
            loaded.node_groups=['Smooth by Angle']
        group=loaded.node_groups[0];group[_TAG]=True
    current=target.modifiers.new('Smooth by Angle','NODES')
    current.node_group=group
    current.show_viewport=True;current.show_render=True
    set_input(current,angle_socket(group),source.pd_structure.smooth_angle)
    ignore=next((s for s in group.interface.items_tree if s.item_type=='SOCKET' and s.in_out=='INPUT' and s.name=='Ignore Sharpness'),None)
    if ignore is not None:set_input(current,ignore,True)
    return current


# собираем видимые настройки этого раздела
def draw(layout,target):
    mod=modifier(target)
    if mod is None:
        layout.label(text='Smooth by Angle is added on Update.',icon='MOD_SMOOTH')
        return
    row=layout.row(align=True)
    row.prop(mod,'show_viewport',text='Smooth by Angle',toggle=True)
    row.prop(mod,'show_render',text='',icon='RESTRICT_RENDER_OFF' if mod.show_render else 'RESTRICT_RENDER_ON')
    socket=angle_socket(mod.node_group)
    inputs=getattr(getattr(mod,'properties',None),'inputs',None)
    if inputs is not None:layout.prop(getattr(inputs,socket.identifier),'value',text='Angle')
    else:layout.prop(mod,'["'+socket.identifier+'"]',text='Angle')
