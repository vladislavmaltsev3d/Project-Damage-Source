# отвязываем меш от родителя, сохраняя его положение и форму
import bpy
import numpy as np
from mathutils import Matrix

TRANSFORMS=('location','rotation_euler','rotation_quaternion','rotation_axis_angle','scale',
            'delta_location','delta_rotation_euler','delta_rotation_quaternion','delta_scale')


def capture_transform(obj):
    return {name:tuple(getattr(obj,name)) for name in TRANSFORMS}


def restore_transform(obj,values):
    for name,value in values.items():setattr(obj,name,value)


def bake_mesh(mesh,matrix):
    normals=None
    if mesh.has_custom_normals:
        normals=np.empty((len(mesh.loops),3),dtype=np.float32)
        mesh.corner_normals.foreach_get('vector',normals.ravel())
        normal_matrix=np.asarray(matrix.to_3x3().inverted_safe().transposed(),dtype=np.float64)
        normals=normals@normal_matrix.T
        lengths=np.linalg.norm(normals,axis=1);normals/=np.maximum(lengths[:,None],1e-20)
    vertices=[tuple(poly.vertices) for poly in mesh.polygons] if normals is not None else None
    mesh.transform(matrix,shape_keys=True)
    if matrix.to_3x3().determinant()<0:
        if normals is not None:
            by_face=[dict(zip(face,normals[poly.loop_start:poly.loop_start+poly.loop_total]))
                     for poly,face in zip(mesh.polygons,vertices)]
        mesh.flip_normals()
        if normals is not None:
            normals=np.array([by_face[i][v] for i,poly in enumerate(mesh.polygons) for v in poly.vertices])
    if normals is not None:mesh.normals_split_custom_set(normals.tolist())
    mesh.update()


class Preparation:
    def __init__(self,context,obj):
        self.context=context;self.obj=obj;self.data=obj.data;self.mesh=None
        self.parent=obj.parent;self.parent_type=obj.parent_type;self.parent_bone=obj.parent_bone
        self.parent_inverse=obj.matrix_parent_inverse.copy();self.transform=capture_transform(obj)
        self.children=[(child,child.matrix_parent_inverse.copy(),capture_transform(child),child.evaluated_get(context.evaluated_depsgraph_get()).matrix_world.copy())
                       for child in obj.children]

    def apply(self):
        obj=self.obj;context=self.context;context.view_layer.update()
        world=obj.evaluated_get(context.evaluated_depsgraph_get()).matrix_world.copy()
        from .material_ids import normalize_mesh
        self.mesh=normalize_mesh(obj.data.copy());obj.data=self.mesh
        obj.parent=None;obj.matrix_parent_inverse=Matrix.Identity(4)
        obj.delta_rotation_euler=(0,0,0);obj.delta_rotation_quaternion=(1,0,0,0);obj.delta_scale=(1,1,1)
        obj.rotation_euler=(0,0,0);obj.rotation_quaternion=(1,0,0,0);obj.rotation_axis_angle=(0,0,0,1);obj.scale=(1,1,1)
        obj.location=world.translation-obj.delta_location
        context.view_layer.update()
        current=obj.evaluated_get(context.evaluated_depsgraph_get()).matrix_world.copy()
        transform=current.inverted_safe()@world
        bake_mesh(self.mesh,transform)
        context.view_layer.update()
        for child,inverse,transform,desired in self.children:
            current=child.evaluated_get(context.evaluated_depsgraph_get()).matrix_world.copy()
            basis=child.matrix_basis.copy()
            if abs(current.determinant())>1e-12 and abs(basis.determinant())>1e-12:
                child.matrix_parent_inverse=inverse@basis@current.inverted()@desired@basis.inverted()
            else:child.matrix_world=desired
        context.view_layer.update()

    def rollback(self):
        obj=self.obj;obj.data=self.data;obj.parent=self.parent
        obj.parent_type=self.parent_type;obj.parent_bone=self.parent_bone
        obj.matrix_parent_inverse=self.parent_inverse;restore_transform(obj,self.transform)
        for child,inverse,transform,world in self.children:
            child.matrix_parent_inverse=inverse;restore_transform(child,transform)
        self.context.view_layer.update();self.cleanup()

    def cleanup(self):
        if self.mesh is not None and self.mesh.users==0:
            bpy.data.meshes.remove(self.mesh);self.mesh=None


# сохраняем положение меша при отвязке от родителя
def prepare(context,obj):
    context.view_layer.update()
    if obj.parent is None and all(abs(obj.matrix_world[i][j] - (1.0 if i==j else 0.0)) < 1e-8 for i in range(3) for j in range(3)):return None
    if not obj.is_editable:raise ValueError('Make this mesh object local before adding Project Damage')
    state=Preparation(context,obj)
    try:state.apply()
    except Exception:
        state.rollback();raise
    return state


def normalize_legacy(context,obj):
    if obj.mode!='OBJECT' or obj.parent or obj.animation_data or obj.constraints:return False
    if any(m.show_viewport for m in obj.modifiers):return False
    scale=obj.matrix_world.to_scale()
    if max(abs(s-1.) for s in scale)<1e-6:return False
    from . import mesh_states
    old_world=obj.matrix_world.copy();initial=obj.get(mesh_states.INITIAL)
    state=prepare(context,obj)
    if state is None:return False
    if isinstance(initial,bpy.types.Mesh):
        preserved=initial.copy();bake_mesh(preserved,obj.matrix_world.inverted_safe()@old_world)
        obj[mesh_states.INITIAL]=preserved
    state.cleanup()
    return True
