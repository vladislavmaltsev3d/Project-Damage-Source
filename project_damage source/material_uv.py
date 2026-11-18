# выбираем UV для материалов и сохраняем настройки текстур
# SPDX-License-Identifier: GPL-3.0-or-later
"""Object-local shader UV variants; canonical materials and shared groups stay untouched."""
from .blender_compat import uses_nodes
import hashlib
import re
import bpy
import numpy as np

UV_NAME='ProjectDamage_Zones'
TAG='project_damage_shader_uv'
SOURCE='project_damage_shader_source'
OWNER='project_damage_shader_owner'
PRESERVE_OUTPUT_UV='project_damage_preserve_output_uv'
VECTOR_TEXTURES={'TEX_IMAGE','TEX_NOISE','TEX_VORONOI','TEX_WAVE','TEX_MAGIC',
                 'TEX_BRICK','TEX_CHECKER','TEX_GRADIENT','TEX_MUSGRAVE'}


def base_material(material):
    seen=set()
    while material is not None and material.as_pointer() not in seen:
        seen.add(material.as_pointer())
        original=material.get(SOURCE)
        if not isinstance(original,bpy.types.Material):break
        material=original
    return material


def has_uv(obj):
    return obj is not None and obj.type=='MESH' and obj.data.uv_layers.get(UV_NAME) is not None


def used_elsewhere(material,obj):
    return any(other!=obj and any(slot.material==material for slot in other.material_slots)
               for other in bpy.data.objects)


def _needs_uv(tree,seen=None):
    seen=set() if seen is None else seen
    if tree is None or tree.as_pointer() in seen:return False
    seen.add(tree.as_pointer())
    for node in tree.nodes:
        if node.type in VECTOR_TEXTURES or node.type in {'UVMAP','TEX_COORD','NORMAL_MAP','TANGENT'}:return True
        if node.type=='GROUP' and _needs_uv(node.node_tree,seen):return True
    return False


def _configure_tree(tree,memo,uv_name):

    for node in list(tree.nodes):
        if node.type=='GROUP' and _needs_uv(node.node_tree):
            original=node.node_tree
            key=original.as_pointer()
            if key not in memo:
                clone=original.copy();clone.name=original.name+' | PD UV'
                memo[key]=clone
                _configure_tree(clone,memo,uv_name)
            node.node_tree=memo[key]
    uv=next((n for n in tree.nodes if n.type=='UVMAP' and n.get('project_damage_uv_node')),None)
    if uv is None:
        uv=tree.nodes.new('ShaderNodeUVMap')
        uv.name='PD_UV_' + uv_name;uv.label=uv_name;uv['project_damage_uv_node']=True
        uv.location=(min((n.location.x for n in tree.nodes if n!=uv),default=0)-230,0)
    uv.uv_map=uv_name
    for node in list(tree.nodes):
        if node.type in {'UVMAP','NORMAL_MAP','TANGENT'} and hasattr(node,'uv_map'):
            node.uv_map=uv_name
        if node.type=='TEX_COORD':
            socket=node.outputs.get('UV')
            if socket is not None:
                for link in list(socket.links):
                    target=link.to_socket;tree.links.remove(link)
                    tree.links.new(uv.outputs['UV'],target)
        if node.type in VECTOR_TEXTURES or node.type=='MAPPING':
            socket=node.inputs.get('Vector')
            if socket is not None and not socket.is_linked:tree.links.new(uv.outputs['UV'],socket)
        if node.type=='GROUP' and _needs_uv(node.node_tree):
            for name in ('Vector','UV','Coordinates','UV Map'):
                socket=node.inputs.get(name)
                if socket is not None and socket.type=='VECTOR' and not socket.is_linked:
                    tree.links.new(uv.outputs['UV'],socket)


def preserve_generated_uv(material):
    from .material_defaults import NAMES
    original=base_material(material)
    if original is None:return False
    name=re.sub(r'\.\d{3}$','',original.name.split(' | PD UV')[0])
    return name in {NAMES[key] for key in ('wood_planks','framing','rebar','brick','mortar')}


def preserve_output_uv(obj):
    from .uv_zones import preserves_output_uv
    return obj is not None and preserves_output_uv(obj)


