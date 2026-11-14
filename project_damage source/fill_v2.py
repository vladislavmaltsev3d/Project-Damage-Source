# считаем заполнение отдельно около отмеченных зон повреждения
# SPDX-License-Identifier: GPL-3.0-or-later
"""Experimental, on-demand local Fill. No generator-time geometry or cache.

Damage face groups define padded local boxes. Each Boolean sees only the
faces intersecting its box. Artificial clipping faces are marked as interior;
components reaching those faces are rejected, not returned as fake fills.
"""
import time
import bpy
import bmesh
import numpy as np
from mathutils import Vector
from . import damage_physics as dp, effect_groups as eg
from .fill_clip import cap_boundaries, close_box

SELECTED = '_pd_fill_v2_selected'


class MeshData:
    """One bulk snapshot; extracting an island copies only its own data."""
    def __init__(self, mesh, transform=None):
        self.mesh=mesh
        self._solid=None;self._bvh=None;self._closed=None
        self.co=np.empty((len(mesh.vertices),3),np.float32);mesh.vertices.foreach_get('co',self.co.ravel())
        if transform is not None:
            mat=np.asarray(transform,dtype=np.float64)
            self.co=(self.co@mat[:3,:3].T+mat[:3,3]).astype(np.float32)
        self.total=np.empty(len(mesh.polygons),np.int32);mesh.polygons.foreach_get('loop_total',self.total)
        self.start=np.empty_like(self.total);mesh.polygons.foreach_get('loop_start',self.start)
        self.lv=np.empty(len(mesh.loops),np.int32);mesh.loops.foreach_get('vertex_index',self.lv)
        self.smooth=np.empty(len(mesh.polygons),bool);mesh.polygons.foreach_get('use_smooth',self.smooth)
        self.material=np.empty(len(mesh.polygons),np.int32);mesh.polygons.foreach_get('material_index',self.material)
        self.materials=list(mesh.materials)
        self.attrs=[];self.uv={}
        for layer in mesh.uv_layers:
            a=np.empty((len(mesh.loops),2),np.float32);layer.uv.foreach_get('vector',a.ravel());self.uv[layer.name]=a
        self.uv_active=mesh.uv_layers.active.name if mesh.uv_layers.active else None
        self.uv_render=next((layer.name for layer in mesh.uv_layers if layer.active_render),None)
        kinds={'INT':('value',1,np.int32),'BOOLEAN':('value',1,bool),'FLOAT':('value',1,np.float32),
               'FLOAT_VECTOR':('vector',3,np.float32),'FLOAT2':('vector',2,np.float32),
               'FLOAT_COLOR':('color',4,np.float32),'BYTE_COLOR':('color',4,np.float32)}
        for attr in mesh.attributes:
            if attr.domain not in {'FACE','CORNER','POINT'} or attr.name in self.uv or attr.name.startswith('.') or attr.name in {'position','sharp_face','sharp_edge'}:continue
            spec=kinds.get(attr.data_type)
            if spec is None:continue
            field,width,dtype=spec;a=np.empty((len(attr.data),width),dtype);attr.data.foreach_get(field,a.ravel())
            self.attrs.append((attr.name,attr.domain,attr.data_type,field,a))
        xyz=self.co[self.lv]
        self.lo=np.minimum.reduceat(xyz,self.start,axis=0) if len(self.start) else np.empty((0,3))
        self.hi=np.maximum.reduceat(xyz,self.start,axis=0) if len(self.start) else np.empty((0,3))

    def query(self,lo,hi):
        return np.flatnonzero(np.all(self.hi>=lo,axis=1)&np.all(self.lo<=hi,axis=1))

    def closed(self):
        if self._closed is None:
            nxt=np.arange(len(self.lv))+1
            nxt[self.start+self.total-1]=self.start
            pairs=np.sort(np.column_stack((self.lv,self.lv[nxt])),axis=1)
            _,counts=np.unique(pairs,axis=0,return_counts=True)
            self._closed=bool(len(counts)) and bool(np.all(counts==2))
        return self._closed

    def solid(self):
        if self.closed():return self
        if self._solid is not None:return self._solid
        mesh=self.extract(np.arange(len(self.total)), '_pd_v2_closed_input')
        bm=bmesh.new()
        try:
            bm.from_mesh(mesh)
            dp._split_pinches(bm)
            caps=cap_boundaries(bm)
            if caps is None or not all(e.is_manifold for e in bm.edges):
                raise ValueError('input boundary could not be safely closed')
            layer=bm.faces.layers.int.get('_pd_fill_outer')
            if layer:
                for face in caps:face[layer]=0
            bmesh.ops.recalc_face_normals(bm,faces=bm.faces[:])
            bm.to_mesh(mesh)
            self._solid=MeshData(mesh)
            self._solid._closed=True
        finally:
            bm.free();bpy.data.meshes.remove(mesh)
        return self._solid

    def contains(self,point):
        from mathutils.bvhtree import BVHTree
        if self._bvh is None:
            polygons=[self.lv[start:start+count].tolist() for start,count in zip(self.start,self.total)]
            self._bvh=BVHTree.FromPolygons(self.co.tolist(),polygons,all_triangles=False)
            self._ray_length=max(float(np.linalg.norm(np.ptp(self.co,axis=0)))*3,1.)
        step=max(self._ray_length*2e-7,1e-7)
        votes=[]
        for direction in ((1,.371,.127),(.213,1,.517),(.719,.271,1)):
            direction=Vector(direction).normalized();origin=Vector(point);hits=0
            for _ in range(512):
                location,_,_,distance=self._bvh.ray_cast(origin,direction,self._ray_length)
                if location is None:break
                hits+=1;origin=location+direction*step
            else:raise ValueError('local solid classification did not converge')
            votes.append(hits%2)
            if len(votes)==2 and votes[0]==votes[1]:return bool(votes[0])
        return sum(votes)>=2

    def extract(self,faces,name):
        faces=np.asarray(faces,dtype=np.int64)
        count=self.total[faces];starts=np.r_[0,np.cumsum(count)[:-1]] if len(count) else np.empty(0,np.int32)
        loops=np.repeat(self.start[faces],count)+np.arange(int(count.sum()))-np.repeat(starts,count)
        vertices,indices=np.unique(self.lv[loops],return_inverse=True)
        mesh=bpy.data.meshes.new(name)
        mesh.vertices.add(len(vertices));mesh.vertices.foreach_set('co',self.co[vertices].ravel())
        mesh.loops.add(len(loops));mesh.loops.foreach_set('vertex_index',indices.astype(np.int32))
        mesh.polygons.add(len(faces));mesh.polygons.foreach_set('loop_start',starts.astype(np.int32));mesh.polygons.foreach_set('loop_total',count)
        mesh.polygons.foreach_set('use_smooth',self.smooth[faces]);mesh.polygons.foreach_set('material_index',self.material[faces])
        mesh.update(calc_edges=True)
        for mat in self.materials:mesh.materials.append(mat)
        for name,a in self.uv.items():mesh.uv_layers.new(name=name).uv.foreach_set('vector',a[loops].ravel())
        if self.uv_active is not None and self.uv_active in mesh.uv_layers:mesh.uv_layers.active=mesh.uv_layers[self.uv_active]
        if self.uv_render is not None and self.uv_render in mesh.uv_layers:mesh.uv_layers[self.uv_render].active_render=True
        for name,domain,kind,field,a in self.attrs:
            attr=mesh.attributes.get(name) or mesh.attributes.new(name,kind,domain)
            rows={'FACE':faces,'CORNER':loops,'POINT':vertices}[domain]
            attr.data.foreach_set(field,a[rows].ravel())
        return mesh


