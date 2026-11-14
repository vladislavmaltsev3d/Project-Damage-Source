# размещаем отдельные обломки и связываем их с источником
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned, per-piece outputs for removed material; never write the source."""
from . import output_collections
from .blender_compat import enable_nodes
import bpy
import numpy as np
from mathutils import Matrix
from . import ownership,live

STAMP='project_damage_debris_of'
COLLECTION_STAMP='project_damage_debris_collection'

                                                                          
                                                 
KINDS={'debris':(STAMP,COLLECTION_STAMP,'_DamageDebris','debris_material','ProjectDamage_Debris'),
       'rbf':('project_damage_rbf_of','project_damage_rbf_collection','_RigidBodyPieces','rbf_inner_material','ProjectDamage_RigidBodyInner'),
       'cellf':('project_damage_cellf_of','project_damage_cellf_collection','_CellFracture','cellf_inner_material','ProjectDamage_CellFractureInner')}

def objects_of(source,kind='debris'):
    uid=source.get('project_damage_uid')
    stamp=KINDS[kind][0]
    return [o for o in bpy.data.objects if uid and o.get(stamp)==uid]

def _remove(obj,context,source):
    for child in [c for c in getattr(obj, 'children', ()) if c.get('project_damage_rough_detail')]:
        child_mesh = child.data
        bpy.data.objects.remove(child, do_unlink=True)
        if child_mesh is not None and child_mesh.users == 0:
            bpy.data.meshes.remove(child_mesh)
    if context.view_layer.objects.active==obj:
        from . import build
        target=build.result_of(source) or source
        target.hide_set(False);target.select_set(True)
        context.view_layer.objects.active=target
    mesh=obj.data if obj.type=='MESH' else None
    bpy.data.objects.remove(obj,do_unlink=True)
    if mesh is not None and mesh.users==0:bpy.data.meshes.remove(mesh)

def place_debris(context,source,out,factor,source_snapshot=None,uv_layer_name=None,kind='debris',centers=None):
    from . import build
    old=objects_of(source,kind)
    stamp,coll_stamp,coll_suffix,mat_prop,default_name=KINDS[kind]
    piece_key='project_damage_'+kind+'_piece'
    faces=np.asarray(out.get('debris_polys',()),dtype=np.int32).reshape(-1,4)
    if not len(faces):
        for obj in old:_remove(obj,context,source)
        return []
    points=np.asarray(out['debris_points'],dtype=np.float64)/factor
    ids=np.asarray(out['debris_piece'],dtype=np.int32)
    if len(ids)!=len(faces) or not np.isfinite(points).all() or faces.min()<0 or faces.max()>=len(points):
        raise ValueError('Invalid native debris output')
    base=np.asarray(out.get('debris_base_poly',np.full(len(faces),-1)),dtype=np.int32)
    uvs=np.asarray(out.get('debris_uvs',()),dtype=np.float64)
    if len(base)!=len(faces) or uvs.size!=len(faces)*8:
        raise ValueError('Missing or mismatched native debris material/UV data')
    uvs=uvs.reshape(len(faces),4,2)
    if not np.isfinite(uvs[:,:3]).all():raise ValueError('Non-finite debris UVs')
    normals=np.asarray(out.get('debris_normals',()),dtype=np.float64)
    if normals.size!=len(faces)*12 or not np.isfinite(normals).all():
        raise ValueError('Missing or invalid native debris normals; restart Blender after updating')
    normals=normals.reshape(len(faces),4,3)
    if source_snapshot is None:
        source_materials=tuple(slot.material for slot in source.material_slots)
        source_indices=np.empty(len(source.data.polygons),dtype=np.int32)
        source.data.polygons.foreach_get('material_index',source_indices)
    else:
        # Evaluated meshes can expose evaluated Material IDs. Persistent
        # output objects must reference the authored datablocks instead.
        source_materials=tuple(getattr(m,'original',m) if m is not None else None
                               for m in source_snapshot.materials)
        source_indices=source_snapshot.material_index
    uid=source['project_damage_uid']
    coll=next((c for c in bpy.data.collections if c.get(coll_stamp)==uid),None)
    if coll is None:
        coll=bpy.data.collections.new(source.name+coll_suffix);coll[coll_stamp]=uid
    output_collections.link_collection(context,source,coll)
    mat=getattr(source.project_damage,mat_prop)
    if mat is None:
        mat=bpy.data.materials.get(default_name)
        if mat is None:
            mat=bpy.data.materials.new(default_name);enable_nodes(mat)
            mat.diffuse_color=(.3,.27,.23,1)
            shader=mat.node_tree.nodes.get('Principled BSDF')
            shader.inputs['Base Color'].default_value=mat.diffuse_color
            shader.inputs['Roughness'].default_value=.85
    by_id={o.get(piece_key):o for o in old};outputs=[]
    for piece in np.unique(ids):
        f=faces[ids==piece];used,remap=np.unique(f,return_inverse=True)
        p=points[used];center=(p.min(axis=0)+p.max(axis=0))*.5 if centers is None else centers[int(piece)]
        name=source.name+{'debris':'_Debris_%03d','rbf':'_RigidBody_%03d'}.get(kind,'_CellPiece_%03d')%(int(piece)+1)
        mesh=build._result_mesh(name,p-center,remap.reshape(-1,4),build.ShadingProvenance.generated_object())
        for source_material in source_materials:mesh.materials.append(source_material)
        inner_slot=len(source_materials);mesh.materials.append(mat)
        selected=ids==piece;bp=base[selected]
        material_index=np.full(len(f),inner_slot,dtype=np.int32)
        inherited=(bp>=0)&(bp<len(source_indices))
        target=source_indices[bp[inherited]]
        inherited_rows=np.flatnonzero(inherited)
        valid_slots=(target>=0)&(target<len(source_materials))
        material_index[inherited_rows[valid_slots]]=target[valid_slots]
        mesh.polygons.foreach_set('material_index',material_index)
        attr_base=mesh.attributes.new('project_damage_debris_base_poly','INT','FACE')
        attr_base.data.foreach_set('value',bp)
        uv=mesh.uv_layers.new(name=uv_layer_name or 'ProjectDamage_DebrisUV')
        local_uv=uvs[selected]
        totals=np.empty(len(f),dtype=np.int32);mesh.polygons.foreach_get('loop_total',totals)
        flat=local_uv[np.arange(4)[None,:]<totals[:,None]]
        uv.data.foreach_set('uv',flat.ravel())
        mesh.uv_layers.active=uv;uv.active_render=True
                                                                        
                                                                              
        mesh.polygons.foreach_set('use_smooth',np.ones(len(f),dtype=bool))
        loop_normals=normals[selected][np.arange(4)[None,:]<totals[:,None]]
        mesh.normals_split_custom_set(loop_normals.tolist())
        mesh.update()
        attr=mesh.attributes.new('project_damage_debris_piece','INT','FACE')
        attr.data.foreach_set('value',np.full(len(f),piece,dtype=np.int32))
        obj=by_id.get(int(piece))
        if obj is None:obj=bpy.data.objects.new(name,mesh);coll.objects.link(obj)
        else:
            prior=obj.data;obj.data=mesh
            if prior.users==0:bpy.data.meshes.remove(prior)
        obj[stamp]=uid;obj[piece_key]=int(piece)
        obj['project_damage_debris_source_uvs']=bool(out.get('debris_source_uvs',False))
        ownership.bind(source,obj)
        obj.parent=source;obj.matrix_parent_inverse=Matrix.Identity(4)
        obj.matrix_world=source.matrix_world@Matrix.Translation(center.tolist())
        obj['project_damage_fracture_rest_matrix']=[float(v) for row in (source.matrix_world.inverted()@obj.matrix_world) for v in row]
        outputs.append(obj)
    try:
        from . import inner_rough
        inner_rough.after_place(context,source,outputs,kind)
    except Exception as exc:
        print('Project Damage: Break Faces skipped: %s'%exc)
    keep={o.as_pointer() for o in outputs}
    for obj in old:
        if obj.as_pointer() not in keep:_remove(obj,context,source)
    return outputs

def _collections(root):
    yield root
    for child in root.children:yield from _collections(child)

# действие «Generate / Refresh Debris»
class PROJECT_DAMAGE_OT_generate_debris(bpy.types.Operator):
    bl_idname='project_damage.generate_debris'
    bl_label='Generate / Refresh Debris'
    bl_description='Rebuild enabled damage and create owned objects from the removed material; rebuilding replaces generated chunks'
    bl_options={'REGISTER','UNDO'}
    @classmethod
    def poll(cls,context):
        from . import build
        source=ownership.context_source(context)
        return (not live._HISTORY_RESTORING and context.mode=='OBJECT' and source is not None
                and source.type=='MESH' and build.result_of(source) is not None)
    def execute(self,context):
        from . import build
        source=ownership.context_source(context)
        previous=live._APPLYING;live._APPLYING=True
        try:
            source.project_damage.debris_enable=True;source.project_damage.all_off=False
            result,reports=build.rebuild_object(context,source)
        finally:live._APPLYING=previous
        for kind,msg in reports:self.report(kind,msg)
        if result is None:return {'CANCELLED'}
        self.report({'INFO'},'Damage Debris: %d chunks'%len(objects_of(source)))
        return {'FINISHED'}

# подключаем свойства и действия этого модуля к Blender
def register():bpy.utils.register_class(PROJECT_DAMAGE_OT_generate_debris)
# убираем подключения модуля при выключении аддона
def unregister():bpy.utils.unregister_class(PROJECT_DAMAGE_OT_generate_debris)