def for_object(material,obj,uv_name=UV_NAME):
    if material is None:return None
    pinned = isinstance(material.get('project_damage_pin_owner'), bpy.types.Object)
    if not uv_name or obj is None or obj.type != 'MESH' or obj.data.uv_layers.get(uv_name) is None:return material if pinned else base_material(material)
    from . import fracture_uv
    if uv_name != fracture_uv.layer_name(obj.data):
        if preserve_generated_uv(material) or preserve_output_uv(obj):
            return material if pinned else base_material(material)
        from . import ownership
        source = obj.get('project_damage_baked_backup')
        if not isinstance(source, bpy.types.Object):
            source = ownership.source_of(obj)
        if source is not None and source != obj and source.type == 'MESH':
            source_materials = {slot.material for slot in source.material_slots}
            if material in source_materials:
                return material
            original = base_material(material)
            if original in source_materials:
                return original
    if material.get(TAG)!=uv_name and (not uses_nodes(material) or not _needs_uv(material.node_tree)):
        return material
    if material.get(TAG)==uv_name and material.get(OWNER)==obj and not used_elsewhere(material,obj):
        return material
    original=material if pinned else base_material(material)
    for existing in bpy.data.materials:
        if (existing.get(TAG)==uv_name and existing.get(OWNER)==obj
                and (existing.get(SOURCE)==original if pinned else base_material(existing)==original)
                and not used_elsewhere(existing,obj)):
            return existing


    result=material.copy();result.name=material.name+' | PD UV'
    result[TAG]=uv_name;result[SOURCE]=original;result[OWNER]=obj
    if uses_nodes(result) and result.node_tree is not None:_configure_tree(result.node_tree,{},uv_name)
    return result


def configure_zones(obj,mask,uv_name=UV_NAME):
    """Assign object-local variants to damaged faces only, not untouched skin."""
    if not uv_name or obj.data.uv_layers.get(uv_name) is None:return 0
    from . import fracture_uv
    if uv_name != fracture_uv.layer_name(obj.data) and preserve_output_uv(obj):return 0
    mesh=obj.data
    slots={face.material_index for face,yes in zip(mesh.polygons,mask) if yes}
    remap={}
    for slot in slots:
        if slot>=len(obj.material_slots) or obj.material_slots[slot].material is None:continue
        material=for_object(obj.material_slots[slot].material,obj,uv_name)
        index=next((i for i,m in enumerate(obj.material_slots) if m.material==material),None)
        if index is None:index=len(mesh.materials);mesh.materials.append(material)
        remap[slot]=index
    for face,yes in zip(mesh.polygons,mask):
        if yes and face.material_index in remap:face.material_index=remap[face.material_index]
    return len(remap)


def target_object(context,source):
    from . import ownership,build
    active=context.active_object
    candidates=[]
    if active is not None and ownership.source_of(active)==source:candidates.append(active)
    if source is not None and source.type=='MESH':candidates.extend([build.result_of(source),source])
    return next((obj for obj in candidates if has_uv(obj)),None)


def _owned(source):
    from . import ownership
    return [obj for obj in bpy.data.objects if obj.type=='MESH' and obj!=source and ownership.source_of(obj)==source]


def _signature(mesh):
    h=hashlib.sha256()
    for data,field,width,dtype in ((mesh.vertices,'co',3,np.float32),(mesh.loops,'vertex_index',1,np.int32),
                                   (mesh.polygons,'loop_start',1,np.int32),(mesh.polygons,'loop_total',1,np.int32)):
        values=np.empty(len(data)*width,dtype=dtype);data.foreach_get(field,values);h.update(values.tobytes())
    return h.digest()


def _loop_keys(mesh):



    coords=[tuple(round(float(c),6) for c in vertex.co) for vertex in mesh.vertices]
    keys=[None]*len(mesh.loops)
    for face in mesh.polygons:
        shape=tuple(sorted(coords[index] for index in face.vertices))
        for index in face.loop_indices:keys[index]=(shape,coords[mesh.loops[index].vertex_index])
    return keys


def _reordered_uv(mesh,snapshot):
    if len(mesh.loops)*2!=len(snapshot[1]):return None
    old={}
    for key,uv in zip(snapshot[2],snapshot[1].reshape(-1,2)):
        if key in old and not np.array_equal(old[key],uv):return None
        old[key]=uv
    keys=_loop_keys(mesh)
    if set(keys)!=set(old):return None
    return np.asarray([old[key] for key in keys],dtype=np.float32).reshape(-1)


def capture(source):
    snapshots={}
    for obj in _owned(source):
        if preserve_output_uv(obj):continue
        layer=obj.data.uv_layers.get(UV_NAME)
        if layer is None:continue
        values=np.empty(len(obj.data.loops)*2,np.float32);layer.data.foreach_get('uv',values)
        snapshots[obj.name]=(_signature(obj.data),values,_loop_keys(obj.data))
    return snapshots


def after_build(source,snapshots):
    from . import uv_zones
    for obj in _owned(source):
        if preserve_output_uv(obj):continue
        snapshot=snapshots.get(obj.name)
        values=None
        if snapshot is not None:
            values=snapshot[1] if _signature(obj.data)==snapshot[0] else _reordered_uv(obj.data,snapshot)
        if values is not None:
            active=obj.data.uv_layers.active
            layer=obj.data.uv_layers.get(UV_NAME) or obj.data.uv_layers.new(name=UV_NAME)
            layer.data.foreach_set('uv',values)
            if active is not None:obj.data.uv_layers.active=active
        if has_uv(obj):configure_zones(obj,uv_zones.zone_mask(obj))
        else:


            for slot in obj.material_slots:
                if slot.material is not None and slot.material.get(TAG)==UV_NAME:
                    slot.material=base_material(slot.material)