def region_mask(source,result,module):
    """Prefer exact face groups; vertex groups are a legacy fallback."""
    from . import physics_workflow,sidebar
    selected=physics_workflow.selected_key(source)
    key=selected if sidebar.PHYSICS_MODULES.get(selected)==module else {'bead':'buildup'}.get(module,module)
    names=dict(result.data.get(eg.OWNED_FACES,{}))
    mask=eg._face_values(result.data,names.get(key,''),bool)
    if mask is not None and mask.any():return mask
    values=eg._face_values(result.data,eg.ATTRIBUTE,np.int32)
    ids=dp.EFFECT_IDS.get(module)
    if module=='surface':ids=(3,31)
    if ids is None:
        stamp=eg.stamp_of(key)
        ids=(stamp,) if stamp is not None else ()
    if values is not None and ids:
        mask=np.isin(values,ids)
        if mask.any():return mask
    groups=dict(result.get(eg.OWNED,{}));group=result.vertex_groups.get(groups.get(key,''))
    if group is None:return np.zeros(len(result.data.polygons),bool)
    vertex=np.zeros(len(result.data.vertices),bool)
    for v in result.data.vertices:vertex[v.index]=any(g.group==group.index and g.weight>0 for g in v.groups)
    lv=np.empty(len(result.data.loops),np.int32);result.data.loops.foreach_get('vertex_index',lv)
    starts=np.empty(len(result.data.polygons),np.int32);result.data.polygons.foreach_get('loop_start',starts)
    return np.logical_and.reduceat(vertex[lv],starts)


def boxes(result,data,mask,padding):
    bm=bmesh.new();bm.from_mesh(result.data);bm.faces.ensure_lookup_table()
    seen=np.zeros(len(mask),bool);regions=[]
    try:
        for i in np.flatnonzero(mask):
            if seen[i]:continue
            seen[i]=True;stack=[bm.faces[int(i)]];ids=[]
            while stack:
                face=stack.pop();ids.append(face.index)
                for edge in face.edges:
                    for other in edge.link_faces:
                        if mask[other.index] and not seen[other.index]:seen[other.index]=True;stack.append(other)
            lo=data.lo[ids].min(0);hi=data.hi[ids].max(0)
            margin=max(padding,float(np.linalg.norm(hi-lo))*.025)
            regions.append((lo-margin,hi+margin))
    finally:bm.free()
                                                                        
    merged=[]
    for lo,hi in regions:
        changed=True
        while changed:
            changed=False;remaining=[]
            for a,b in merged:
                if np.all(hi>=a) and np.all(lo<=b):lo=np.minimum(lo,a);hi=np.maximum(hi,b);changed=True
                else:remaining.append((a,b))
            merged=remaining
        merged.append((lo,hi))
    return merged


def through_regions(source,module,regions,base,padding):
    """A Boolean hole has disjoint rim patches. Include the cutter volume
    between them, not two narrow rim slices rejected at their ROI caps.
    Keep separate damage locations local; never use one global source box."""
    if module=='boolean':
        inverse=source.matrix_world.inverted_safe()
        for item in source.project_damage.breakers:
            obj=item.obj
            if obj is None or not getattr(item,'enable',True):continue
            points=np.array([inverse@obj.matrix_world@Vector(co) for co in obj.bound_box])
            low=points.min(0);high=points.max(0)
            if not np.any(high>low):continue
            hits=[i for i,(lo,hi) in enumerate(regions) if np.all(hi>=low-padding) and np.all(lo<=high+padding)]
            if not hits:continue
            low=np.maximum(low,base.co.min(0))-padding
            high=np.minimum(high,base.co.max(0))+padding
            for i in reversed(hits):
                lo,hi=regions.pop(i);low=np.minimum(low,lo);high=np.maximum(high,hi)
            regions.append((low,high))
    merged=[]
    for low,high in regions:
        changed=True
        while changed:
            changed=False;remaining=[]
            for lo,hi in merged:
                if np.all(high>=lo) and np.all(low<=hi):
                    low=np.minimum(low,lo);high=np.maximum(high,hi);changed=True
                else:remaining.append((lo,hi))
            merged=remaining
        merged.append((low,high))
    return merged


def snap_unchanged(reference,changed,tolerance):
                                                                        
                                                                        
    from mathutils.kdtree import KDTree
    tree=KDTree(len(reference.co))
    for i,co in enumerate(reference.co):tree.insert(co,i)
    tree.balance()
    for i,co in enumerate(changed.co):
        nearest,index,distance=tree.find(co)
        if distance<=tolerance:changed.co[i]=reference.co[index]
    xyz=changed.co[changed.lv]
    changed.lo=np.minimum.reduceat(xyz,changed.start,axis=0)
    changed.hi=np.maximum.reduceat(xyz,changed.start,axis=0)


def triangulate_consistent(bm):
    fixed=[];alternate=[];ngons=[]
    for face in bm.faces:
        if len(face.verts)==4:
            first=min(range(4),key=lambda i:tuple(face.verts[i].co))
            (alternate if first%2 else fixed).append(face)
        elif len(face.verts)>4:ngons.append(face)
    if fixed:bmesh.ops.triangulate(bm,faces=fixed,quad_method='FIXED',ngon_method='EAR_CLIP')
    if alternate:bmesh.ops.triangulate(bm,faces=alternate,quad_method='ALTERNATE',ngon_method='EAR_CLIP')
    if ngons:bmesh.ops.triangulate(bm,faces=ngons,quad_method='FIXED',ngon_method='EAR_CLIP')


# ограничиваем расчёт небольшой областью вокруг повреждения
def crop(data,lo,hi,name,outer):
    data=data.solid()
    if not len(data.co) or np.any(data.co.max(0)<lo) or np.any(data.co.min(0)>hi):return None
    ids=data.query(lo,hi)
    mesh=data.extract(ids,name);bm=bmesh.new();success=False
    try:
        bm.from_mesh(mesh)
        triangulate_consistent(bm)
        tolerance=max(float(np.linalg.norm(hi-lo))*1e-8,1e-9)
        for axis in range(3):
            for value,sign in ((lo[axis],1.),(hi[axis],-1.)):
                if not bm.faces:break
                point=Vector((0,0,0));point[axis]=float(value)
                normal=Vector((0,0,0));normal[axis]=sign
                bmesh.ops.bisect_plane(bm,geom=bm.verts[:]+bm.edges[:]+bm.faces[:],dist=tolerance,
                    plane_co=point,plane_no=normal,clear_inner=True,clear_outer=False)
        weld=max(float(np.max(np.abs(data.co)))*2e-7,tolerance*10)
        for vertex in bm.verts:
            for axis in range(3):
                for value in (lo[axis],hi[axis]):
                    if abs(vertex.co[axis]-value)<=weld:vertex.co[axis]=value
        bmesh.ops.remove_doubles(bm,verts=bm.verts[:],dist=weld)
        created=close_box(bm,data,lo,hi)
        if created is None:return None
        loose=[e for e in bm.edges if not e.link_faces]
        if loose:bmesh.ops.delete(bm,geom=loose,context='EDGES')
        loose=[v for v in bm.verts if not v.link_faces]
        if loose:bmesh.ops.delete(bm,geom=loose,context='VERTS')
        if not bm.faces:return None
        face_outer=bm.faces.layers.int.get('_pd_fill_outer') or bm.faces.layers.int.new('_pd_fill_outer')
        corner_normal=bm.loops.layers.float_vector.get('_pd_fill_outer_normal')
        created=set(created)
        for face in bm.faces:
            if not outer or face in created:
                face[face_outer]=0
                if corner_normal:
                    for loop in face.loops:loop[corner_normal]=(0.,0.,0.)
        bmesh.ops.recalc_face_normals(bm,faces=bm.faces[:])
        if not all(e.is_manifold and e.is_contiguous for e in bm.edges):return None
        if bm.calc_volume(signed=True)<0:bmesh.ops.reverse_faces(bm,faces=bm.faces[:])
        bm.to_mesh(mesh);mesh.update()
        if len(mesh.polygons)>180000:raise ValueError('local region exceeds 180,000 faces')
        success=True
        return mesh
    finally:
        bm.free()
        if not success and not mesh.users:bpy.data.meshes.remove(mesh)


def valid_piece(mesh,min_volume,min_thickness):
    if len(mesh.vertices)<4 or len(mesh.polygons)<4:return False
    bm=bmesh.new()
    try:
        bm.from_mesh(mesh)
        if not bm.faces or not all(e.is_manifold and e.is_contiguous for e in bm.edges):return False
        vol=abs(bm.calc_volume(signed=True));area=sum(f.calc_area() for f in bm.faces)
        return vol>=min_volume and 2*vol/max(area,1e-30)>=min_thickness
    finally:bm.free()


# собираем локальные восстановленные объёмы и оставляем удачные результаты
@dp._operation
def fill(context,source,module):
    """Calculate only when Fill V2 is pressed; the old Fill path is untouched."""
    started=time.perf_counter();p=source.project_damage;s=dp.settings(source)
    reason=dp.fill_block_reason(source)
    if reason:return [],[({'WARNING'},reason)]
    if module in dp.ADDITIVE:
        return [],[({'WARNING'},'Fill V2 is for cut damage; use the existing Detach / Fill Segments tool')]
    if p.all_off or not dp.module_enabled(p,module):return [],[({'ERROR'},'Fill V2: enable the selected effect first')]
    result=dp.build.result_of(source)
    if result is None:return [],[({'ERROR'},'Fill V2: press Update first')]
    from . import fill_surface
    recovered = fill_surface.try_fill(context, source, module, v2=True)
    if recovered is not None:
        if not recovered[0]:
            from . import fill_exact
            recovered = fill_exact.recover(context, source, module, 'Fill V2') or recovered
        return recovered
    from .boolean_break import fragments_of
    bodies=[result,*fragments_of(source)]
    masks=[region_mask(source,obj,module) for obj in bodies]
    if not any(mask.any() for mask in masks):
        from . import fill_exact
        recovered = fill_exact.recover(context, source, module, 'Fill V2')
        if recovered is not None:
            return recovered
        return [], [({'WARNING'}, 'Fill V2: no damage face group found for this effect')]
    from .params import unit_factor
    factor=unit_factor(context)
    g=dp._gscale(p);minimum=max(1e-9,p.debris_min_volume*g**3)/factor**3
    thickness=0. if s.fill_cracks else p.debris_min_thickness*g/factor
    if module=='hit':thickness=min(thickness,.3*max(0.,p.hit_layer_thickness)*g/factor)
    if module=='bullet':minimum*=.2;thickness*=.5
    base=dp._fill_base(context,source,triangulate=False)
    if not len(base.materials):base.materials.append(None)
    base.materials.append(dp._inner_material(source))
    if base.attributes.get(eg.ATTRIBUTE) is None:base.attributes.new(eg.ATTRIBUTE,'INT','FACE')
    previous=base.attributes.get(SELECTED)
    if previous is not None:base.attributes.remove(previous)
    base.attributes.new(SELECTED,'BOOLEAN','FACE')
    pending=[];created=[];errors=[];notices=[];dropped=0;body=None
    try:
        a=MeshData(base);wall=[];regions=[]
        padding=max(.02*g/factor,1e-6)
        for obj,mask in zip(bodies,masks):
            data=MeshData(obj.data,source.matrix_world.inverted()@obj.matrix_world)
            data.attrs=[row for row in data.attrs if row[0]!=SELECTED]
            data.attrs.append((SELECTED,'FACE','BOOLEAN','value',mask[:,None]))
            snap_unchanged(a,data,max(1e-7,float(np.linalg.norm(np.ptp(a.co,axis=0)))*1e-7))
                                                                        
                                                                               
            epsilon=max(1e-7,float(np.linalg.norm(np.ptp(a.co,axis=0)))*1e-6)
            normal=np.empty_like(data.co);obj.data.vertices.foreach_get('normal',normal.ravel())
            matrix=np.asarray(source.matrix_world.inverted()@obj.matrix_world)[:3,:3]
            normal=normal@np.linalg.inv(matrix)
            normal/=np.maximum(np.linalg.norm(normal,axis=1)[:,None],1e-12)
            data.co+=normal*epsilon
            xyz=data.co[data.lv]
            data.lo=np.minimum.reduceat(xyz,data.start,axis=0)
            data.hi=np.maximum.reduceat(xyz,data.start,axis=0)
            wall.append((obj,data))
            regions.extend(boxes(obj,data,mask,padding))
        regions=through_regions(source,module,regions,a,padding)
        if module=='erosion' and regions:
                                                                             
                                                                           
            regions=[(a.co.min(0)-4*padding,a.co.max(0)+4*padding)]

                                                                         
                                                                             
        ignored_stamps=('project_damage_rebar_of','project_damage_pipes_of','project_damage_cables_of','project_damage_bfrag_of')
        uid=dp._uid(source)
        inside=[(o,MeshData(o.data,source.matrix_world.inverted()@o.matrix_world))
                for o in dp._outputs_inside(source)
                if not any(o.get(stamp)==uid for stamp in ignored_stamps)]
        depth=dp._auto_depth(context,source,module,s)
        if depth is not None:
            body=dp._inner_body(context,source,depth)
            if body is None:notices.append(({'WARNING'},'Fill V2: source cannot support this Surface Layer depth; filled Whole Cut'))
            else:inside.append((body,MeshData(body.data)))
        for ri,(lo,hi) in enumerate(regions):
            temps=[];local=None;out=None
            try:
                local=crop(a,lo,hi,'_pd_v2_source',True)
                if local is None:raise ValueError('local source could not be closed')
                for original,data in wall:
                    target=crop(data,lo-padding,hi+padding,'_pd_v2_result',False)
                    if target is not None:temps.append(dp._temp_object(context,'_pd_v2_result',target,source))
                if not temps:raise ValueError('local damage surfaces could not be closed')
                for original,data in inside:
                    part=crop(data,lo,hi,'_pd_v2_inside',False)
                    if part is not None:temps.append(dp._temp_object(context,'_pd_v2_inside',part,source))
                out,solver=dp._boolean(context,local,source,temps);local=None
                if solver!='MANIFOLD':raise ValueError('local solid requires the slow Exact solver')
                bm=bmesh.new()
                try:
                    bm.from_mesh(out);parts=dp._solid_islands(bm)
                finally:bm.free()
                data=MeshData(out);tol=max(1e-7,float(np.linalg.norm(hi-lo))*1e-5)
                selected=next((a for n,d,k,f,a in data.attrs if n==SELECTED),None)
                outer=next((a for n,d,k,f,a in data.attrs if n=='_pd_fill_outer'),None)
                for ids in parts:
                    low=data.lo[ids].min(0);high=data.hi[ids].max(0)
                                                                                 
                    if np.any(low<=lo+tol) or np.any(high>=hi-tol):dropped+=1;continue
                    part_indices=np.asarray(ids,dtype=np.int64)
                    if selected is None or not np.any(selected[part_indices]):dropped+=1;continue
                    if outer is None or not np.any(outer[part_indices]):dropped+=1;continue
                    piece=data.extract(ids,'_pd_v2_piece')
                    if not valid_piece(piece,minimum,thickness):bpy.data.meshes.remove(piece);dropped+=1;continue
                    pending.append(piece)
            except (ValueError,RuntimeError) as exc:
                errors.append('region %d: %s'%(ri+1,exc))
            finally:
                for obj in temps:dp._remove(obj)
                for mesh in (local,out):
                    if mesh is not None and mesh.users==0:bpy.data.meshes.remove(mesh)
        if not pending:
            from . import fill_exact
            recovered = fill_exact.recover(context, source, module, 'Fill V2')
            if recovered is not None:
                return recovered
            return [],[({'WARNING'},'Fill V2: no valid closed fills; previous fills were kept. '+('; '.join(errors[:3]) if errors else 'No removed volume above the minimum size'))]
        coll=dp._collection(context,source,dp.FILL_COLLECTION,'_DamageFill')
        old=dp.fills_of(source,module)
        try:
            for i,mesh in enumerate(pending):
                obj,volume=dp._make_object(source.name+'_FillV2_%03d'%(i+1),mesh,coll,source)
                obj[dp.FILL_STAMP]=dp._uid(source);obj[dp.FILL_KIND]=module;obj['project_damage_fill_v2']=True
                created.append(obj)
        except Exception:
            for obj in created:dp._remove(obj)
            raise
        for obj in old:dp._remove(obj)
        reports=notices+[({'INFO'},'Fill V2: %d fills from %d local regions in %.2f s'%(len(created),len(regions),time.perf_counter()-started))]
        if errors:reports.append(({'WARNING'},'Fill V2: %d regions skipped (%s)'%(len(errors),'; '.join(errors[:3]))))
        return created,reports
    finally:
        if body is not None:dp._remove(body)
        for mesh in pending:
            try:
                if mesh.users==0:bpy.data.meshes.remove(mesh)
            except ReferenceError:pass
        if base.users==0:bpy.data.meshes.remove(base)
